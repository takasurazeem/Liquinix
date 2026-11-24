// Fotos de várias janelas de uma vez pela captura do Windows (Windows.Graphics.Capture): tudo na placa de
// vídeo, sem passar pela memória do Shell. Cada janela manda quadros (o primeiro em ~1 atualização da
// tela, todas ao mesmo tempo), copiados direto para superfícies do compositor. Sem a borda amarela de
// captura e sem o cursor. A que falhar volta vazia (quem chamou usa a foto por CPU, foto.cpp).
// Abrir uma captura custa ~8 ms (conversa com o DWM): Start abre todas em paralelo e as deixa abertas,
// guardando o último quadro de cada janela; Grab tira as fotos na hora (~10 ms); Stop fecha.
#include "shell.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "windowsapp.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wd3 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
wd3::IDirect3DDevice g_dev{ nullptr };

struct Cap {
    HWND h = nullptr;
    RECT src{};
    wgc::GraphicsCaptureItem item{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    std::mutex m;
    wgc::Direct3D11CaptureFrame frame{ nullptr };  // o último que chegou
    HANDLE ev = nullptr;                            // sinal: chegou o primeiro
    bool failed = false;
};
}  // namespace

struct foto::Live { std::vector<std::shared_ptr<Cap>> caps; };

foto::Live* foto::Start(const std::vector<HWND>& hs, const std::vector<RECT>& srcs) {
    ID3D11Device* d3d = ui::D3D();
    if (!d3d || !ui::Comp() || hs.empty()) return nullptr;
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) return nullptr;
        if (!g_dev) {
            winrt::com_ptr<IDXGIDevice> dx;
            d3d->QueryInterface(IID_PPV_ARGS(dx.put()));
            winrt::com_ptr<::IInspectable> insp;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dx.get(), insp.put()));
            g_dev = insp.as<wd3::IDirect3DDevice>();
        }
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        auto* live = new Live;
        size_t n = std::min<size_t>(hs.size(), 60);
        live->caps.resize(n);
        std::vector<std::thread> ths;  // todas ao mesmo tempo, uma thread por janela
        for (size_t i = 0; i < n; i++) {
            live->caps[i] = std::make_shared<Cap>();
            std::shared_ptr<Cap> c = live->caps[i];
            c->h = hs[i]; c->src = srcs[i];
            c->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ths.emplace_back([c, interop] {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                try {
                    winrt::check_hresult(interop->CreateForWindow(c->h, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(c->item)));
                    c->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(g_dev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, c->item.Size());
                    c->session = c->pool.CreateCaptureSession(c->item);
                    try { c->session.IsCursorCaptureEnabled(false); } catch (...) {}
                    try { c->session.IsBorderRequired(false); } catch (...) {}
                    std::weak_ptr<Cap> wk = c;  // um quadro atrasado depois do Stop não encontra nada
                    c->pool.FrameArrived([wk](wgc::Direct3D11CaptureFramePool const& p, auto&&) {
                        auto c = wk.lock();
                        if (!c) return;
                        auto f = p.TryGetNextFrame();
                        if (!f) return;
                        wgc::Direct3D11CaptureFrame old{ nullptr };
                        { std::lock_guard<std::mutex> lk(c->m); old = c->frame; c->frame = f; }
                        if (old) old.Close();  // devolve o buffer: o próximo quadro pode chegar
                        SetEvent(c->ev);
                    });
                    c->session.StartCapture();
                } catch (...) {
                    c->failed = true;
                    SetEvent(c->ev);
                }
                winrt::uninit_apartment();
            });
        }
        for (auto& t : ths) t.join();
        return live;
    } catch (...) {
        Log(L"foto (GPU): captura indisponível");
        return nullptr;
    }
}

std::vector<foto::Gpu> foto::Grab(Live* live, int timeoutMs) {
    std::vector<Gpu> out(live ? live->caps.size() : 0);
    if (!live) return out;
    ID3D11Device* d3d = ui::D3D();
    IDCompositionDevice* cd = ui::Comp();
    std::vector<HANDLE> evs;
    for (auto& c : live->caps) evs.push_back(c->ev);
    if (!evs.empty()) WaitForMultipleObjects((DWORD)evs.size(), evs.data(), TRUE, (DWORD)timeoutMs);
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    d3d->GetImmediateContext(ctx.put());
    for (size_t i = 0; i < live->caps.size(); i++) {
        Cap* c = live->caps[i].get();
        if (c->failed) continue;
        try {
            std::lock_guard<std::mutex> lk(c->m);  // o quadro não é trocado enquanto copia
            if (!c->frame) continue;
            auto access = c->frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
            D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
            auto cs = c->frame.ContentSize();
            // o quadro é a janela inteira (com a borda invisível), como a miniatura do DWM: o mesmo recorte
            RECT wr; GetWindowRect(c->h, &wr);
            RECT s = c->src;
            int fw = std::min<int>(cs.Width, (int)d.Width), fh = std::min<int>(cs.Height, (int)d.Height);
            if (fw != wr.right - wr.left || fh != wr.bottom - wr.top) {  // quadro só da parte visível
                RECT efb = wr; DwmGetWindowAttribute(c->h, DWMWA_EXTENDED_FRAME_BOUNDS, &efb, sizeof(efb));
                OffsetRect(&s, -(efb.left - wr.left), -(efb.top - wr.top));
            }
            s.left = std::clamp<LONG>(s.left, 0, fw); s.right = std::clamp<LONG>(s.right, 0, fw);
            s.top = std::clamp<LONG>(s.top, 0, fh); s.bottom = std::clamp<LONG>(s.bottom, 0, fh);
            UINT w = (UINT)(s.right - s.left), h = (UINT)(s.bottom - s.top);
            IDCompositionSurface* surf = nullptr;
            if (w < 4 || h < 4 || FAILED(cd->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surf))) continue;
            ID3D11Texture2D* dst = nullptr; POINT off{};
            if (SUCCEEDED(surf->BeginDraw(nullptr, IID_PPV_ARGS(&dst), &off))) {
                D3D11_BOX box{ (UINT)s.left, (UINT)s.top, 0, (UINT)s.right, (UINT)s.bottom, 1 };
                ctx->CopySubresourceRegion(dst, 0, off.x, off.y, 0, tex.get(), 0, &box);
                dst->Release();
                surf->EndDraw();
                out[i] = { surf, w, h };
            } else surf->Release();
        } catch (...) {}
    }
    return out;
}

void foto::Stop(Live* live) {
    if (!live) return;
    for (auto& c : live->caps) {
        try {
            if (c->session) c->session.Close();
            if (c->pool) c->pool.Close();   // fora da trava: um quadro chegando termina antes
            std::lock_guard<std::mutex> lk(c->m);
            if (c->frame) { c->frame.Close(); c->frame = nullptr; }
        } catch (...) {}
        if (c->ev) CloseHandle(c->ev);
    }
    delete live;
}

std::vector<foto::Gpu> foto::Windows(const std::vector<HWND>& hs, const std::vector<RECT>& srcs, int timeoutMs) {
    double t0 = anim::Now();
    Live* live = Start(hs, srcs);
    if (!live) return std::vector<Gpu>(hs.size());
    double t1 = anim::Now();
    auto out = Grab(live, timeoutMs);
    double t2 = anim::Now();
    Stop(live);
    out.resize(hs.size());
    Log(L"foto (GPU): %zu janelas, abrir capturas %.1f ms, fotos %.1f ms", hs.size(), t1 - t0, t2 - t1);
    return out;
}
