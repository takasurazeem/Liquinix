// Dashboard do Mac OS X Leopard (tema Leopard; Win+F12, como o F12 do Mac): a tela escurece e os widgets aparecem
// por cima de tudo, crescendo um pouco ao entrar. Widgets do 10.5 desenhados aqui: o Relógio (mostrador branco, aro
// cromado, ponteiro vermelho dos segundos e a cidade embaixo), o Calendário (a folha do dia com a faixa vermelha e o
// mês ao lado, hoje marcado) e a Calculadora (corpo grafite, visor cinza-esverdeado, teclas que funcionam, também
// pelo teclado). O "+" no canto de baixo abre a barra de widgets para ligar/desligar cada um; os widgets se arrastam
// e a posição fica guardada (ajustes dashboard_pos / dashboard_on). Clicar fora dos widgets, Esc ou Win+F12 fecha.
#include "shell.h"
#include <cmath>
#include <algorithm>
#include "../comum/idioma.h"

namespace {

const UINT WM_DB_FRAME = WM_APP + 181;
const int T_CLOCK = 1;
enum { W_CLOCK, W_CAL, W_CALC, W_COUNT };
struct Wd { float w, h; float x, y; bool on; };
Wd g_w[W_COUNT] = { { 160, 178, 0, 0, true }, { 300, 150, 0, 0, true }, { 156, 226, 0, 0, true } };

HWND g_wnd;
ui::GpuSurface g_s;
bool g_open, g_closing, g_bar;
double g_at;          // início da animação de abrir/fechar
float g_W, g_H;       // tela em DIP
int g_drag = -1; D2D1_POINT_2F g_dragOff{}, g_downPt{}; bool g_moved;
int g_press = -1;     // tecla da calculadora apertada (índice)
// calculadora
double g_acc = 0, g_mem = 0; wchar_t g_op = 0; std::wstring g_disp = L"0"; bool g_fresh = true;

float Sc() { return ui::Scale(g_wnd); }

IDWriteTextFormat* F(float size, bool bold, bool center = true) {
    auto* f = ui::FormatFace(L"Lucida Sans Unicode", size, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    if (f) {
        f->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}
void Txt(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (!f) return;
    br->SetColor(c);
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, br);
}
void Grad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, std::initializer_list<D2D1_GRADIENT_STOP> st, bool ellipse = false) {
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st.begin(), (UINT32)st.size(), &gs)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb))) {
        if (ellipse) dc->FillEllipse(D2D1::Ellipse({ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, (r.right - r.left) / 2, (r.bottom - r.top) / 2), lb);
        else dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    }
    if (lb) lb->Release();
    if (gs) gs->Release();
}
void Shadow(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float rad, bool ellipse = false) {
    for (int i = 4; i >= 1; i--) {  // sombra macia feita de camadas
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.07f));
        D2D1_RECT_F q = D2D1::RectF(r.left - i, r.top - i + 4, r.right + i, r.bottom + i + 5);
        if (ellipse) dc->FillEllipse(D2D1::Ellipse({ (q.left + q.right) / 2, (q.top + q.bottom) / 2 }, (q.right - q.left) / 2, (q.bottom - q.top) / 2), br);
        else dc->FillRoundedRectangle(D2D1::RoundedRect(q, rad + i, rad + i), br);
    }
}

// ---- Relógio
void PaintClock(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r) {
    float R = (r.right - r.left) / 2 - 8;
    D2D1_POINT_2F c{ (r.left + r.right) / 2, r.top + 8 + R };
    D2D1_RECT_F o = D2D1::RectF(c.x - R, c.y - R, c.x + R, c.y + R);
    Shadow(dc, br, o, R, true);
    Grad(dc, o, 0, { { 0, D2D1::ColorF(0.98f, 0.98f, 0.99f) }, { 0.5f, D2D1::ColorF(0.58f, 0.59f, 0.62f) }, { 1, D2D1::ColorF(0.93f, 0.93f, 0.95f) } }, true);
    float r2 = R * 0.87f;
    Grad(dc, D2D1::RectF(c.x - r2, c.y - r2, c.x + r2, c.y + r2), 0, { { 0, D2D1::ColorF(0.84f, 0.84f, 0.86f) }, { 1, D2D1::ColorF(1, 1, 1) } }, true);
    static IDWriteTextFormat* num = F(15, true);
    for (int i = 1; i <= 12; i++) {
        float a = i * 3.14159265f / 6, x = c.x + std::sin(a) * r2 * 0.76f, y = c.y - std::cos(a) * r2 * 0.76f;
        Txt(dc, br, std::to_wstring(i), num, D2D1::RectF(x - 14, y - 10, x + 14, y + 10), D2D1::ColorF(0.08f, 0.08f, 0.08f));
    }
    SYSTEMTIME t; GetLocalTime(&t);
    float mn = t.wMinute + t.wSecond / 60.f, hr = (t.wHour % 12) + mn / 60;
    auto hand = [&](float ang, float len, float wd, D2D1_COLOR_F col, float tail) {
        br->SetColor(col);
        D2D1_POINT_2F d{ std::sin(ang), -std::cos(ang) };
        dc->DrawLine({ c.x - d.x * tail, c.y - d.y * tail }, { c.x + d.x * len, c.y + d.y * len }, br, wd);
    };
    hand(hr * 3.14159265f / 6, r2 * 0.50f, 4, D2D1::ColorF(0.08f, 0.08f, 0.08f), r2 * 0.08f);
    hand(mn * 3.14159265f / 30, r2 * 0.74f, 3, D2D1::ColorF(0.08f, 0.08f, 0.08f), r2 * 0.08f);
    hand(t.wSecond * 3.14159265f / 30, r2 * 0.80f, 1.3f, D2D1::ColorF(0.85f, 0.10f, 0.08f), r2 * 0.18f);
    br->SetColor(D2D1::ColorF(0.85f, 0.10f, 0.08f));
    dc->FillEllipse(D2D1::Ellipse(c, 3, 3), br);
    // o brilho de vidro em cima do mostrador
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.25f));
    dc->FillEllipse(D2D1::Ellipse({ c.x, c.y - r2 * 0.45f }, r2 * 0.70f, r2 * 0.42f), br);
    static IDWriteTextFormat* city = F(12, true);
    Txt(dc, br, Tr(L"Hora Local"), city, D2D1::RectF(r.left - 20, r.bottom - 16, r.right + 20, r.bottom + 2), D2D1::ColorF(1, 1, 1, 0.95f));
}

// ---- Calendário
void PaintCal(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r) {
    Shadow(dc, br, r, 12);
    Grad(dc, r, 12, { { 0, D2D1::ColorF(0.30f, 0.30f, 0.32f) }, { 1, D2D1::ColorF(0.12f, 0.12f, 0.13f) } });
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.25f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 11.5f, 11.5f), br, 1);
    SYSTEMTIME t; GetLocalTime(&t);
    // a folha do dia (como o ícone do iCal)
    D2D1_RECT_F pg = D2D1::RectF(r.left + 10, r.top + 10, r.left + 120, r.bottom - 10);
    Grad(dc, pg, 6, { { 0, D2D1::ColorF(1, 1, 1) }, { 1, D2D1::ColorF(0.88f, 0.88f, 0.89f) } });
    D2D1_RECT_F band = D2D1::RectF(pg.left, pg.top, pg.right, pg.top + 30);
    Grad(dc, band, 6, { { 0, D2D1::ColorF(0.93f, 0.36f, 0.33f) }, { 1, D2D1::ColorF(0.72f, 0.07f, 0.07f) } });
    br->SetColor(D2D1::ColorF(0.72f, 0.07f, 0.07f));
    dc->FillRectangle(D2D1::RectF(band.left, band.bottom - 6, band.right, band.bottom), br);
    static const wchar_t* dias[] = { L"Domingo", L"Segunda", L"Terça", L"Quarta", L"Quinta", L"Sexta", L"Sábado" };
    static IDWriteTextFormat *fd = F(13, true), *big = F(64, true), *fm = F(12, true), *fs = F(10, false);
    Txt(dc, br, Tr(dias[t.wDayOfWeek]), fd, band, D2D1::ColorF(1, 1, 1));
    Txt(dc, br, std::to_wstring(t.wDay), big, D2D1::RectF(pg.left, band.bottom, pg.right, pg.bottom), D2D1::ColorF(0.1f, 0.1f, 0.1f));
    // o mês
    static const wchar_t* meses[] = { L"Janeiro", L"Fevereiro", L"Março", L"Abril", L"Maio", L"Junho", L"Julho", L"Agosto", L"Setembro", L"Outubro", L"Novembro", L"Dezembro" };
    float gx = pg.right + 10, gw = r.right - 10 - gx;
    Txt(dc, br, std::wstring(Tr(meses[t.wMonth - 1])) + L" " + std::to_wstring(t.wYear), fm, D2D1::RectF(gx, r.top + 8, gx + gw, r.top + 24), D2D1::ColorF(1, 1, 1, 0.95f));
    static const wchar_t* ini[] = { L"D", L"S", L"T", L"Q", L"Q", L"S", L"S" };
    float cw = gw / 7;
    for (int i = 0; i < 7; i++) Txt(dc, br, ini[i], fs, D2D1::RectF(gx + i * cw, r.top + 26, gx + (i + 1) * cw, r.top + 38), D2D1::ColorF(1, 1, 1, 0.55f));
    SYSTEMTIME f1 = t; f1.wDay = 1; FILETIME ft; SystemTimeToFileTime(&f1, &ft); FileTimeToSystemTime(&ft, &f1);
    int first = f1.wDayOfWeek;
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int days = dm[t.wMonth - 1] + (t.wMonth == 2 && ((t.wYear % 4 == 0 && t.wYear % 100) || t.wYear % 400 == 0) ? 1 : 0);
    float rh = (r.bottom - 8 - (r.top + 40)) / 6;
    for (int d = 1; d <= days; d++) {
        int idx = first + d - 1, col = idx % 7, row = idx / 7;
        D2D1_RECT_F cr = D2D1::RectF(gx + col * cw, r.top + 40 + row * rh, gx + (col + 1) * cw, r.top + 40 + (row + 1) * rh);
        if (d == t.wDay) {
            Grad(dc, D2D1::RectF(cr.left + 1, cr.top, cr.right - 1, cr.bottom), 3, { { 0, D2D1::ColorF(0.95f, 0.45f, 0.40f) }, { 1, D2D1::ColorF(0.75f, 0.10f, 0.10f) } });
        }
        Txt(dc, br, std::to_wstring(d), fs, cr, D2D1::ColorF(1, 1, 1, d == t.wDay ? 1.f : 0.85f));
    }
}

// ---- Calculadora
const wchar_t* KEYS[] = { L"mc", L"m+", L"m-", L"mr", L"C", L"±", L"÷", L"×", L"7", L"8", L"9", L"−", L"4", L"5", L"6", L"+", L"1", L"2", L"3", L"=", L"0", L".", };
D2D1_RECT_F KeyRect(D2D1_RECT_F r, int i) {
    float x0 = r.left + 12, y0 = r.top + 70, kw = (r.right - r.left - 24 - 3 * 5) / 4, kh = 20, g = 5;
    if (i < 20) {
        int col = i % 4, row = i / 4;
        D2D1_RECT_F k = D2D1::RectF(x0 + col * (kw + g), y0 + row * (kh + g), x0 + col * (kw + g) + kw, y0 + row * (kh + g) + kh);
        if (i == 19) k.bottom += kh + g;  // "=" alto
        return k;
    }
    float y = y0 + 5 * (kh + g);
    if (i == 20) return D2D1::RectF(x0, y, x0 + 2 * kw + g, y + kh);  // "0" largo
    return D2D1::RectF(x0 + 2 * (kw + g), y, x0 + 2 * (kw + g) + kw, y + kh);
}
void PaintCalc(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r) {
    Shadow(dc, br, r, 16);
    Grad(dc, r, 16, { { 0, D2D1::ColorF(0.40f, 0.40f, 0.42f) }, { 0.5f, D2D1::ColorF(0.22f, 0.22f, 0.23f) }, { 1, D2D1::ColorF(0.12f, 0.12f, 0.13f) } });
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 15.5f, 15.5f), br, 1);
    // o visor de cristal líquido
    D2D1_RECT_F lcd = D2D1::RectF(r.left + 12, r.top + 14, r.right - 12, r.top + 56);
    Grad(dc, lcd, 6, { { 0, D2D1::ColorF(0.62f, 0.67f, 0.60f) }, { 0.25f, D2D1::ColorF(0.80f, 0.85f, 0.77f) }, { 1, D2D1::ColorF(0.86f, 0.90f, 0.83f) } });
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(lcd.left + 0.5f, lcd.top + 0.5f, lcd.right - 0.5f, lcd.bottom - 0.5f), 5.5f, 5.5f), br, 1);
    static IDWriteTextFormat* fl = F(24, false, false), *fk = F(11, true);
    Txt(dc, br, g_disp, fl, D2D1::RectF(lcd.left + 6, lcd.top, lcd.right - 8, lcd.bottom), D2D1::ColorF(0.10f, 0.12f, 0.10f));
    if (g_mem != 0) { static IDWriteTextFormat* fm = F(9, true); Txt(dc, br, L"M", fm, D2D1::RectF(lcd.left + 4, lcd.top + 2, lcd.left + 16, lcd.top + 14), D2D1::ColorF(0.1f, 0.12f, 0.1f, 0.8f)); }
    for (int i = 0; i < 22; i++) {
        D2D1_RECT_F k = KeyRect(r, i);
        bool op = i < 8 || i % 4 == 3 || i == 19;
        bool down = g_press == i;
        Grad(dc, k, 6, { { 0, down ? D2D1::ColorF(0.20f, 0.20f, 0.21f) : op ? D2D1::ColorF(0.55f, 0.55f, 0.57f) : D2D1::ColorF(0.42f, 0.42f, 0.44f) },
                         { 0.5f, down ? D2D1::ColorF(0.12f, 0.12f, 0.13f) : op ? D2D1::ColorF(0.36f, 0.36f, 0.38f) : D2D1::ColorF(0.25f, 0.25f, 0.27f) },
                         { 1, down ? D2D1::ColorF(0.16f, 0.16f, 0.17f) : op ? D2D1::ColorF(0.30f, 0.30f, 0.32f) : D2D1::ColorF(0.18f, 0.18f, 0.19f) } });
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left + 0.5f, k.top + 0.5f, k.right - 0.5f, k.bottom - 0.5f), 5.5f, 5.5f), br, 1);
        Txt(dc, br, KEYS[i], fk, k, D2D1::ColorF(1, 1, 1, 0.95f));
    }
}
std::wstring Fmt(double v) {
    if (!std::isfinite(v)) return L"Erro";
    wchar_t b[64]; swprintf_s(b, L"%.10g", v);
    std::wstring s = b;
    for (auto& ch : s) if (ch == L'.') ch = L',';
    return s;
}
double Val() { std::wstring s = g_disp; for (auto& ch : s) if (ch == L',') ch = L'.'; return _wtof(s.c_str()); }
void Apply() {
    double v = Val();
    switch (g_op) {
    case L'+': g_acc += v; break; case L'−': g_acc -= v; break;
    case L'×': g_acc *= v; break; case L'÷': g_acc = v != 0 ? g_acc / v : NAN; break;
    default: g_acc = v;
    }
    g_disp = Fmt(g_acc);
}
void CalcKey(const std::wstring& k) {
    if (k.size() == 1 && k[0] >= L'0' && k[0] <= L'9') {
        if (g_fresh || g_disp == L"0") g_disp.clear();
        if (g_disp.size() < 14) g_disp += k;
        g_fresh = false;
    } else if (k == L"." || k == L",") {
        if (g_fresh) { g_disp = L"0"; g_fresh = false; }
        if (g_disp.find(L',') == std::wstring::npos) g_disp += L',';
    } else if (k == L"C") { g_disp = L"0"; g_acc = 0; g_op = 0; g_fresh = true; }
    else if (k == L"±") { if (g_disp != L"0") g_disp = g_disp[0] == L'-' ? g_disp.substr(1) : L"-" + g_disp; }
    else if (k == L"=") { Apply(); g_op = 0; g_fresh = true; }
    else if (k == L"mc") g_mem = 0;
    else if (k == L"m+") { g_mem += Val(); g_fresh = true; }
    else if (k == L"m-") { g_mem -= Val(); g_fresh = true; }
    else if (k == L"mr") { g_disp = Fmt(g_mem); g_fresh = true; }
    else if (k == L"+" || k == L"−" || k == L"×" || k == L"÷") { if (!g_fresh || !g_op) Apply(); g_op = k[0]; g_fresh = true; }
}

D2D1_RECT_F WRect(int i) { return D2D1::RectF(g_w[i].x, g_w[i].y, g_w[i].x + g_w[i].w, g_w[i].y + g_w[i].h); }
D2D1_RECT_F PlusRect() { return D2D1::RectF(18, g_H - 52, 52, g_H - 18); }
const float BAR_H = 132;
D2D1_RECT_F BarItem(int i) { float x = 100 + i * 170.f; return D2D1::RectF(x, g_H - BAR_H + 14, x + 150, g_H - 14); }

void LoadPos() {
    std::wstring on = shell::Setting(L"dashboard_on", L"111");
    for (int i = 0; i < W_COUNT; i++) g_w[i].on = i < (int)on.size() ? on[i] == L'1' : true;
    std::wstring p = shell::Setting(L"dashboard_pos", L"");
    float def[W_COUNT][2] = { { 0.22f, 0.26f }, { 0.40f, 0.30f }, { 0.68f, 0.24f } };
    for (int i = 0; i < W_COUNT; i++) { g_w[i].x = g_W * def[i][0]; g_w[i].y = g_H * def[i][1]; }
    size_t s = 0; int i = 0;
    while (i < W_COUNT && s < p.size()) {
        size_t e = p.find(L';', s); if (e == std::wstring::npos) e = p.size();
        float fx = 0, fy = 0;
        if (swscanf_s(p.substr(s, e - s).c_str(), L"%f,%f", &fx, &fy) == 2) { g_w[i].x = fx * g_W; g_w[i].y = fy * g_H; }
        s = e + 1; i++;
    }
    for (auto& w : g_w) { w.x = std::clamp(w.x, 0.f, g_W - w.w); w.y = std::clamp(w.y, 24.f, g_H - w.h); }
}
void SavePos() {
    std::wstring p, on;
    for (int i = 0; i < W_COUNT; i++) {
        wchar_t b[48]; swprintf_s(b, L"%.4f,%.4f;", g_w[i].x / g_W, g_w[i].y / g_H);
        p += b; on += g_w[i].on ? L'1' : L'0';
    }
    shell::SetSetting(L"dashboard_pos", p);
    shell::SetSetting(L"dashboard_on", on);
}

float Progress() {
    float k = (float)std::clamp((anim::Now() - g_at) / 260.0, 0.0, 1.0);
    k = anim::Ease(k);
    return g_closing ? 1 - k : k;
}

void Paint() {
    if (!g_open) return;
    float sc = Sc();
    RECT rc; GetClientRect(g_wnd, &rc);
    auto* dc = ui::GpuBegin(g_s, rc.right, rc.bottom, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    float k = Progress();
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.48f * k));  // a tela escurece por trás
    dc->FillRectangle(D2D1::RectF(0, 0, g_W, g_H), br);
    float lift = g_bar ? BAR_H : 0;
    ID2D1Layer* layer = nullptr; dc->CreateLayer(nullptr, &layer);
    for (int i = 0; i < W_COUNT; i++) {
        if (!g_w[i].on) continue;
        D2D1_RECT_F r = WRect(i);
        float s = 1.12f - 0.12f * k;  // entram crescendo um pouco (e saem do mesmo jeito)
        D2D1_POINT_2F c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * D2D1::Matrix3x2F::Translation(0, -lift * 0.0f));
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), k), nullptr);
        if (i == W_CLOCK) PaintClock(dc, br, r); else if (i == W_CAL) PaintCal(dc, br, r); else PaintCalc(dc, br, r);
        dc->PopLayer();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), k), nullptr);
    if (g_bar) {  // a barra de widgets: faixa de metal escovado embaixo
        D2D1_RECT_F b = D2D1::RectF(0, g_H - BAR_H, g_W, g_H);
        Grad(dc, b, 0, { { 0, D2D1::ColorF(0.62f, 0.62f, 0.64f) }, { 0.08f, D2D1::ColorF(0.50f, 0.50f, 0.52f) }, { 1, D2D1::ColorF(0.30f, 0.30f, 0.32f) } });
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.5f));
        dc->DrawLine({ 0, b.top + 0.5f }, { g_W, b.top + 0.5f }, br, 1);
        static const wchar_t* nomes[] = { L"Relógio", L"Calendário", L"Calculadora" };
        static IDWriteTextFormat* fn = F(11, true);
        for (int i = 0; i < W_COUNT; i++) {
            D2D1_RECT_F it = BarItem(i);
            D2D1_RECT_F mini = D2D1::RectF(it.left + 40, it.top + 4, it.right - 40, it.bottom - 26);
            // miniatura: o próprio widget reduzido
            float ww = g_w[i].w, hh = g_w[i].h, s = std::min((mini.right - mini.left) / ww, (mini.bottom - mini.top) / hh);
            D2D1_RECT_F wr = D2D1::RectF(0, 0, ww, hh);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation((mini.left + mini.right) / 2 - ww * s / 2, (mini.top + mini.bottom) / 2 - hh * s / 2));
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), g_w[i].on ? 0.45f : 1.f), nullptr);
            if (i == W_CLOCK) PaintClock(dc, br, wr); else if (i == W_CAL) PaintCal(dc, br, wr); else PaintCalc(dc, br, wr);
            dc->PopLayer();
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            Txt(dc, br, std::wstring(Tr(nomes[i])) + (g_w[i].on ? L" ✓" : L""), fn, D2D1::RectF(it.left, it.bottom - 22, it.right, it.bottom), D2D1::ColorF(1, 1, 1, 0.95f));
        }
    }
    // o "+" (ou o "×" com a barra aberta) no canto de baixo à esquerda
    D2D1_RECT_F pr = PlusRect();
    if (g_bar) { pr.top -= BAR_H; pr.bottom -= BAR_H; }
    Shadow(dc, br, pr, 17, true);
    Grad(dc, pr, 0, { { 0, D2D1::ColorF(0.55f, 0.55f, 0.57f) }, { 1, D2D1::ColorF(0.20f, 0.20f, 0.21f) } }, true);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
    dc->DrawEllipse(D2D1::Ellipse({ (pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2 }, 16.5f, 16.5f), br, 1.2f);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
    float cx = (pr.left + pr.right) / 2, cy = (pr.top + pr.bottom) / 2;
    if (g_bar) { dc->DrawLine({ cx - 6, cy - 6 }, { cx + 6, cy + 6 }, br, 3); dc->DrawLine({ cx + 6, cy - 6 }, { cx - 6, cy + 6 }, br, 3); }
    else { dc->DrawLine({ cx - 8, cy }, { cx + 8, cy }, br, 3); dc->DrawLine({ cx, cy - 8 }, { cx, cy + 8 }, br, 3); }
    dc->PopLayer();
    layer->Release();
    br->Release();
    ui::GpuEnd(g_s);
}

void HideNow() {
    ui::FrameClock(g_wnd, WM_DB_FRAME, false);
    KillTimer(g_wnd, T_CLOCK);
    ShowWindow(g_wnd, SW_HIDE);
    g_open = g_closing = false;
}
void Close() {
    if (!g_open || g_closing) return;
    SavePos();
    g_closing = true; g_at = anim::Now();
    ui::FrameClock(g_wnd, WM_DB_FRAME, true);
}

int HitWidget(D2D1_POINT_2F p) {
    for (int i = W_COUNT - 1; i >= 0; i--) {
        if (!g_w[i].on) continue;
        D2D1_RECT_F r = WRect(i);
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return i;
    }
    return -1;
}
int HitKey(D2D1_POINT_2F p) {
    if (!g_w[W_CALC].on) return -1;
    for (int i = 0; i < 22; i++) { D2D1_RECT_F k = KeyRect(WRect(W_CALC), i); if (p.x >= k.left && p.x < k.right && p.y >= k.top && p.y < k.bottom) return i; }
    return -1;
}
D2D1_POINT_2F Pt(LPARAM l) { float sc = Sc(); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }
bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_DB_FRAME:
        ui::FrameAck(h, WM_DB_FRAME);
        Paint();
        if (anim::Now() - g_at > 280) { if (g_closing) HideNow(); else ui::FrameClock(h, WM_DB_FRAME, false); }
        return 0;
    case WM_TIMER: if (w == T_CLOCK) Paint(); return 0;
    case WM_LBUTTONDOWN: {
        SetCapture(h);
        D2D1_POINT_2F p = Pt(l); g_downPt = p; g_moved = false;
        D2D1_RECT_F pr = PlusRect(); if (g_bar) { pr.top -= BAR_H; pr.bottom -= BAR_H; }
        if (In(pr, p)) { g_bar = !g_bar; Paint(); return 0; }
        if (g_bar && p.y >= g_H - BAR_H) {
            for (int i = 0; i < W_COUNT; i++) if (In(BarItem(i), p)) { g_w[i].on = !g_w[i].on; SavePos(); Paint(); }
            return 0;
        }
        int k = HitKey(p);
        if (k >= 0) { g_press = k; CalcKey(KEYS[k]); Paint(); return 0; }
        int wi = HitWidget(p);
        if (wi >= 0) { g_drag = wi; g_dragOff = { p.x - g_w[wi].x, p.y - g_w[wi].y }; return 0; }
        Close();  // fora dos widgets: fecha, como no Mac
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag >= 0 && (w & MK_LBUTTON)) {
            D2D1_POINT_2F p = Pt(l);
            g_w[g_drag].x = std::clamp(p.x - g_dragOff.x, 0.f, g_W - g_w[g_drag].w);
            g_w[g_drag].y = std::clamp(p.y - g_dragOff.y, 24.f, g_H - g_w[g_drag].h);
            g_moved = true; Paint();
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (g_press >= 0) { g_press = -1; Paint(); }
        if (g_drag >= 0) { g_drag = -1; if (g_moved) SavePos(); }
        return 0;
    case WM_KEYDOWN: {
        if (w == VK_ESCAPE || w == VK_F12) { Close(); return 0; }
        std::wstring k;
        if (w >= '0' && w <= '9' && GetKeyState(VK_SHIFT) >= 0) k = std::wstring(1, (wchar_t)w);
        else if (w >= VK_NUMPAD0 && w <= VK_NUMPAD9) k = std::wstring(1, (wchar_t)(L'0' + (w - VK_NUMPAD0)));
        else if (w == VK_ADD || (w == VK_OEM_PLUS && GetKeyState(VK_SHIFT) < 0)) k = L"+";
        else if (w == VK_SUBTRACT || w == VK_OEM_MINUS) k = L"−";
        else if (w == VK_MULTIPLY) k = L"×";
        else if (w == VK_DIVIDE || w == VK_OEM_2) k = L"÷";
        else if (w == VK_RETURN || w == VK_OEM_PLUS) k = L"=";
        else if (w == VK_DECIMAL || w == VK_OEM_COMMA || w == VK_OEM_PERIOD) k = L",";
        else if (w == VK_BACK || w == VK_DELETE) k = L"C";
        if (!k.empty() && g_w[W_CALC].on) { CalcKey(k); Paint(); }
        return 0;
    }
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && g_open && !g_closing) Close(); return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_CLOSE: Close(); return 0;  // nunca destrói
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool dashboard::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellDashboard"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Dashboard", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    apps::ours.push_back(g_wnd);
    g_s.hwnd = g_wnd;
    return true;
}

bool dashboard::IsOpen() { return g_open && !g_closing; }

void dashboard::Toggle() {
    if (!g_wnd && !dashboard::Create(shell::inst)) return;
    if (g_open && !g_closing) { Close(); return; }
    POINT c; GetCursorPos(&c);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r = mi.rcMonitor;
    SetWindowPos(g_wnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    float sc = Sc();
    g_W = (r.right - r.left) / sc; g_H = (r.bottom - r.top) / sc;
    LoadPos();
    g_open = true; g_closing = false; g_bar = false; g_at = anim::Now();
    Paint();
    ShowWindow(g_wnd, SW_SHOW);
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    if (att) AttachThreadInput(me, ft, FALSE);
    SetTimer(g_wnd, T_CLOCK, 1000, nullptr);
    ui::FrameClock(g_wnd, WM_DB_FRAME, true);
}
