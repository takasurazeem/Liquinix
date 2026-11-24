// svg2png: converte um .svg em .png quadrado (Direct2D ID2D1SvgDocument + WIC), sem nada instalado.
// Usado por tools\icones-mactahoe.ps1 para montar icones\tahoe a partir do tema MacTahoe (SVG).
//   svg2png entrada.svg saida.png [tamanho=512] [escala=1]  (escala < 1: o desenho menor, centrado, com margem)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1_3.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { fwprintf(stderr, L"uso: svg2png entrada.svg saida.png [tamanho]\n"); return 2; }
    UINT size = argc > 3 ? (UINT)_wtoi(argv[3]) : 512;
    float k = argc > 4 ? (float)_wtof(argv[4]) : 1.f;
    if (k <= 0 || k > 1) k = 1;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    ID2D1Factory1* d2d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&d2d))) return 3;
    IWICBitmap* bmp = nullptr;
    wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp, props, &rt))) return 4;
    ID2D1DeviceContext5* dc = nullptr;
    if (FAILED(rt->QueryInterface(&dc))) { fwprintf(stderr, L"sem ID2D1DeviceContext5 (SVG)\n"); return 5; }
    IStream* in = nullptr;
    if (FAILED(SHCreateStreamOnFileEx(argv[1], STGM_READ, 0, FALSE, nullptr, &in))) { fwprintf(stderr, L"nao abriu %ls\n", argv[1]); return 6; }
    ID2D1SvgDocument* svg = nullptr;
    HRESULT hr = dc->CreateSvgDocument(in, D2D1::SizeF((float)size, (float)size), &svg);
    if (FAILED(hr) || !svg) { fwprintf(stderr, L"SVG invalido (0x%08X): %ls\n", (unsigned)hr, argv[1]); return 7; }
    // o viewBox do arquivo vira o quadrado inteiro: sem viewBox, ele sai da largura/altura do arquivo;
    // depois largura e altura viram o tamanho pedido (senão o desenho fica no tamanho original, ex. 16 px)
    ID2D1SvgElement* root = nullptr;
    svg->GetRoot(&root);
    if (root) {
        D2D1_SVG_VIEWBOX vb{};
        bool has = root->IsAttributeSpecified(L"viewBox") &&
                   SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) && vb.width > 0;
        if (!has) {
            D2D1_SVG_LENGTH w{ 0, D2D1_SVG_LENGTH_UNITS_NUMBER }, h{ 0, D2D1_SVG_LENGTH_UNITS_NUMBER };
            root->GetAttributeValue(L"width", &w);
            root->GetAttributeValue(L"height", &h);
            if (w.value > 0 && h.value > 0 && w.units == D2D1_SVG_LENGTH_UNITS_NUMBER) {
                vb = { 0, 0, w.value, h.value };
                root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb));
            }
        }
        root->SetAttributeValue(L"width", D2D1_SVG_LENGTH{ (float)size, D2D1_SVG_LENGTH_UNITS_NUMBER });
        root->SetAttributeValue(L"height", D2D1_SVG_LENGTH{ (float)size, D2D1_SVG_LENGTH_UNITS_NUMBER });
        root->Release();
    }
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(size / 2.f, size / 2.f)));
    dc->DrawSvgDocument(svg);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    hr = dc->EndDraw();
    if (FAILED(hr)) { fwprintf(stderr, L"desenho falhou (0x%08X)\n", (unsigned)hr); return 8; }
    IWICStream* out = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    wic->CreateStream(&out);
    if (FAILED(out->InitializeFromFilename(argv[2], GENERIC_WRITE))) { fwprintf(stderr, L"nao gravou %ls\n", argv[2]); return 9; }
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(out, WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&fr, nullptr);
    fr->Initialize(nullptr);
    fr->SetSize(size, size);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
    fr->SetPixelFormat(&fmt);
    // PNG com alfa direto (não pré-multiplicado): converte antes de gravar
    IWICFormatConverter* conv = nullptr;
    wic->CreateFormatConverter(&conv);
    conv->Initialize(bmp, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    WICPixelFormatGUID f2 = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&f2);
    fr->WriteSource(conv, nullptr);
    fr->Commit();
    enc->Commit();
    return 0;
}
