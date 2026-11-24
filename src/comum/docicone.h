#pragma once
// Ícone de documento do Mac para arquivo sem miniatura (.md, .txt, .zip, tipos sem programa...): folha branca com o
// canto dobrado e, no meio, o símbolo do tipo (linhas de texto, zíper, nota, filme, foto) ou a extensão escrita.
// É o mesmo desenho do Finder (src\finder\main.cpp, DrawDocument), aqui em pixels (BGRA pré-multiplicado) para
// quem só sabe mostrar imagens: a mesa, as pilhas da mesa e a pilha de Downloads do Dock.
// Pode rodar em qualquer thread (fábricas próprias).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace docicone {

inline std::wstring Ext(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.'), sl = name.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sl != std::wstring::npos && dot < sl)) return L"";
    std::wstring e = name.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

// programas e atalhos mostram o próprio ícone (como os apps no Mac)
inline bool IsApp(const std::wstring& path) {
    std::wstring e = Ext(path);
    for (auto a : { L"exe", L"msi", L"lnk", L"url", L"appref-ms", L"scr", L"com", L"cpl" }) if (e == a) return true;
    return false;
}

inline bool Pixels(const std::wstring& name, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    if (px < 8) return false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ID2D1Factory* f = nullptr;
    IDWriteFactory* dw = nullptr;
    IWICBitmap* bmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    bool ok = false;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
    if (f && dw && SUCCEEDED(wic->CreateBitmap(px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
        SUCCEEDED(f->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &rt)) &&
        SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br))) {
        float S = (float)px;
        float pw = 0.74f * S, ph = 0.94f * S, fold = 0.24f * pw, r = std::max(1.f, 0.04f * S);
        D2D1_RECT_F pg{ S / 2 - pw / 2, S - ph, S / 2 + pw / 2, S - 0.5f };
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        auto page = [&](float dy) {
            ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
            f->CreatePathGeometry(&g);
            if (!g || FAILED(g->Open(&s))) return g;
            s->BeginFigure({ pg.left, pg.top + r + dy }, D2D1_FIGURE_BEGIN_FILLED);
            s->AddArc(D2D1::ArcSegment({ pg.left + r, pg.top + dy }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->AddLine({ pg.right - fold, pg.top + dy });
            s->AddLine({ pg.right, pg.top + fold + dy });
            s->AddLine({ pg.right, pg.bottom - r + dy });
            s->AddArc(D2D1::ArcSegment({ pg.right - r, pg.bottom + dy }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->AddLine({ pg.left + r, pg.bottom + dy });
            s->AddArc(D2D1::ArcSegment({ pg.left, pg.bottom - r + dy }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close(); s->Release();
            return g;
        };
        pg.bottom -= 0.8f;
        if (auto* sh = page(0.8f)) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.22f)); rt->FillGeometry(sh, br); sh->Release(); }
        if (auto* body = page(0)) {
            br->SetColor(D2D1::ColorF(0.985f, 0.985f, 0.985f)); rt->FillGeometry(body, br);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f)); rt->DrawGeometry(body, br, 0.8f);
            body->Release();
        }
        ID2D1PathGeometry* tri = nullptr; ID2D1GeometrySink* s = nullptr;  // canto dobrado
        f->CreatePathGeometry(&tri);
        if (tri && SUCCEEDED(tri->Open(&s))) {
            s->BeginFigure({ pg.right - fold, pg.top }, D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine({ pg.right - fold, pg.top + fold - r });
            s->AddArc(D2D1::ArcSegment({ pg.right - fold + r, pg.top + fold }, { r, r }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->AddLine({ pg.right, pg.top + fold });
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close(); s->Release();
            br->SetColor(D2D1::ColorF(0.86f, 0.86f, 0.87f)); rt->FillGeometry(tri, br);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f)); rt->DrawGeometry(tri, br, 0.8f);
        }
        if (tri) tri->Release();

        std::wstring ext = Ext(name);
        auto is = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (ext == x) return true; return false; };
        float mx = (pg.left + pg.right) / 2;
        auto text = [&](const std::wstring& t, const wchar_t* fam, float size, DWRITE_FONT_WEIGHT wt, D2D1_RECT_F box) {
            IDWriteTextFormat* tf = nullptr;
            if (FAILED(dw->CreateTextFormat(fam, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf)) || !tf) return;
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            rt->DrawTextW(t.c_str(), (UINT32)t.size(), tf, box, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            tf->Release();
        };
        if (is({ L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"cab", L"iso" })) {  // zíper
            float zw = 0.16f * pw, zx = mx - zw / 2, step = std::max(2.f, 0.05f * S);
            br->SetColor(D2D1::ColorF(0.55f, 0.55f, 0.57f));
            int k = 0;
            for (float y = pg.top + step * 0.5f; y < pg.top + 0.55f * ph; y += step, k++) {
                bool left = (k % 2) == 0;
                rt->FillRectangle(D2D1::RectF(left ? zx : mx, y, left ? mx : zx + zw, y + step * 0.55f), br);
            }
            D2D1_RECT_F pull{ zx - 0.02f * S, pg.top + 0.56f * ph, zx + zw + 0.02f * S, pg.top + 0.72f * ph };
            rt->FillRoundedRectangle(D2D1::RoundedRect(pull, 0.02f * S, 0.02f * S), br);
        } else if (is({ L"txt", L"md", L"log", L"json", L"xml", L"csv", L"ini", L"cfg", L"yaml", L"yml", L"cpp", L"c", L"h", L"hpp",
                        L"py", L"js", L"ts", L"html", L"css", L"rtf", L"cmd", L"bat", L"ps1", L"sh", L"toml" })) {  // linhas de texto
            br->SetColor(D2D1::ColorF(0.62f, 0.62f, 0.64f));
            float lx = pg.left + 0.16f * pw, lw = pw * 0.68f, lh = std::max(0.8f, 0.018f * S), gap = 0.065f * S;
            int lines = 0;
            float bottom = ext == L"md" || ext == L"json" || ext == L"csv" ? pg.bottom - 0.30f * ph : pg.bottom - 0.12f * ph;
            for (float y = pg.top + 0.30f * ph; y < bottom; y += gap, lines++)
                rt->FillRectangle(D2D1::RectF(lx, y, lx + ((lines % 4 == 3) ? lw * 0.55f : lw), y + lh), br);
            if (bottom < pg.bottom - 0.2f * ph && S >= 28) {  // .md/.json/.csv: a extensão embaixo, como no Mac
                std::wstring lab = ext; for (auto& c : lab) c = (wchar_t)towupper(c);
                br->SetColor(D2D1::ColorF(0.50f, 0.50f, 0.52f));
                text(lab, L"SF Pro Text", std::round(std::max(6.f, 0.13f * S)), DWRITE_FONT_WEIGHT_BOLD,
                     D2D1::RectF(pg.left, pg.bottom - 0.27f * ph, pg.right, pg.bottom - 0.06f * ph));
            }
        } else {
            wchar_t glyph = 0;
            D2D1_COLOR_F gc = D2D1::ColorF(0.45f, 0.45f, 0.48f);
            if (is({ L"wav", L"mp3", L"flac", L"aac", L"m4a", L"ogg", L"aif", L"aiff", L"wma", L"mid", L"midi", L"opus" })) { glyph = 0xE8D6; gc = D2D1::ColorF(0.93f, 0.27f, 0.45f); }
            else if (is({ L"mp4", L"mov", L"mkv", L"avi", L"webm", L"wmv", L"m4v" })) { glyph = 0xE714; gc = D2D1::ColorF(0.35f, 0.45f, 0.95f); }
            else if (is({ L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"heic", L"tif", L"tiff", L"svg", L"ico" })) { glyph = 0xE91B; gc = D2D1::ColorF(0.25f, 0.62f, 0.95f); }
            if (ext == L"pdf") gc = D2D1::ColorF(0.86f, 0.22f, 0.20f);
            if (glyph) {
                br->SetColor(gc);
                std::wstring g(1, glyph);
                text(g, L"Segoe Fluent Icons", std::round(0.34f * S), DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(pg.left, pg.top + 0.15f * ph, pg.right, pg.top + 0.65f * ph));
            }
            std::wstring label = ext.substr(0, 4);
            for (auto& c : label) c = (wchar_t)towupper(c);
            if (!label.empty() && S >= 28) {
                br->SetColor(glyph ? D2D1::ColorF(0.50f, 0.50f, 0.52f) : gc);
                text(label, L"SF Pro Text", std::round(std::max(6.f, 0.15f * S)), DWRITE_FONT_WEIGHT_BOLD,
                     glyph ? D2D1::RectF(pg.left, pg.top + 0.66f * ph, pg.right, pg.top + 0.86f * ph) : D2D1::RectF(pg.left, pg.top + 0.45f * ph, pg.right, pg.top + 0.75f * ph));
            }
        }
        ok = SUCCEEDED(rt->EndDraw());
        if (ok) {
            w = h = (UINT)px;
            bgra.resize((size_t)px * px * 4);
            ok = SUCCEEDED(bmp->CopyPixels(nullptr, px * 4, (UINT)bgra.size(), bgra.data()));
        }
    }
    if (br) br->Release();
    if (rt) rt->Release();
    if (bmp) bmp->Release();
    if (dw) dw->Release();
    if (f) f->Release();
    wic->Release();
    return ok;
}

}  // namespace docicone
