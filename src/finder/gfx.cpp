// Direct2D sobre DirectComposition: swap chain com alfa pré-multiplicado ligada à janela
// (WS_EX_NOREDIRECTIONBITMAP). É o caminho do WinUI 3; evita o "quadro em branco" que o
// HwndRenderTarget tinha sobre o acrílico.
#include "finder.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <algorithm>

IDWriteFactory* gfx::dw;

namespace {
ID3D11Device* d3d;
IDXGIDevice* dxgiDev;
ID2D1Factory1* d2d;
ID2D1Device* d2dDev;
ID2D1DeviceContext* dc;
IDXGISwapChain1* swap;
IDCompositionDevice* dcomp;
IDCompositionTarget* target;
IDCompositionVisual* visual;
ID2D1Bitmap1* backbuffer;

void BindBackbuffer() {
    IDXGISurface* surface = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc->CreateBitmapFromDxgiSurface(surface, &props, &backbuffer);
    surface->Release();
    dc->SetTarget(backbuffer);
}
}  // namespace

bool gfx::Init(HWND hwnd) {
    // VIDEO_SUPPORT: o Quick Look toca vídeo neste mesmo dispositivo (Media Foundation)
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr))  // sem GPU utilizável (VM, área remota): renderizador por software
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) { Log(L"erro: D3D11CreateDevice 0x%08X", (unsigned)hr); return false; }
    d3d->QueryInterface(IID_PPV_ARGS(&dxgiDev));

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&d2d);
    d2d->CreateDevice(dxgiDev, &d2dDev);
    d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType não combina com fundo transparente
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);

    RECT rc; GetClientRect(hwnd, &rc);
    IDXGIFactory2* factory = nullptr;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = std::max<LONG>(rc.right, 1);
    desc.Height = std::max<LONG>(rc.bottom, 1);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = factory->CreateSwapChainForComposition(d3d, &desc, nullptr, &swap);
    factory->Release();
    if (FAILED(hr)) { Log(L"erro: CreateSwapChainForComposition 0x%08X", (unsigned)hr); return false; }

    hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&dcomp));
    if (FAILED(hr)) { Log(L"erro: DCompositionCreateDevice 0x%08X", (unsigned)hr); return false; }
    dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
    dcomp->CreateVisual(&visual);
    visual->SetContent(swap);
    target->SetRoot(visual);
    dcomp->Commit();
    BindBackbuffer();
    return true;
}

void gfx::Resize(UINT w, UINT h) {
    if (!swap || !w || !h) return;
    dc->SetTarget(nullptr);
    if (backbuffer) { backbuffer->Release(); backbuffer = nullptr; }
    HRESULT hr = swap->ResizeBuffers(2, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) Log(L"erro: ResizeBuffers 0x%08X", (unsigned)hr);
    BindBackbuffer();
}

ID2D1DeviceContext* gfx::Begin(float dpi) {
    if (!dc || !backbuffer) return nullptr;
    dc->SetDpi(dpi, dpi);
    dc->BeginDraw();
    return dc;
}

void gfx::End() {
    HRESULT hr = dc->EndDraw();
    if (FAILED(hr)) Log(L"erro: EndDraw 0x%08X", (unsigned)hr);
    swap->Present(1, 0);
}

ID2D1Bitmap* gfx::CreateBitmapBGRA(UINT w, UINT h, const void* pixels, UINT stride) {
    ID2D1Bitmap* bmp = nullptr;
    auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (dc) dc->CreateBitmap(D2D1::SizeU(w, h), pixels, stride, props, &bmp);
    return bmp;
}

IUnknown* gfx::Device() { return d3d; }

ID2D1Bitmap* gfx::BitmapFromTexture(IUnknown* texture) {
    IDXGISurface* surface = nullptr;
    if (!dc || FAILED(texture->QueryInterface(IID_PPV_ARGS(&surface)))) return nullptr;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1Bitmap1* b = nullptr;
    HRESULT hr = dc->CreateBitmapFromDxgiSurface(surface, &props, &b);
    surface->Release();
    if (FAILED(hr)) Log(L"erro: bitmap do vídeo 0x%08X", (unsigned)hr);
    return b;
}
