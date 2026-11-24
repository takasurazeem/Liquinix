// Controles do Windows no visual do Mac, dentro das janelas de diálogo que a MacJanela.dll trata
// (Propriedades do Mouse, Som, a maioria dos diálogos de configuração): fundo e texto do Mac (claro ou
// escuro, como o Windows), fonte SF Pro, botões do Mac (o padrão em azul), caixas de seleção e rádios
// azuis, abas como o controle segmentado sobre uma caixa arredondada, grupos como caixas, menus
// suspensos como o botão de menu do Mac, campos com borda arredondada, listas no tema escuro e
// controles deslizantes com a bolinha branca.
// Cada controle é subclassificado e desenhado por nós (Direct2D num DC, com antisserrilhado); o
// comportamento continua o do Windows. Desfazer (o Shell sai): tudo volta ao original.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <map>
#include "../comum/leopard.h"  // tema Mac OS X Leopard: botões em pílula Aqua (o padrão em gel azul), fundo cinza 232
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace controles {
void Style(HWND top);
void Child(HWND h);
void Unstyle(HWND top);
bool IsDialog(HWND h);
bool IsAlert(HWND h);
}

namespace {

const UINT_PTR SUB_ID = 0x4D41434B;  // 'MACK'
const wchar_t* PROP_FONT = L"MacCtlFonte";  // fonte original do controle (para desfazer)

enum Type { T_DIALOG = 1, T_BUTTON, T_TAB, T_COMBO, T_EDIT, T_LIST };

ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
std::wstring g_face;  // SF Pro Text se instalada

bool Dark() {
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}

void Init() {
    if (g_d2d) return;
    D2D1_FACTORY_OPTIONS o{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), &o, (void**)&g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    g_face = L"Segoe UI";
    if (g_dw) {
        IDWriteFontCollection* fc = nullptr; g_dw->GetSystemFontCollection(&fc);
        for (auto n : { L"SF Pro Text", L"Inter" }) {
            UINT32 i; BOOL ok = FALSE;
            if (fc) fc->FindFamilyName(n, &i, &ok);
            if (ok) { g_face = n; break; }
        }
        if (fc) fc->Release();
    }
}

// ---- cores do Mac (claro / escuro)
struct Pal {
    COLORREF bg, box, boxBorder, text, text2, field, fieldBorder, button, buttonTop, buttonBorder, accent, track;
};
Pal Colors() {
    // Leopard: o cinza 232 das janelas do 10.5 (sempre claro: o 10.5 não tinha modo escuro), destaque azul do Aqua
    if (leopard::On()) return { RGB(232, 232, 232), RGB(222, 222, 222), RGB(190, 190, 190), RGB(0, 0, 0), RGB(110, 110, 110),
                                RGB(255, 255, 255), RGB(150, 150, 150), RGB(255, 255, 255), RGB(255, 255, 255), RGB(140, 140, 140), RGB(56, 117, 215), RGB(200, 200, 200) };
    if (Dark()) return { RGB(36, 36, 38), RGB(48, 48, 50), RGB(62, 62, 64), RGB(223, 223, 225), RGB(150, 150, 155),
                         RGB(30, 30, 31), RGB(70, 70, 72), RGB(88, 88, 90), RGB(100, 100, 102), RGB(28, 28, 30), RGB(10, 132, 255), RGB(70, 70, 72) };
    return { RGB(236, 236, 236), RGB(226, 226, 226), RGB(210, 210, 210), RGB(30, 30, 30), RGB(120, 120, 124),
             RGB(255, 255, 255), RGB(196, 196, 198), RGB(255, 255, 255), RGB(255, 255, 255), RGB(186, 186, 190), RGB(0, 122, 255), RGB(212, 212, 214) };
}

D2D1_COLOR_F C(COLORREF c, float a = 1) { return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a); }

std::map<COLORREF, HBRUSH> g_brushes;
HBRUSH Brush(COLORREF c) {
    auto it = g_brushes.find(c);
    if (it != g_brushes.end()) return it->second;
    return g_brushes[c] = CreateSolidBrush(c);
}

std::wstring ClassOf(HWND h) { wchar_t b[64] = {}; GetClassNameW(h, b, 64); return b; }

float Scale(HWND h) { UINT d = GetDpiForWindow(h); return d ? d / 96.f : 1.f; }

// página de uma folha de propriedades (diálogo filho ao lado de uma barra de abas): fica na cor da caixa
bool IsPage(HWND dlg) {
    if (!(GetWindowLongW(dlg, GWL_STYLE) & WS_CHILD)) return false;
    HWND p = GetParent(dlg);
    return p && FindWindowExW(p, nullptr, WC_TABCONTROLW, nullptr) != nullptr;
}

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t), (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

COLORREF BaseOf(HWND dlg) { Pal p = Colors(); return IsPage(dlg) ? p.box : p.bg; }
COLORREF BoxOn(COLORREF base) { return Dark() ? Mix(base, RGB(255, 255, 255), 0.06f) : Mix(base, RGB(0, 0, 0), 0.045f); }

bool IsGroupBox(HWND h) { return ClassOf(h) == L"Button" && (GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX; }

// retângulo da caixa de um grupo (abaixo do título), nas coordenadas do pai
RECT GroupBoxRect(HWND g) {
    RECT r; GetWindowRect(g, &r);
    MapWindowPoints(nullptr, GetParent(g), (POINT*)&r, 2);
    r.top += (int)(8 * Scale(g));
    return r;
}

// cor embaixo de um controle: a do diálogo ou a de uma caixa de grupo que o contém
COLORREF UnderOf(HWND ctl) {
    HWND p = GetParent(ctl);
    COLORREF base = BaseOf(p);
    if (IsGroupBox(ctl)) return base;
    RECT r; GetWindowRect(ctl, &r);
    MapWindowPoints(nullptr, p, (POINT*)&r, 2);
    for (HWND c = GetWindow(p, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (c == ctl || !IsWindowVisible(c) || !IsGroupBox(c)) continue;
        RECT g = GroupBoxRect(c);
        if (r.left >= g.left && r.right <= g.right && r.top >= g.top && r.bottom <= g.bottom) return BoxOn(base);
    }
    return base;
}

// ---- desenho com Direct2D num DC (com cópia na memória: sem piscar)
struct Canvas {
    HDC target, mem = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    RECT rc;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    Canvas(HDC dc, RECT r) : target(dc), rc(r) {
        Init();
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, std::max<int>(1, r.right - r.left), std::max<int>(1, r.bottom - r.top));
        old = (HBITMAP)SelectObject(mem, bmp);
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (g_d2d && SUCCEEDED(g_d2d->CreateDCRenderTarget(&p, &rt))) {
            RECT b{ 0, 0, r.right - r.left, r.bottom - r.top };
            rt->BindDC(mem, &b);
            rt->BeginDraw();
            rt->SetTransform(D2D1::Matrix3x2F::Translation(-(float)r.left, -(float)r.top));
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
        }
    }
    ~Canvas() {
        if (rt) { rt->EndDraw(); if (br) br->Release(); rt->Release(); }
        BitBlt(target, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    }
    void Fill(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) { br->SetColor(c); rad > 0 ? rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br) : rt->FillRectangle(r, br); }
    void Stroke(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1) { br->SetColor(c); rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br, w); }
    void Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, bool wrap = false) {
        if (!g_dw || s.empty()) return;
        IDWriteTextFormat* tf = nullptr;
        if (FAILED(g_dw->CreateTextFormat(g_face.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf))) return;
        if (!wrap) {  // a SF Pro é mais larga que a fonte do Windows: encolhe até 85% para caber no controle
            IDWriteTextLayout* lay = nullptr;
            if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 10000, 1000, &lay))) {
                DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); lay->Release();
                float avail = r.right - r.left;
                if (m.widthIncludingTrailingWhitespace > avail && avail > 0) {
                    float k = std::max(0.85f, avail / m.widthIncludingTrailingWhitespace);
                    tf->Release(); tf = nullptr;
                    if (FAILED(g_dw->CreateTextFormat(g_face.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * k, L"pt-br", &tf))) return;
                }
            }
        }
        tf->SetTextAlignment(al);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tf->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        tf->SetTrimming(&tr, nullptr);
        br->SetColor(c);
        rt->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        tf->Release();
    }
};

D2D1_RECT_F RF(RECT r) { return D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom); }

std::wstring CleanText(HWND h) {  // sem o "&" dos atalhos
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) { if (s[i] == L'&') { if (i + 1 < s.size() && s[i + 1] == L'&') { o += L'&'; i++; } continue; } o += s[i]; }
    return o;
}

float FontPx(HWND h) {  // tamanho do texto do controle (px)
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    LOGFONTW lf{};
    if (f && GetObjectW(f, sizeof(lf), &lf) && lf.lfHeight) return (float)std::abs(lf.lfHeight) * (lf.lfHeight < 0 ? 1.f : 0.8f);
    return 12 * Scale(h);
}

// ---- fonte: SF Pro do mesmo tamanho (a disposição do diálogo continua a mesma)
std::map<std::pair<HFONT, int>, HFONT> g_fonts;
HFONT MacFont(HFONT f) {
    if (!f) return f;
    LOGFONTW lf{};
    if (!GetObjectW(f, sizeof(lf), &lf)) return f;
    if (!_wcsicmp(lf.lfFaceName, g_face.c_str())) return f;
    auto key = std::make_pair(f, 0);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    wcsncpy_s(lf.lfFaceName, g_face.c_str(), _TRUNCATE);
    lf.lfQuality = CLEARTYPE_QUALITY;
    HFONT n = CreateFontIndirectW(&lf);
    return g_fonts[key] = n ? n : f;
}

// ---- botões
void PaintButton(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    LONG st = GetWindowLongW(h, GWL_STYLE);
    UINT type = st & BS_TYPEMASK;
    LRESULT state = SendMessageW(h, BM_GETSTATE, 0, 0);
    bool enabled = IsWindowEnabled(h), pressed = (state & BST_PUSHED) != 0, focused = GetFocus() == h;
    bool hideFocus = (SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
    Pal p = Colors();
    float s = Scale(h), fs = FontPx(h);
    COLORREF under = UnderOf(h);
    Canvas cv(dc, rc);
    if (!cv.rt) return;
    cv.Fill(RF(rc), 0, C(under));
    float a = enabled ? 1.f : 0.4f;
    std::wstring text = CleanText(h);
    bool check = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE;
    bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    if ((check || radio) && !(st & BS_PUSHLIKE)) {
        UINT checked = (UINT)SendMessageW(h, BM_GETCHECK, 0, 0);
        float b = std::round(14 * s), cy = (rc.top + rc.bottom) / 2.f;
        if (st & BS_TOP) cy = rc.top + fs * 0.75f;
        D2D1_RECT_F box = D2D1::RectF(rc.left + 1 * s, cy - b / 2, rc.left + 1 * s + b, cy + b / 2);
        float rad = radio ? b / 2 : 3.5f * s;
        if (checked) {
            cv.Fill(box, rad, C(p.accent, a));
            cv.br->SetColor(D2D1::ColorF(1, 1, 1, a));
            if (radio) cv.rt->FillEllipse(D2D1::Ellipse({ (box.left + box.right) / 2, cy }, b * 0.2f, b * 0.2f), cv.br);
            else if (checked == BST_INDETERMINATE) cv.rt->DrawLine({ box.left + b * 0.28f, cy }, { box.right - b * 0.28f, cy }, cv.br, 1.8f * s);
            else {  // ✓
                ID2D1PathGeometry* g = nullptr; g_d2d->CreatePathGeometry(&g);
                ID2D1GeometrySink* k = nullptr;
                if (g && SUCCEEDED(g->Open(&k))) {
                    k->BeginFigure({ box.left + b * 0.24f, cy + b * 0.02f }, D2D1_FIGURE_BEGIN_HOLLOW);
                    k->AddLine({ box.left + b * 0.43f, cy + b * 0.22f });
                    k->AddLine({ box.left + b * 0.77f, cy - b * 0.24f });
                    k->EndFigure(D2D1_FIGURE_END_OPEN); k->Close(); k->Release();
                    ID2D1StrokeStyle* ss = nullptr;
                    g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &ss);
                    cv.rt->DrawGeometry(g, cv.br, 1.9f * s, ss);
                    if (ss) ss->Release();
                }
                if (g) g->Release();
            }
        } else {
            cv.Fill(box, rad, Dark() ? D2D1::ColorF(1, 1, 1, 0.12f * a) : C(RGB(255, 255, 255), a));
            cv.Stroke(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), rad, Dark() ? D2D1::ColorF(1, 1, 1, 0.18f * a) : D2D1::ColorF(0, 0, 0, 0.28f * a));
        }
        if (focused && !hideFocus)
            cv.Stroke(D2D1::RectF(box.left - 1.5f * s, box.top - 1.5f * s, box.right + 1.5f * s, box.bottom + 1.5f * s), rad + 1.5f * s, C(p.accent, 0.5f), 2.5f * s);
        D2D1_COLOR_F tc = C(p.text, a);
        cv.Text(text, D2D1::RectF(box.right + 6 * s, (float)rc.top, (float)rc.right, (float)rc.bottom), fs, tc, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_FONT_WEIGHT_NORMAL, (st & BS_MULTILINE) != 0);
        return;
    }
    // botão de apertar (e caixas "como botão")
    bool def = type == BS_DEFPUSHBUTTON;
    if (leopard::On()) {  // Leopard: pílula Aqua de 21 (perolada; o padrão e o apertado em gel azul), texto preto
        bool on2 = (st & BS_PUSHLIKE) && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        float hh = std::min<float>((float)(rc.bottom - rc.top) - 3 * s, 21 * s), yc = (rc.top + rc.bottom) / 2.f;
        D2D1_RECT_F r = D2D1::RectF(rc.left + 2 * s, yc - hh / 2, rc.right - 2 * s, yc + hh / 2);
        float rd = hh / 2;
        bool blue = def || on2 || pressed;
        cv.Fill(D2D1::RectF(r.left, r.top + 1 * s, r.right, r.bottom + 1 * s), rd, D2D1::ColorF(1, 1, 1, 0.6f * a));
        D2D1_GRADIENT_STOP gst[4] = { { 0, blue ? D2D1::ColorF(0.66f, 0.80f, 0.98f, a) : D2D1::ColorF(1, 1, 1, a) },
                                      { 0.5f, blue ? D2D1::ColorF(0.38f, 0.60f, 0.93f, a) : D2D1::ColorF(0.95f, 0.95f, 0.95f, a) },
                                      { 0.5f, blue ? D2D1::ColorF(0.24f, 0.47f, 0.87f, a) : D2D1::ColorF(0.88f, 0.88f, 0.88f, a) },
                                      { 1, blue ? D2D1::ColorF(0.58f, 0.82f, 1.0f, a) : D2D1::ColorF(0.98f, 0.98f, 0.98f, a) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(cv.rt->CreateGradientStopCollection(gst, 4, &gs)) &&
            SUCCEEDED(cv.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
            cv.rt->FillRoundedRectangle(D2D1::RoundedRect(r, rd, rd), lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
        cv.Stroke(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rd - 0.5f,
                  blue ? D2D1::ColorF(0.12f, 0.27f, 0.58f, 0.9f * a) : D2D1::ColorF(0, 0, 0, 0.42f * a));
        if (focused && !hideFocus && !def)
            cv.Stroke(D2D1::RectF(r.left - 1.5f * s, r.top - 1.5f * s, r.right + 1.5f * s, r.bottom + 1.5f * s), rd + 1.5f * s, D2D1::ColorF(0.36f, 0.60f, 0.95f, 0.75f), 3 * s);
        cv.Text(text, D2D1::RectF(r.left + 8 * s, r.top, r.right - 8 * s, r.bottom), fs, D2D1::ColorF(0, 0, 0, 0.9f * a), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL, (st & BS_MULTILINE) != 0);
        return;
    }
    bool on = (st & BS_PUSHLIKE) && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    float hgt = std::min<float>((float)(rc.bottom - rc.top) - 2 * s, 22 * s + fs * 0.3f), cy = (rc.top + rc.bottom) / 2.f;
    D2D1_RECT_F r = D2D1::RectF(rc.left + 2 * s, cy - hgt / 2, rc.right - 2 * s, cy + hgt / 2);
    float rad = 5.5f * s;
    if (!Dark()) {  // sombra fina embaixo, como no Mac claro
        cv.Fill(D2D1::RectF(r.left, r.top + 0.5f * s, r.right, r.bottom + 0.8f * s), rad, D2D1::ColorF(0, 0, 0, 0.10f * a));
    }
    D2D1_COLOR_F fill = def || on ? C(p.accent, a) : C(p.button, a);
    if (pressed) fill = def || on ? C(Mix(p.accent, RGB(0, 0, 0), 0.18f), a) : C(Dark() ? Mix(p.button, RGB(255, 255, 255), 0.15f) : Mix(p.button, RGB(0, 0, 0), 0.10f), a);
    cv.Fill(r, rad, fill);
    if (!def && !on) {
        if (Dark()) {  // brilho de 1 px em cima
            cv.br->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
            cv.rt->DrawLine({ r.left + rad, r.top + 0.5f }, { r.right - rad, r.top + 0.5f }, cv.br, 1);
        } else cv.Stroke(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, D2D1::ColorF(0, 0, 0, 0.16f * a));
    }
    if (focused && !hideFocus && !def)
        cv.Stroke(D2D1::RectF(r.left - 1, r.top - 1, r.right + 1, r.bottom + 1), rad + 1, C(p.accent, 0.55f), 2.5f * s);
    D2D1_COLOR_F tc = def || on ? D2D1::ColorF(1, 1, 1, a) : C(p.text, a);
    cv.Text(text, D2D1::RectF(r.left + 6 * s, r.top, r.right - 6 * s, r.bottom), fs, tc, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL, (st & BS_MULTILINE) != 0);
}

bool OurButton(HWND h) {
    LONG st = GetWindowLongW(h, GWL_STYLE);
    UINT t = st & BS_TYPEMASK;
    if (st & (BS_BITMAP | BS_ICON)) return false;  // botões com imagem: o desenho é do app
    return t == BS_PUSHBUTTON || t == BS_DEFPUSHBUTTON || t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_3STATE ||
           t == BS_AUTO3STATE || t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON || t == BS_GROUPBOX;
}

// ---- abas: o controle segmentado do Mac, centrado, sobre a caixa das páginas
std::map<HWND, std::vector<RECT>> g_segments;

void PaintTab(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    Pal p = Colors();
    float s = Scale(h), fs = FontPx(h);
    COLORREF base = BaseOf(GetParent(h));
    Canvas cv(dc, rc);
    if (!cv.rt) return;
    cv.Fill(RF(rc), 0, C(base));
    int n = TabCtrl_GetItemCount(h), cur = TabCtrl_GetCurSel(h);
    std::vector<std::wstring> names(n);
    std::vector<float> widths(n);
    float total = 0;
    for (int i = 0; i < n; i++) {
        wchar_t b[128] = {};
        TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = b; it.cchTextMax = 127;
        TabCtrl_GetItem(h, i, &it);
        std::wstring t;
        for (wchar_t* c = b; *c; c++) if (*c != L'&') t += *c;
        names[i] = t;
        // largura aproximada pelo DirectWrite
        IDWriteTextFormat* tf = nullptr; IDWriteTextLayout* lay = nullptr; float w = 40 * s;
        if (g_dw && SUCCEEDED(g_dw->CreateTextFormat(g_face.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs, L"pt-br", &tf))) {
            if (SUCCEEDED(g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), tf, 2000, 100, &lay))) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; lay->Release(); }
            tf->Release();
        }
        widths[i] = w + 24 * s;
        total += widths[i];
    }
    RECT disp = rc; TabCtrl_AdjustRect(h, FALSE, &disp);
    float segH = std::round(22 * s), segTop = std::max(2 * s, (disp.top - segH) / 2.f - 2 * s);
    float maxW = (rc.right - rc.left) - 16 * s;
    if (total > maxW) { float k = maxW / total; for (auto& w : widths) w *= k; total = maxW; }
    float x0 = (rc.left + rc.right - total) / 2;
    // caixa das páginas: começa no meio do controle segmentado
    D2D1_RECT_F box = D2D1::RectF((float)rc.left + 0.5f, segTop + segH / 2, (float)rc.right - 0.5f, (float)rc.bottom - 0.5f);
    cv.Fill(box, 8 * s, C(p.box));
    cv.Stroke(box, 8 * s, C(p.boxBorder));
    // trilho e segmentos
    D2D1_RECT_F track = D2D1::RectF(x0, segTop, x0 + total, segTop + segH);
    cv.Fill(track, 6 * s, Dark() ? C(RGB(62, 62, 64)) : C(RGB(222, 222, 224)));
    auto& seg = g_segments[h];
    seg.clear();
    float x = x0;
    for (int i = 0; i < n; i++) {
        D2D1_RECT_F r = D2D1::RectF(x, segTop, x + widths[i], segTop + segH);
        seg.push_back({ (LONG)r.left, (LONG)r.top, (LONG)r.right, (LONG)r.bottom });
        if (i == cur) {
            D2D1_RECT_F k = D2D1::RectF(r.left + 2 * s, r.top + 2 * s, r.right - 2 * s, r.bottom - 2 * s);
            if (!Dark()) cv.Fill(D2D1::RectF(k.left, k.top + 0.5f * s, k.right, k.bottom + 0.8f * s), 4.5f * s, D2D1::ColorF(0, 0, 0, 0.12f));
            cv.Fill(k, 4.5f * s, Dark() ? C(RGB(104, 104, 106)) : C(RGB(255, 255, 255)));
        } else if (i > 0 && i - 1 != cur) {  // divisória entre dois não escolhidos
            cv.br->SetColor(Dark() ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.14f));
            cv.rt->DrawLine({ r.left, r.top + 5 * s }, { r.left, r.bottom - 5 * s }, cv.br, 1);
        }
        cv.Text(names[i], r, fs, C(p.text), DWRITE_TEXT_ALIGNMENT_CENTER, i == cur ? DWRITE_FONT_WEIGHT_MEDIUM : DWRITE_FONT_WEIGHT_NORMAL);
        x += widths[i];
    }
}

// ---- menu suspenso (lista fixa): o botão de menu do Mac, com as setinhas no quadrado azul
void PaintCombo(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    Pal p = Colors();
    float s = Scale(h), fs = FontPx(h);
    bool enabled = IsWindowEnabled(h), focused = GetFocus() == h;
    float a = enabled ? 1.f : 0.4f;
    Canvas cv(dc, rc);
    if (!cv.rt) return;
    cv.Fill(RF(rc), 0, C(UnderOf(h)));
    float hgt = std::min<float>((float)(rc.bottom - rc.top) - 2 * s, 22 * s + fs * 0.3f), cy = (rc.top + rc.bottom) / 2.f;
    D2D1_RECT_F r = D2D1::RectF(rc.left + 1 * s, cy - hgt / 2, rc.right - 1 * s, cy + hgt / 2);
    float rad = 5.5f * s;
    if (!Dark()) cv.Fill(D2D1::RectF(r.left, r.top + 0.5f * s, r.right, r.bottom + 0.8f * s), rad, D2D1::ColorF(0, 0, 0, 0.10f * a));
    cv.Fill(r, rad, C(p.button, a));
    if (!Dark()) cv.Stroke(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, D2D1::ColorF(0, 0, 0, 0.16f * a));
    if (focused) cv.Stroke(D2D1::RectF(r.left - 1, r.top - 1, r.right + 1, r.bottom + 1), rad + 1, C(p.accent, 0.55f), 2.5f * s);
    // quadrado azul com ⌃⌄
    float q = hgt - 6 * s;
    D2D1_RECT_F sq = D2D1::RectF(r.right - 3 * s - q, cy - q / 2, r.right - 3 * s, cy + q / 2);
    cv.Fill(sq, 4 * s, C(p.accent, a));
    cv.br->SetColor(D2D1::ColorF(1, 1, 1, a));
    float mx = (sq.left + sq.right) / 2, w = q * 0.2f, gap = q * 0.1f, hh = q * 0.13f;
    cv.rt->DrawLine({ mx - w, cy - gap }, { mx, cy - gap - hh }, cv.br, 1.4f * s);
    cv.rt->DrawLine({ mx, cy - gap - hh }, { mx + w, cy - gap }, cv.br, 1.4f * s);
    cv.rt->DrawLine({ mx - w, cy + gap }, { mx, cy + gap + hh }, cv.br, 1.4f * s);
    cv.rt->DrawLine({ mx, cy + gap + hh }, { mx + w, cy + gap }, cv.br, 1.4f * s);
    // texto escolhido
    std::wstring t;
    int sel = ComboBox_GetCurSel(h);
    if (sel >= 0) { int n = ComboBox_GetLBTextLen(h, sel); if (n > 0) { t.resize(n); ComboBox_GetLBText(h, sel, &t[0]); } }
    cv.Text(t, D2D1::RectF(r.left + 9 * s, r.top, sq.left - 6 * s, r.bottom), fs, C(p.text, a));
}

// ---- borda arredondada dos campos e das listas (área de fora do cliente)
void PaintFieldFrame(HWND h) {
    HDC dc = GetWindowDC(h);
    if (!dc) return;
    RECT wr; GetWindowRect(h, &wr);
    RECT r{ 0, 0, wr.right - wr.left, wr.bottom - wr.top };
    POINT c{ 0, 0 }; ClientToScreen(h, &c);
    RECT cr; GetClientRect(h, &cr);
    int l = c.x - wr.left, t = c.y - wr.top;
    ExcludeClipRect(dc, l, t, l + cr.right, t + cr.bottom);
    Pal p = Colors();
    float s = Scale(h);
    bool focused = GetFocus() == h && ClassOf(h) == L"Edit";
    {
        Canvas cv(dc, r);
        if (cv.rt) {
            cv.Fill(RF(r), 0, C(UnderOf(h)));
            D2D1_RECT_F f = D2D1::RectF(0.5f, 0.5f, r.right - 0.5f, r.bottom - 0.5f);
            cv.Fill(f, 4 * s, C(p.field));
            cv.Stroke(f, 4 * s, focused ? C(p.accent) : C(p.fieldBorder), focused ? 2 * s : 1);
        }
    }
    ReleaseDC(h, dc);
}

// ---- controle deslizante (custom draw do pai)
LRESULT TrackbarDraw(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HWND h = cd->hdr.hwndFrom;
    Pal p = Colors();
    float s = Scale(h);
    bool vert = (GetWindowLongW(h, GWL_STYLE) & TBS_VERT) != 0;
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        RECT rc = cd->rc;
        RECT thumb{}; SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        // o canal vem com a altura do Windows; o do Mac é um trilho fino
        Canvas cv(cd->hdc, rc);
        if (cv.rt) {
            cv.Fill(RF(rc), 0, C(UnderOf(h)));
            float th = 4 * s;
            if (!vert) {
                float cy = (rc.top + rc.bottom) / 2.f;
                D2D1_RECT_F t = D2D1::RectF((float)rc.left, cy - th / 2, (float)rc.right, cy + th / 2);
                cv.Fill(t, th / 2, C(p.track));
                float mid = (thumb.left + thumb.right) / 2.f;
                cv.Fill(D2D1::RectF(t.left, t.top, std::max(t.left + th, mid), t.bottom), th / 2, C(p.accent));
            } else {
                float cx = (rc.left + rc.right) / 2.f;
                cv.Fill(D2D1::RectF(cx - th / 2, (float)rc.top, cx + th / 2, (float)rc.bottom), th / 2, C(p.track));
            }
        }
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        RECT rc = cd->rc;
        Canvas cv(cd->hdc, rc);
        if (cv.rt) {
            cv.Fill(RF(rc), 0, C(UnderOf(h)));
            float d = std::min<float>((float)(rc.right - rc.left), (float)(rc.bottom - rc.top)) - 2 * s, cx = (rc.left + rc.right) / 2.f, cy = (rc.top + rc.bottom) / 2.f;
            d = std::min(d, 20 * s);
            cv.br->SetColor(D2D1::ColorF(0, 0, 0, 0.25f));
            cv.rt->FillEllipse(D2D1::Ellipse({ cx, cy + 0.6f * s }, d / 2 + 0.5f, d / 2 + 0.5f), cv.br);
            cv.br->SetColor(D2D1::ColorF(1, 1, 1));
            cv.rt->FillEllipse(D2D1::Ellipse({ cx, cy }, d / 2, d / 2), cv.br);
        }
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref);
SUBCLASSPROC CtlProcPtr() { return CtlProc; }

// ---- grupos (desenhados no fundo do diálogo, antes dos controles)
void PaintGroups(HWND dlg, HDC dc) {
    Pal p = Colors();
    COLORREF base = BaseOf(dlg);
    for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindowVisible(c) || !IsGroupBox(c) || !GetWindowSubclass(c, CtlProcPtr(), SUB_ID, nullptr)) continue;
        RECT wr; GetWindowRect(c, &wr);
        MapWindowPoints(nullptr, dlg, (POINT*)&wr, 2);
        float s = Scale(c), fs = FontPx(c);
        RECT g = GroupBoxRect(c);
        Canvas cv(dc, wr);
        if (!cv.rt) continue;
        cv.Fill(RF(wr), 0, C(base));
        D2D1_RECT_F box = D2D1::RectF(g.left + 0.5f, g.top + 0.5f, g.right - 0.5f, g.bottom - 0.5f);
        cv.Fill(box, 7 * s, C(BoxOn(base)));
        cv.Stroke(box, 7 * s, Dark() ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.07f));
        // título: em cima da caixa, à esquerda, como os títulos de seção do Mac
        std::wstring t = CleanText(c);
        if (!t.empty()) {
            float tw = 0;
            IDWriteTextFormat* tf = nullptr; IDWriteTextLayout* lay = nullptr;
            if (g_dw && SUCCEEDED(g_dw->CreateTextFormat(g_face.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs * 0.92f, L"pt-br", &tf))) {
                if (SUCCEEDED(g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), tf, 2000, 100, &lay))) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); tw = m.width; lay->Release(); }
                tf->Release();
            }
            D2D1_RECT_F tr = D2D1::RectF(g.left + 8 * s, (float)wr.top, g.left + 8 * s + tw + 10 * s, (float)g.top + fs * 0.75f);
            cv.Fill(D2D1::RectF(tr.left - 3 * s, tr.top, tr.right, (float)g.top + 1.5f * s), 0, C(base));  // o título "corta" a borda de cima
            cv.Text(t, D2D1::RectF(tr.left, (float)wr.top, tr.right + 20 * s, (float)g.top + fs * 0.4f), fs * 0.92f, C(p.text2), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
    }
}

void RedrawNow(HWND h, int type) {
    if (!IsWindowVisible(h)) return;
    HDC dc = GetDC(h);
    if (!dc) return;
    if (type == T_BUTTON) {
        if (!IsGroupBox(h)) PaintButton(h, dc);
    } else if (type == T_TAB) PaintTab(h, dc);
    else if (type == T_COMBO) PaintCombo(h, dc);
    ReleaseDC(h, dc);
}

void ApplyListColors(HWND h, bool undo) {
    std::wstring cls = ClassOf(h);
    Pal p = Colors();
    bool dark = Dark() && !undo;
    SetWindowTheme(h, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (cls == WC_LISTVIEWW) {
        COLORREF bk = undo ? CLR_DEFAULT : p.field, tx = undo ? CLR_DEFAULT : p.text;
        ListView_SetBkColor(h, bk); ListView_SetTextBkColor(h, bk); ListView_SetTextColor(h, tx);
        if (HWND hd = ListView_GetHeader(h)) SetWindowTheme(hd, dark ? L"DarkMode_ItemsView" : nullptr, nullptr);
    } else if (cls == WC_TREEVIEWW) {
        TreeView_SetBkColor(h, undo ? (COLORREF)-1 : p.field);
        TreeView_SetTextColor(h, undo ? (COLORREF)-1 : p.text);
    }
    InvalidateRect(h, nullptr, TRUE);
}

// ---- caixas de mensagem como o alerta do Mac: sem barra de título, ícone do app grande no meio,
// título em negrito centralizado, texto informativo menor e os botões na largura toda (o principal,
// azul, à direita; três ou mais, um embaixo do outro)
struct Alert {
    std::wstring title, info;
    HICON app = nullptr, badge = nullptr;  // badge: o ícone de aviso/erro do Windows, quando houver
    float titleH = 0, infoH = 0;
    SIZE before{};                          // tamanho do cliente original (para desfazer)
};
std::map<HWND, Alert> g_alerts;

const float AW = 260, APAD = 20, AICON = 64, ABTN = 28, AGAP = 8;
// Leopard (alerta do 10.5): 420 de largura, ícone de 64 em (24, 15), o texto a 104 da borda (negrito 13 e 11 embaixo),
// botões em pílula de 21 embaixo à direita, o padrão na ponta direita
const float LW = 420, LTX = 104, LTOP = 15, LBTN = 21;
float TextWidthD(const std::wstring& s, float size) {
    if (s.empty() || !g_dw) return 0;
    IDWriteTextFormat* tf = nullptr; IDWriteTextLayout* lay = nullptr; float w = 0;
    if (SUCCEEDED(g_dw->CreateTextFormat(g_face.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf))) {
        if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 4000, 400, &lay))) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; lay->Release(); }
        tf->Release();
    }
    return w;
}

// caixa de mensagem: diálogo de cima com o texto (id 0xFFFF), ícone (id 20) e só botões de apertar
bool IsMessageBox(HWND dlg) {
    if (GetWindowLongW(dlg, GWL_STYLE) & WS_CHILD) return false;
    HWND text = GetDlgItem(dlg, 0xFFFF);
    if (!text || ClassOf(text) != L"Static") return false;
    int buttons = 0;
    for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        std::wstring cls = ClassOf(c);
        if (cls == L"Static") continue;
        if (cls != L"Button") return false;
        UINT t = GetWindowLongW(c, GWL_STYLE) & BS_TYPEMASK;
        if (t != BS_PUSHBUTTON && t != BS_DEFPUSHBUTTON) return false;
        if (IsWindowVisible(c)) buttons++;
    }
    return buttons >= 1 && buttons <= 4;
}

float TextHeight(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w, float width) {
    if (s.empty() || !g_dw) return 0;
    IDWriteTextFormat* tf = nullptr; IDWriteTextLayout* lay = nullptr; float h = 0;
    if (SUCCEEDED(g_dw->CreateTextFormat(g_face.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf))) {
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, width, 4000, &lay))) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); h = m.height; lay->Release(); }
        tf->Release();
    }
    return h;
}

HICON AppIcon(int px) {  // ícone do programa (o .exe deste processo), grande
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HICON big = nullptr;
    SHDefExtractIconW(exe, 0, 0, &big, nullptr, MAKELONG(px, 16));
    if (!big) ExtractIconExW(exe, 0, &big, nullptr, 1);
    return big;
}

void MakeAlert(HWND dlg) {
    Alert a;
    HWND text = GetDlgItem(dlg, 0xFFFF);
    std::wstring s = CleanText(text);
    // título e texto informativo: o primeiro parágrafo (ou a primeira frase de um texto longo) é o título
    size_t cut = s.find(L"\n\n");
    if (cut == std::wstring::npos) cut = s.find(L'\n');
    if (cut == std::wstring::npos && s.size() > 90) {
        size_t dot = s.find_first_of(L".?!");
        if (dot != std::wstring::npos && dot + 1 < s.size()) cut = dot + 1;
    }
    a.title = cut == std::wstring::npos ? s : s.substr(0, cut);
    a.info = cut == std::wstring::npos ? L"" : s.substr(cut);
    auto trim = [](std::wstring& t) { while (!t.empty() && iswspace(t.front())) t.erase(t.begin()); while (!t.empty() && iswspace(t.back())) t.pop_back(); };
    trim(a.title); trim(a.info);
    float s1 = Scale(dlg);
    a.app = AppIcon((int)(AICON * s1));
    if (HWND ic = GetDlgItem(dlg, 20)) {
        HICON h = (HICON)SendMessageW(ic, STM_GETICON, 0, 0);
        if (h && (h == LoadIconW(nullptr, IDI_WARNING) || h == LoadIconW(nullptr, IDI_ERROR))) a.badge = h;
        ShowWindow(ic, SW_HIDE);
    }
    ShowWindow(text, SW_HIDE);
    const bool leo = leopard::On();
    float inner = leo ? (LW - LTX - 20) * s1 : (AW - 2 * APAD) * s1;
    a.titleH = TextHeight(a.title, 13 * s1, DWRITE_FONT_WEIGHT_BOLD, inner);
    a.infoH = TextHeight(a.info, 11 * s1, DWRITE_FONT_WEIGHT_NORMAL, inner);
    RECT cr; GetClientRect(dlg, &cr);
    a.before = { cr.right, cr.bottom };
    // botões: o padrão (azul) à direita / em cima
    std::vector<HWND> btns;
    for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        if (ClassOf(c) == L"Button" && IsWindowVisible(c)) btns.push_back(c);
    auto isDef = [](HWND b) { return (GetWindowLongW(b, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON; };
    float y = APAD * s1 + AICON * s1 + 12 * s1 + a.titleH + (a.infoH > 0 ? 6 * s1 + a.infoH : 0) + 16 * s1;
    int W = (int)std::lround(AW * s1), pad = (int)std::lround(APAD * s1), bh = (int)std::lround(ABTN * s1), gap = (int)std::lround(AGAP * s1);
    if (leo) {  // Leopard: botões lado a lado embaixo à direita, o padrão por último (na ponta)
        W = (int)std::lround(LW * s1);
        y = std::max(LTOP + AICON, LTOP + 2 + a.titleH / s1 + (a.infoH > 0 ? 8 + a.infoH / s1 : 0)) * s1 + 18 * s1;
        std::stable_partition(btns.begin(), btns.end(), [&](HWND b) { return !isDef(b); });
        int x = W - (int)std::lround(20 * s1);
        bh = (int)std::lround(LBTN * s1);
        for (int k = (int)btns.size() - 1; k >= 0; k--) {
            wchar_t t[128] = {}; GetWindowTextW(btns[k], t, 128);
            std::wstring lab = t; lab.erase(std::remove(lab.begin(), lab.end(), L'&'), lab.end());
            int bw = (int)std::lround(std::max(82.f, TextWidthD(lab, 13) + 32) * s1);
            SetWindowPos(btns[k], nullptr, x - bw, (int)y, bw, bh + (int)std::lround(4 * s1), SWP_NOZORDER | SWP_NOACTIVATE);
            x -= bw + (int)std::lround(12 * s1);
        }
        y += bh + 4 * s1;
    } else
    if (btns.size() <= 2) {
        if (btns.size() == 2 && isDef(btns[0])) std::swap(btns[0], btns[1]);
        int bw = (W - 2 * pad - gap * ((int)btns.size() - 1)) / (int)btns.size();
        for (size_t i = 0; i < btns.size(); i++) SetWindowPos(btns[i], nullptr, pad + (int)i * (bw + gap), (int)y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
        y += bh;
    } else {
        std::stable_partition(btns.begin(), btns.end(), isDef);
        for (auto b : btns) { SetWindowPos(b, nullptr, pad, (int)y, W - 2 * pad, bh, SWP_NOZORDER | SWP_NOACTIVATE); y += bh + gap; }
        y -= gap;
    }
    int H = (int)std::lround(y + APAD * s1);
    // janela do tamanho novo, no mesmo centro (a barra de título fica para fora do recorte)
    RECT wr; GetWindowRect(dlg, &wr);
    RECT want{ 0, 0, W, H };
    AdjustWindowRectExForDpi(&want, GetWindowLongW(dlg, GWL_STYLE), FALSE, GetWindowLongW(dlg, GWL_EXSTYLE), GetDpiForWindow(dlg));
    int ww = want.right - want.left, wh = want.bottom - want.top;
    int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
    g_alerts[dlg] = a;
    SetWindowPos(dlg, nullptr, cx - ww / 2, cy - wh / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

void PaintAlert(HWND dlg, HDC dc) {
    auto it = g_alerts.find(dlg);
    if (it == g_alerts.end()) return;
    Alert& a = it->second;
    RECT rc; GetClientRect(dlg, &rc);
    Pal p = Colors();
    float s = Scale(dlg), pad = APAD * s, ic = AICON * s;
    if (leopard::On()) {  // Leopard: o ícone à esquerda e o texto ao lado, alinhado à esquerda (alerta do 10.5)
        {
            Canvas cv(dc, rc);
            if (!cv.rt) return;
            cv.Fill(RF(rc), 0, C(p.bg));
            float y = LTOP * s + 2 * s;
            cv.Text(a.title, D2D1::RectF(LTX * s, y, rc.right - 20 * s, y + a.titleH), 13 * s, C(p.text), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_BOLD, true);
            y += a.titleH + 8 * s;
            if (a.infoH > 0) cv.Text(a.info, D2D1::RectF(LTX * s, y, rc.right - 20 * s, y + a.infoH), 11 * s, C(p.text), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_NORMAL, true);
        }
        int n = (int)std::lround(ic), x = (int)std::lround(24 * s), y = (int)std::lround(LTOP * s);
        if (a.app) DrawIconEx(dc, x, y, a.app, n, n, 0, nullptr, DI_NORMAL);
        if (a.badge) { int m = n * 45 / 100; DrawIconEx(dc, x + n - m, y + n - m, a.badge, m, m, 0, nullptr, DI_NORMAL); }  // o aviso no canto do ícone do app, como no 10.5
        return;
    }
    {
        Canvas cv(dc, rc);
        if (!cv.rt) return;
        cv.Fill(RF(rc), 0, C(p.bg));
        float y = pad + ic + 12 * s;
        cv.Text(a.title, D2D1::RectF(pad, y, rc.right - pad, y + a.titleH), 13 * s, C(p.text), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_BOLD, true);
        y += a.titleH + 6 * s;
        if (a.infoH > 0) cv.Text(a.info, D2D1::RectF(pad, y, rc.right - pad, y + a.infoH), 11 * s, C(p.text), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL, true);
        if (a.badge) {  // o triângulo amarelo de atenção do Mac (vetor: cantos arredondados, "!" escuro)
            float cx = rc.right / 2.f, t = pad + ic * 0.06f, b = pad + ic * 0.90f, hw = ic * 0.47f, r = ic * 0.09f;
            ID2D1PathGeometry* g = nullptr; g_d2d->CreatePathGeometry(&g);
            ID2D1GeometrySink* k = nullptr;
            if (g && SUCCEEDED(g->Open(&k))) {
                k->BeginFigure({ cx, t + r }, D2D1_FIGURE_BEGIN_FILLED);
                k->AddLine({ cx + hw - r, b - r }); k->AddLine({ cx - hw + r, b - r });
                k->EndFigure(D2D1_FIGURE_END_CLOSED); k->Close(); k->Release();
                ID2D1StrokeStyle* ss = nullptr;
                g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &ss);
                ID2D1LinearGradientBrush* lg = nullptr; ID2D1GradientStopCollection* stops = nullptr;
                D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1.0f, 0.86f, 0.25f) }, { 1, D2D1::ColorF(0.98f, 0.70f, 0.05f) } };
                cv.rt->CreateGradientStopCollection(st, 2, &stops);
                if (stops) cv.rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ cx, t }, { cx, b }), stops, &lg);
                ID2D1Brush* fill = lg ? (ID2D1Brush*)lg : (ID2D1Brush*)cv.br;
                if (!lg) cv.br->SetColor(D2D1::ColorF(1.0f, 0.80f, 0.10f));
                cv.rt->FillGeometry(g, fill);
                cv.rt->DrawGeometry(g, fill, r * 2, ss);
                // "!"
                cv.br->SetColor(D2D1::ColorF(0.12f, 0.12f, 0.12f));
                float bw = ic * 0.075f, top = t + ic * 0.30f, bot = t + ic * 0.58f;
                cv.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - bw / 2, top, cx + bw / 2, bot), bw / 2, bw / 2), cv.br);
                cv.rt->FillEllipse(D2D1::Ellipse({ cx, bot + ic * 0.11f }, bw * 0.62f, bw * 0.62f), cv.br);
                if (lg) lg->Release(); if (stops) stops->Release(); if (ss) ss->Release();
            }
            if (g) g->Release();
        }
    }
    // ícones (GDI, por cima): aviso/erro grande com o app pequeno no canto, como no Mac; senão o app
    int x = (int)std::lround((rc.right - ic) / 2), y = (int)std::lround(pad), n = (int)std::lround(ic);
    if (a.badge) {  // o triângulo já foi desenhado; o app pequeno no canto, como no Mac
        if (a.app) { int m = n * 45 / 100; DrawIconEx(dc, x + n - m + n / 8, y + n - m + n / 10, a.app, m, m, 0, nullptr, DI_NORMAL); }
    } else if (a.app) DrawIconEx(dc, x, y, a.app, n, n, 0, nullptr, DI_NORMAL);
}

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    int type = (int)ref;
    switch (m) {
    case WM_SETFONT: return DefSubclassProc(h, m, (WPARAM)MacFont((HFONT)w), l);  // o diálogo põe a fonte depois de criar
    case WM_NCDESTROY: RemoveWindowSubclass(h, CtlProc, SUB_ID); g_segments.erase(h); g_alerts.erase(h); break;
    }
    if (type == 0) return DefSubclassProc(h, m, w, l);  // texto fixo, controle deslizante...: só a fonte
    if (type == T_DIALOG) {
        switch (m) {
        case WM_PAINT:  // alerta: a caixa de mensagem pintaria a faixa branca de cima por cima do título
            if (g_alerts.count(h)) { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintAlert(h, dc); EndPaint(h, &ps); return 0; }
            break;
        case WM_ERASEBKGND: {
            if (g_alerts.count(h)) { PaintAlert(h, (HDC)w); return 1; }
            RECT rc; GetClientRect(h, &rc);
            FillRect((HDC)w, &rc, Brush(BaseOf(h)));
            PaintGroups(h, (HDC)w);
            return 1;
        }
        case WM_CTLCOLORDLG: return (LRESULT)Brush(BaseOf(h));
        case WM_NCHITTEST: {  // alerta: sem barra de título, arrasta pelo fundo
            LRESULT r = DefSubclassProc(h, m, w, l);
            return r == HTCLIENT && g_alerts.count(h) ? HTCAPTION : r;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
            HWND c = (HWND)l;
            COLORREF under = UnderOf(c);
            Pal p = Colors();
            SetTextColor((HDC)w, IsWindowEnabled(c) ? p.text : p.text2);
            SetBkColor((HDC)w, under);
            if (ClassOf(c) == L"Edit") { SetBkColor((HDC)w, p.field); return (LRESULT)Brush(p.field); }  // campo só de leitura
            return (LRESULT)Brush(under);
        }
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            Pal p = Colors();
            SetTextColor((HDC)w, p.text);
            SetBkColor((HDC)w, p.field);
            return (LRESULT)Brush(p.field);
        }
        case WM_NOTIFY: {
            auto* nm = (NMHDR*)l;
            if (nm->code == (UINT)NM_CUSTOMDRAW && ClassOf(nm->hwndFrom) == TRACKBAR_CLASSW) {
                LRESULT r = TrackbarDraw((NMCUSTOMDRAW*)l);
                SetWindowLongPtrW(h, DWLP_MSGRESULT, r);  // em diálogos a resposta do WM_NOTIFY vai aqui
                return r;
            }
            break;
        }
        case WM_SETTINGCHANGE: case WM_THEMECHANGED: RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME); break;
        }
        return DefSubclassProc(h, m, w, l);
    }
    if (type == T_EDIT || type == T_LIST) {
        if (m == WM_NCPAINT) { LRESULT r = DefSubclassProc(h, m, w, l); PaintFieldFrame(h); return r; }
        LRESULT r = DefSubclassProc(h, m, w, l);
        if (m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_ENABLE) PaintFieldFrame(h);
        return r;
    }
    if (type == T_BUTTON && IsGroupBox(h)) {  // o grupo é desenhado no fundo do diálogo
        if (m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_SETTEXT) { LRESULT r = DefSubclassProc(h, m, w, l); InvalidateRect(GetParent(h), nullptr, TRUE); return r; }
        return DefSubclassProc(h, m, w, l);
    }
    if (type == T_TAB && m == WM_LBUTTONDOWN) {  // os segmentos não ficam onde o Windows põe as abas
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        auto it = g_segments.find(h);
        if (it != g_segments.end())
            for (size_t i = 0; i < it->second.size(); i++)
                if (PtInRect(&it->second[i], pt)) { SetFocus(h); TabCtrl_SetCurFocus(h, (int)i); RedrawNow(h, type); return 0; }
        return 0;
    }
    if (m == WM_PAINT || m == WM_PRINTCLIENT) {
        PAINTSTRUCT ps; HDC dc = m == WM_PAINT ? BeginPaint(h, &ps) : (HDC)w;
        if (type == T_BUTTON) PaintButton(h, dc);
        else if (type == T_TAB) PaintTab(h, dc);
        else if (type == T_COMBO) PaintCombo(h, dc);
        if (m == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    LRESULT r = DefSubclassProc(h, m, w, l);
    // o Windows desenha alguns estados direto, sem WM_PAINT: por cima vai o nosso desenho de novo
    switch (m) {
    case BM_SETSTATE: case BM_SETCHECK: case BM_SETSTYLE: case BM_CLICK: case WM_SETTEXT: case WM_ENABLE:
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_KEYDOWN: case WM_KEYUP:
    case WM_MOUSELEAVE: case WM_CAPTURECHANGED: case WM_UPDATEUISTATE:
    case CB_SETCURSEL: case CB_SELECTSTRING: case TCM_SETCURSEL: case TCM_SETCURFOCUS: case WM_TIMER:
        RedrawNow(h, type);
        break;
    }
    return r;
}

void Sub(HWND h, int type) {
    if (GetWindowSubclass(h, CtlProc, SUB_ID, nullptr)) return;
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (f && !GetPropW(h, PROP_FONT)) SetPropW(h, PROP_FONT, (HANDLE)f);
    SetWindowSubclass(h, CtlProc, SUB_ID, (DWORD_PTR)type);
    if (f) SendMessageW(h, WM_SETFONT, (WPARAM)MacFont(f), FALSE);
    if (type == T_LIST) ApplyListColors(h, false);
    if (type == T_EDIT || type == T_LIST) SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(h, nullptr, TRUE);
}

}  // namespace

bool controles::IsDialog(HWND h) { return ClassOf(h) == L"#32770"; }
bool controles::IsAlert(HWND h) { return g_alerts.count(h) != 0; }

void controles::Child(HWND h) {
    Init();
    std::wstring cls = ClassOf(h);
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (cls == L"#32770") Sub(h, T_DIALOG);
    else if (cls == L"Button") { if (OurButton(h)) Sub(h, T_BUTTON); }
    else if (cls == WC_TABCONTROLW) { if (!(st & (TCS_BUTTONS | TCS_VERTICAL | TCS_OWNERDRAWFIXED))) Sub(h, T_TAB); }
    else if (cls == WC_COMBOBOXW) { if ((st & 3) == CBS_DROPDOWNLIST && !(st & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE))) Sub(h, T_COMBO); }
    else if (cls == L"Edit") { if ((ex & WS_EX_CLIENTEDGE) || (st & WS_BORDER)) Sub(h, T_EDIT); else Sub(h, 0); }
    else if (cls == WC_LISTVIEWW || cls == WC_TREEVIEWW || cls == L"ListBox") Sub(h, T_LIST);
    else if (cls == L"Static" || cls == TRACKBAR_CLASSW || cls == UPDOWN_CLASSW) Sub(h, 0);  // só a fonte (e as cores pelo pai)
}

void controles::Style(HWND top) {
    Init();
    SetPropW(top, L"MacCtl", (HANDLE)1);  // controles criados depois também entram (gancho em macjanela.cpp)
    Child(top);
    EnumChildWindows(top, [](HWND c, LPARAM) -> BOOL { controles::Child(c); return TRUE; }, 0);
    if (IsMessageBox(top)) MakeAlert(top);
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void controles::Unstyle(HWND top) {
    auto undo = [](HWND c) {
        DWORD_PTR ref = 0;
        if (!GetWindowSubclass(c, CtlProc, SUB_ID, &ref)) return;
        RemoveWindowSubclass(c, CtlProc, SUB_ID);
        if (HFONT f = (HFONT)RemovePropW(c, PROP_FONT)) SendMessageW(c, WM_SETFONT, (WPARAM)f, FALSE);
        if ((int)ref == T_LIST) ApplyListColors(c, true);
        SetWindowPos(c, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    };
    RemovePropW(top, L"MacCtl");
    if (g_alerts.erase(top)) {  // alerta: os textos do Windows voltam (o tamanho fica)
        if (HWND t = GetDlgItem(top, 0xFFFF)) ShowWindow(t, SW_SHOW);
        if (HWND i = GetDlgItem(top, 20)) ShowWindow(i, SW_SHOW);
    }
    undo(top);
    EnumChildWindows(top, [](HWND c, LPARAM p) -> BOOL { (*(decltype(undo)*)p)(c); return TRUE; }, (LPARAM)&undo);
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}
