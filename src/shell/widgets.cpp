// Widgets da mesa, como no macOS Sequoia: Relógio Mundial, Tempo, Relógio, Calendário e Nascer do Sol.
// Ficam logo acima da área de trabalho (abaixo de todas as janelas), arrastáveis; posições em ajustes.txt.
// Medidas da referência (Sequoia, 0,72 px por pt): pequeno 170 × 170 pt, médio 364 × 170 pt, 16 pt entre
// eles, canto de 21 pt. Claro: as cores de cada widget (Tempo em degradê do céu, Calendário branco, Relógio
// Mundial preto, Relógio em vidro). Escuro: todos em vidro desfocado da mesa, com texto e símbolos brancos.
// O tempo vem do Open-Meteo (sem chave), a cada 15 min, para a cidade em ajustes.txt (padrão: a do fuso do Windows, comum/cidade_padrao.h);
// a última resposta fica em %APPDATA%\Shell\tempo.json para aparecer já ao ligar.
#include "shell.h"
#include "../comum/cidade_padrao.h"
#include "../menu/context_menu.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include "../comum/idioma.h"
#pragma comment(lib, "winhttp.lib")

namespace {

const float SMALL = 170, MEDIUM = 356, GAP = 16, RADIUS_SEQ = 21, MARGIN = 16;
inline float WidgetRadius() { return liquid::On() ? 26.f : RADIUS_SEQ; }  // Tahoe: mais redondos
const UINT WM_WX_READY = WM_APP + 60;
const float PI = 3.14159265f;

enum Kind { K_WORLD, K_WEATHER, K_CLOCK, K_CALENDAR, K_SUN };
struct Widget {
    Kind kind; const wchar_t* id; const wchar_t* name; float w, h;
    HWND wnd = nullptr, back = nullptr;
    bool glass = false, on = true;
    int rc = -1, row = -1;  // casa na grade (rc: coluna da borda direita, contando da direita)
    ui::GpuSurface s;
};
std::vector<Widget> g_w;
HWND g_menuOwner;        // dono invisível do menu de contexto (o menu precisa de um dono em primeiro plano)
HWND g_ghost;            // sombra da casa de destino ao arrastar
HWND g_timer;            // janela que recebe os relógios e a chegada do tempo
bool g_dark, g_placing;
int g_lastMinute = -1;

// arrastar
Widget* g_drag; POINT g_dragStart, g_winStart; bool g_dragging;

// ---------------------------------------------------------------- tempo (Open-Meteo)
struct Hour { double t; float temp; int code; bool day; };
struct Weather {
    bool ok = false;
    float temp = 0; int code = 0; bool day = true;
    float hi = 0, lo = 0;
    double sunrise[2]{}, sunset[2]{};
    int utcOffset = -3 * 3600;
    std::vector<Hour> hours;
};
std::mutex g_mx;
Weather g_wx;
std::atomic<bool> g_fetching{ false };

std::wstring City() { static cidade::Padrao def; return shell::Setting(L"tempoCidade", def.nome.c_str()); }  // sem cidade: a do fuso do Windows

std::string Section(const std::string& j, const char* key) {
    size_t p = j.find(std::string("\"") + key + "\":{");
    if (p == std::string::npos) return "";
    p = j.find('{', p);
    int depth = 0;
    for (size_t i = p; i < j.size(); i++) {
        if (j[i] == '{') depth++;
        else if (j[i] == '}' && --depth == 0) return j.substr(p, i - p + 1);
    }
    return "";
}
double Number(const std::string& s, const char* key) {
    size_t p = s.find(std::string("\"") + key + "\":");
    if (p == std::string::npos) return NAN;
    p += strlen(key) + 3;
    while (p < s.size() && s[p] == ' ') p++;
    if (s.compare(p, 4, "null") == 0) return NAN;
    return strtod(s.c_str() + p, nullptr);
}
std::vector<double> Array(const std::string& s, const char* key) {
    std::vector<double> v;
    size_t p = s.find(std::string("\"") + key + "\":[");
    if (p == std::string::npos) return v;
    p = s.find('[', p) + 1;
    while (p < s.size() && s[p] != ']') {
        while (p < s.size() && (s[p] == ' ' || s[p] == ',')) p++;
        if (s[p] == ']') break;
        if (s.compare(p, 4, "null") == 0) { v.push_back(NAN); p += 4; continue; }
        char* e = nullptr;
        v.push_back(strtod(s.c_str() + p, &e));
        if (e == s.c_str() + p) break;
        p = e - s.c_str();
    }
    return v;
}

bool Parse(const std::string& j, Weather& w) {
    std::string cur = Section(j, "current"), hr = Section(j, "hourly"), dl = Section(j, "daily");
    if (cur.empty() || hr.empty() || dl.empty()) return false;
    double off = Number(j, "utc_offset_seconds");
    if (!std::isnan(off)) w.utcOffset = (int)off;
    w.temp = (float)Number(cur, "temperature_2m");
    w.code = (int)Number(cur, "weather_code");
    w.day = Number(cur, "is_day") > 0.5;
    auto t = Array(hr, "time"), tp = Array(hr, "temperature_2m"), c = Array(hr, "weather_code"), d = Array(hr, "is_day");
    w.hours.clear();
    for (size_t i = 0; i < t.size() && i < tp.size() && i < c.size() && i < d.size(); i++)
        w.hours.push_back({ t[i], (float)tp[i], (int)c[i], d[i] > 0.5 });
    auto mx = Array(dl, "temperature_2m_max"), mn = Array(dl, "temperature_2m_min"), sr = Array(dl, "sunrise"), ss = Array(dl, "sunset");
    if (mx.empty() || mn.empty() || sr.size() < 2 || ss.size() < 2 || std::isnan(w.temp)) return false;
    w.hi = (float)mx[0]; w.lo = (float)mn[0];
    for (int i = 0; i < 2; i++) { w.sunrise[i] = sr[i]; w.sunset[i] = ss[i]; }
    w.ok = true;
    return true;
}

std::wstring CachePath() {
    PWSTR p = nullptr; std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { f = std::wstring(p) + L"\\Shell\\tempo.json"; CoTaskMemFree(p); }
    return f;
}

std::string HttpGet(const wchar_t* host, const std::wstring& path) {
    std::string body;
    HINTERNET ses = WinHttpOpen(L"ShellProprio/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return body;
    WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 256 * 1024) {
            std::string chunk(avail, '\0'); DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
        }
        if (status != 200) body.clear();
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return body;
}

void Fetch() {
    if (g_fetching.exchange(true)) return;
    static cidade::Padrao def;
    std::wstring lat = shell::Setting(L"tempoLat", def.lat.c_str()), lon = shell::Setting(L"tempoLon", def.lon.c_str());
    std::thread([lat, lon] {
        std::wstring path = L"/v1/forecast?latitude=" + lat + L"&longitude=" + lon +
            L"&current=temperature_2m,weather_code,is_day&hourly=temperature_2m,weather_code,is_day"
            L"&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset&timezone=auto&forecast_days=3&timeformat=unixtime";
        std::string body = HttpGet(L"api.open-meteo.com", path);
        Weather w;
        if (!body.empty() && Parse(body, w)) {
            { std::lock_guard<std::mutex> lk(g_mx); g_wx = w; }
            FILE* fp = nullptr;
            if (!_wfopen_s(&fp, CachePath().c_str(), L"wb") && fp) { fwrite(body.data(), 1, body.size(), fp); fclose(fp); }
            Log(L"widgets: tempo atualizado (%.1f°, código %d)", w.temp, w.code);
            PostMessageW(g_timer, WM_WX_READY, 0, 0);
        } else Log(L"widgets: não deu para buscar o tempo (%zu bytes)", body.size());
        g_fetching = false;
    }).detach();
}

void LoadCache() {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, CachePath().c_str(), L"rb") || !fp) return;
    std::string body; char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) body.append(buf, n);
    fclose(fp);
    Weather w;
    if (Parse(body, w)) { std::lock_guard<std::mutex> lk(g_mx); g_wx = w; }
}

enum Icon { I_SUN, I_MOON, I_PARTLY, I_PARTLY_NIGHT, I_CLOUD, I_FOG, I_DRIZZLE, I_RAIN, I_SNOW, I_THUNDER };
Icon IconOf(int code, bool day) {
    if (code == 0) return day ? I_SUN : I_MOON;
    if (code <= 2) return day ? I_PARTLY : I_PARTLY_NIGHT;
    if (code == 3) return I_CLOUD;
    if (code == 45 || code == 48) return I_FOG;
    if (code >= 51 && code <= 57) return I_DRIZZLE;
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return I_RAIN;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return I_SNOW;
    if (code >= 95) return I_THUNDER;
    return I_CLOUD;
}
const wchar_t* ConditionOf(int code, bool day) {
    if (code == 0) return day ? Tr(L"Ensolarado") : Tr(L"Céu Limpo");
    if (code == 1) return day ? Tr(L"Predom. Ensolarado") : Tr(L"Predom. Limpo");
    if (code == 2) return Tr(L"Parcialmente Nublado");
    if (code == 3) return Tr(L"Nublado");
    if (code == 45 || code == 48) return Tr(L"Neblina");
    if (code >= 51 && code <= 57) return Tr(L"Garoa");
    if (code == 65 || code == 82) return Tr(L"Chuva Forte");
    if (code == 66 || code == 67) return Tr(L"Chuva Congelada");
    if ((code >= 61 && code <= 63) || code == 80 || code == 81) return Tr(L"Chuva");
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Tr(L"Neve");
    if (code >= 95) return Tr(L"Tempestades");
    return Tr(L"Nublado");
}

// hora local da cidade do tempo, a partir de um instante unix
int LocalMinuteOfDay(double t, int off) { long long s = (long long)t + off; s %= 86400; if (s < 0) s += 86400; return (int)(s / 60); }
std::wstring HourMin(double t, int off) { int m = LocalMinuteOfDay(t, off); wchar_t b[16]; swprintf_s(b, L"%02d:%02d", m / 60, m % 60); return b; }

// ---------------------------------------------------------------- desenho
D2D1_COLOR_F C(float r, float g, float b, float a = 1) { return D2D1::ColorF(r, g, b, a); }
const D2D1_COLOR_F WHITE = { 1, 1, 1, 1 }, ORANGE = { 1, 0.584f, 0, 1 }, RED = { 1, 0.231f, 0.188f, 1 };

void Txt(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, float size, DWRITE_FONT_WEIGHT wgt,
         DWRITE_TEXT_ALIGNMENT al, D2D1_RECT_F r, D2D1_COLOR_F c, bool display = false) {
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(ui::dw->CreateTextFormat(display ? L"SF Pro Display" : ui::family.c_str(), nullptr, wgt, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf)) || !tf) return;
    tf->SetTextAlignment(al);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    br->SetColor(c);
    dc->DrawText(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_NONE);
    tf->Release();
}
float Width(const std::wstring& s, float size, DWRITE_FONT_WEIGHT wgt, bool display = false) {
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(ui::dw->CreateTextFormat(display ? L"SF Pro Display" : ui::family.c_str(), nullptr, wgt, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf)) || !tf) return 0;
    float w = ui::TextWidth(tf, s);
    tf->Release();
    return w;
}

ID2D1StrokeStyle* RoundCaps(ID2D1DeviceContext* dc) {
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1StrokeStyle* st = nullptr;
    if (f) { f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &st); f->Release(); }
    return st;
}

ID2D1Geometry* Combine(ID2D1Factory* f, ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode) {
    ID2D1PathGeometry* p = nullptr;
    if (a && b && SUCCEEDED(f->CreatePathGeometry(&p))) {
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(p->Open(&s))) { a->CombineWithGeometry(b, mode, nullptr, s); s->Close(); s->Release(); }
    }
    if (a) a->Release();
    if (b) b->Release();
    return p;
}
ID2D1Geometry* Circle(ID2D1Factory* f, float x, float y, float r) {
    ID2D1EllipseGeometry* e = nullptr; f->CreateEllipseGeometry(D2D1::Ellipse({ x, y }, r, r), &e); return e;
}
// nuvem do SF Symbols: três bolhas sobre uma base arredondada (s = tamanho do símbolo)
ID2D1Geometry* Cloud(ID2D1Factory* f, float cx, float cy, float s) {
    ID2D1RoundedRectangleGeometry* rr = nullptr;
    f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(cx - 0.40f * s, cy + 0.02f * s, cx + 0.44f * s, cy + 0.25f * s), 0.115f * s, 0.115f * s), &rr);
    ID2D1Geometry* g = Combine(f, Circle(f, cx - 0.19f * s, cy + 0.05f * s, 0.20f * s), Circle(f, cx + 0.04f * s, cy - 0.06f * s, 0.27f * s), D2D1_COMBINE_MODE_UNION);
    g = Combine(f, g, Circle(f, cx + 0.26f * s, cy + 0.08f * s, 0.17f * s), D2D1_COMBINE_MODE_UNION);
    return Combine(f, g, rr, D2D1_COMBINE_MODE_UNION);
}
ID2D1Geometry* Moon(ID2D1Factory* f, float cx, float cy, float s) {
    return Combine(f, Circle(f, cx, cy, 0.32f * s), Circle(f, cx + 0.19f * s, cy - 0.13f * s, 0.27f * s), D2D1_COMBINE_MODE_EXCLUDE);
}

void Sun(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ID2D1StrokeStyle* st, float cx, float cy, float s, D2D1_COLOR_F c) {
    br->SetColor(c);
    dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 0.2f * s, 0.2f * s), br);
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4, dx = std::sin(a), dy = -std::cos(a);
        dc->DrawLine({ cx + dx * 0.31f * s, cy + dy * 0.31f * s }, { cx + dx * 0.44f * s, cy + dy * 0.44f * s }, br, 0.075f * s, st);
    }
}

// símbolos do tempo no estilo multicolorido do SF Symbols (mono: tudo branco, para o vidro do modo escuro)
void DrawIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, Icon ic, float cx, float cy, float s, bool mono) {
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    if (!f) return;
    ID2D1StrokeStyle* st = RoundCaps(dc);
    D2D1_COLOR_F yellow = mono ? WHITE : C(1, 0.84f, 0.04f), drop = mono ? WHITE : C(0.39f, 0.78f, 1), cloud = WHITE;
    auto fill = [&](ID2D1Geometry* g, D2D1_COLOR_F c) { if (g) { br->SetColor(c); dc->FillGeometry(g, br); g->Release(); } };
    float ccy = cy - 0.12f * s;  // nuvem mais alta quando tem algo caindo embaixo
    switch (ic) {
    case I_SUN: Sun(dc, br, st, cx, cy, s, yellow); break;
    case I_MOON: fill(Moon(f, cx, cy, s), WHITE); break;
    case I_PARTLY:
        Sun(dc, br, st, cx - 0.14f * s, cy - 0.14f * s, 0.78f * s, yellow);
        fill(Cloud(f, cx + 0.08f * s, cy + 0.06f * s, 0.82f * s), cloud);
        break;
    case I_PARTLY_NIGHT:
        fill(Moon(f, cx - 0.14f * s, cy - 0.16f * s, 0.72f * s), WHITE);
        fill(Cloud(f, cx + 0.08f * s, cy + 0.06f * s, 0.82f * s), cloud);
        break;
    case I_CLOUD: fill(Cloud(f, cx, cy, s), cloud); break;
    case I_FOG:
        fill(Cloud(f, cx, cy - 0.1f * s, 0.88f * s), cloud);
        br->SetColor(C(1, 1, 1, mono ? 1 : 0.85f));
        dc->DrawLine({ cx - 0.36f * s, cy + 0.28f * s }, { cx + 0.36f * s, cy + 0.28f * s }, br, 0.07f * s, st);
        dc->DrawLine({ cx - 0.26f * s, cy + 0.40f * s }, { cx + 0.30f * s, cy + 0.40f * s }, br, 0.07f * s, st);
        break;
    case I_DRIZZLE: case I_RAIN: case I_SNOW: case I_THUNDER: {
        fill(Cloud(f, cx, ccy, 0.92f * s), cloud);
        br->SetColor(ic == I_SNOW ? WHITE : drop);
        if (ic == I_THUNDER) {
            ID2D1PathGeometry* p = nullptr;
            if (SUCCEEDED(f->CreatePathGeometry(&p))) {
                ID2D1GeometrySink* sk = nullptr; p->Open(&sk);
                D2D1_POINT_2F pts[] = { { cx + 0.02f * s, cy + 0.12f * s }, { cx - 0.12f * s, cy + 0.33f * s }, { cx - 0.01f * s, cy + 0.33f * s },
                                        { cx - 0.06f * s, cy + 0.50f * s }, { cx + 0.12f * s, cy + 0.26f * s }, { cx + 0.01f * s, cy + 0.26f * s } };
                sk->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED); sk->AddLines(pts + 1, 5); sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close(); sk->Release();
                br->SetColor(yellow); dc->FillGeometry(p, br); p->Release();
            }
            break;
        }
        for (int i = -1; i <= 1; i++) {
            float x = cx + i * 0.19f * s, y = cy + 0.22f * s;
            if (ic == I_RAIN) dc->DrawLine({ x + 0.04f * s, y }, { x - 0.04f * s, y + 0.2f * s }, br, 0.075f * s, st);
            else if (ic == I_DRIZZLE) { dc->FillEllipse(D2D1::Ellipse({ x, y + 0.06f * s }, 0.045f * s, 0.045f * s), br); dc->FillEllipse(D2D1::Ellipse({ x - 0.04f * s, y + 0.2f * s }, 0.045f * s, 0.045f * s), br); }
            else dc->FillEllipse(D2D1::Ellipse({ x, y + 0.12f * s + (i == 0 ? 0.06f * s : 0) }, 0.06f * s, 0.06f * s), br);
        }
        break;
    }
    }
    if (st) st->Release();
    f->Release();
}

ID2D1LinearGradientBrush* Gradient(ID2D1DeviceContext* dc, float y0, float y1, D2D1_COLOR_F a, D2D1_COLOR_F b) {
    D2D1_GRADIENT_STOP stops[] = { { 0, a }, { 1, b } };
    ID2D1GradientStopCollection* sc = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    dc->CreateGradientStopCollection(stops, 2, &sc);
    if (sc) { dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, y0 }, { 0, y1 }), sc, &lb); sc->Release(); }
    return lb;
}
// céu do app Tempo: azul de dia, noite escura, cinza com nuvem ou chuva
void Sky(ID2D1DeviceContext* dc, float w, float h, int code, bool day) {
    D2D1_COLOR_F a, b;
    bool rain = code >= 51, cloudy = code == 3 || code == 45 || code == 48;
    if (day) {
        if (rain) { a = C(0.29f, 0.35f, 0.43f); b = C(0.45f, 0.51f, 0.59f); }
        else if (cloudy) { a = C(0.40f, 0.49f, 0.60f); b = C(0.58f, 0.66f, 0.75f); }
        else { a = C(0.16f, 0.40f, 0.74f); b = C(0.36f, 0.60f, 0.88f); }
    } else {
        if (rain || cloudy) { a = C(0.12f, 0.14f, 0.19f); b = C(0.24f, 0.27f, 0.33f); }
        else { a = C(0.03f, 0.06f, 0.16f); b = C(0.12f, 0.19f, 0.35f); }
    }
    if (auto* lb = Gradient(dc, 0, h, a, b)) { dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), WidgetRadius(), WidgetRadius()), lb); lb->Release(); }
}

// mostrador analógico (relógio e relógio mundial)
// Tahoe, "Estilo de ícones e widgets" Transparente ou Tingido (macOS 26): o widget vira vidro claro e tudo por cima
// fica branco — os mostradores também (anel translúcido, ponteiros e números brancos)
bool g_vidroWidgets;
void Face(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ID2D1StrokeStyle* st, float cx, float cy, float R,
          D2D1_COLOR_F face, D2D1_COLOR_F ink, float numSize, bool ticks, const SYSTEMTIME& t) {
    if (g_vidroWidgets) { face = C(1, 1, 1, 0.14f); ink = WHITE; }
    br->SetColor(face);
    dc->FillEllipse(D2D1::Ellipse({ cx, cy }, R, R), br);
    if (ticks) {
        D2D1_COLOR_F tc = ink; tc.a *= 0.35f; br->SetColor(tc);
        for (int i = 0; i < 60; i++) {
            if (i % 5 == 0) continue;
            float a = i * PI / 30, dx = std::sin(a), dy = -std::cos(a);
            dc->DrawLine({ cx + dx * R * 0.93f, cy + dy * R * 0.93f }, { cx + dx * R * 0.97f, cy + dy * R * 0.97f }, br, std::max(0.8f, R * 0.012f));
        }
    }
    float nr = R * (ticks ? 0.79f : 0.74f);
    for (int i = 1; i <= 12; i++) {
        float a = i * PI / 6, x = cx + std::sin(a) * nr, y = cy - std::cos(a) * nr;
        Txt(dc, br, std::to_wstring(i), numSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
            D2D1::RectF(x - numSize, y - numSize * 0.75f, x + numSize, y + numSize * 0.75f), ink);
    }
    float sec = t.wSecond + t.wMilliseconds / 1000.f;
    float mn = t.wMinute + sec / 60, hr = (t.wHour % 12) + mn / 60;
    auto hand = [&](float frac, float from, float to, float width) {
        float a = frac * 2 * PI, dx = std::sin(a), dy = -std::cos(a);
        dc->DrawLine({ cx + dx * from, cy + dy * from }, { cx + dx * to, cy + dy * to }, br, width, st);
    };
    br->SetColor(ink);
    hand(hr / 12, 0, R * 0.14f, std::max(1.2f, R * 0.028f));
    hand(hr / 12, R * 0.14f, R * 0.50f, R * 0.075f);
    hand(mn / 60, 0, R * 0.14f, std::max(1.2f, R * 0.028f));
    hand(mn / 60, R * 0.14f, R * 0.84f, R * 0.075f);
    dc->FillEllipse(D2D1::Ellipse({ cx, cy }, R * 0.06f, R * 0.06f), br);
    br->SetColor(ORANGE);
    hand(sec / 60, -R * 0.16f, R * 0.90f, std::max(1.f, R * 0.018f));
    dc->FillEllipse(D2D1::Ellipse({ cx, cy }, R * 0.045f, R * 0.045f), br);
    br->SetColor(face);
    dc->FillEllipse(D2D1::Ellipse({ cx, cy }, R * 0.017f, R * 0.017f), br);
}

// ---------------------------------------------------------------- relógio mundial
struct CityTz { std::wstring name, tz; };
std::vector<CityTz> Cities() {
    // ajustes.txt: relogioCidades=Cupertino|Pacific Standard Time;Tóquio|Tokyo Standard Time;...
    std::wstring s = shell::Setting(L"relogioCidades",
        L"Cupertino|Pacific Standard Time;Tóquio|Tokyo Standard Time;Sydney|AUS Eastern Standard Time;Paris|Romance Standard Time");
    std::vector<CityTz> v;
    size_t p = 0;
    while (p < s.size() && v.size() < 4) {
        size_t e = s.find(L';', p); if (e == std::wstring::npos) e = s.size();
        std::wstring item = s.substr(p, e - p);
        size_t bar = item.find(L'|');
        if (bar != std::wstring::npos) v.push_back({ item.substr(0, bar), item.substr(bar + 1) });
        p = e + 1;
    }
    return v;
}
bool TimeIn(const std::wstring& key, const SYSTEMTIME& utc, SYSTEMTIME& out) {
    static std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION> cache;
    auto it = cache.find(key);
    if (it == cache.end()) {
        DYNAMIC_TIME_ZONE_INFORMATION d{};
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; i++)
            if (!_wcsicmp(d.TimeZoneKeyName, key.c_str())) { it = cache.emplace(key, d).first; break; }
        if (it == cache.end()) return false;
    }
    return SystemTimeToTzSpecificLocalTimeEx(&it->second, &utc, &out) != FALSE;
}
long long Minutes(const SYSTEMTIME& t) { FILETIME f; SystemTimeToFileTime(&t, &f); return (long long)(((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) / 600000000ULL); }

void PaintWorld(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ID2D1StrokeStyle* st, float w, float h, bool dark) {
    if (!dark) { br->SetColor(C(0.11f, 0.11f, 0.118f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), WidgetRadius(), WidgetRadius()), br); }
    SYSTEMTIME utc, local; GetSystemTime(&utc); GetLocalTime(&local);
    long long lm = Minutes(local);
    auto cities = Cities();
    float col = w / 4;
    for (size_t i = 0; i < cities.size(); i++) {
        SYSTEMTIME t;
        if (!TimeIn(cities[i].tz, utc, t)) continue;
        t.wMilliseconds = local.wMilliseconds;
        float cx = col * (i + 0.5f);
        bool night = t.wHour < 6 || t.wHour >= 18;
        D2D1_COLOR_F face, ink;
        if (dark) { face = night ? C(0, 0, 0, 0.30f) : C(1, 1, 1, 0.88f); ink = night ? WHITE : C(0, 0, 0); }
        else { face = night ? C(0.23f, 0.23f, 0.24f) : WHITE; ink = night ? WHITE : C(0, 0, 0); }
        Face(dc, br, st, cx, 60, 33, face, ink, 8.5f, false, t);
        Txt(dc, br, Tr(cities[i].name), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(cx - col / 2, 99, cx + col / 2, 117), WHITE);
        long long diff = Minutes(t) - lm;  // em minutos
        int dayDiff = (int)((t.wYear * 10000 + t.wMonth * 100 + t.wDay) > (local.wYear * 10000 + local.wMonth * 100 + local.wDay)) -
                      (int)((t.wYear * 10000 + t.wMonth * 100 + t.wDay) < (local.wYear * 10000 + local.wMonth * 100 + local.wDay));
        D2D1_COLOR_F sub = C(1, 1, 1, 0.55f);
        Txt(dc, br, dayDiff > 0 ? Tr(L"Amanhã") : dayDiff < 0 ? Tr(L"Ontem") : Tr(L"Hoje"), 10, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER,
            D2D1::RectF(cx - col / 2, 117, cx + col / 2, 132), sub);
        long long ad = diff < 0 ? -diff : diff;
        wchar_t b[24];
        if (ad % 60) swprintf_s(b, L"%ls%lld:%02lld H", diff < 0 ? L"−" : L"+", ad / 60, ad % 60);
        else swprintf_s(b, L"%ls%lld H", diff < 0 ? L"−" : L"+", ad / 60);
        Txt(dc, br, b, 10, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(cx - col / 2, 131, cx + col / 2, 146), sub);
    }
}

// ---------------------------------------------------------------- tempo
void PaintWeather(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float w, float h, bool dark) {
    Weather wx; { std::lock_guard<std::mutex> lk(g_mx); wx = g_wx; }
    if (!dark) Sky(dc, w, h, wx.ok ? wx.code : 0, wx.ok ? wx.day : true);
    std::wstring city = City();
    Txt(dc, br, city, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D2D1::RectF(17, 12, 250, 32), WHITE);
    {   // seta de localização (location.fill)
        float x = 17 + Width(city, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 5, y = 22;
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        ID2D1PathGeometry* p = nullptr;
        if (f && SUCCEEDED(f->CreatePathGeometry(&p))) {
            ID2D1GeometrySink* sk = nullptr; p->Open(&sk);
            D2D1_POINT_2F pts[] = { { x + 9, y - 5 }, { x + 5, y + 5 }, { x + 4, y + 1 }, { x, y } };
            sk->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED); sk->AddLines(pts + 1, 3); sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close(); sk->Release();
            br->SetColor(WHITE); dc->FillGeometry(p, br); p->Release();
        }
        if (f) f->Release();
    }
    wchar_t b[64];
    if (wx.ok) swprintf_s(b, L"%ld°", lround(wx.temp)); else wcscpy_s(b, L"--°");
    Txt(dc, br, b, 44, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D2D1::RectF(14, 32, 200, 86), WHITE, true);
    if (!wx.ok) {
        Txt(dc, br, Tr(L"Buscando o tempo…"), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(0, 110, w, 140), C(1, 1, 1, 0.7f));
        return;
    }
    DrawIcon(dc, br, IconOf(wx.code, wx.day), w - 26, 24, 22, dark);
    Txt(dc, br, ConditionOf(wx.code, wx.day), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, D2D1::RectF(120, 38, w - 17, 56), WHITE);
    swprintf_s(b, Tr(L"Máx.: %ld° Mín.: %ld°"), lround(wx.hi), lround(wx.lo));
    Txt(dc, br, b, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, D2D1::RectF(120, 57, w - 17, 75), WHITE);
    // próximas 6 horas
    double now = (double)time(nullptr);
    size_t first = 0;
    while (first < wx.hours.size() && wx.hours[first].t <= now) first++;
    float pitch = (w - 34) / 6;
    for (int i = 0; i < 6 && first + i < wx.hours.size(); i++) {
        auto& hr = wx.hours[first + i];
        float cx = 17 + pitch * (i + 0.5f);
        swprintf_s(b, L"%dh", LocalMinuteOfDay(hr.t, wx.utcOffset) / 60);
        Txt(dc, br, b, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(cx - 30, 92, cx + 30, 108), C(1, 1, 1, 0.75f));
        DrawIcon(dc, br, IconOf(hr.code, hr.day), cx, 125, 21, dark);
        swprintf_s(b, L"%ld°", lround(hr.temp));
        Txt(dc, br, b, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(cx - 30, 144, cx + 30, 162), WHITE);
    }
}

// ---------------------------------------------------------------- relógio
void PaintClock(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ID2D1StrokeStyle* st, float w, float h, bool dark) {
    SYSTEMTIME t; GetLocalTime(&t);
    Face(dc, br, st, w / 2, h / 2, 75, dark ? C(0.07f, 0.07f, 0.08f, 0.92f) : WHITE, dark ? WHITE : C(0, 0, 0), 20, true, t);
}

// ---------------------------------------------------------------- calendário
void PaintCalendar(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float w, float h, bool dark) {
    if (!dark) { br->SetColor(WHITE); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), WidgetRadius(), WidgetRadius()), br); }
    static const wchar_t* months[] = { L"JANEIRO", L"FEVEREIRO", L"MARÇO", L"ABRIL", L"MAIO", L"JUNHO", L"JULHO", L"AGOSTO", L"SETEMBRO", L"OUTUBRO", L"NOVEMBRO", L"DEZEMBRO" };
    SYSTEMTIME t; GetLocalTime(&t);
    Txt(dc, br, Tr(months[t.wMonth - 1]), 11, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, D2D1::RectF(16, 16, w, 33), RED);
    SYSTEMTIME f = t; f.wDay = 1; f.wHour = 12; FILETIME ft; SystemTimeToFileTime(&f, &ft); FileTimeToSystemTime(&ft, &f);
    int startDow = f.wDayOfWeek;
    static const int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int days = dim[t.wMonth - 1] + (t.wMonth == 2 && ((t.wYear % 4 == 0 && t.wYear % 100) || t.wYear % 400 == 0));
    int rows = (startDow + days + 6) / 7;
    D2D1_COLOR_F ink = dark ? WHITE : C(0, 0, 0), weekend = dark ? C(1, 1, 1, 0.45f) : C(0, 0, 0, 0.40f);
    std::wstring heads = idioma::En() ? L"SMTWTFS" : idioma::Pt() ? L"DSTQQSS" : L"";
    if (heads.empty())  // os outros idiomas: a inicial do nome mais curto do Windows ("D L M M J V S", "日月火水木金土")
        for (int c = 0; c < 7; c++) { std::wstring n = idioma::Cap(idioma::DayName(c, 2)); heads += n.empty() ? L'?' : n[0]; }
    float pitchX = 21.2f, x0 = 21, top = 44, pitchY = rows > 5 ? 17 : 19.8f;
    for (int c = 0; c < 7; c++) {
        wchar_t s[2] = { heads[c], 0 };
        Txt(dc, br, s, 10, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(x0 + c * pitchX - 10, top - 8, x0 + c * pitchX + 10, top + 8),
            dark ? C(1, 1, 1, 0.55f) : C(0, 0, 0, 0.45f));
    }
    for (int d = 1; d <= days; d++) {
        int cell = startDow + d - 1, c = cell % 7, r = cell / 7;
        float cx = x0 + c * pitchX, cy = top + 20 + r * pitchY;
        bool today = d == t.wDay;
        if (today) { br->SetColor(RED); dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 9, 9), br); }
        Txt(dc, br, std::to_wstring(d), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(cx - 11, cy - 8, cx + 11, cy + 8),
            today ? WHITE : (c == 0 || c == 6) ? weekend : ink);
    }
}

// ---------------------------------------------------------------- nascer e pôr do sol
void PaintSun(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ID2D1StrokeStyle* st, float w, float h, bool dark) {
    Weather wx; { std::lock_guard<std::mutex> lk(g_mx); wx = g_wx; }
    if (!dark) Sky(dc, w, h, 0, wx.ok ? wx.day : true);
    double now = (double)time(nullptr);
    bool toSunset = false; double ev = 0, other = 0;
    if (wx.ok) {
        if (now < wx.sunrise[0]) { ev = wx.sunrise[0]; other = wx.sunset[0]; }
        else if (now < wx.sunset[0]) { toSunset = true; ev = wx.sunset[0]; other = wx.sunrise[1]; }
        else { ev = wx.sunrise[1]; other = wx.sunset[1]; }
    }
    // cabeçalho
    float hx = 16;
    Sun(dc, br, st, hx + 6, 22, 13, WHITE);
    Txt(dc, br, toSunset ? Tr(L"PÔR DO SOL") : Tr(L"NASCER DO SOL"), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
        D2D1::RectF(hx + 16, 14, w, 30), C(1, 1, 1, 0.92f));
    Txt(dc, br, wx.ok ? HourMin(ev, wx.utcOffset) : L"--:--", 30, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
        D2D1::RectF(hx - 1, 32, w, 68), WHITE, true);
    // curva do dia: meia-noite embaixo, meio-dia em cima; a linha do horizonte passa no nascer do sol
    float cy = 104, A = 18;
    auto yOf = [&](float u) { return cy + A * std::cos(2 * PI * u); };
    float horizon = wx.ok ? yOf(LocalMinuteOfDay(wx.sunrise[0], wx.utcOffset) / 1440.f) : cy;
    br->SetColor(C(1, 1, 1, 0.35f));
    dc->DrawLine({ 0, horizon }, { w, horizon }, br, 1);
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    for (int pass = 0; pass < 2 && f; pass++) {  // abaixo do horizonte apagada, acima clara
        ID2D1PathGeometry* p = nullptr;
        if (FAILED(f->CreatePathGeometry(&p))) break;
        ID2D1GeometrySink* sk = nullptr; p->Open(&sk);
        sk->BeginFigure({ 0, yOf(0) }, D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= 96; i++) sk->AddLine({ w * i / 96.f, yOf(i / 96.f) });
        sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->Close(); sk->Release();
        D2D1_RECT_F clip = pass == 0 ? D2D1::RectF(0, horizon, w, h) : D2D1::RectF(0, 0, w, horizon);
        dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        br->SetColor(C(1, 1, 1, pass == 0 ? 0.30f : 0.85f));
        dc->DrawGeometry(p, br, 2, st);
        dc->PopAxisAlignedClip();
        p->Release();
    }
    if (f) f->Release();
    if (wx.ok) {
        float u = LocalMinuteOfDay(now, wx.utcOffset) / 1440.f, x = w * u, y = yOf(u);
        br->SetColor(C(1, 1, 1, 0.25f)); dc->FillEllipse(D2D1::Ellipse({ x, y }, 8, 8), br);
        br->SetColor(WHITE); dc->FillEllipse(D2D1::Ellipse({ x, y }, 5, 5), br);
        std::wstring bottom = std::wstring(toSunset ? Tr(L"Nascer do sol: ") : Tr(L"Pôr do sol: ")) + HourMin(other, wx.utcOffset);
        Txt(dc, br, bottom, 11, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, D2D1::RectF(hx, 138, w, 156), C(1, 1, 1, 0.85f));
    }
}

// ---------------------------------------------------------------- janelas
void Paint(Widget& wd) {
    if (!wd.on || !wd.wnd) return;
    float sc = ui::Scale(wd.wnd);
    int wpx = (int)std::lround(wd.w * sc), hpx = (int)std::lround(wd.h * sc);
    auto* dc = ui::GpuBegin(wd.s, wpx, hpx, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(WHITE, &br);
    ID2D1StrokeStyle* st = RoundCaps(dc);
    std::wstring est = ui::IconStyle();  // Tahoe/Golden Gate: Aparência > Estilo de ícones e widgets (Escuro "Auto" incluído)
    g_vidroWidgets = est == L"transparente" || est == L"tingido";
    bool dark = g_dark || g_vidroWidgets;  // vidro: o conteúdo em branco, como no modo escuro
    if (br) {
        switch (wd.kind) {
        case K_WORLD: PaintWorld(dc, br, st, wd.w, wd.h, dark); break;
        case K_WEATHER: PaintWeather(dc, br, wd.w, wd.h, dark); break;
        case K_CLOCK: PaintClock(dc, br, st, wd.w, wd.h, dark); break;
        case K_CALENDAR: PaintCalendar(dc, br, wd.w, wd.h, dark); break;
        case K_SUN: PaintSun(dc, br, st, wd.w, wd.h, dark); break;
        }
        if (liquid::On()) liquid::Rim(dc, D2D1::RectF(0, 0, wd.w, wd.h), WidgetRadius(), dark, 0.9f);  // Tahoe: contorno especular
        else if (dark) {  // borda clarinha do vidro
            br->SetColor(C(1, 1, 1, 0.10f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, wd.w - 0.5f, wd.h - 0.5f), WidgetRadius() - 0.5f, WidgetRadius() - 0.5f), br, 1);
        }
        br->Release();
    }
    if (st) st->Release();
    ui::GpuEnd(wd.s);
    D2D1_COLOR_F tint = dark ? C(0.10f, 0.10f, 0.11f, 0.38f)
                      : wd.kind == K_CLOCK ? C(0.96f, 0.95f, 1.0f, 0.30f) : C(1, 1, 1, 0);
    if (g_vidroWidgets) {  // vidro claro (Transparente) ou na cor de destaque (Tingido), o papel de parede aparece
        if (est == L"tingido") { D2D1_COLOR_F a = ui::Accent(); tint = C(a.r, a.g, a.b, 0.42f); }
        else tint = C(1, 1, 1, g_dark ? 0.12f : 0.22f);
    }
    if (wd.glass) backdrop::Set(wd.back, (float)wpx, (float)hpx, WidgetRadius() * sc, tint);
}

void PaintAll() { for (auto& w : g_w) Paint(w); }

// a janela mais alta da área de trabalho (Progman / WorkerW): os widgets ficam logo acima dela
HWND DesktopTop() {
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        wchar_t c[32] = {}; GetClassNameW(w, c, 32);
        if (mesa::IsDeskClass(c) && IsWindowVisible(w)) return w;
    }
    return nullptr;
}
bool IsOurs(HWND h) { if (h && h == g_ghost) return true; for (auto& w : g_w) if (w.wnd == h || w.back == h) return true; return false; }

void PlaceAtDesktop(Widget& wd) {
    HWND desk = DesktopTop();
    HWND above = desk ? GetWindow(desk, GW_HWNDPREV) : nullptr;
    while (above && IsOurs(above)) above = GetWindow(above, GW_HWNDPREV);  // acima dos outros widgets também
    g_placing = true;
    SetWindowPos(wd.wnd, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(wd.back, wd.wnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_placing = false;
}
// alguém (o Explorer, "mostrar a mesa") pôs a mesa por cima dos widgets?
bool BelowDesktop(const Widget& wd) {
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == wd.wnd) return false;
        wchar_t c[32] = {}; GetClassNameW(w, c, 32);
        if (mesa::IsDeskClass(c) && IsWindowVisible(w)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- grade
// Como no Mac: os widgets se encaixam em casas de 170 pt com 16 pt entre elas, contadas a partir do canto
// superior direito da tela (o médio ocupa duas casas). Arrastando, uma sombra mostra a casa livre mais perto
// e desliza até ela; ao soltar, o widget vai para a casa com a mola do sistema.
const float PITCH = SMALL + GAP;
const float SPRING_RESPONSE = 0.40f, SPRING_DAMPING = 0.86f;
const UINT WM_WG_FRAME = WM_APP + 61;

struct Grid { float left, top, right, bottom, sc; };
Grid GridNow(HWND h) {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    float sc = ui::Scale(h);
    RECT r = shell::WorkAreaFor(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));  // a do Shell (issue #1)
    return { r.left / sc, r.top / sc, r.right / sc, r.bottom / sc, sc };
}
int Cols(const Widget& w) { return w.w > SMALL + 1 ? 2 : 1; }
int MaxRc(const Grid& g, const Widget& w) { return std::max(0, (int)std::floor((g.right - g.left - 2 * MARGIN - w.w) / PITCH)); }
int MaxRow(const Grid& g, const Widget& w) { return std::max(0, (int)std::floor((g.bottom - g.top - 2 * MARGIN - w.h) / PITCH)); }
POINT CellPx(const Grid& g, const Widget& w, int rc, int row) {  // rc: casa da borda direita do widget, contando da direita
    float x = g.right - MARGIN - w.w - rc * PITCH, y = g.top + MARGIN + row * PITCH;
    return { (LONG)std::lround(x * g.sc), (LONG)std::lround(y * g.sc) };
}
bool Overlaps(const Widget& w, int rc, int row) {
    int a0 = rc, a1 = rc + Cols(w) - 1;
    for (auto& o : g_w) {
        if (&o == &w || !o.on || o.rc < 0 || o.row != row) continue;
        int b0 = o.rc, b1 = o.rc + Cols(o) - 1;
        if (a0 <= b1 && b0 <= a1) return true;
    }
    return false;
}
// a casa livre mais perto de px (canto superior esquerdo do widget, em px)
void Nearest(const Grid& g, const Widget& w, POINT px, int& rc, int& row) {
    double best = 1e300; rc = 0; row = 0;
    for (int r = 0; r <= MaxRow(g, w); r++)
        for (int c = 0; c <= MaxRc(g, w); c++) {
            if (Overlaps(w, c, r)) continue;
            POINT p = CellPx(g, w, c, r);
            double dx = p.x - px.x, dy = p.y - px.y, d = dx * dx + dy * dy;
            if (d < best) { best = d; rc = c; row = r; }
        }
}
void DefaultCell(const Widget& wd, int& rc, int& row) {
    switch (wd.kind) {
    case K_WEATHER: rc = 0; row = 0; break;
    case K_WORLD: rc = 2; row = 0; break;
    case K_CLOCK: rc = 1; row = 1; break;
    case K_CALENDAR: rc = 0; row = 1; break;
    case K_SUN: rc = 2; row = 1; break;
    }
}

void MoveWin(Widget& w, int x, int y) {
    g_placing = true;
    HDWP d = BeginDeferWindowPos(2);
    if (d) d = DeferWindowPos(d, w.back, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (d) d = DeferWindowPos(d, w.wnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (d) EndDeferWindowPos(d);
    g_placing = false;
}

// animações de encaixe
struct Move { Widget* w; POINT from, to; double t0; };
std::vector<Move> g_moves;
// sombra da casa de destino enquanto arrasta
ui::Surface g_ghostS;
double g_gx, g_gy, g_lastFrame; POINT g_ghostTo; int g_targetRc = -1, g_targetRow = -1;

void Animate(bool on) { ui::FrameClock(g_timer, WM_WG_FRAME, on); g_lastFrame = anim::Now(); }

void AnimateTo(Widget& w, POINT to) {
    RECT r; GetWindowRect(w.wnd, &r);
    g_moves.erase(std::remove_if(g_moves.begin(), g_moves.end(), [&](const Move& m) { return m.w == &w; }), g_moves.end());
    if (r.left == to.x && r.top == to.y) return;
    g_moves.push_back({ &w, { r.left, r.top }, to, anim::Now() });
    Animate(true);
}

void PaintGhost(const Widget& w) {
    float sc = ui::Scale(w.wnd);
    int wpx = (int)std::lround(w.w * sc), hpx = (int)std::lround(w.h * sc);
    auto* rt = ui::Begin(g_ghostS, wpx, hpx, sc);
    if (!rt) return;
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(C(1, 1, 1, g_dark ? 0.10f : 0.22f), &br);
    if (br) {
        auto rr = D2D1::RoundedRect(D2D1::RectF(1, 1, w.w - 1, w.h - 1), WidgetRadius() - 1, WidgetRadius() - 1);
        rt->FillRoundedRectangle(rr, br);
        br->SetColor(C(1, 1, 1, g_dark ? 0.30f : 0.55f));
        rt->DrawRoundedRectangle(rr, br, 1.5f);
        br->Release();
    }
    ui::End(g_ghostS, true, { (LONG)g_gx, (LONG)g_gy });
}

void Frame() {
    double now = anim::Now();
    static float dur = anim::SpringDuration(SPRING_RESPONSE, SPRING_DAMPING);
    for (size_t i = 0; i < g_moves.size();) {
        auto& m = g_moves[i];
        float t = (float)((now - m.t0) / 1000.0);
        if (t >= dur) { MoveWin(*m.w, m.to.x, m.to.y); g_moves.erase(g_moves.begin() + i); continue; }
        float p = anim::Spring(t, SPRING_RESPONSE, SPRING_DAMPING);
        MoveWin(*m.w, (int)std::lround(m.from.x + (m.to.x - m.from.x) * p), (int)std::lround(m.from.y + (m.to.y - m.from.y) * p));
        i++;
    }
    if (g_ghost && IsWindowVisible(g_ghost)) {  // a sombra segue a casa de destino, suave (sem degrau)
        double dt = std::min(50.0, now - g_lastFrame), k = 1 - std::exp(-dt / 55.0);
        g_gx += (g_ghostTo.x - g_gx) * k; g_gy += (g_ghostTo.y - g_gy) * k;
        SetWindowPos(g_ghost, nullptr, (int)std::lround(g_gx), (int)std::lround(g_gy), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (!g_dragging && std::fabs(g_ghostTo.x - g_gx) < 0.5 && std::fabs(g_ghostTo.y - g_gy) < 0.5 && g_moves.empty()) ShowWindow(g_ghost, SW_HIDE);
    }
    g_lastFrame = now;
    if (g_moves.empty() && !g_dragging && !(g_ghost && IsWindowVisible(g_ghost))) Animate(false);
}

void SaveCell(const Widget& w) {
    wchar_t b[32]; swprintf_s(b, L"g:%d,%d", w.rc, w.row);
    shell::SetSetting((std::wstring(L"widget.") + w.id).c_str(), b);
}

void Position(Widget& wd, bool reset, bool animate = false) {
    Grid g = GridNow(wd.wnd);
    int rc = -1, row = -1, a, b;
    std::wstring saved = reset ? L"" : shell::Setting((std::wstring(L"widget.") + wd.id).c_str(), L"");
    wd.rc = -1;
    if (!saved.empty() && swscanf_s(saved.c_str(), L"g:%d,%d", &a, &b) == 2) { rc = a; row = b; }
    else if (!saved.empty() && swscanf_s(saved.c_str(), L"%d,%d", &a, &b) == 2) Nearest(g, wd, { a, b }, rc, row);  // posição antiga, em px
    else DefaultCell(wd, rc, row);
    rc = std::clamp(rc, 0, MaxRc(g, wd)); row = std::clamp(row, 0, MaxRow(g, wd));
    if (Overlaps(wd, rc, row)) Nearest(g, wd, CellPx(g, wd, rc, row), rc, row);
    wd.rc = rc; wd.row = row;
    POINT p = CellPx(g, wd, rc, row);
    int wpx = (int)std::lround(wd.w * g.sc), hpx = (int)std::lround(wd.h * g.sc);
    g_placing = true;
    UINT fl = SWP_NOZORDER | SWP_NOACTIVATE | (animate ? SWP_NOMOVE : 0);
    SetWindowPos(wd.back, nullptr, p.x, p.y, wpx, hpx, fl);
    SetWindowPos(wd.wnd, nullptr, p.x, p.y, wpx, hpx, fl);
    g_placing = false;
    if (animate) AnimateTo(wd, p);
}

void Show(Widget& wd, bool on) {
    wd.on = on;
    if (!on) wd.rc = -1;  // a casa fica livre
    if (on) {
        Position(wd, false);
        ShowWindow(wd.back, SW_SHOWNOACTIVATE);
        ShowWindow(wd.wnd, SW_SHOWNOACTIVATE);
        PlaceAtDesktop(wd);
        Paint(wd);
    } else {
        ShowWindow(wd.wnd, SW_HIDE);
        ShowWindow(wd.back, SW_HIDE);
    }
}

Widget* Of(HWND h) { for (auto& w : g_w) if (w.wnd == h || w.back == h) return &w; return nullptr; }

void Click(Widget& wd) {
    const wchar_t* target = nullptr;
    std::wstring url;
    switch (wd.kind) {
    case K_WEATHER: case K_SUN: {
        url = L"https://www.google.com/search?q=tempo+" + City();
        for (auto& c : url) if (c == L' ') c = L'+';
        target = url.c_str(); break;
    }
    case K_CLOCK: case K_WORLD: target = L"ms-clock:"; break;
    case K_CALENDAR: target = L"outlookcal:"; break;
    }
    if (target) ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
}

void Menu(Widget* wd, POINT pt) {  // wd nulo: "Editar Widgets…" da mesa (só ligar/desligar e organizar)
    std::vector<ctxmenu::Item> items;
    for (size_t i = 0; i < g_w.size(); i++) {
        ctxmenu::Item it; it.label = Tr(g_w[i].name); it.id = 10 + (int)i; it.checked = g_w[i].on; items.push_back(it);
    }
    ctxmenu::Item sep; sep.separator = true; items.push_back(sep);
    if (wd) { ctxmenu::Item rm; rm.label = Tr(L"Remover Widget"); rm.id = 1; items.push_back(rm); }
    ctxmenu::Item reset; reset.label = Tr(L"Organizar Widgets"); reset.id = 2; items.push_back(reset);
    if (wd && (wd->kind == K_WEATHER || wd->kind == K_SUN)) {  // os dados do Open-Meteo são CC BY 4.0: crédito com link
        ctxmenu::Item sep2; sep2.separator = true; items.push_back(sep2);
        ctxmenu::Item om; om.label = Tr(L"Dados do tempo por Open-Meteo.com"); om.id = 3; items.push_back(om);
    }
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(g_menuOwner);
    if (att) AttachThreadInput(me, ft, FALSE);
    int id = ctxmenu::ShowItems(g_menuOwner, pt, items);
    if (id == 1 && wd) { Show(*wd, false); shell::SetSetting((std::wstring(L"widget.") + wd->id + L".on").c_str(), L"0"); }
    else if (id == 3) std::thread([] {  // o site abre numa thread, sem prender o shell
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", L"https://open-meteo.com/", nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
    else if (id == 2) {
        for (auto& w : g_w) w.rc = -1;
        for (auto& w : g_w) { shell::SetSetting((std::wstring(L"widget.") + w.id).c_str(), L""); if (w.on) Position(w, true, true); }
    } else if (id >= 10 && id < 10 + (int)g_w.size()) {
        Widget& t = g_w[id - 10];
        Show(t, !t.on);
        shell::SetSetting((std::wstring(L"widget.") + t.id + L".on").c_str(), t.on ? L"1" : L"0");
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Widget* wd = Of(h);
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); if (wd && wd->wnd == h) Paint(*wd); return 0; }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING: {  // só nós mexemos na ordem: ficam sempre logo acima da mesa
        auto* p = (WINDOWPOS*)lp;
        if (!g_placing) p->flags |= SWP_NOZORDER;
        break;
    }
    case WM_LBUTTONDOWN:
        if (wd) { g_drag = wd; GetCursorPos(&g_dragStart); RECT r; GetWindowRect(wd->wnd, &r); g_winStart = { r.left, r.top }; g_dragging = false; SetCapture(h); }
        return 0;
    case WM_MOUSEMOVE:
        if (g_drag && GetCapture() == h) {
            POINT c; GetCursorPos(&c);
            int dx = c.x - g_dragStart.x, dy = c.y - g_dragStart.y;
            // só vira arrastar depois de ~8 px: com 3 px um clique com a mão tremendo mudava o widget de casa
            // e gravava o lugar novo (os widgets "pulavam" para o meio da tela, 17/11/2026)
            int lim = std::max(8, GetSystemMetrics(SM_CXDRAG) * 2);
            if (!g_dragging && (abs(dx) > lim || abs(dy) > lim)) {  // começou a arrastar: por cima dos outros, sombra na casa atual
                g_dragging = true;
                g_moves.erase(std::remove_if(g_moves.begin(), g_moves.end(), [&](const Move& mv) { return mv.w == g_drag; }), g_moves.end());
                PlaceAtDesktop(*g_drag);
                Grid g = GridNow(g_drag->wnd);
                g_targetRc = g_drag->rc; g_targetRow = g_drag->row;
                g_drag->rc = -1;  // a casa dele fica livre enquanto arrasta
                if (g_targetRc < 0) Nearest(g, *g_drag, g_winStart, g_targetRc, g_targetRow);
                g_ghostTo = CellPx(g, *g_drag, g_targetRc, g_targetRow);
                g_gx = g_ghostTo.x; g_gy = g_ghostTo.y;
                PaintGhost(*g_drag);
                g_placing = true;
                SetWindowPos(g_ghost, g_drag->back, (int)g_gx, (int)g_gy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                g_placing = false;
                Animate(true);
            }
            if (g_dragging) {
                POINT p{ g_winStart.x + dx, g_winStart.y + dy };
                MoveWin(*g_drag, p.x, p.y);
                Grid g = GridNow(g_drag->wnd);
                Nearest(g, *g_drag, p, g_targetRc, g_targetRow);
                g_ghostTo = CellPx(g, *g_drag, g_targetRc, g_targetRow);
            }
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (g_drag && (m == WM_CAPTURECHANGED || GetCapture() == h)) {
            Widget* d = g_drag; g_drag = nullptr;
            bool dragged = g_dragging; g_dragging = false;
            if (m == WM_LBUTTONUP) ReleaseCapture();
            if (dragged) {  // encaixa na casa com a mola
                d->rc = g_targetRc; d->row = g_targetRow;
                SaveCell(*d);
                AnimateTo(*d, CellPx(GridNow(d->wnd), *d, d->rc, d->row));
                Animate(true);
            } else if (m == WM_LBUTTONUP) Click(*d);
        }
        return 0;
    case WM_RBUTTONUP:
        if (wd) { POINT c; GetCursorPos(&c); Menu(wd, c); }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

const UINT WM_WG_DESK = WM_APP + 62;
// a mesa veio para a frente (Win+D, "mostrar a área de trabalho", clique na mesa): os widgets sobem na hora
// (antes só na conferência de 3 s: no Win+D eles demoravam a aparecer)
void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h) return;
    wchar_t c[32] = {}; GetClassNameW(h, c, 32);
    if (mesa::IsDeskClass(c)) PostMessageW(g_timer, WM_WG_DESK, 0, 0);
}

LRESULT CALLBACK TimerProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_WG_FRAME) { Frame(); return 0; }
    if (m == WM_WG_DESK) { if (!g_drag) for (auto& w : g_w) if (w.on && BelowDesktop(w)) { for (auto& x : g_w) if (x.on) PlaceAtDesktop(x); break; } return 0; }
    if (m == WM_WX_READY) { for (auto& w : g_w) if (w.kind == K_WEATHER || w.kind == K_SUN) Paint(w); return 0; }
    if (m == WM_TIMER && wp == 1) {
        SYSTEMTIME t; GetLocalTime(&t);
        SetTimer(h, 1, 1000 - t.wMilliseconds + 5, nullptr);  // na virada do segundo
        bool dark = ui::Dark();
        bool all = dark != g_dark || t.wMinute != g_lastMinute;
        g_dark = dark; g_lastMinute = t.wMinute;
        static int tick;
        bool checkZ = true;  // ordem das janelas: todo segundo, só pelo primeiro widget ligado (barato: para no primeiro achado)
        ++tick;
        {   // área de trabalho mudou (a barra de menus reservou a faixa de cima depois que os widgets nasceram, ou o
            // Dock mudou): reposiciona. O Shell muda a área sem avisar todo mundo, então o WM_SETTINGCHANGE não chegava
            // e os widgets ficavam por cima da barra de menus (17/11/2026)
            static RECT lastWork{};
            RECT work = shell::WorkAreaFor(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));  // a do Shell (issue #1)
            if (!EqualRect(&work, &lastWork) && !g_drag && g_moves.empty()) {
                bool first = lastWork.right == 0;
                lastWork = work;
                (void)first;  // também na primeira: os widgets podem ter nascido com a área antiga (sem a barra)
                for (auto& w : g_w) if (w.on) { Position(w, false); Paint(w); }
            }
        }
        bool below = false;
        if (checkZ) for (auto& w : g_w) if (w.on) { below = BelowDesktop(w); break; }
        for (auto& w : g_w) {
            if (!w.on) continue;
            if (below) PlaceAtDesktop(w);
            if (all || w.kind == K_CLOCK || w.kind == K_WORLD) Paint(w);
        }
        return 0;
    }
    if (m == WM_TIMER && wp == 2) { Fetch(); return 0; }
    if (m == WM_DISPLAYCHANGE || (m == WM_SETTINGCHANGE && wp == SPI_SETWORKAREA)) {
        for (auto& w : g_w) if (w.on) { Position(w, false); Paint(w); }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_WINDOWPOSCHANGING && !g_placing) ((WINDOWPOS*)l)->flags |= SWP_NOZORDER;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// "Editar Widgets…" (menu da mesa e da Central): a lista dos widgets para ligar ou desligar, mesmo sem nenhum na
// tela (antes só dava pelo menu de um widget: removidos todos, não havia como trazer de volta)
void widgets::EditMenu(POINT pt) { if (!g_w.empty()) Menu(nullptr, pt); }

// Ajustes > Mesa e Dock > Personalizar a Mesa liga/desliga cada widget (widget.<id>.on): segue o arquivo
void widgets::SettingsChanged() {
    for (auto& wd : g_w) {
        if (!wd.wnd) continue;
        bool want = shell::Setting((std::wstring(L"widget.") + wd.id + L".on").c_str(), L"1") != L"0";
        if (want != wd.on) Show(wd, want);
    }
}

void widgets::Create(HINSTANCE hi) {
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellWidgetFundo";
    RegisterClassW(&bc);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellWidget"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW tc{}; tc.lpfnWndProc = TimerProc; tc.hInstance = hi; tc.lpszClassName = L"ShellWidgetsRelogio";
    RegisterClassW(&tc);
    g_timer = CreateWindowExW(WS_EX_TOOLWINDOW, tc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);  // recebe WM_DISPLAYCHANGE
    WNDCLASSW gc{}; gc.lpfnWndProc = BackProc; gc.hInstance = hi; gc.lpszClassName = L"ShellWidgetSombra";
    RegisterClassW(&gc);
    g_ghost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, gc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_ghostS.hwnd = g_ghost;
    g_menuOwner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, tc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, -32000, -32000, 1, 1, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(g_menuOwner, 0, 0, LWA_ALPHA);
    apps::ours.push_back(g_menuOwner);
    apps::ours.push_back(g_ghost);

    g_w = { { K_WEATHER, L"tempo", L"Tempo", MEDIUM, SMALL },
            { K_CLOCK, L"relogio", L"Relógio", SMALL, SMALL },
            { K_CALENDAR, L"calendario", L"Calendário", SMALL, SMALL },
            { K_WORLD, L"mundial", L"Relógio Mundial", MEDIUM, SMALL },
            { K_SUN, L"sol", L"Nascer do Sol", SMALL, SMALL } };
    g_w.reserve(g_w.size());
    g_dark = ui::Dark();
    LoadCache();
    for (auto& wd : g_w) {
        wd.back = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        if (!wd.back) continue;
        wd.glass = backdrop::Attach(wd.back);
        if (!wd.glass) {
            DestroyWindow(wd.back);
            wd.back = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
            DWORD corner = 2; DwmSetWindowAttribute(wd.back, 33, &corner, sizeof(corner));
            ui::Acrylic(wd.back, g_dark ? 0x60201E1Eu : 0x30F6F6F6u);
        } else { DWORD corner = 1; DwmSetWindowAttribute(wd.back, 33, &corner, sizeof(corner)); }
        COLORREF none = 0xFFFFFFFE;
        DwmSetWindowAttribute(wd.back, 34, &none, sizeof(none));
        BOOL off = TRUE; DwmSetWindowAttribute(wd.back, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        wd.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, wd.name, WS_POPUP, 0, 0, 10, 10, wd.back, nullptr, hi, nullptr);
        if (!wd.wnd) continue;
        DwmSetWindowAttribute(wd.wnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
        apps::ours.push_back(wd.wnd);
        apps::ours.push_back(wd.back);
        wd.s.hwnd = wd.wnd;
        Show(wd, shell::Setting((std::wstring(L"widget.") + wd.id + L".on").c_str(), L"1") != L"0");
    }
    SetTimer(g_timer, 1, 1000, nullptr);
    SetTimer(g_timer, 2, 15 * 60 * 1000, nullptr);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    Fetch();
    Log(L"widgets: %zu na mesa (tempo de %ls)", g_w.size(), City().c_str());
}

std::vector<RECT> widgets::Areas() {
    std::vector<RECT> r;
    for (auto& w : g_w) if (w.on && w.wnd && IsWindowVisible(w.wnd)) { RECT x; GetWindowRect(w.wnd, &x); r.push_back(x); }
    return r;
}
