#pragma once
// Base dos apps pequenos desenhados com Direct2D (Lembretes, Calendário): fábricas, fonte SF/Inter/Segoe,
// tema claro/escuro, texto, campo de edição (EDIT do Windows por cima, sem borda) e o arquivo de dados em
// %APPDATA%\Shell. A barra de título do Mac vem da WacOSWindow.dll (a janela é comum, com barra do Windows),
// que o próprio app carrega no CreateMain (barra_mac.h), com ou sem o Shell.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
#include "idioma.h"
#include "semaforo.h"  // semaforo::Aqua(): tema macOS Golden Gate
#include "barra_mac.h"  // a WacOSWindow.dll carregada pelo próprio app (barra do Mac sem o Shell)
#include <commctrl.h>
#include <shellapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace app2d {

inline ID2D1Factory* d2d;
inline IDWriteFactory* dw;
inline std::wstring family = L"Segoe UI";
inline HWND wnd;
inline ID2D1RenderTarget* rt;        // onde se desenha agora (a janela, ou a foto do --foto)
inline ID2D1HwndRenderTarget* hrt;   // o da janela
inline ID2D1SolidColorBrush* hbr;
inline ID2D1SolidColorBrush* br;

inline bool HasFont(const wchar_t* name) {
    IDWriteFontCollection* c = nullptr; dw->GetSystemFontCollection(&c);
    UINT32 i; BOOL ex = FALSE;
    if (c) { c->FindFamilyName(name, &i, &ex); c->Release(); }
    return ex != FALSE;
}

inline bool Init() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
    if (!d2d || !dw) return false;
    family = HasFont(L"SF Pro Text") ? L"SF Pro Text" : HasFont(L"Inter") ? L"Inter" : L"Segoe UI";
    return true;
}

inline bool Dark() {
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}
inline float Sc() { return GetDpiForWindow(wnd) / 96.f; }

inline IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING,
                              bool wrap = false, const wchar_t* fam = nullptr) {
    IDWriteTextFormat* f = nullptr;
    dw->CreateTextFormat(fam ? fam : family.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(a);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* e = nullptr; dw->CreateEllipsisTrimmingSign(f, &e);
        f->SetTrimming(&t, e); if (e) e->Release();
    }
    return f;
}
inline const wchar_t* IconFamily() { static const wchar_t* f = HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets"; return f; }

inline float TextW(IDWriteTextFormat* f, const std::wstring& s) {
    IDWriteTextLayout* l = nullptr;
    dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 100, &l);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); l->Release();
    return m.widthIncludingTrailingWhitespace;
}

inline void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (s.empty() || !f) return;
    br->SetColor(c);
    // CLIP | ENABLE_COLOR_FONT (0x4): emojis em cor (Clima, Fotos...)
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, br, (D2D1_DRAW_TEXT_OPTIONS)(D2D1_DRAW_TEXT_OPTIONS_CLIP | 0x4));
}
inline void Fill(D2D1_RECT_F r, D2D1_COLOR_F c, float rad = 0) {
    br->SetColor(c);
    if (rad > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br); else rt->FillRectangle(r, br);
}
inline void Circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col, bool fill, float stroke = 1.5f) {
    br->SetColor(col);
    if (fill) rt->FillEllipse(D2D1::Ellipse(c, r, r), br); else rt->DrawEllipse(D2D1::Ellipse(c, r, r), br, stroke);
}
inline D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
inline bool In(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// ---- macOS 27 (Golden Gate): barra de ferramentas unificada. Sem a barra de título do Windows: a área do app
// começa no topo da janela e os primeiros BAR pt são a barra dele (botões redondos de vidro na mesma linha dos
// três botões da janela, que a WacOSWindow.dll desenha por cima a 26 pt da borda). Arrasta pela barra, menos
// pelos botões que o app registra ao pintar (BarButton).
inline float BAR = 0;          // 0: barra comum do Windows (Sequoia/Tahoe)
inline float barWanted = 0;    // o app que já tem o layout do macOS 27 pede (52) antes do CreateMain
inline std::vector<D2D1_RECT_F> barButtons;
inline void BarButton(D2D1_RECT_F r) { barButtons.push_back(r); }
inline LRESULT CALLBACK UnifiedSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_NCCALCSIZE && w) {  // tira só a barra de cima; as bordas de redimensionar continuam
        auto* p = (NCCALCSIZE_PARAMS*)l;
        LONG top = p->rgrc[0].top;
        LRESULT r = DefSubclassProc(h, m, w, l);
        p->rgrc[0].top = top;
        if (IsZoomed(h)) { UINT dpi = GetDpiForWindow(h); p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi); }
        return r;
    }
    if (m == WM_NCHITTEST) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        if (r != HTCLIENT) return r;
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        float s = GetDpiForWindow(h) / 96.f, x = p.x / s, y = p.y / s;
        if (!IsZoomed(h) && p.y < (int)(5 * s)) return HTTOP;
        if (y >= BAR || x < (BAR > 40 ? 84 : 72)) return HTCLIENT;
        for (auto& b : barButtons) if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) return HTCLIENT;
        return HTCAPTION;
    }
    return DefSubclassProc(h, m, w, l);
}

// Botão redondo de vidro da barra do macOS 27 (círculo de 36 ou cápsula): branco leitoso com contorno fino e
// sombra curta no claro; cinza translúcido no escuro
inline void GlassButton(D2D1_RECT_F r, bool dark, bool hot = false) {
    float rad = (r.bottom - r.top) / 2;
    for (int k = 3; k >= 1; k--) {
        br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.10f : 0.035f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k * 0.5f, r.top + k * 0.6f, r.right + k * 0.5f, r.bottom + k * 1.1f), rad + k, rad + k), br);
    }
    br->SetColor(dark ? D2D1::ColorF(0.24f, 0.24f, 0.245f, 1) : D2D1::ColorF(hot ? 0.95f : 0.985f, hot ? 0.95f : 0.985f, hot ? 0.95f : 0.985f, 1));
    if (dark && hot) br->SetColor(D2D1::ColorF(0.29f, 0.29f, 0.295f, 1));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.13f) : D2D1::ColorF(0, 0, 0, 0.09f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.25f, r.top + 0.25f, r.right - 0.25f, r.bottom - 0.25f), rad - 0.25f, rad - 0.25f), br, 0.5f);
}
// campo de busca em cápsula do macOS 27 (barra lateral): vidro claro com contorno
inline void GlassField(D2D1_RECT_F r, bool dark) {
    float rad = (r.bottom - r.top) / 2;
    br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.12f : 0.03f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1.5f), rad, rad), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0.975f, 0.98f, 0.985f, 1));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.08f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.25f, r.top + 0.25f, r.right - 0.25f, r.bottom - 0.25f), rad - 0.25f, rad - 0.25f), br, 0.5f);
}
// ícone de "barra lateral" (retângulo com a coluna da esquerda e três tracinhos) e lupa, desenhados
inline void IconSidebar(D2D1_POINT_2F c, D2D1_COLOR_F col) {
    br->SetColor(col);
    D2D1_RECT_F r = D2D1::RectF(c.x - 9, c.y - 7, c.x + 9, c.y + 7);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 3, 3), br, 1.5f);
    rt->DrawLine({ c.x - 3, r.top }, { c.x - 3, r.bottom }, br, 1.5f);
    for (int i = 0; i < 3; i++) rt->DrawLine({ c.x - 7, c.y - 3.5f + i * 2.5f }, { c.x - 5, c.y - 3.5f + i * 2.5f }, br, 1.1f);
}
inline void IconPlus(D2D1_POINT_2F c, D2D1_COLOR_F col, float s = 7) {
    br->SetColor(col);
    rt->DrawLine({ c.x - s, c.y }, { c.x + s, c.y }, br, 1.6f);
    rt->DrawLine({ c.x, c.y - s }, { c.x, c.y + s }, br, 1.6f);
}
inline void IconSearch(D2D1_POINT_2F c, D2D1_COLOR_F col, float s = 1) {
    br->SetColor(col);
    rt->DrawEllipse(D2D1::Ellipse({ c.x - 1.5f * s, c.y - 1.5f * s }, 5 * s, 5 * s), br, 1.5f);
    rt->DrawLine({ c.x + 2.2f * s, c.y + 2.2f * s }, { c.x + 6 * s, c.y + 6 * s }, br, 1.6f);
}

// --foto <arquivo.png> (teste): o app abre escondido, desenha a janela inteira num bitmap e salva (sem passar pela
// tela). Assim dá para conferir o desenho mesmo quando a tela não mostra o app (programas abertos de dentro de
// outros contêineres).
inline ID2D1RenderTarget* fotoRt;
inline ID2D1SolidColorBrush* fotoBr;
inline IWICBitmap* fotoBmp;
inline std::wstring FotoPath() {
    static int done = 0; static std::wstring p;
    if (done) return p;
    done = 1;
    int n = 0; LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; a && i + 1 < n; i++) if (!wcscmp(a[i], L"--foto")) p = a[i + 1];
    if (a) LocalFree(a);
    return p;
}
// no modo foto tudo é desenhado desde o começo no bitmap (miniaturas e ícones criados lá valem no desenho final)
inline void FotoTarget() {
    if (fotoRt || FotoPath().empty() || !wnd) return;
    RECT r; GetClientRect(wnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return;
    float dpi = (float)GetDpiForWindow(wnd);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (!wic) return;
    if (SUCCEEDED(wic->CreateBitmap(r.right, r.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &fotoBmp)))
        d2d->CreateWicBitmapRenderTarget(fotoBmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi), &fotoRt);
    if (fotoRt) { fotoRt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &fotoBr); rt = fotoRt; br = fotoBr; }
    wic->Release();
}
inline bool BeginPaint() {
    FotoTarget();
    if (fotoRt) {  // desenhando para a foto
        rt = fotoRt; br = fotoBr;
        rt->BeginDraw();
        barButtons.clear();
        return true;
    }
    if (!hrt) {
        RECT r; GetClientRect(wnd, &r);
        float dpi = (float)GetDpiForWindow(wnd);
        d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi),
                                    D2D1::HwndRenderTargetProperties(wnd, D2D1::SizeU(r.right, r.bottom)), &hrt);
        if (hrt) hrt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &hbr);
    }
    rt = hrt; br = hbr;
    if (!rt) return false;
    rt->BeginDraw();
    barButtons.clear();
    return true;
}
inline void EndPaint() {
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET && rt == hrt) { hbr->Release(); hbr = nullptr; hrt->Release(); hrt = nullptr; rt = nullptr; br = nullptr; InvalidateRect(wnd, nullptr, FALSE); }
}
inline void OnSize(LPARAM l) { if (hrt) hrt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l))); InvalidateRect(wnd, nullptr, FALSE); }
inline void OnDpi(WPARAM w, LPARAM l) {
    RECT* r = (RECT*)l;
    SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (hrt) hrt->SetDpi((float)HIWORD(w), (float)HIWORD(w));
}
inline void CALLBACK FotoTick(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    FotoTarget();
    std::wstring path = FotoPath();
    if (fotoRt && fotoBmp) {
        SendMessageW(wnd, WM_PAINT, 0, 0);
        UINT w = 0, h = 0; fotoBmp->GetSize(&w, &h);
        IWICImagingFactory* wic = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
        if (wic && SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr))) {
            fr->SetSize(w, h);
            WICPixelFormatGUID f = GUID_WICPixelFormat32bppPBGRA; fr->SetPixelFormat(&f);
            fr->WriteSource(fotoBmp, nullptr); fr->Commit(); enc->Commit();
        }
        if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wic) wic->Release();
    }
    DestroyWindow(wnd);
}
inline D2D1_SIZE_F Size() { RECT r; GetClientRect(wnd, &r); float s = Sc(); return { r.right / s, r.bottom / s }; }

// ---- campo de edição sobre o desenho (EDIT do Windows, sem borda, na fonte do app)
struct Field {
    HWND h = nullptr;
    WNDPROC old = nullptr;
    HFONT font = nullptr;
    std::function<void(const std::wstring&, int key)> done;  // key: VK_RETURN, VK_ESCAPE, VK_TAB ou 0 (perdeu o foco)
    bool ending = false;
};
inline Field field;
inline LRESULT CALLBACK FieldProc(HWND h, UINT m, WPARAM w, LPARAM l);
inline void FieldEnd(int key) {
    if (!field.h || field.ending) return;
    field.ending = true;
    int n = GetWindowTextLengthW(field.h);
    std::wstring t(n, L'\0'); GetWindowTextW(field.h, t.data(), n + 1);
    HWND h = field.h; field.h = nullptr;
    auto done = field.done; field.done = nullptr;
    DestroyWindow(h);
    field.ending = false;
    if (done) done(t, key);
    InvalidateRect(wnd, nullptr, FALSE);
}
inline LRESULT CALLBACK FieldProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) { FieldEnd((int)w); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == 27 || w == L'\t')) return 0;
    if (m == WM_KILLFOCUS) { PostMessageW(wnd, WM_APP + 77, 0, 0); }
    return CallWindowProcW(field.old, h, m, w, l);
}
// r em DIP; size em pt
inline void FieldOpen(D2D1_RECT_F r, const std::wstring& text, float size, bool bold, std::function<void(const std::wstring&, int)> done) {
    FieldEnd(0);
    float s = Sc();
    if (field.font) DeleteObject(field.font);
    field.font = CreateFontW(-(int)std::lround(size * s), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, family.c_str());
    int hgt = (int)std::lround(size * s * 1.45f);
    int y = (int)std::lround((r.top + r.bottom) / 2 * s) - hgt / 2;
    field.h = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, (int)std::lround(r.left * s), y,
                              (int)std::lround((r.right - r.left) * s), hgt, wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!field.h) return;
    field.done = std::move(done);
    SendMessageW(field.h, WM_SETFONT, (WPARAM)field.font, TRUE);
    field.old = (WNDPROC)SetWindowLongPtrW(field.h, GWLP_WNDPROC, (LONG_PTR)FieldProc);
    SendMessageW(field.h, EM_SETSEL, 0, -1);
    SetFocus(field.h);
}
// cores do campo (WM_CTLCOLOREDIT): fundo do app e texto do tema
inline HBRUSH g_fieldBrush;
inline LRESULT FieldColors(HDC dc, COLORREF bg) {
    bool dark = Dark();
    SetTextColor(dc, dark ? RGB(240, 240, 240) : RGB(20, 20, 20));
    SetBkColor(dc, bg);
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(bg);
    return (LRESULT)g_fieldBrush;
}

// ---- dados em %APPDATA%\Shell
inline std::wstring DataPath(const wchar_t* name) {
    wchar_t ad[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    std::wstring d = std::wstring(ad) + L"\\Shell";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\" + name;
}
inline std::vector<std::wstring> ReadLines(const std::wstring& path) {
    std::vector<std::wstring> v;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") || !f) return v;
    wchar_t line[8192];
    while (fgetws(line, 8192, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        v.push_back(s);
    }
    fclose(f);
    return v;
}
inline bool WriteLines(const std::wstring& path, const std::vector<std::wstring>& lines) {
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w, ccs=UTF-8") || !f) return false;
    for (auto& l : lines) fwprintf(f, L"%ls\n", l.c_str());
    fclose(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}
// campos separados por TAB; TAB e quebra de linha dentro do texto viram \t e \n
inline std::wstring Esc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) { if (c == L'\\') o += L"\\\\"; else if (c == L'\t') o += L"\\t"; else if (c == L'\n') o += L"\\n"; else if (c != L'\r') o += c; }
    return o;
}
inline std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) { wchar_t n = s[++i]; o += n == L't' ? L'\t' : n == L'n' ? L'\n' : n; }
        else o += s[i];
    }
    return o;
}
inline std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> v; size_t a = 0, b;
    while ((b = s.find(L'\t', a)) != std::wstring::npos) { v.push_back(s.substr(a, b - a)); a = b + 1; }
    v.push_back(s.substr(a));
    return v;
}

// ---- datas: minutos desde 1970 (hora local)
inline long long NowMin() { return (long long)time(nullptr) / 60; }
inline long long ToMin(const SYSTEMTIME& st) { tm t{}; t.tm_year = st.wYear - 1900; t.tm_mon = st.wMonth - 1; t.tm_mday = st.wDay; t.tm_hour = st.wHour; t.tm_min = st.wMinute; t.tm_isdst = -1; return (long long)mktime(&t) / 60; }
inline SYSTEMTIME FromMin(long long m) {
    time_t s = (time_t)(m * 60); tm t{}; localtime_s(&t, &s);
    SYSTEMTIME st{}; st.wYear = (WORD)(t.tm_year + 1900); st.wMonth = (WORD)(t.tm_mon + 1); st.wDay = (WORD)t.tm_mday; st.wHour = (WORD)t.tm_hour; st.wMinute = (WORD)t.tm_min; st.wDayOfWeek = (WORD)t.tm_wday;
    return st;
}
inline long long DayStart(long long m) { SYSTEMTIME s = FromMin(m); s.wHour = s.wMinute = 0; return ToMin(s); }
inline const wchar_t* MonthName(int m) {  // 1..12
    static const wchar_t* pt[] = { L"janeiro", L"fevereiro", L"março", L"abril", L"maio", L"junho", L"julho", L"agosto", L"setembro", L"outubro", L"novembro", L"dezembro" };
    static const wchar_t* en[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August", L"September", L"October", L"November", L"December" };
    if (idioma::Pt()) return pt[(m - 1) % 12];
    if (idioma::En()) return en[(m - 1) % 12];
    static std::wstring cache[idioma::COUNT][12];  // os outros idiomas: o nome do Windows, com maiúscula ("Novembre", "Ноябрь")
    std::wstring& s = cache[idioma::Current()][(m - 1) % 12];
    if (s.empty()) s = idioma::Cap(idioma::MonthName(m));
    return s.c_str();
}
inline const wchar_t* WeekdayShort(int d) {  // 0 = domingo
    static const wchar_t* pt[] = { L"dom.", L"seg.", L"ter.", L"qua.", L"qui.", L"sex.", L"sáb." };
    static const wchar_t* en[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    if (idioma::Pt()) return pt[d % 7];
    if (idioma::En()) return en[d % 7];
    static std::wstring cache[idioma::COUNT][7];
    std::wstring& s = cache[idioma::Current()][d % 7];
    if (s.empty()) s = idioma::DayName(d, 1);
    return s.c_str();
}
// "Hoje, 14:30" / "Amanhã" / "12/11/2026, 09:00"
inline std::wstring DueText(long long m, bool withTime) {
    if (!m) return L"";
    long long today = DayStart(NowMin()), day = DayStart(m);
    SYSTEMTIME s = FromMin(m);
    wchar_t b[64];
    std::wstring d;
    if (day == today) d = Tr(L"Hoje");
    else if (day == today + 24 * 60) d = Tr(L"Amanhã");
    else if (day == today - 24 * 60) d = Tr(L"Ontem");
    else if (idioma::Pt()) { swprintf_s(b, L"%02d/%02d/%04d", s.wDay, s.wMonth, s.wYear); d = b; }
    else { GetDateFormatEx(idioma::Locale(), DATE_SHORTDATE, &s, nullptr, b, 64, nullptr); d = b; }  // 11/12/2026, 12.11.2026, 2026/11/12
    if (withTime) { swprintf_s(b, L", %02d:%02d", s.wHour, s.wMinute); d += b; }
    return d;
}

inline HWND CreateMain(HINSTANCE hi, const wchar_t* cls, const wchar_t* title, WNDPROC proc, int w, int h) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    MONITORINFO mi{ sizeof(mi) }; POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    float s = GetDpiForSystem() / 96.f;
    int W = (int)(w * s), H = (int)(h * s);
    // os três botões do Mac vêm da WacOSWindow.dll, carregada aqui mesmo (barra_mac.h): sem ela (DLL faltando),
    // fica a barra comum do Windows, nunca a barra unificada sem botão nenhum
    bool mac = barramac::Ligar();
    wnd = CreateWindowExW(0, cls, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2,
                          mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2, W, H, nullptr, nullptr, hi, nullptr);
    if (wnd) { BOOL dark = Dark(); DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark)); }
    if (wnd && mac && semaforo::Aqua() && barWanted > 0) {  // macOS 27: barra de ferramentas unificada
        BAR = barWanted;
        SetPropW(wnd, L"WacOSBarraUnificada", (HANDLE)(INT_PTR)std::lround(BAR));
        SetWindowSubclass(wnd, UnifiedSub, 0x55A1, 0);
        SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return wnd;
}

// ---- barra lateral tingida pelo papel de parede (macOS 26, "Permitir tingir janelas com o papel de parede";
// Ajustes > Aparência > Transparência, tingir_janelas=1 por padrão). Só a "cor" do papel de parede entra (o
// quanto ele foge do cinza), não o claro/escuro: a barra continua clara no tema claro e escura no escuro.
inline bool TintOn() {
    for (auto& l : ReadLines(DataPath(L"ajustes.txt"))) if (l.rfind(L"tingir_janelas=", 0) == 0) return l.substr(15) != L"0";
    return true;
}
inline bool WallpaperAverage(float& r, float& g, float& b) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // (já iniciado: não faz nada)
    wchar_t wp[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0) || !wp[0]) return false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(wp, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, 24, 24, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        std::vector<BYTE> px(24 * 24 * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, 24 * 4, (UINT)px.size(), px.data()))) {
            double R = 0, G = 0, B = 0;
            for (size_t i = 0; i < px.size(); i += 4) { B += px[i]; G += px[i + 1]; R += px[i + 2]; }
            double n = 24 * 24 * 255.0;
            r = (float)(R / n); g = (float)(G / n); b = (float)(B / n);
            ok = true;
        }
    }
    if (cv) cv->Release(); if (sc) sc->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    wic->Release();
    return ok;
}
inline D2D1_COLOR_F Tinted(D2D1_COLOR_F base) {
    static int state = -1;  // -1: não lido; 0: sem tinta; 1: com
    static float dr, dg, db;
    if (state < 0) {
        state = 0;
        float r, g, b;
        if (TintOn() && WallpaperAverage(r, g, b)) {
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            auto lim = [](float v) { return std::clamp(v, -0.3f, 0.3f); };
            dr = lim(r - lum); dg = lim(g - lum); db = lim(b - lum);
            state = 1;
        }
    }
    if (state != 1) return base;
    float k = Dark() ? 0.11f : 0.09f;  // leve, como no Mac (mais forte a barra ficava da cor do papel de parede)
    auto cl = [](float v) { return std::clamp(v, 0.f, 1.f); };
    return D2D1::ColorF(cl(base.r + dr * k), cl(base.g + dg * k), cl(base.b + db * k), base.a);
}
// macOS 27 Golden Gate (capturas do 512 Pixels): a barra lateral é uma cor cheia, sem a tinta do papel de parede
inline D2D1_COLOR_F SidebarColor() {
    if (semaforo::Aqua()) return Dark() ? D2D1::ColorF(43 / 255.f, 41 / 255.f, 40 / 255.f) : D2D1::ColorF(237 / 255.f, 237 / 255.f, 236 / 255.f);
    return Tinted(Dark() ? D2D1::ColorF(40 / 255.f, 40 / 255.f, 42 / 255.f) : D2D1::ColorF(242 / 255.f, 242 / 255.f, 245 / 255.f));
}

// Grava um ajuste no %APPDATA%\Shell\ajustes.txt (o mesmo dos Ajustes do Sistema). Com o Shell aberto, a mudança vai
// para ele (WM_COPYDATA 'SJST'): ele grava no arquivo dele e recarrega; sem o Shell (modo só apps), grava aqui.
inline void SetAppSetting(const wchar_t* key, const std::wstring& v) {
    HWND sh = FindWindowExW(HWND_MESSAGE, nullptr, L"ShellMensagens", nullptr);
    if (!sh) sh = FindWindowW(L"ShellMensagens", nullptr);
    std::wstring kv = std::wstring(key) + L"=" + v;
    if (sh) {
        COPYDATASTRUCT cd{ 0x534A5354, (DWORD)((kv.size() + 1) * sizeof(wchar_t)), (void*)kv.c_str() };
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(sh, WM_COPYDATA, (WPARAM)wnd, (LPARAM)&cd, SMTO_ABORTIFHUNG, 500, &r) && r) return;
    }
    std::wstring path = DataPath(L"ajustes.txt"), want = std::wstring(key) + L"=";
    auto lines = ReadLines(path);
    bool done = false;
    for (auto& l : lines) if (l.rfind(want, 0) == 0) { l = kv; done = true; }
    if (!done) lines.push_back(kv);
    WriteLines(path, lines);
    static UINT msg = RegisterWindowMessageW(L"ShellAjustesMudaram");
    if (sh) PostMessageW(sh, msg, 0, 0);
}

inline int Run() {
    if (!FotoPath().empty() && wnd) SetTimer(nullptr, 0, 1800, FotoTick);  // teste: espera carregar e salva a foto
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

}  // namespace app2d
