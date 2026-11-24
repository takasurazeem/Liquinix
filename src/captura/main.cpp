// Captura de Tela do macOS (Cmd+Shift+3/4/5) no lugar da Ferramenta de Captura: WacOSScreenshot.exe.
// Fica residente (o Shell abre com /residente, sem janela visível) para a mira aparecer na hora: o Shell
// manda a mensagem registrada "SequoiaCaptura" para a janela "SequoiaCapturaMsg" (wParam 3 = tela inteira,
// 4 = região/janela, 5 = barra de ferramentas; lParam 1 = Ctrl seguro: só para a área de transferência).
// Linha de comando: /3 /4 /5 [/c] (já captura; com outro aberto, repassa para ele), /residente.
//
// Como no Mac: a tela é congelada no instante do atalho (a foto é tirada antes de qualquer janela nossa
// aparecer) e as janelas que cobrem cada monitor mostram essa foto com a mira, o escurecimento e a barra.
// A janela é fotografada sozinha pela captura do Windows (Windows.Graphics.Capture), mesmo coberta, com os
// cantos arredondados e a sombra do Mac em PNG transparente. Coordenadas sempre em pixels físicos (o
// processo é Per-Monitor V2): DPI alto e vários monitores saem certos.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <unknwn.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "../comum/nomes.h"
#include "../comum/simbolos.h"
#include "../menu/context_menu.h"
#include "../comum/idioma.h"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winmm.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wd3 = winrt::Windows::Graphics::DirectX::Direct3D11;

static HINSTANCE g_inst;
static FILE* g_log;
static ID2D1Factory* g_f;
static IDWriteFactory* g_dw;
static IWICImagingFactory* g_wic;
static std::wstring g_dir, g_root, g_family = L"Segoe UI Variable Text";
static HWND g_msg;
static UINT WM_CAPTURA;
const UINT WM_APP_KEY = WM_APP + 1;
const UINT_PTR T_COUNT = 7;

static void Logf(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(g_log, L"%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

static double Now() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- opções (Shell\captura.txt)
struct Cfg {
    int dest = 0;      // 0 Mesa, 1 Documentos, 2 Área de Transferência
    int timer = 0;     // 0, 5, 10 segundos
    int mode = 2;      // última ferramenta da barra: 0 tela inteira, 1 janela, 2 região
    bool thumb = true, remember = true, pointer = false, hasSel = false;
    RECT sel{};        // última região (pixels físicos da área de trabalho virtual)
} g_cfg;

static std::wstring CfgDir() {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell"; CoTaskMemFree(p); }
    return d;
}

static void LoadCfg() {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (CfgDir() + L"\\captura.txt").c_str(), L"r, ccs=UTF-8") || !fp) return;
    wchar_t line[256];
    while (fgetws(line, 256, fp)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos || s[0] == L'#') continue;
        std::wstring k = s.substr(0, eq), v = s.substr(eq + 1);
        int n = _wtoi(v.c_str());
        if (k == L"destino") g_cfg.dest = std::clamp(n, 0, 2);
        else if (k == L"temporizador") g_cfg.timer = n == 5 || n == 10 ? n : 0;
        else if (k == L"modo") g_cfg.mode = std::clamp(n, 0, 2);
        else if (k == L"miniatura") g_cfg.thumb = n != 0;
        else if (k == L"lembrar") g_cfg.remember = n != 0;
        else if (k == L"ponteiro") g_cfg.pointer = n != 0;
        else if (k == L"selecao") {
            RECT r{};
            if (swscanf_s(v.c_str(), L"%ld,%ld,%ld,%ld", &r.left, &r.top, &r.right, &r.bottom) == 4 && r.right - r.left >= 4 && r.bottom - r.top >= 4) { g_cfg.sel = r; g_cfg.hasSel = true; }
        }
    }
    fclose(fp);
}

static void SaveCfg() {
    std::wstring d = CfgDir();
    if (d.empty()) return;
    CreateDirectoryW(d.c_str(), nullptr);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (d + L"\\captura.txt").c_str(), L"w, ccs=UTF-8") || !fp) return;
    fwprintf(fp, L"# Captura de Tela (WacOSScreenshot.exe): opções do menu Opções da barra (Win+Shift+5)\n");
    fwprintf(fp, L"destino=%d\ntemporizador=%d\nmodo=%d\nminiatura=%d\nlembrar=%d\nponteiro=%d\n", g_cfg.dest, g_cfg.timer, g_cfg.mode, g_cfg.thumb, g_cfg.remember, g_cfg.pointer);
    if (g_cfg.hasSel) fwprintf(fp, L"selecao=%ld,%ld,%ld,%ld\n", g_cfg.sel.left, g_cfg.sel.top, g_cfg.sel.right, g_cfg.sel.bottom);
    fclose(fp);
}

// ---------------------------------------------------------------- monitores
struct Mon { HMONITOR h = nullptr; RECT rc{}, work{}; float sc = 1; bool primary = false; };

static std::vector<Mon> Monitors() {
    std::vector<Mon> v;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
        UINT dx = 96, dy = 96; GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        ((std::vector<Mon>*)lp)->push_back({ hm, mi.rcMonitor, mi.rcWork, dx / 96.f, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        return TRUE;
    }, (LPARAM)&v);
    return v;
}

static int MonIndexAt(const std::vector<Mon>& ms, POINT p) {
    for (size_t i = 0; i < ms.size(); i++) if (PtInRect(&ms[i].rc, p)) return (int)i;
    for (size_t i = 0; i < ms.size(); i++) if (ms[i].primary) return (int)i;
    return ms.empty() ? -1 : 0;
}

static POINT Center(const RECT& r) { return { (r.left + r.right) / 2, (r.top + r.bottom) / 2 }; }

// ---------------------------------------------------------------- imagens
struct Img { int w = 0, h = 0; std::vector<BYTE> px; bool alpha = false; };  // BGRA, alfa direto (não pré-multiplicado)
struct Shot { RECT vr{}; Img img; HCURSOR cur = nullptr; POINT curPt{}; };

static bool Snap(Shot& s) {
    double t0 = Now();
    s.vr.left = GetSystemMetrics(SM_XVIRTUALSCREEN); s.vr.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    s.vr.right = s.vr.left + GetSystemMetrics(SM_CXVIRTUALSCREEN); s.vr.bottom = s.vr.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int w = s.vr.right - s.vr.left, h = s.vr.bottom - s.vr.top;
    bool ok = false;
    // vidro real do Shell (Liquid Glass): Dock, barra de menus e painéis ficam fora das capturas para o
    // vidro não ver a si mesmo; na hora da foto, o Shell os mostra (e esconde de novo logo depois)
    static UINT vis = RegisterWindowMessageW(L"ShellCapturaVisivel");
    HWND vidro = FindWindowW(L"ShellMensagens", nullptr);
    if (vidro) SendMessageTimeoutW(vidro, vis, 1, 0, SMTO_ABORTIFHUNG, 300, nullptr);
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bm) {
        HGDIOBJ old = SelectObject(mdc, bm);
        ok = BitBlt(mdc, 0, 0, w, h, sdc, s.vr.left, s.vr.top, SRCCOPY | CAPTUREBLT) != 0;
        if (vidro) SendMessageTimeoutW(vidro, vis, 0, 0, SMTO_ABORTIFHUNG, 300, nullptr);
        GdiFlush();
        if (ok) {
            s.img.w = w; s.img.h = h; s.img.alpha = false;
            s.img.px.assign((BYTE*)bits, (BYTE*)bits + (size_t)w * h * 4);
            for (size_t i = 3; i < s.img.px.size(); i += 4) s.img.px[i] = 255;
        }
        SelectObject(mdc, old); DeleteObject(bm);
    }
    DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    CURSORINFO ci{ sizeof(ci) };
    s.cur = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) ? ci.hCursor : nullptr;
    s.curPt = ci.ptScreenPos;
    Logf(L"foto da tela %dx%d em (%ld,%ld): %.0f ms%ls", w, h, s.vr.left, s.vr.top, Now() - t0, ok ? L"" : L" (FALHOU)");
    return ok;
}

static Img Crop(const Img& s, RECT r) {  // r nas coordenadas da imagem
    r.left = std::clamp<LONG>(r.left, 0, s.w); r.right = std::clamp<LONG>(r.right, 0, s.w);
    r.top = std::clamp<LONG>(r.top, 0, s.h); r.bottom = std::clamp<LONG>(r.bottom, 0, s.h);
    Img o; o.w = r.right - r.left; o.h = r.bottom - r.top; o.alpha = s.alpha;
    if (o.w <= 0 || o.h <= 0) { o.w = o.h = 0; return o; }
    o.px.resize((size_t)o.w * o.h * 4);
    for (int y = 0; y < o.h; y++) memcpy(&o.px[(size_t)y * o.w * 4], &s.px[((size_t)(r.top + y) * s.w + r.left) * 4], (size_t)o.w * 4);
    return o;
}

// o ponteiro do mouse (opção "Mostrar Ponteiro do Mouse") por cima de uma imagem opaca
static void DrawPointer(Img& im, POINT origin, HCURSOR c, POINT pt) {
    if (!c || im.w <= 0 || im.alpha) return;
    ICONINFO ii{};
    if (!GetIconInfo(c, &ii)) return;
    int x = pt.x - (int)ii.xHotspot - origin.x, y = pt.y - (int)ii.yHotspot - origin.y;
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (x > im.w || y > im.h || x < -256 || y < -256) return;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), im.w, -im.h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bm) {
        memcpy(bits, im.px.data(), im.px.size());
        HGDIOBJ old = SelectObject(mdc, bm);
        DrawIconEx(mdc, x, y, c, 0, 0, 0, nullptr, DI_NORMAL);
        GdiFlush();
        memcpy(im.px.data(), bits, im.px.size());
        for (size_t i = 3; i < im.px.size(); i += 4) im.px[i] = 255;
        SelectObject(mdc, old); DeleteObject(bm);
    }
    DeleteDC(mdc); ReleaseDC(nullptr, sdc);
}

static bool EncodePng(const Img& im, float dpi, IStream* st) {
    IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* bag = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &bag)) && SUCCEEDED(fr->Initialize(bag))) {
        fr->SetSize(im.w, im.h);
        fr->SetResolution(dpi, dpi);  // como o Mac grava 144 ppp nas telas Retina
        WICPixelFormatGUID want = im.alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR, fmt = want;
        fr->SetPixelFormat(&fmt);
        HRESULT hr = E_FAIL;
        if (fmt == GUID_WICPixelFormat32bppBGRA) {
            std::vector<BYTE> b = im.px;
            if (!im.alpha) for (size_t i = 3; i < b.size(); i += 4) b[i] = 255;
            hr = fr->WritePixels(im.h, im.w * 4, (UINT)b.size(), b.data());
        } else if (fmt == GUID_WICPixelFormat24bppBGR) {
            UINT stride = (im.w * 3 + 3) & ~3u;
            std::vector<BYTE> b((size_t)stride * im.h);
            for (int y = 0; y < im.h; y++) {
                const BYTE* s = &im.px[(size_t)y * im.w * 4];
                BYTE* d = &b[(size_t)y * stride];
                for (int x = 0; x < im.w; x++, s += 4, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            }
            hr = fr->WritePixels(im.h, stride, (UINT)b.size(), b.data());
        }
        ok = SUCCEEDED(hr) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    Rel(bag); Rel(fr); Rel(enc);
    return ok;
}

static bool SavePng(const Img& im, float dpi, const std::wstring& path) {
    IWICStream* s = nullptr;
    bool ok = SUCCEEDED(g_wic->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) && EncodePng(im, dpi, s);
    Rel(s);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// área de transferência: "PNG" (com transparência) e CF_DIB (sobre branco, para quem não lê PNG)
static bool ToClipboard(const Img& im, float dpi) {
    HGLOBAL png = nullptr;
    IStream* ms = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &ms)) && EncodePng(im, dpi, ms)) {
        STATSTG stt{}; ms->Stat(&stt, STATFLAG_NONAME);
        HGLOBAL src = nullptr; GetHGlobalFromStream(ms, &src);
        SIZE_T n = (SIZE_T)stt.cbSize.QuadPart;
        png = GlobalAlloc(GMEM_MOVEABLE, n);
        if (png && src) { memcpy(GlobalLock(png), GlobalLock(src), n); GlobalUnlock(png); GlobalUnlock(src); }
    }
    Rel(ms);
    size_t row = (size_t)im.w * 4;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + row * im.h);
    if (dib) {
        BYTE* p = (BYTE*)GlobalLock(dib);
        BITMAPINFOHEADER bh{ sizeof(bh), im.w, im.h, 1, 32, BI_RGB };
        bh.biXPelsPerMeter = bh.biYPelsPerMeter = (LONG)std::lround(dpi / 0.0254f);
        memcpy(p, &bh, sizeof(bh));
        BYTE* d = p + sizeof(bh);
        for (int y = 0; y < im.h; y++) {  // de baixo para cima
            const BYTE* s = &im.px[(size_t)(im.h - 1 - y) * row];
            BYTE* o = d + (size_t)y * row;
            for (int x = 0; x < im.w; x++, s += 4, o += 4) {
                int a = im.alpha ? s[3] : 255;
                for (int k = 0; k < 3; k++) o[k] = (BYTE)((s[k] * a + 255 * (255 - a) + 127) / 255);
                o[3] = 255;
            }
        }
        GlobalUnlock(dib);
    }
    bool ok = false;
    for (int i = 0; i < 10 && !ok; i++) { ok = OpenClipboard(g_msg) != 0; if (!ok) Sleep(20); }
    if (ok) {
        EmptyClipboard();
        if (dib && SetClipboardData(CF_DIB, dib)) dib = nullptr;
        if (png && SetClipboardData(RegisterClipboardFormatW(L"PNG"), png)) png = nullptr;
        CloseClipboard();
    }
    if (dib) GlobalFree(dib);
    if (png) GlobalFree(png);
    return ok;
}

// ---------------------------------------------------------------- janelas
struct WinItem { HWND h; RECT r; };

static std::vector<WinItem> ListWindows() {
    std::vector<WinItem> v;
    DWORD me = GetCurrentProcessId();
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h)) continue;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == me) continue;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT) continue;  // sombras e camadas que deixam o clique passar
        BOOL cloaked = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue;
        if (ex & WS_EX_LAYERED) {
            BYTE a = 255; DWORD fl = 0; COLORREF key;
            if (GetLayeredWindowAttributes(h, &key, &a, &fl) && (fl & LWA_ALPHA) && a == 0) continue;
        }
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
        if (r.right - r.left < 8 || r.bottom - r.top < 8) continue;
        wchar_t cls[64] = L""; GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) continue;
        v.push_back({ h, r });
    }
    return v;
}

// Windows.Graphics.Capture de uma janela só: a imagem dela inteira mesmo se estiver coberta
static ID3D11Device* g_d3d;
static ID3D11DeviceContext* g_ctx;

static bool EnsureD3D() {
    if (g_d3d) return true;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr, &g_ctx))) return false;
    if (ID3D11Multithread* mt = nullptr; SUCCEEDED(g_d3d->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    return true;
}

static bool WgcWindow(HWND h, const RECT& efb, Img& out, HMONITOR mon = nullptr) {
    if (!EnsureD3D()) return false;
    bool ok = false;
    double t0 = Now();
    std::thread([&] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            // o dispositivo WinRT nasce nesta thread (MTA): criado na thread da janela (STA, parada no join)
            // as chamadas voltariam para ela e travavam
            winrt::com_ptr<IDXGIDevice> dx;
            g_d3d->QueryInterface(IID_PPV_ARGS(dx.put()));
            winrt::com_ptr<::IInspectable> insp;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dx.get(), insp.put()));
            auto wdev = insp.as<wd3::IDirect3DDevice>();
            if (wgc::GraphicsCaptureSession::IsSupported()) {
                auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
                wgc::GraphicsCaptureItem item{ nullptr };
                if (mon) winrt::check_hresult(interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
                else winrt::check_hresult(interop->CreateForWindow(h, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
                auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(wdev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
                auto ses = pool.CreateCaptureSession(item);
                try { ses.IsCursorCaptureEnabled(false); } catch (...) {}
                try { ses.IsBorderRequired(false); } catch (...) {}
                ses.StartCapture();
                wgc::Direct3D11CaptureFrame f{ nullptr };
                for (int i = 0; i < 120 && !f; i++) { f = pool.TryGetNextFrame(); if (!f) Sleep(5); }
                if (f) {
                    auto access = f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    winrt::com_ptr<ID3D11Texture2D> tex;
                    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
                    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
                    auto cs = f.ContentSize();
                    int fw = std::min<int>(cs.Width, (int)d.Width), fh = std::min<int>(cs.Height, (int)d.Height);
                    // o quadro é a janela inteira (com a borda invisível) ou só a parte visível (EXTENDED_FRAME_BOUNDS)
                    RECT wr = efb; if (!mon) GetWindowRect(h, &wr);
                    RECT s{ 0, 0, efb.right - efb.left, efb.bottom - efb.top };
                    if (fw == wr.right - wr.left && fh == wr.bottom - wr.top) OffsetRect(&s, efb.left - wr.left, efb.top - wr.top);
                    s.left = std::clamp<LONG>(s.left, 0, fw); s.right = std::clamp<LONG>(s.right, 0, fw);
                    s.top = std::clamp<LONG>(s.top, 0, fh); s.bottom = std::clamp<LONG>(s.bottom, 0, fh);
                    D3D11_TEXTURE2D_DESC sd = d;
                    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
                    winrt::com_ptr<ID3D11Texture2D> stg;
                    if (s.right - s.left >= 4 && s.bottom - s.top >= 4 && SUCCEEDED(g_d3d->CreateTexture2D(&sd, nullptr, stg.put()))) {
                        g_ctx->CopyResource(stg.get(), tex.get());
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(g_ctx->Map(stg.get(), 0, D3D11_MAP_READ, 0, &m))) {
                            out.w = s.right - s.left; out.h = s.bottom - s.top; out.alpha = true;
                            out.px.resize((size_t)out.w * out.h * 4);
                            for (int y = 0; y < out.h; y++) memcpy(&out.px[(size_t)y * out.w * 4], (BYTE*)m.pData + (size_t)(s.top + y) * m.RowPitch + (size_t)s.left * 4, (size_t)out.w * 4);
                            g_ctx->Unmap(stg.get(), 0);
                            ok = true;
                        }
                    }
                    f.Close();
                }
                ses.Close(); pool.Close();
            }
        } catch (winrt::hresult_error const& e) { Logf(L"captura do Windows: erro 0x%08X %ls", (unsigned)e.code(), e.message().c_str()); } catch (...) { Logf(L"captura do Windows: erro"); }
        winrt::uninit_apartment();
    }).join();
    Logf(L"janela %p pela captura do Windows: %ls (%dx%d, %.0f ms)", h, ok ? L"ok" : L"falhou", out.w, out.h, Now() - t0);
    return ok;
}

// cantos arredondados do Windows 11 (alfa direto)
static void Corners(Img& im, float r) {
    int R = (int)std::ceil(r);
    for (int y = 0; y < R && y < im.h; y++)
        for (int x = 0; x < R && x < im.w; x++) {
            float dx = r - (x + 0.5f), dy = r - (y + 0.5f);
            float a = std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
            if (a >= 1) continue;
            int xs[2] = { x, im.w - 1 - x }, ys[2] = { y, im.h - 1 - y };
            for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
                BYTE* c = &im.px[((size_t)ys[j] * im.w + xs[i]) * 4];
                c[3] = (BYTE)std::lround(c[3] * a);
            }
        }
}

// perfil 1D do desfoque gaussiano de um intervalo [a, b] (a sombra de um retângulo é o produto de dois)
static std::vector<float> Profile(int n, float a, float b, float sig) {
    std::vector<float> v(n);
    float k = 1.f / (sig * 1.41421356f);
    for (int i = 0; i < n; i++) { float x = i + 0.5f; v[i] = 0.5f * (std::erf((x - a) * k) - std::erf((x - b) * k)); }
    return v;
}

// a sombra das janelas do Mac em volta da foto (PNG transparente, como o Cmd+Shift+4 e Espaço)
static Img WithShadow(const Img& win, float sc) {
    float sig = 17 * sc, off = 13 * sc;
    int side = (int)std::ceil(2.6f * sig), top = std::max(0, side - (int)off), bot = side + (int)off;
    Img o; o.w = win.w + 2 * side; o.h = win.h + top + bot; o.alpha = true;
    o.px.assign((size_t)o.w * o.h * 4, 0);
    float x0 = (float)side, x1 = (float)(side + win.w), y0 = (float)top, y1 = (float)(top + win.h);
    auto px = Profile(o.w, x0 + 2 * sc, x1 - 2 * sc, sig), py = Profile(o.h, y0 + off, y1 + off, sig);  // sombra larga, para baixo
    auto qx = Profile(o.w, x0, x1, 1.2f * sc), qy = Profile(o.h, y0 + 0.5f * sc, y1 + 0.5f * sc, 1.2f * sc);  // contorno fino
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            float a1 = 0.48f * px[x] * py[y], a2 = 0.28f * qx[x] * qy[y];
            float as = 1 - (1 - a1) * (1 - a2);
            BYTE* d = &o.px[((size_t)y * o.w + x) * 4];
            int wx = x - side, wy = y - top;
            float aw = 0; const BYTE* s = nullptr;
            if (wx >= 0 && wy >= 0 && wx < win.w && wy < win.h) { s = &win.px[((size_t)wy * win.w + wx) * 4]; aw = s[3] / 255.f; }
            float ao = aw + as * (1 - aw);
            if (ao <= 0) continue;
            for (int k = 0; k < 3; k++) d[k] = s ? (BYTE)std::lround(s[k] * aw / ao) : 0;
            d[3] = (BYTE)std::lround(ao * 255);
        }
    return o;
}

static bool Win11() {
    static int v = -1;
    if (v < 0) {
        typedef LONG(WINAPI * Fn)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW os{ sizeof(os) };
        auto f = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        v = f && f(&os) == 0 && os.dwBuildNumber >= 22000;
    }
    return v == 1;
}

static Img WindowImage(HWND h, const RECT& efb, const Shot* shot, bool shadow, float sc) {
    Img im;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE), st = GetWindowLongW(h, GWL_STYLE);
    bool layered = (ex & WS_EX_LAYERED) != 0;
    if (WgcWindow(h, efb, im)) {
        if (layered) {  // transparência de verdade: tira a pré-multiplicação
            for (size_t i = 0; i < im.px.size(); i += 4) {
                int a = im.px[i + 3];
                if (a > 0 && a < 255) for (int k = 0; k < 3; k++) im.px[i + k] = (BYTE)std::min(255, im.px[i + k] * 255 / a);
            }
        } else for (size_t i = 3; i < im.px.size(); i += 4) im.px[i] = 255;
    } else if (shot) {  // sem a captura do Windows: o pedaço da foto da tela
        RECT r = efb; OffsetRect(&r, -shot->vr.left, -shot->vr.top);
        im = Crop(shot->img, r);
    }
    if (im.w <= 0) return im;
    im.alpha = true;
    if (Win11() && !IsZoomed(h) && !layered && (st & (WS_CAPTION | WS_THICKFRAME))) Corners(im, 8 * sc);
    if (shadow) im = WithShadow(im, sc);
    return im;
}

// ---------------------------------------------------------------- desenho (Direct2D)
static IDWriteTextFormat* Fmt(float px, DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_NORMAL) {
    static std::map<std::pair<int, int>, IDWriteTextFormat*> cache;
    auto key = std::make_pair((int)std::lround(px * 10), (int)wt);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(g_family.c_str(), nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, idioma::Locale(), &f);
    if (f) { f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    cache[key] = f;
    return f;
}

static float TextW(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr; float w = 0;
    if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 100, &l))) { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; l->Release(); }
    return w;
}

static void Text(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
    f->SetTextAlignment(al);
    br->SetColor(c);
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, br, D2D1_DRAW_TEXT_OPTIONS_NONE);
}

static std::wstring PickFamily(std::initializer_list<const wchar_t*> names) {
    IDWriteFontCollection* c = nullptr;
    std::wstring out = L"Segoe UI";
    if (SUCCEEDED(g_dw->GetSystemFontCollection(&c, FALSE))) {
        for (auto n : names) { UINT32 i; BOOL ex = FALSE; if (SUCCEEDED(c->FindFamilyName(n, &i, &ex)) && ex) { out = n; break; } }
        c->Release();
    }
    return out;
}

// janela em camadas (miniatura, contagem regressiva): desenha num DIB com Direct2D e UpdateLayeredWindow
struct Layer {
    HWND h = nullptr; HDC mdc = nullptr; HBITMAP bm = nullptr; HGDIOBJ old = nullptr; void* bits = nullptr; int w = 0, hh = 0;
    ID2D1DCRenderTarget* rt = nullptr;
    bool Create(const wchar_t* cls, WNDPROC proc, int W, int H, DWORD exExtra) {
        WNDCLASSEXW wc{ sizeof(wc) };
        if (!GetClassInfoExW(g_inst, cls, &wc)) { wc = { sizeof(wc) }; wc.lpfnWndProc = proc; wc.hInstance = g_inst; wc.lpszClassName = cls; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassExW(&wc); }
        h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | exExtra, cls, L"", WS_POPUP, 0, 0, W, H, nullptr, nullptr, g_inst, nullptr);
        w = W; hh = H;
        mdc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
        bm = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old = SelectObject(mdc, bm);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        g_f->CreateDCRenderTarget(&props, &rt);
        RECT rc{ 0, 0, W, H };
        if (rt) rt->BindDC(mdc, &rc);
        return h && bm && rt;
    }
    void Show(POINT pt, BYTE alpha) {
        POINT z{}; SIZE sz{ w, hh };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
        UpdateLayeredWindow(h, nullptr, &pt, &sz, mdc, &z, 0, &bf, ULW_ALPHA);
        if (!IsWindowVisible(h)) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    void Destroy() {
        Rel(rt);
        if (mdc) { SelectObject(mdc, old); DeleteDC(mdc); mdc = nullptr; }
        if (bm) { DeleteObject(bm); bm = nullptr; }
        if (h) { DestroyWindow(h); h = nullptr; }
    }
};

// ---------------------------------------------------------------- som, nome do arquivo, entrega
static void Shutter() {
    PlaySoundW((g_root + L"\\sons\\mac\\Screen_Capture.wav").c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

static std::wstring DestFolder() {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(g_cfg.dest == 1 ? FOLDERID_Documents : FOLDERID_Desktop, 0, nullptr, &p))) { d = p; CoTaskMemFree(p); }
    return d;
}

static void ShowThumb(const Img& im, const std::wstring& path, const Mon& m);
static void DismissThumb();
static bool g_thumbOn;

static void Deliver(std::vector<Img>& ims, const std::vector<float>& dpis, bool clip, const Mon& where) {
    if (ims.empty() || ims[0].w <= 0) { Logf(L"nada para entregar"); return; }
    Shutter();
    if (clip) {
        bool ok = ToClipboard(ims[0], dpis[0]);
        Logf(L"captura %dx%d na área de transferência: %ls", ims[0].w, ims[0].h, ok ? L"ok" : L"FALHOU");
        return;
    }
    std::wstring folder = DestFolder();
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t base[128];
    swprintf_s(base, Tr(L"Captura de Tela %04d-%02d-%02d às %02d.%02d.%02d"), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring first;
    for (size_t i = 0; i < ims.size(); i++) {
        if (ims[i].w <= 0) continue;
        std::wstring name = base;
        if (ims.size() > 1) name += L" (" + std::to_wstring(i + 1) + L")";
        std::wstring path = folder + L"\\" + name + L".png";
        for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; n++) path = folder + L"\\" + name + L" " + std::to_wstring(n) + L".png";
        double t0 = Now();
        bool ok = SavePng(ims[i], dpis[i], path);
        Logf(L"captura %dx%d (%.0f ppp) em %ls: %ls (%.0f ms)", ims[i].w, ims[i].h, dpis[i], path.c_str(), ok ? L"ok" : L"FALHOU", Now() - t0);
        if (ok) { SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr); if (first.empty()) first = path; }
    }
    if (g_cfg.thumb && !first.empty()) ShowThumb(ims[0], first, where);
}

// ---------------------------------------------------------------- capturas
enum JobKind { J_SCREENS, J_REGION, J_WINDOW };
struct Job { JobKind kind = J_SCREENS; RECT r{}; HWND w = nullptr; int mon = -1; bool clip = false, shadow = true; };

static void Run(Job j, Shot* frozen) {
    Shot fresh;
    Shot* shot = frozen;
    std::vector<Mon> mons = Monitors();
    if (!shot && j.kind != J_WINDOW) { DwmFlush(); if (!Snap(fresh)) return; shot = &fresh; }
    std::vector<Img> ims; std::vector<float> dpis;
    int where = MonIndexAt(mons, Center(j.r));
    auto pointer = [&](Img& im, POINT origin) { if (g_cfg.pointer && shot) DrawPointer(im, origin, shot->cur, shot->curPt); };
    if (j.kind == J_SCREENS) {
        for (size_t i = 0; i < mons.size(); i++) {
            if (j.mon >= 0 && (int)i != j.mon) continue;
            RECT r = mons[i].rc; OffsetRect(&r, -shot->vr.left, -shot->vr.top);
            ims.push_back(Crop(shot->img, r)); dpis.push_back(96 * mons[i].sc);
            pointer(ims.back(), { mons[i].rc.left, mons[i].rc.top });
        }
        // a miniatura vai para o monitor principal (ou o escolhido)
        where = j.mon >= 0 ? j.mon : MonIndexAt(mons, { 0, 0 });
        // o principal primeiro, como a numeração do Mac
        if (j.mon < 0) for (size_t i = 0; i < mons.size(); i++) if (mons[i].primary && i) { std::swap(ims[0], ims[i]); std::swap(dpis[0], dpis[i]); }
    } else if (j.kind == J_REGION) {
        RECT r = j.r; OffsetRect(&r, -shot->vr.left, -shot->vr.top);
        ims.push_back(Crop(shot->img, r)); dpis.push_back(where >= 0 ? 96 * mons[where].sc : 96);
        pointer(ims.back(), { j.r.left, j.r.top });
    } else {
        if (!IsWindow(j.w)) { Logf(L"a janela sumiu"); return; }
        RECT efb{};
        if (FAILED(DwmGetWindowAttribute(j.w, DWMWA_EXTENDED_FRAME_BOUNDS, &efb, sizeof(efb)))) GetWindowRect(j.w, &efb);
        where = MonIndexAt(mons, Center(efb));
        float sc = where >= 0 ? mons[where].sc : 1;
        ims.push_back(WindowImage(j.w, efb, frozen, j.shadow, sc)); dpis.push_back(96 * sc);
    }
    if (where < 0 || mons.empty()) return;
    Deliver(ims, dpis, j.clip, mons[where]);
}

// ---------------------------------------------------------------- sessão interativa (Win+Shift+4 / 5)
enum Mode { M_SCREEN, M_WINDOW, M_REGION };
enum DragKind { D_NONE, D_NEW, D_MOVE, D_SIZE, D_BAR, D_CLICK };
enum Btn { B_CLOSE, B_SCREEN, B_WINDOW, B_REGION, B_RSCREEN, B_RREGION, B_OPTIONS, B_CAPTURE, B_COUNT };

struct Ov {
    HWND h = nullptr; Mon m;
    ID2D1HwndRenderTarget* rt = nullptr; ID2D1Bitmap* bmp = nullptr; ID2D1SolidColorBrush* br = nullptr; ID2D1StrokeStyle* dash = nullptr;
};
static std::vector<Ov> g_ov;

struct Sess {
    bool on = false, bar = false, clip = false;
    Mode mode = M_REGION;
    Shot shot;
    std::vector<Mon> mons;
    std::vector<WinItem> wins;
    int hover = -1;
    DragKind drag = D_NONE; int handle = -1;
    POINT p0{}, cur{}, last{};
    RECT sel{}, sel0{}; bool hasSel = false, moved = false;
    bool space = false, alt = false;
    int barOv = 0; RECT btn[B_COUNT]{}; RECT barRc{}; int hot = -1, press = -1;
    HHOOK kb = nullptr;
} S;

static HCURSOR g_cross, g_camera;
static float g_cursorSc;

static RECT Norm(POINT a, POINT b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) }; }

static RECT SelNow() {
    if (S.drag == D_NEW) {
        POINT a = S.p0;
        if (S.alt) a = { 2 * S.p0.x - S.cur.x, 2 * S.p0.y - S.cur.y };  // Option: a partir do centro
        return Norm(a, S.cur);
    }
    return S.sel;
}

// a mira do Mac: cruz fina preta com contorno branco
static HCURSOR MakeCross(float sc) {
    int R = std::max(8, (int)std::lround(11 * sc)), n = 2 * R + 1;
    std::vector<uint32_t> px((size_t)n * n, 0);
    int t = std::max(1, (int)std::lround(sc));  // espessura da linha preta
    int o = std::max(1, (int)std::lround(sc));  // contorno branco
    auto box = [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = std::max(0, y0); y <= std::min(n - 1, y1); y++) for (int x = std::max(0, x0); x <= std::min(n - 1, x1); x++) px[(size_t)y * n + x] = c;
    };
    int a = R - t / 2, b = a + t - 1;
    box(a - o, 0, b + o, n - 1, 0xFFFFFFFF); box(0, a - o, n - 1, b + o, 0xFFFFFFFF);
    box(a, o, b, n - 1 - o, 0xFF000000); box(o, a, n - 1 - o, b, 0xFF000000);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP col = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!col) return LoadCursorW(nullptr, IDC_CROSS);
    memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{ FALSE, (DWORD)R, (DWORD)R, mask, col };
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(col);
    return c ? c : LoadCursorW(nullptr, IDC_CROSS);
}

// o ponteiro de câmera do modo janela
static HCURSOR MakeCamera(float sc) {
    int n = std::max(24, (int)std::lround(30 * sc));

    HDC mdc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP col = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, col);
    ID2D1DCRenderTarget* rt = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    bool drawn = false;
    if (SUCCEEDED(g_f->CreateDCRenderTarget(&props, &rt))) {
        RECT rc{ 0, 0, n, n }; rt->BindDC(mdc, &rc);
        ID2D1SolidColorBrush* b = nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &b);
        rt->BeginDraw(); rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        float m = 3.5f * sc;
        D2D1_RECT_F box = D2D1::RectF(m, m, n - m, n - m);
        ID2D1Factory* f = nullptr; rt->GetFactory(&f);
        simbolo::Sym* s = simbolo::Load(f, L"camera.fill");
        f->Release();
        if (s && b) {
            // contorno branco grosso e o símbolo cinza-escuro por cima
            D2D1_RECT_F g{}; s->geo->GetBounds(nullptr, &g);
            float gw = g.right - g.left, gh = g.bottom - g.top, bw = box.right - box.left, bh = box.bottom - box.top;
            float k = std::min(bw / gw, bh / gh);
            float x = box.left + (bw - gw * k) / 2 - g.left * k, y = box.top + (bh - gh * k) / 2 - g.top * k;
            rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x, y));
            rt->DrawGeometry(s->geo, b, 2.6f * sc / k);
            rt->FillGeometry(s->geo, b);
            b->SetColor(D2D1::ColorF(0.13f, 0.13f, 0.14f, 1));
            rt->FillGeometry(s->geo, b);
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            drawn = true;
        }
        rt->EndDraw();
        Rel(b); Rel(rt);
    }
    SelectObject(mdc, old); DeleteDC(mdc);
    if (!drawn) { DeleteObject(col); return LoadCursorW(nullptr, IDC_HAND); }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{ FALSE, (DWORD)(n / 2), (DWORD)(n / 2), mask, col };
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(col);
    return c ? c : LoadCursorW(nullptr, IDC_HAND);
}

static void EnsureCursors() {
    UINT dpi = GetDpiForSystem();
    float sc = dpi / 96.f;
    if (g_cross && g_cursorSc == sc) return;
    if (g_cross) DestroyCursor(g_cross);
    if (g_camera) DestroyCursor(g_camera);
    g_cross = MakeCross(sc); g_camera = MakeCamera(sc); g_cursorSc = sc;
}

static void InvalidateAll() { for (auto& o : g_ov) InvalidateRect(o.h, nullptr, FALSE); }

static void LayoutBar() {
    Ov& o = g_ov[S.barOv];
    float sc = o.m.sc;
    float bh = 52 * sc, pad = 8 * sc, bw = 44 * sc, ih = 38 * sc, cw = 34 * sc, sep = 17 * sc;
    float optW = TextW(Tr(L"Opções"), Fmt(13 * sc)) + 34 * sc, capW = TextW(Tr(L"Capturar"), Fmt(13 * sc)) + 30 * sc;
    float total = pad + cw + sep + 3 * bw + sep + 2 * bw + sep + optW + 4 * sc + capW + pad;
    float cx = (o.m.rc.left + o.m.rc.right) / 2.f;
    float bottom = std::min<float>(o.m.rc.bottom - 120 * sc, o.m.work.bottom - 24 * sc);
    float x = cx - total / 2, y0 = bottom - bh, iy = y0 + (bh - ih) / 2;
    S.barRc = { (LONG)x, (LONG)y0, (LONG)(x + total), (LONG)bottom };
    auto put = [&](int b, float w) { S.btn[b] = { (LONG)x, (LONG)iy, (LONG)(x + w), (LONG)(iy + ih) }; x += w; };
    x += pad;
    put(B_CLOSE, cw); x += sep;
    put(B_SCREEN, bw); put(B_WINDOW, bw); put(B_REGION, bw); x += sep;
    put(B_RSCREEN, bw); put(B_RREGION, bw); x += sep;
    put(B_OPTIONS, optW); x += 4 * sc;
    put(B_CAPTURE, capW);
}

static bool BtnEnabled(int b) {
    if (b == B_RSCREEN || b == B_RREGION) return false;  // gravar a tela: ainda não
    if (b == B_CAPTURE) return S.mode != M_WINDOW;
    return true;
}

static int BarHit(POINT p) {
    if (!S.bar || !PtInRect(&S.barRc, p)) return -1;
    for (int b = 0; b < B_COUNT; b++) if (PtInRect(&S.btn[b], p)) return b;
    return -2;  // o fundo da barra
}

static void HandlePoints(const RECT& r, POINT out[8]) {
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    POINT p[8] = { { r.left, r.top }, { cx, r.top }, { r.right, r.top }, { r.right, cy }, { r.right, r.bottom }, { cx, r.bottom }, { r.left, r.bottom }, { r.left, cy } };
    for (int i = 0; i < 8; i++) out[i] = p[i];
}

static int HandleHit(POINT p) {
    if (!S.bar || !S.hasSel || S.mode != M_REGION) return -1;
    POINT hp[8]; HandlePoints(S.sel, hp);
    int mi = MonIndexAt(S.mons, p);
    float sc = mi >= 0 ? S.mons[mi].sc : 1;
    int tol = (int)std::lround(9 * sc);
    for (int i = 0; i < 8; i++) if (std::abs(p.x - hp[i].x) <= tol && std::abs(p.y - hp[i].y) <= tol) return i;
    return -1;
}

static int WindowAt(POINT p) {
    for (size_t i = 0; i < S.wins.size(); i++) if (PtInRect(&S.wins[i].r, p)) return (int)i;
    return -1;
}

static HCURSOR CursorNow() {
    static HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW), all = LoadCursorW(nullptr, IDC_SIZEALL), nwse = LoadCursorW(nullptr, IDC_SIZENWSE),
                   nesw = LoadCursorW(nullptr, IDC_SIZENESW), we = LoadCursorW(nullptr, IDC_SIZEWE), ns = LoadCursorW(nullptr, IDC_SIZENS);
    if (S.drag == D_BAR || (S.drag == D_NONE && BarHit(S.cur) != -1)) return arrow;
    if (S.mode == M_WINDOW || S.mode == M_SCREEN) return g_camera;
    int h = S.drag == D_SIZE ? S.handle : S.drag == D_NONE ? HandleHit(S.cur) : -1;
    if (h >= 0) { static const int k[8] = { 0, 3, 1, 2, 0, 3, 1, 2 }; HCURSOR cs[4] = { nwse, nesw, we, ns }; return cs[k[h]]; }
    if (S.drag == D_MOVE || (S.drag == D_NONE && S.bar && S.hasSel && PtInRect(&S.sel, S.cur))) return all;
    return g_cross;
}

static void DrawIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, ID2D1StrokeStyle* dash, int b, D2D1_POINT_2F c, float sc, float alpha) {
    D2D1_COLOR_F white = D2D1::ColorF(0.93f, 0.93f, 0.94f, alpha);
    br->SetColor(white);
    float lw = 1.35f * sc;
    auto rr = [&](float w, float h, float dx = 0, float dy = 0) { return D2D1::RoundedRect(D2D1::RectF(c.x - w / 2 + dx, c.y - h / 2 + dy, c.x + w / 2 + dx, c.y + h / 2 + dy), 2 * sc, 2 * sc); };
    switch (b) {
    case B_CLOSE: {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.22f * alpha));
        rt->FillEllipse(D2D1::Ellipse(c, 8.5f * sc, 8.5f * sc), br);
        br->SetColor(white);
        float k = 3.3f * sc;
        rt->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y + k }, br, 1.5f * sc);
        rt->DrawLine({ c.x + k, c.y - k }, { c.x - k, c.y + k }, br, 1.5f * sc);
        break;
    }
    case B_SCREEN:
        rt->DrawRoundedRectangle(rr(24 * sc, 17 * sc), br, lw);
        rt->FillRoundedRectangle(rr(17 * sc, 10 * sc), br);
        break;
    case B_WINDOW:
        rt->DrawRoundedRectangle(rr(24 * sc, 17 * sc), br, lw);
        // uma janela menor dentro da tela: contorno e barra de título cheia
        rt->DrawRoundedRectangle(rr(14 * sc, 9.5f * sc, 0, 0.5f * sc), br, 1.1f * sc);
        rt->FillRectangle(D2D1::RectF(c.x - 7 * sc, c.y - 4.25f * sc, c.x + 7 * sc, c.y - 1.5f * sc), br);
        break;
    case B_REGION:
        rt->DrawRoundedRectangle(rr(24 * sc, 17 * sc), br, lw, dash);
        break;
    case B_RSCREEN: case B_RREGION:
        rt->DrawRoundedRectangle(rr(24 * sc, 17 * sc), br, lw, b == B_RREGION ? dash : nullptr);
        rt->FillEllipse(D2D1::Ellipse({ c.x + 9 * sc, c.y + 6 * sc }, 4.6f * sc, 4.6f * sc), br);
        br->SetColor(D2D1::ColorF(0.16f, 0.16f, 0.17f, alpha));
        rt->FillEllipse(D2D1::Ellipse({ c.x + 9 * sc, c.y + 6 * sc }, 2.4f * sc, 2.4f * sc), br);
        break;
    }
}

static void DrawBar(Ov& o) {
    auto* rt = o.rt; auto* br = o.br;
    float sc = o.m.sc, ox = (float)o.m.rc.left, oy = (float)o.m.rc.top;
    auto L = [&](const RECT& r) { return D2D1::RectF(r.left - ox, r.top - oy, r.right - ox, r.bottom - oy); };
    D2D1_RECT_F bar = L(S.barRc);
    // sombra suave e o fundo escuro translúcido do HUD do Mac
    for (int i = 4; i >= 1; i--) {
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.05f));
        float g = i * 2.5f * sc;
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left - g, bar.top - g + 3 * sc, bar.right + g, bar.bottom + g + 3 * sc), 12 * sc + g, 12 * sc + g), br);
    }
    br->SetColor(D2D1::ColorF(0.15f, 0.15f, 0.16f, 0.94f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(bar, 12 * sc, 12 * sc), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.13f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left + 0.5f, bar.top + 0.5f, bar.right - 0.5f, bar.bottom - 0.5f), 12 * sc, 12 * sc), br, 1);
    auto sepAt = [&](int after) {
        float x = (L(S.btn[after]).right + L(S.btn[after + 1]).left) / 2;
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.18f));
        rt->FillRectangle(D2D1::RectF(x - 0.5f * sc, bar.top + 14 * sc, x + 0.5f * sc, bar.bottom - 14 * sc), br);
    };
    sepAt(B_CLOSE); sepAt(B_REGION); sepAt(B_RREGION);
    for (int b = 0; b < B_COUNT; b++) {
        D2D1_RECT_F r = L(S.btn[b]);
        bool en = BtnEnabled(b);
        bool selected = (b == B_SCREEN && S.mode == M_SCREEN) || (b == B_WINDOW && S.mode == M_WINDOW) || (b == B_REGION && S.mode == M_REGION);
        bool hot = en && S.hot == b, down = en && S.press == b && S.hot == b;
        float a = selected ? 0.24f : down ? 0.2f : hot ? 0.1f : 0;
        if (b == B_CAPTURE && en) a = down ? 0.3f : hot ? 0.24f : 0.16f;
        if (a > 0 && b != B_CLOSE) {
            br->SetColor(D2D1::ColorF(1, 1, 1, a));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1 * sc, r.top, r.right - 1 * sc, r.bottom), 7 * sc, 7 * sc), br);
        }
        D2D1_POINT_2F c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        float al = en ? (b == B_CLOSE && !hot ? 0.85f : 1.f) : 0.32f;
        if (b == B_OPTIONS) {
            Text(rt, br, Tr(L"Opções"), Fmt(13 * sc), D2D1::RectF(r.left + 12 * sc, r.top, r.right, r.bottom), D2D1::ColorF(0.93f, 0.93f, 0.94f, al));
            br->SetColor(D2D1::ColorF(0.93f, 0.93f, 0.94f, al));
            float x = r.right - 15 * sc, k = 3.2f * sc;
            rt->DrawLine({ x - k, c.y - k / 2 }, { x, c.y + k / 2 }, br, 1.4f * sc);
            rt->DrawLine({ x, c.y + k / 2 }, { x + k, c.y - k / 2 }, br, 1.4f * sc);
        } else if (b == B_CAPTURE) {
            Text(rt, br, Tr(L"Capturar"), Fmt(13 * sc), r, D2D1::ColorF(0.95f, 0.95f, 0.96f, en ? 1.f : 0.32f), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else DrawIcon(rt, br, o.dash, b, c, sc, al);
    }
}

static void DrawLabel(Ov& o) {
    float sc = o.m.sc, ox = (float)o.m.rc.left, oy = (float)o.m.rc.top;
    std::wstring a, b;
    if (S.drag == D_NEW && S.moved) {
        RECT s = SelNow();
        a = std::to_wstring(std::lround((s.right - s.left) / sc)); b = std::to_wstring(std::lround((s.bottom - s.top) / sc));
    } else {
        a = std::to_wstring(std::lround((S.cur.x - o.m.rc.left) / sc)); b = std::to_wstring(std::lround((S.cur.y - o.m.rc.top) / sc));
    }
    IDWriteTextFormat* f = Fmt(11.5f * sc, DWRITE_FONT_WEIGHT_MEDIUM);
    float w = std::max(TextW(a, f), TextW(b, f)) + 2 * sc, lh = 14 * sc;
    float x = S.cur.x - ox + 13 * sc, y = S.cur.y - oy + 4 * sc;
    float W = (float)(o.m.rc.right - o.m.rc.left), H = (float)(o.m.rc.bottom - o.m.rc.top);
    if (x + w > W - 2 * sc) x = S.cur.x - ox - 13 * sc - w;
    if (y + 2 * lh > H - 2 * sc) y = S.cur.y - oy - 4 * sc - 2 * lh;
    const std::wstring* t[2] = { &a, &b };
    for (int i = 0; i < 2; i++) {
        D2D1_RECT_F r = D2D1::RectF(x, y + i * lh, x + w + 4 * sc, y + (i + 1) * lh);
        for (auto [dx, dy] : { std::pair<float, float>{ -1.f, 0.f }, { 1.f, 0.f }, { 0.f, -1.f }, { 0.f, 1.f }, { 1.f, 1.f } }) {
            D2D1_RECT_F q = r; q.left += dx * sc * 0.8f; q.right += dx * sc * 0.8f; q.top += dy * sc * 0.8f; q.bottom += dy * sc * 0.8f;
            Text(o.rt, o.br, *t[i], f, q, D2D1::ColorF(0, 0, 0, 0.55f));
        }
        Text(o.rt, o.br, *t[i], f, r, D2D1::ColorF(1, 1, 1, 1));
    }
}

static bool CreateTarget(Ov& o) {
    if (o.rt) return true;
    RECT rc; GetClientRect(o.h, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    auto hp = D2D1::HwndRenderTargetProperties(o.h, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(g_f->CreateHwndRenderTarget(props, hp, &o.rt))) return false;
    o.rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &o.br);
    if (!o.dash) {
        float d[2] = { 2.2f, 1.6f };
        g_f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM), d, 2, &o.dash);
    }
    return true;
}

static void Upload(Ov& o) {
    if (!CreateTarget(o) || S.shot.img.w <= 0) return;
    int w = o.m.rc.right - o.m.rc.left, h = o.m.rc.bottom - o.m.rc.top;
    const BYTE* src = &S.shot.img.px[((size_t)(o.m.rc.top - S.shot.vr.top) * S.shot.img.w + (o.m.rc.left - S.shot.vr.left)) * 4];
    UINT pitch = (UINT)S.shot.img.w * 4;
    if (o.bmp) {
        D2D1_SIZE_U s = o.bmp->GetPixelSize();
        if ((int)s.width == w && (int)s.height == h && SUCCEEDED(o.bmp->CopyFromMemory(nullptr, src, pitch))) return;
        Rel(o.bmp);
    }
    o.rt->CreateBitmap(D2D1::SizeU(w, h), src, pitch, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &o.bmp);
}

static void Render(Ov& o) {
    if (!CreateTarget(o)) return;
    if (!o.bmp) Upload(o);
    auto* rt = o.rt; auto* br = o.br;
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    float W = (float)(o.m.rc.right - o.m.rc.left), H = (float)(o.m.rc.bottom - o.m.rc.top), sc = o.m.sc;
    float ox = (float)o.m.rc.left, oy = (float)o.m.rc.top;
    if (o.bmp) rt->DrawBitmap(o.bmp, D2D1::RectF(0, 0, W, H), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    else rt->Clear(D2D1::ColorF(0, 0, 0, 1));
    if (S.on) {
        bool showSel = S.mode == M_REGION && ((S.drag == D_NEW && S.moved) || (S.bar && S.hasSel));
        if (showSel) {
            RECT s = SelNow();
            D2D1_RECT_F f = D2D1::RectF(s.left - ox, s.top - oy, s.right - ox, s.bottom - oy);
            float l = std::clamp(f.left, 0.f, W), r = std::clamp(f.right, 0.f, W), t = std::clamp(f.top, 0.f, H), b = std::clamp(f.bottom, 0.f, H);
            br->SetColor(D2D1::ColorF(0, 0, 0, S.bar ? 0.42f : 0.32f));
            rt->FillRectangle(D2D1::RectF(0, 0, W, t), br);
            rt->FillRectangle(D2D1::RectF(0, b, W, H), br);
            rt->FillRectangle(D2D1::RectF(0, t, l, b), br);
            rt->FillRectangle(D2D1::RectF(r, t, W, b), br);
            if (r > l && b > t) {
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.85f));
                rt->DrawRectangle(D2D1::RectF(f.left + 0.5f, f.top + 0.5f, f.right - 0.5f, f.bottom - 0.5f), br, 1);
            }
            if (S.bar) {
                POINT hp[8]; HandlePoints(s, hp);
                for (auto& p : hp) {
                    D2D1_ELLIPSE e = D2D1::Ellipse({ p.x - ox, p.y - oy }, 4.5f * sc, 4.5f * sc);
                    br->SetColor(D2D1::ColorF(1, 1, 1, 1)); rt->FillEllipse(e, br);
                    br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f)); rt->DrawEllipse(e, br, 1);
                }
            }
        }
        if (S.mode == M_WINDOW && S.hover >= 0 && S.hover < (int)S.wins.size()) {
            RECT r = S.wins[S.hover].r;
            br->SetColor(D2D1::ColorF(0.30f, 0.56f, 1.f, 0.30f));
            rt->FillRectangle(D2D1::RectF(r.left - ox, r.top - oy, r.right - ox, r.bottom - oy), br);
        }
        if (S.mode == M_REGION && !(S.bar && S.drag != D_NEW) && PtInRect(&o.m.rc, S.cur) && BarHit(S.cur) == -1) DrawLabel(o);
        if (S.bar && &o == &g_ov[S.barOv]) DrawBar(o);
    }
    HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { Rel(o.bmp); Rel(o.br); Rel(o.rt); InvalidateRect(o.h, nullptr, FALSE); }
}

static void DestroyOverlays() {
    for (auto& o : g_ov) { Rel(o.bmp); Rel(o.br); Rel(o.dash); Rel(o.rt); if (o.h) DestroyWindow(o.h); }
    g_ov.clear();
}

static LRESULT CALLBACK OvProc(HWND, UINT, WPARAM, LPARAM);

static void EnsureOverlays(const std::vector<Mon>& ms) {
    bool same = ms.size() == g_ov.size();
    for (size_t i = 0; same && i < ms.size(); i++) same = EqualRect(&ms[i].rc, &g_ov[i].m.rc) && ms[i].sc == g_ov[i].m.sc;
    if (same) { for (size_t i = 0; i < ms.size(); i++) g_ov[i].m = ms[i]; return; }
    DestroyOverlays();
    WNDCLASSEXW wc{ sizeof(wc) };
    if (!GetClassInfoExW(g_inst, L"SequoiaCapturaTela", &wc)) {
        wc = { sizeof(wc) }; wc.lpfnWndProc = OvProc; wc.hInstance = g_inst; wc.lpszClassName = L"SequoiaCapturaTela"; wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
    }
    for (auto& m : ms) {
        Ov o; o.m = m;
        o.h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SequoiaCapturaTela", L"Captura de Tela", WS_POPUP, m.rc.left, m.rc.top, m.rc.right - m.rc.left, m.rc.bottom - m.rc.top, nullptr, nullptr, g_inst, nullptr);
        BOOL t = TRUE; DwmSetWindowAttribute(o.h, DWMWA_TRANSITIONS_FORCEDISABLED, &t, sizeof(t));
        int corner = 1; DwmSetWindowAttribute(o.h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));  // sem cantos arredondados
        g_ov.push_back(o);
    }
    for (auto& o : g_ov) CreateTarget(o);
}

static LRESULT CALLBACK KbProc(int c, WPARAM w, LPARAM l) {
    if (c == HC_ACTION && S.on && !ctxmenu::IsOpen()) {
        auto* k = (KBDLLHOOKSTRUCT*)l;
        bool down = !(k->flags & LLKHF_UP);
        DWORD vk = k->vkCode;
        if (vk == VK_ESCAPE || vk == VK_SPACE || vk == VK_RETURN) { PostMessageW(g_msg, WM_APP_KEY, vk, down); return 1; }
        if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU) { S.alt = down; PostMessageW(g_msg, WM_APP_KEY, VK_MENU, down); return 1; }
    }
    return CallNextHookEx(nullptr, c, w, l);
}

static void EndSession() {
    if (!S.on) return;
    S.on = false;
    if (S.kb) { UnhookWindowsHookEx(S.kb); S.kb = nullptr; }
    if (GetCapture()) ReleaseCapture();
    for (auto& o : g_ov) ShowWindow(o.h, SW_HIDE);
    S.wins.clear();
    S.drag = D_NONE;
}

static void StartCountdown(Job j, int secs);

static void Launch(Job j) {
    j.clip = j.clip || S.clip || GetAsyncKeyState(VK_CONTROL) < 0 || g_cfg.dest == 2;
    int timer = S.bar ? g_cfg.timer : 0;
    if (S.bar) {
        g_cfg.mode = S.mode;
        if (S.hasSel) { g_cfg.sel = S.sel; g_cfg.hasSel = true; }
        SaveCfg();
    }
    Shot shot = std::move(S.shot);
    S.shot = Shot{};
    EndSession();
    if (timer > 0) { StartCountdown(j, timer); return; }
    Run(j, &shot);
}

static void CaptureNow() {  // Enter ou o botão Capturar
    Job j;
    if (S.mode == M_REGION) {
        if (!S.hasSel) return;
        j.kind = J_REGION; j.r = S.sel;
    } else if (S.mode == M_SCREEN) { j.kind = J_SCREENS; j.mon = -1; }
    else return;
    Launch(j);
}

static RECT DefaultSel() {
    for (auto& m : S.mons) if (m.primary) {
        int w = (m.rc.right - m.rc.left) / 2, h = (m.rc.bottom - m.rc.top) / 2;
        int x = (m.rc.left + m.rc.right - w) / 2, y = (m.rc.top + m.rc.bottom - h) / 2 - h / 6;
        return { x, y, x + w, y + h };
    }
    return { 0, 0, 400, 300 };
}

static void ShowOptions() {
    using It = ctxmenu::Item;
    std::vector<It> items;
    auto item = [](const wchar_t* s, int id, bool chk) { It i; i.label = s; i.id = id; i.checked = chk; return i; };
    auto header = [](const wchar_t* s) { It i; i.label = s; i.disabled = true; return i; };
    auto sep = [] { It i; i.separator = true; return i; };
    items.push_back(header(Tr(L"Salvar em")));
    items.push_back(item(Tr(L"Mesa"), 101, g_cfg.dest == 0));
    items.push_back(item(Tr(L"Documentos"), 102, g_cfg.dest == 1));
    items.push_back(item(Tr(L"Área de Transferência"), 103, g_cfg.dest == 2));
    items.push_back(sep());
    items.push_back(header(Tr(L"Temporizador")));
    items.push_back(item(Tr(L"Nenhum"), 201, g_cfg.timer == 0));
    items.push_back(item(Tr(L"5 Segundos"), 202, g_cfg.timer == 5));
    items.push_back(item(Tr(L"10 Segundos"), 203, g_cfg.timer == 10));
    items.push_back(sep());
    items.push_back(header(Tr(L"Opções")));
    items.push_back(item(Tr(L"Mostrar Miniatura Flutuante"), 301, g_cfg.thumb));
    items.push_back(item(Tr(L"Lembrar Última Seleção"), 302, g_cfg.remember));
    items.push_back(item(Tr(L"Mostrar Ponteiro do Mouse"), 303, g_cfg.pointer));
    Ov& o = g_ov[S.barOv];
    SetForegroundWindow(o.h);
    POINT pt{ S.btn[B_OPTIONS].left, S.btn[B_OPTIONS].top - (LONG)(6 * o.m.sc) };
    int id = ctxmenu::ShowItems(o.h, pt, items);
    if (!S.on) return;
    switch (id) {
    case 101: g_cfg.dest = 0; break;
    case 102: g_cfg.dest = 1; break;
    case 103: g_cfg.dest = 2; break;
    case 201: g_cfg.timer = 0; break;
    case 202: g_cfg.timer = 5; break;
    case 203: g_cfg.timer = 10; break;
    case 301: g_cfg.thumb = !g_cfg.thumb; break;
    case 302: g_cfg.remember = !g_cfg.remember; break;
    case 303: g_cfg.pointer = !g_cfg.pointer; break;
    }
    if (id) { SaveCfg(); Logf(L"opções: destino %d, temporizador %d, miniatura %d, lembrar %d, ponteiro %d", g_cfg.dest, g_cfg.timer, g_cfg.thumb, g_cfg.remember, g_cfg.pointer); }
    S.hot = S.press = -1;
    InvalidateAll();
}

static void BarClick(int b) {
    switch (b) {
    case B_CLOSE: EndSession(); break;
    case B_SCREEN: S.mode = M_SCREEN; break;
    case B_WINDOW: S.mode = M_WINDOW; S.hover = WindowAt(S.cur); break;
    case B_REGION: S.mode = M_REGION; if (!S.hasSel) { S.sel = DefaultSel(); S.hasSel = true; } break;
    case B_OPTIONS: ShowOptions(); break;
    case B_CAPTURE: if (BtnEnabled(b)) CaptureNow(); break;
    }
    InvalidateAll();
}

static void StartSession(int kind, bool clip) {
    if (S.on) return;
    double t0 = Now();
    bool hadThumb = g_thumbOn;
    DismissThumb();
    if (hadThumb) DwmFlush();  // a miniatura não pode sair na foto
    if (kind == 3) {
        Shot sh;
        if (!Snap(sh)) return;
        Job j; j.kind = J_SCREENS; j.clip = clip || g_cfg.dest == 2;  // o Ctrl vem do Shell (estado na hora do atalho)
        Run(j, &sh);
        Logf(L"Win+Shift+3 em %.0f ms", Now() - t0);
        return;
    }
    S = Sess{};
    S.mons = Monitors();
    if (!Snap(S.shot)) return;
    S.on = true; S.bar = kind == 5; S.clip = clip;
    S.wins = ListWindows();
    GetCursorPos(&S.cur);
    S.last = S.cur;
    if (S.bar) {
        S.mode = (Mode)g_cfg.mode;
        RECT vr = S.shot.vr, inter{};
        if (g_cfg.remember && g_cfg.hasSel && IntersectRect(&inter, &g_cfg.sel, &vr) && EqualRect(&inter, &g_cfg.sel)) S.sel = g_cfg.sel;
        else S.sel = DefaultSel();
        S.hasSel = true;
    }
    EnsureCursors();
    EnsureOverlays(S.mons);
    for (auto& o : g_ov) Upload(o);
    if (S.bar) {
        S.barOv = 0;
        for (size_t i = 0; i < g_ov.size(); i++) if (g_ov[i].m.primary) S.barOv = (int)i;
        LayoutBar();
    }
    S.hover = S.mode == M_WINDOW ? WindowAt(S.cur) : -1;
    S.kb = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, g_inst, 0);
    double t1 = Now();
    // escondidas (cloak) enquanto o primeiro quadro é desenhado: a tela não pisca
    for (auto& o : g_ov) {
        BOOL c = TRUE; DwmSetWindowAttribute(o.h, DWMWA_CLOAK, &c, sizeof(c));
        SetWindowPos(o.h, HWND_TOPMOST, o.m.rc.left, o.m.rc.top, o.m.rc.right - o.m.rc.left, o.m.rc.bottom - o.m.rc.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    for (auto& o : g_ov) { Render(o); ValidateRect(o.h, nullptr); }
    for (auto& o : g_ov) { BOOL c = FALSE; DwmSetWindowAttribute(o.h, DWMWA_CLOAK, &c, sizeof(c)); }
    int mi = MonIndexAt(S.mons, S.cur);
    HWND front = g_ov[mi >= 0 && mi < (int)g_ov.size() ? mi : 0].h;
    SetForegroundWindow(front);
    SetCursor(CursorNow());
    Logf(L"Win+Shift+%d aberto em %.0f ms (desenho %.0f ms), %zu janelas, %zu monitores", kind, Now() - t0, Now() - t1, S.wins.size(), S.mons.size());
}

static void OnMove(POINT p) {
    POINT d{ p.x - S.last.x, p.y - S.last.y };
    S.cur = p;
    switch (S.drag) {
    case D_NEW:
        if (S.space) { S.p0.x += d.x; S.p0.y += d.y; }  // Espaço seguro: move a seleção
        if (std::abs(S.cur.x - S.p0.x) > 2 || std::abs(S.cur.y - S.p0.y) > 2) S.moved = true;
        break;
    case D_MOVE: {
        RECT r = S.sel0; OffsetRect(&r, p.x - S.p0.x, p.y - S.p0.y);
        RECT v = S.shot.vr;
        if (r.left < v.left) OffsetRect(&r, v.left - r.left, 0);
        if (r.top < v.top) OffsetRect(&r, 0, v.top - r.top);
        if (r.right > v.right) OffsetRect(&r, v.right - r.right, 0);
        if (r.bottom > v.bottom) OffsetRect(&r, 0, v.bottom - r.bottom);
        S.sel = r;
        break;
    }
    case D_SIZE: {
        RECT r = S.sel0;
        int dx = p.x - S.p0.x, dy = p.y - S.p0.y, h = S.handle;
        if (h == 0 || h == 6 || h == 7) r.left += dx;
        if (h == 2 || h == 3 || h == 4) r.right += dx;
        if (h == 0 || h == 1 || h == 2) r.top += dy;
        if (h == 4 || h == 5 || h == 6) r.bottom += dy;
        S.sel = Norm({ r.left, r.top }, { r.right, r.bottom });
        break;
    }
    default: break;
    }
    S.last = p;
    if (S.bar && (S.drag == D_NONE || S.drag == D_BAR)) {
        int b = BarHit(p);
        S.hot = b >= 0 && BtnEnabled(b) ? b : -1;
    }
    if (S.mode == M_WINDOW && S.drag != D_BAR) {
        S.hover = BarHit(p) == -1 ? WindowAt(p) : -1;
    }
    InvalidateAll();
}

static void OnDown(HWND h, POINT p) {
    S.cur = S.last = p;
    SetCapture(h);
    int b = BarHit(p);
    if (b != -1) { S.drag = D_BAR; S.press = b; InvalidateAll(); return; }
    S.moved = false;
    if (S.mode == M_REGION) {
        int hh = HandleHit(p);
        if (hh >= 0) { S.drag = D_SIZE; S.handle = hh; S.sel0 = S.sel; S.p0 = p; }
        else if (S.bar && S.hasSel && PtInRect(&S.sel, p)) { S.drag = D_MOVE; S.sel0 = S.sel; S.p0 = p; }
        else { S.drag = D_NEW; S.p0 = p; }
    } else S.drag = D_CLICK;
    InvalidateAll();
}

static void OnUp(POINT p) {
    S.cur = p;
    DragKind d = S.drag;
    S.drag = D_NONE;
    if (GetCapture()) ReleaseCapture();
    if (d == D_BAR) {
        int b = S.press; S.press = -1;
        if (b >= 0 && BarHit(p) == b && BtnEnabled(b)) BarClick(b);
        InvalidateAll();
        return;
    }
    if (d == D_NEW) {
        S.drag = D_NEW;  // para SelNow ver a seleção do arraste
        RECT r = SelNow();
        S.drag = D_NONE;
        bool big = S.moved && r.right - r.left >= 3 && r.bottom - r.top >= 3;
        if (S.bar) { if (big) S.sel = r; }
        else if (big) { Job j; j.kind = J_REGION; j.r = r; Launch(j); return; }
    } else if (d == D_CLICK) {
        if (S.mode == M_WINDOW) {
            int i = WindowAt(p);
            if (i >= 0) { Job j; j.kind = J_WINDOW; j.w = S.wins[i].h; j.r = S.wins[i].r; j.shadow = !S.alt; Launch(j); return; }
        } else if (S.mode == M_SCREEN) {
            Job j; j.kind = J_SCREENS; j.mon = MonIndexAt(S.mons, p); j.r = S.mons[j.mon].rc; Launch(j); return;
        }
    }
    InvalidateAll();
}

static void OnKey(UINT vk, bool down) {
    if (!S.on) return;
    if (vk == VK_ESCAPE && down) {
        if (S.drag == D_NEW && S.bar) { S.drag = D_NONE; if (GetCapture()) ReleaseCapture(); InvalidateAll(); return; }
        Logf(L"cancelado (Esc)");
        EndSession();
        return;
    }
    if (vk == VK_SPACE) {
        if (down && !S.space) {
            if (S.drag == D_NEW) S.space = true;
            else if (!S.bar && S.drag == D_NONE) {  // Cmd+Shift+4 e Espaço: alterna a mira e a câmera de janela
                S.mode = S.mode == M_WINDOW ? M_REGION : M_WINDOW;
                GetCursorPos(&S.cur);
                S.hover = S.mode == M_WINDOW ? WindowAt(S.cur) : -1;
                SetCursor(CursorNow());
                InvalidateAll();
            }
            S.space = true;
        } else if (!down) S.space = false;
        return;
    }
    if (vk == VK_RETURN && down && S.bar && S.drag == D_NONE) { CaptureNow(); return; }
    if (vk == VK_MENU) InvalidateAll();
}

static LRESULT CALLBACK OvProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto vp = [&] { POINT p; GetCursorPos(&p); return p; };
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        for (auto& o : g_ov) if (o.h == h) Render(o);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: if (S.on) { SetCursor(CursorNow()); return TRUE; } break;
    case WM_MOUSEMOVE: if (S.on) OnMove(vp()); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: if (S.on) OnDown(h, vp()); return 0;
    case WM_LBUTTONUP: if (S.on) OnUp(vp()); return 0;
    case WM_RBUTTONUP: return 0;
    case WM_CAPTURECHANGED: if (S.on && S.drag != D_NONE && (HWND)l != h) { S.drag = D_NONE; InvalidateAll(); } return 0;
    case WM_DPICHANGED: return 0;  // a janela cobre o monitor em pixels físicos; nada a ajustar
    case WM_CLOSE: if (S.on) EndSession(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- contagem regressiva (Temporizador)
struct Count { Layer L; Job j; int left = 0; float sc = 1; POINT pos{}; };
static Count* g_count;

static void PaintCount() {
    Count& c = *g_count;
    auto* rt = c.L.rt;
    ID2D1SolidColorBrush* br = nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &br);
    rt->BeginDraw(); rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    float W = (float)c.L.w;
    br->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.11f, 0.72f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, W), 22 * c.sc, 22 * c.sc), br);
    Text(rt, br, std::to_wstring(c.left), Fmt(54 * c.sc, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(0, 0, W, W), D2D1::ColorF(1, 1, 1, 0.95f), DWRITE_TEXT_ALIGNMENT_CENTER);
    rt->EndDraw();
    br->Release();
    c.L.Show(c.pos, 255);
}

static void StartCountdown(Job j, int secs) {
    std::vector<Mon> ms = Monitors();
    POINT ctr = j.kind == J_SCREENS && j.mon < 0 ? POINT{ 0, 0 } : Center(j.r);
    int mi = MonIndexAt(ms, ctr);
    if (j.kind == J_SCREENS && j.mon < 0) ctr = Center(ms[mi].rc);
    g_count = new Count;
    g_count->j = j; g_count->left = secs; g_count->sc = ms[mi].sc;
    int n = (int)std::lround(110 * g_count->sc);
    g_count->pos = { ctr.x - n / 2, ctr.y - n / 2 };
    g_count->L.Create(L"SequoiaCapturaContagem", DefWindowProcW, n, n, WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    PaintCount();
    SetTimer(g_msg, T_COUNT, 1000, nullptr);
    Logf(L"temporizador: %d s", secs);
}

static void TickCount() {
    if (!g_count) { KillTimer(g_msg, T_COUNT); return; }
    if (--g_count->left > 0) { PaintCount(); return; }
    KillTimer(g_msg, T_COUNT);
    Job j = g_count->j;
    g_count->L.Destroy();
    delete g_count; g_count = nullptr;
    DwmFlush();
    Run(j, nullptr);  // foto nova: o temporizador existe para pegar a tela de agora
}

// ---------------------------------------------------------------- miniatura flutuante
struct Thumb {
    Layer L; std::wstring path; float sc = 1; POINT home{};
    int phase = 0; double t0 = 0, stayUntil = 0; float off = 0, outFrom = 0; BYTE alpha = 0;
    bool hover = false, down = false, swipe = false, dragging = false, decided = false; POINT downPt{};
    float imgW = 0, imgH = 0, margin = 0;
};
static Thumb* g_thumb;

static void DestroyThumb() {
    if (!g_thumb) return;
    g_thumb->L.Destroy();
    delete g_thumb; g_thumb = nullptr; g_thumbOn = false;
}

static void DismissThumb() { DestroyThumb(); }

static void ThumbOut() {
    if (!g_thumb || g_thumb->phase == 2) return;
    g_thumb->phase = 2; g_thumb->t0 = Now(); g_thumb->outFrom = g_thumb->off;
}

static void ThumbPlace() {
    Thumb& t = *g_thumb;
    t.L.Show({ t.home.x + (int)std::lround(t.off), t.home.y }, t.alpha);
}

static void ThumbTick() {
    if (!g_thumb) return;
    Thumb& t = *g_thumb;
    double now = Now();
    if (t.dragging) return;
    if (t.phase == 0) {
        float k = (float)std::min(1.0, (now - t.t0) / 300.0);
        float e = 1 - (1 - k) * (1 - k) * (1 - k);
        t.off = (1 - e) * 70 * t.sc; t.alpha = (BYTE)std::lround(255 * std::min(1.f, k * 1.6f));
        if (k >= 1) { t.phase = 1; t.stayUntil = now + 5000; }
    } else if (t.phase == 1) {
        t.alpha = 255;
        if (!t.swipe) t.off = 0;
        if (!t.hover && !t.down && now > t.stayUntil) ThumbOut();
    }
    if (t.phase == 2) {
        float k = (float)std::min(1.0, (now - t.t0) / 260.0);
        float e = k * k;
        t.off = t.outFrom + e * (t.L.w + 60 * t.sc);
        t.alpha = (BYTE)std::lround(255 * (1 - k));
        if (k >= 1) { Logf(L"miniatura: guardada"); DestroyThumb(); return; }
    }
    ThumbPlace();
}

static void ThumbMenu(HWND h) {
    using It = ctxmenu::Item;
    auto item = [](const wchar_t* s, int id) { It i; i.label = s; i.id = id; return i; };
    It sep; sep.separator = true;
    std::vector<It> items = { item(Tr(L"Abrir"), 1), item(Tr(L"Editar"), 2), sep, item(Tr(L"Mostrar no Finder"), 3), item(Tr(L"Copiar"), 4), sep, item(Tr(L"Apagar"), 5), item(Tr(L"Fechar"), 6) };
    g_thumb->down = false; g_thumb->hover = true;
    SetForegroundWindow(h);
    POINT pt; GetCursorPos(&pt);
    int id = ctxmenu::ShowItems(h, pt, items);
    if (!g_thumb) return;
    std::wstring path = g_thumb->path;
    switch (id) {
    case 1: ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); DestroyThumb(); return;
    case 2: if ((INT_PTR)ShellExecuteW(nullptr, L"edit", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); DestroyThumb(); return;
    case 3: {
        std::wstring folder = path.substr(0, path.find_last_of(L'\\'));
        std::wstring finder = g_dir + L"\\" EXE_FINDER;
        if (GetFileAttributesW(finder.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(nullptr, L"open", finder.c_str(), (L"\"" + folder + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ThumbOut();
        return;
    }
    case 4: {
        IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
        if (SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
            SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            Img im; UINT w, hh; cv->GetSize(&w, &hh); im.w = w; im.h = hh; im.alpha = true; im.px.resize((size_t)w * hh * 4);
            double dx = 96, dy = 96; fr->GetResolution(&dx, &dy);
            if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)im.px.size(), im.px.data()))) ToClipboard(im, (float)dx);
        }
        Rel(cv); Rel(fr); Rel(dec);
        return;
    }
    case 5: {  // para a Lixeira (dá para desfazer)
        std::wstring from = path + L'\0';
        SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        int r = SHFileOperationW(&op);
        Logf(L"miniatura: %ls para a Lixeira (%d)", path.c_str(), r);
        DestroyThumb();
        return;
    }
    case 6: ThumbOut(); return;
    }
}

static void ThumbDrag(HWND h) {
    Thumb& t = *g_thumb;
    t.dragging = true;
    IShellItem* si = nullptr; IDataObject* dobj = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(t.path.c_str(), nullptr, IID_PPV_ARGS(&si))) && SUCCEEDED(si->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dobj)))) {
        ShowWindow(h, SW_HIDE);
        DWORD eff = DROPEFFECT_NONE;
        HRESULT hr = SHDoDragDrop(h, dobj, nullptr, DROPEFFECT_COPY, &eff);
        Logf(L"miniatura arrastada: 0x%08X, efeito %lu", (unsigned)hr, eff);
        dobj->Release();
        if (!g_thumb) { Rel(si); return; }
        if (hr == DRAGDROP_S_DROP && eff != DROPEFFECT_NONE) { Rel(si); DestroyThumb(); return; }
        t.stayUntil = Now() + 3000;
    }
    Rel(si);
    t.dragging = false; t.down = false;
    ThumbPlace();
}

static LRESULT CALLBACK ThumbProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_thumb) return DefWindowProcW(h, m, w, l);
    Thumb& t = *g_thumb;
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER: ThumbTick(); return 0;
    case WM_MOUSEMOVE: {
        if (!t.hover) { TRACKMOUSEEVENT tm{ sizeof(tm), TME_LEAVE, h, 0 }; TrackMouseEvent(&tm); t.hover = true; }
        if (t.down && t.phase != 2) {
            POINT p; GetCursorPos(&p);
            int dx = p.x - t.downPt.x, dy = p.y - t.downPt.y;
            if (!t.decided && std::abs(dx) + std::abs(dy) > (int)(6 * t.sc)) {
                t.decided = true;
                if (dx > 0 && dx > std::abs(dy) * 1.3) t.swipe = true;  // para a direita: guardar
                else { ThumbDrag(h); return 0; }                         // para os lados/cima: arrastar o arquivo
            }
            if (t.swipe) { t.off = (float)std::max(0, dx); ThumbPlace(); }
        }
        return 0;
    }
    case WM_MOUSELEAVE: t.hover = false; t.stayUntil = std::max(t.stayUntil, Now() + 1500); return 0;
    case WM_LBUTTONDOWN: t.down = true; t.decided = false; t.swipe = false; GetCursorPos(&t.downPt); SetCapture(h); return 0;
    case WM_LBUTTONUP: {
        bool was = t.down; t.down = false;
        if (GetCapture() == h) ReleaseCapture();
        if (!was || !g_thumb) return 0;
        if (t.swipe) {
            t.swipe = false;
            if (t.off > 30 * t.sc) ThumbOut(); else t.off = 0;
            ThumbPlace();
        } else if (!t.decided) {  // clique: abre (Fotos) e a miniatura some
            Logf(L"miniatura: abrir %ls", t.path.c_str());
            ShellExecuteW(nullptr, L"open", t.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            DestroyThumb();
        }
        return 0;
    }
    case WM_RBUTTONUP: ThumbMenu(h); return 0;
    case WM_CAPTURECHANGED: if (t.down && !t.dragging) { t.down = false; if (t.swipe) { t.swipe = false; t.off = 0; } } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowThumb(const Img& im, const std::wstring& path, const Mon& m) {
    DestroyThumb();
    g_thumb = new Thumb;
    Thumb& t = *g_thumb;
    t.path = path; t.sc = m.sc;
    float boxW = 230 * t.sc, boxH = 150 * t.sc;
    float k = std::min({ boxW / im.w, boxH / im.h, 1.f });
    t.imgW = std::max(24.f * t.sc, std::round(im.w * k)); t.imgH = std::max(24.f * t.sc, std::round(im.h * k));
    t.margin = std::round(18 * t.sc);
    int W = (int)(t.imgW + 2 * t.margin), H = (int)(t.imgH + 2 * t.margin);
    if (!t.L.Create(L"SequoiaCapturaMiniatura", ThumbProc, W, H, WS_EX_NOACTIVATE)) { DestroyThumb(); return; }
    // a imagem reduzida (WIC, alta qualidade) e pré-multiplicada para o Direct2D
    IWICBitmap* wb = nullptr; IWICFormatConverter* cv = nullptr; IWICBitmapScaler* scl = nullptr; ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(g_wic->CreateBitmapFromMemory(im.w, im.h, GUID_WICPixelFormat32bppBGRA, im.w * 4, (UINT)im.px.size(), (BYTE*)im.px.data(), &wb)) &&
        SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(g_wic->CreateBitmapScaler(&scl)) && SUCCEEDED(scl->Initialize(cv, (UINT)t.imgW, (UINT)t.imgH, WICBitmapInterpolationModeHighQualityCubic)))
        t.L.rt->CreateBitmapFromWicBitmap(scl, &bmp);
    if (!im.alpha) {  // sombra própria da miniatura (a foto de janela já traz a dela)
        BYTE* px = (BYTE*)t.L.bits;
        float sig = 7 * t.sc;
        auto ax = Profile(W, t.margin, t.margin + t.imgW, sig), ay = Profile(H, t.margin + 3 * t.sc, t.margin + t.imgH + 3 * t.sc, sig);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) px[((size_t)y * W + x) * 4 + 3] = (BYTE)std::lround(255 * 0.38f * ax[x] * ay[y]);
    } else memset(t.L.bits, 0, (size_t)W * H * 4);
    auto* rt = t.L.rt;
    ID2D1SolidColorBrush* br = nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    rt->BeginDraw();
    D2D1_RECT_F r = D2D1::RectF(t.margin, t.margin, t.margin + t.imgW, t.margin + t.imgH);
    float rad = im.alpha ? 0 : 5 * t.sc;
    if (bmp) {
        ID2D1BitmapBrush* bb = nullptr;
        rt->CreateBitmapBrush(bmp, &bb);
        if (bb) {
            bb->SetTransform(D2D1::Matrix3x2F::Translation(r.left, r.top));
            bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), bb);
            bb->Release();
        }
    }
    if (!im.alpha && br) {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, rad), br, 1);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 0.5f, r.top - 0.5f, r.right + 0.5f, r.bottom + 0.5f), rad + 0.5f, rad + 0.5f), br, 1);
    }
    rt->EndDraw();
    Rel(br); Rel(bmp); Rel(scl); Rel(cv); Rel(wb);
    // canto inferior direito da área de trabalho do monitor
    int gap = (int)std::lround(14 * t.sc);
    t.home = { m.work.right - gap - (int)(t.margin + t.imgW), m.work.bottom - gap - (int)(t.margin + t.imgH) };
    t.phase = 0; t.t0 = Now(); t.off = 70 * t.sc; t.alpha = 0;
    ThumbPlace();
    SetTimer(t.L.h, 1, 15, nullptr);
    g_thumbOn = true;
}

// ---------------------------------------------------------------- janela de mensagens e início
static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CAPTURA && WM_CAPTURA) {
        int kind = (int)w;
        if (kind == 3 || kind == 4 || kind == 5) {
            if (g_count) return 0;  // contagem em andamento
            if (S.on) { if (kind == 5 && !S.bar) { EndSession(); StartSession(5, (l & 1) != 0); } return 0; }
            StartSession(kind, (l & 1) != 0);
        } else if (kind == 0) PostQuitMessage(0);  // sair (wParam 0)
        else if (kind >= 103 && kind <= 105 && !S.on) {  // diagnóstico (/teste-regiao, /teste-janela, /teste-barra)
            StartSession(kind == 105 ? 5 : 4, false);
            if (kind == 103 && S.on) { S.drag = D_NEW; S.moved = true; S.p0 = { S.cur.x - 400, S.cur.y - 250 }; }
            if (kind == 104 && S.on) { S.mode = M_WINDOW; S.hover = WindowAt(S.cur); }
            InvalidateAll();
            SetTimer(h, 9, 700, nullptr);
        }
        return 0;
    }
    switch (m) {
    case WM_APP_KEY: OnKey((UINT)w, l != 0); return 0;
    case WM_TIMER:
        if (w == T_COUNT) TickCount();
        if (w == 9) {
            KillTimer(h, 9);
            Shot sh;
            if (Snap(sh)) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); SavePng(sh.img, 96, std::wstring(tmp) + L"captura-visual.png"); }
            S.drag = D_NONE;
            EndSession();
        }
        return 0;
    case WM_DISPLAYCHANGE: if (!S.on) DestroyOverlays(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int ParseCmd(const wchar_t* cmd, bool& clip, bool& resident) {
    int kind = 0;
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"/3" || a == L"--tela") kind = 3;
        else if (a == L"/4" || a == L"--regiao") kind = 4;
        else if (a == L"/5" || a == L"--barra") kind = 5;
        else if (a == L"/c" || a == L"--copiar") clip = true;
        else if (a == L"/residente" || a == L"--residente") resident = true;
        else if (a == L"/sair" || a == L"--sair") kind = -1;
        else if (a == L"/teste-gpu") kind = -2;
        else if (a == L"/teste-regiao") kind = -3;   // diagnóstico: foto da própria tela com a mira arrastando
        else if (a == L"/teste-janela") kind = -4;   // ... com a câmera de janela
        else if (a == L"/teste-barra") kind = -5;    // ... com a barra do Win+Shift+5
    }
    if (argv) LocalFree(argv);
    (void)cmd;
    return kind;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool clip = false, resident = false;
    int kind = ParseCmd(cmd, clip, resident);
    WM_CAPTURA = RegisterWindowMessageW(L"SequoiaCaptura");
    HANDLE one = CreateMutexW(nullptr, TRUE, L"SequoiaCapturaUmaInstancia");
    if (GetLastError() == ERROR_ALREADY_EXISTS && kind != -2) {  // já residente: repassa o pedido
        HWND w = FindWindowW(L"SequoiaCapturaMsg", nullptr);
        if (w && kind) PostMessageW(w, WM_CAPTURA, kind == -1 ? 0 : kind <= -3 ? 100 - kind : kind, clip ? 1 : 0);
        return 0;
    }
    if (kind == -1) return 0;
    g_inst = hi;
    OleInitialize(nullptr);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    g_dir = exe;
    g_root = g_dir.substr(0, g_dir.find_last_of(L'\\'));  // bin fica dentro da pasta do projeto
    g_log = _wfsopen((g_dir + L"\\captura.log").c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    simbolo::Dir() = g_root + L"\\icones\\simbolos";
    ctxmenu::Init(hi, g_log);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_f);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    g_family = PickFamily({ L"SF Pro Text", L"SF Pro", L"Segoe UI Variable Text", L"Segoe UI" });
    LoadCfg();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MsgProc; wc.hInstance = hi; wc.lpszClassName = L"SequoiaCapturaMsg";
    RegisterClassExW(&wc);
    g_msg = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Captura de Tela", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    ChangeWindowMessageFilterEx(g_msg, WM_CAPTURA, MSGFLT_ALLOW, nullptr);  // o Shell pode estar com outro nível de integridade
    Logf(L"Captura de Tela pronta (%ls), fonte %ls, destino %d", resident ? L"residente" : L"avulsa", g_family.c_str(), g_cfg.dest);

    // aquece o que demora na primeira vez (janelas por monitor, alvos do Direct2D, ponteiros, Direct3D)
    double tw = Now();
    EnsureCursors();
    EnsureOverlays(Monitors());
    EnsureD3D();
    Logf(L"aquecido em %.0f ms", Now() - tw);
    if (kind == -2) {  // diagnóstico: fotos de janelas pela captura do Windows, em %TEMP%\captura-teste-*.png
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        auto ws = ListWindows();
        int n = 0;
        for (auto& w : ws) {
            wchar_t cls[64] = L"", title[128] = L""; GetClassNameW(w.h, cls, 64); GetWindowTextW(w.h, title, 128);
            Logf(L"janela %p [%ls] \"%ls\" %ld,%ld,%ld,%ld", w.h, cls, title, w.r.left, w.r.top, w.r.right, w.r.bottom);
        }
        for (auto& w : ws) {
            if (n >= 3) break;
            if ((GetWindowLongW(w.h, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_LAYERED)) || !(GetWindowLongW(w.h, GWL_STYLE) & WS_CAPTION)) continue;
            Img im = WindowImage(w.h, w.r, nullptr, true, 1);
            if (im.w > 0) SavePng(im, 96, std::wstring(tmp) + L"captura-teste-" + std::to_wstring(n++) + L".png");
        }
        return 0;
    }
    if (kind) PostMessageW(g_msg, WM_CAPTURA, kind, clip ? 1 : 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    EndSession();
    DestroyThumb();
    DestroyOverlays();
    Logf(L"saindo");
    if (g_log) fclose(g_log);
    ReleaseMutex(one);
    OleUninitialize();
    return 0;
}
