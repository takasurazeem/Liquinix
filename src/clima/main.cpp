// Clima (Weather do Mac): à esquerda as cidades (a primeira é a dos Ajustes, a mesma do widget); à direita, sobre
// um fundo da cor do céu, a cidade, a temperatura grande, a condição, máx./mín., a previsão por hora (24 h), a de
// 10 dias (com a barra de mín.–máx.) e os quadros de sensação, umidade, vento e sol. Dados do Open-Meteo (sem chave).
// "+" procura uma cidade pelo nome (geocodificação do Open-Meteo); botão direito numa cidade: tornar "Meu Local"
// (vai para os Ajustes, o mesmo do widget) ou apagar (o "Meu Local" também, se houver outra para ficar no lugar).
// Cidades em %APPDATA%\Shell\clima.txt.
#include "../comum/app2d.h"
#include "../comum/cidade_padrao.h"
#include <winhttp.h>
#include <map>
#include <mutex>
#include <thread>
#pragma comment(lib, "winhttp.lib")

using namespace app2d;

namespace {

const float SB_W = 230;
bool g_side = true;  // barra lateral das cidades (o botão dela esconde e mostra; antes não havia como)
const UINT WM_DATA = WM_APP + 5, WM_FIELDLOST = WM_APP + 77;

struct Hour { double t; float temp; int code; bool day; };
struct Day { double t; float hi, lo; int code; double rise, set; };
struct Wx { bool ok = false; float temp = 0, feels = 0, hum = 0, wind = 0; int code = 0; bool day = true; int off = 0; std::vector<Hour> hours; std::vector<Day> days; };
struct Place { std::wstring name; std::wstring lat, lon; Wx wx; bool loading = false; };
std::vector<Place> g_places;
int g_sel = 0;
std::mutex g_mx;
std::wstring g_file;
std::wstring g_status;
IDWriteTextFormat *g_tfSmallC, *g_tfCity, *g_tfTemp, *g_tfCond, *g_tfSmall, *g_tfCard, *g_tfCardBig, *g_tfEmoji, *g_tfEmojiSmall, *g_tfSide, *g_tfSideTemp, *g_tfIcon, *g_tfRowR;
IDWriteTextFormat *g_tfHdrC, *g_tfCardG;  // macOS 27

std::string HttpGet(const wchar_t* host, const std::wstring& path) {
    std::string body;
    HINTERNET ses = WinHttpOpen(L"SequoiaClima/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return body;
    WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 512 * 1024) {
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
std::string Section(const std::string& j, const char* key) {
    size_t p = j.find(std::string("\"") + key + "\":{");
    if (p == std::string::npos) return "";
    p = j.find('{', p);
    int depth = 0;
    for (size_t i = p; i < j.size(); i++) { if (j[i] == '{') depth++; else if (j[i] == '}' && --depth == 0) return j.substr(p, i - p + 1); }
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
        char* e = nullptr; v.push_back(strtod(s.c_str() + p, &e));
        if (e == s.c_str() + p) break;
        p = e - s.c_str();
    }
    return v;
}
std::wstring Utf8(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w; }
std::string ToUtf8(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr); return s; }
std::string StrField(const std::string& s, const char* key) {
    size_t p = s.find(std::string("\"") + key + "\":\"");
    if (p == std::string::npos) return "";
    p += strlen(key) + 4;
    size_t e = s.find('"', p);
    return e == std::string::npos ? "" : s.substr(p, e - p);
}

bool Parse(const std::string& j, Wx& w) {
    std::string cur = Section(j, "current"), hr = Section(j, "hourly"), dl = Section(j, "daily");
    if (cur.empty() || hr.empty() || dl.empty()) return false;
    double off = Number(j, "utc_offset_seconds"); if (!std::isnan(off)) w.off = (int)off;
    w.temp = (float)Number(cur, "temperature_2m"); w.feels = (float)Number(cur, "apparent_temperature");
    w.hum = (float)Number(cur, "relative_humidity_2m"); w.wind = (float)Number(cur, "wind_speed_10m");
    w.code = (int)Number(cur, "weather_code"); w.day = Number(cur, "is_day") > 0.5;
    auto t = Array(hr, "time"), tp = Array(hr, "temperature_2m"), c = Array(hr, "weather_code"), d = Array(hr, "is_day");
    for (size_t i = 0; i < t.size() && i < tp.size() && i < c.size() && i < d.size(); i++) w.hours.push_back({ t[i], (float)tp[i], (int)c[i], d[i] > 0.5 });
    auto dt = Array(dl, "time"), mx = Array(dl, "temperature_2m_max"), mn = Array(dl, "temperature_2m_min"), dc = Array(dl, "weather_code"), sr = Array(dl, "sunrise"), ss = Array(dl, "sunset");
    for (size_t i = 0; i < dt.size() && i < mx.size() && i < mn.size() && i < dc.size(); i++)
        w.days.push_back({ dt[i], (float)mx[i], (float)mn[i], (int)dc[i], i < sr.size() ? sr[i] : 0, i < ss.size() ? ss[i] : 0 });
    w.ok = !std::isnan(w.temp) && !w.days.empty();
    return w.ok;
}

void Fetch(int i) {
    Place& p = g_places[i];
    if (p.loading) return;
    p.loading = true;
    std::wstring lat = p.lat, lon = p.lon;
    HWND h = wnd;
    std::thread([i, lat, lon, h] {
        std::wstring path = L"/v1/forecast?latitude=" + lat + L"&longitude=" + lon +
            L"&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code,is_day"
            L"&hourly=temperature_2m,weather_code,is_day&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
            L"&timezone=auto&forecast_days=10&timeformat=unixtime";
        std::string body = HttpGet(L"api.open-meteo.com", path);
        auto* w = new Wx;
        if (body.empty() || !Parse(body, *w)) w->ok = false;
        if (!PostMessageW(h, WM_DATA, (WPARAM)i, (LPARAM)w)) delete w;
    }).detach();
}

void Save() {
    std::vector<std::wstring> out{ L"# Cidades do Clima (nome, latitude, longitude); a primeira vem dos Ajustes" };
    for (size_t i = 1; i < g_places.size(); i++) out.push_back(L"C\t" + Esc(g_places[i].name) + L"\t" + g_places[i].lat + L"\t" + g_places[i].lon);
    WriteLines(g_file, out);
}

std::wstring Setting(const wchar_t* key, const wchar_t* def) {
    for (auto& l : ReadLines(DataPath(L"ajustes.txt"))) { size_t e = l.find(L'='); if (e != std::wstring::npos && l.substr(0, e) == key) return l.substr(e + 1); }
    return def;
}
// "Meu Local" vai para os Ajustes (tempoCidade/Lat/Lon), o mesmo do widget da mesa
void SetMyPlace(const Place& p) {
    SetAppSetting(L"tempoCidade", p.name);
    SetAppSetting(L"tempoLat", p.lat);
    SetAppSetting(L"tempoLon", p.lon);
}
void Load() {
    g_places.clear();
    cidade::Padrao def;  // sem cidade nos Ajustes: a do fuso do Windows (antes era sempre Campinas)
    g_places.push_back({ Setting(L"tempoCidade", def.nome.c_str()), Setting(L"tempoLat", def.lat.c_str()), Setting(L"tempoLon", def.lon.c_str()) });
    for (auto& l : ReadLines(g_file)) { auto f = Split(l); if (f.size() >= 4 && f[0] == L"C") g_places.push_back({ Unesc(f[1]), f[2], f[3] }); }
}

// procura a cidade (geocodificação) e põe na lista
void Search(const std::wstring& q) {
    if (q.empty()) return;
    g_status = Tr(L"Procurando…");
    InvalidateRect(wnd, nullptr, FALSE);
    HWND h = wnd;
    std::thread([q, h] {
        std::string u = ToUtf8(q), enc;
        for (unsigned char c : u) { if (isalnum(c) || c == '-' || c == '.') enc += (char)c; else { char b[4]; sprintf_s(b, "%%%02X", c); enc += b; } }
        std::wstring path = L"/v1/search?count=1&language=" + std::wstring(idioma::Code()) + L"&name=" + Utf8(enc);
        std::string body = HttpGet(L"geocoding-api.open-meteo.com", path);
        auto* p = new Place;
        if (!body.empty() && body.find("\"results\"") != std::string::npos) {
            double la = Number(body, "latitude"), lo = Number(body, "longitude");
            std::string name = StrField(body, "name"), adm = StrField(body, "admin1");
            if (!std::isnan(la)) {
                wchar_t a[32], b[32]; swprintf_s(a, L"%.4f", la); swprintf_s(b, L"%.4f", lo);
                p->name = Utf8(name); p->lat = a; p->lon = b;
            }
        }
        if (!PostMessageW(h, WM_DATA, (WPARAM)-1, (LPARAM)p)) delete p;
    }).detach();
}

// ---- condições (códigos WMO)
const wchar_t* CondText(int c) {
    if (c == 0) return Tr(L"Céu Limpo");
    if (c <= 2) return Tr(L"Parcialmente Nublado");
    if (c == 3) return Tr(L"Nublado");
    if (c == 45 || c == 48) return Tr(L"Neblina");
    if (c >= 51 && c <= 57) return Tr(L"Garoa");
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return Tr(L"Chuva");
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return Tr(L"Neve");
    if (c >= 95) return Tr(L"Tempestade");
    return L"";
}
const wchar_t* CondEmoji(int c, bool day) {
    if (c == 0) return day ? L"☀️" : L"\U0001F319";
    if (c <= 2) return day ? L"⛅" : L"☁️";
    if (c == 3) return L"☁️";
    if (c == 45 || c == 48) return L"\U0001F32B️";
    if (c >= 51 && c <= 57) return L"\U0001F326️";
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return L"\U0001F327️";
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return L"❄️";
    if (c >= 95) return L"⛈️";
    return L"☁️";
}
// cor do céu (em cima, embaixo)
void Sky(const Wx& w, D2D1_COLOR_F& a, D2D1_COLOR_F& b) {
    bool cloudy = w.code >= 3, rain = w.code >= 51;
    if (!w.day) { a = Rgb(16, 24, 48); b = Rgb(38, 48, 80); }
    else if (rain) { a = Rgb(70, 84, 104); b = Rgb(110, 124, 140); }
    else if (cloudy) { a = Rgb(92, 120, 150); b = Rgb(140, 160, 182); }
    else { a = Rgb(40, 110, 200); b = Rgb(110, 170, 230); }
}
std::wstring HourOf(double unix, int off) { time_t t = (time_t)unix + off; tm g{}; gmtime_s(&g, &t); wchar_t b[8]; swprintf_s(b, L"%d", g.tm_hour); return b; }
std::wstring HmOf(double unix, int off) { time_t t = (time_t)unix + off; tm g{}; gmtime_s(&g, &t); wchar_t b[8]; swprintf_s(b, L"%d:%02d", g.tm_hour, g.tm_min); return b; }
int WdayOf(double unix, int off) { time_t t = (time_t)unix + off; tm g{}; gmtime_s(&g, &t); return g.tm_wday; }

struct Hit { int kind, arg; D2D1_RECT_F r; };
std::vector<Hit> g_hits;

// os dados do Open-Meteo são CC BY 4.0: crédito com link; o site abre numa thread, sem prender a janela
const wchar_t* OpenMeteoCredit() { return Tr(L"Dados do tempo por Open-Meteo.com"); }
void OpenMeteoSite() {
    std::thread([] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", L"https://open-meteo.com/", nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
}
void Card(D2D1_RECT_F r) { Fill(r, Rgb(255, 255, 255, 0.14f), 14); }
const wchar_t* CondText(int c);
const wchar_t* CondEmoji(int c, bool day);
// ---- macOS 27 (captura Weather do 512 Pixels, 2x, medida em pt): o céu ocupa a janela toda (a barra é
// transparente); lateral de 270 com o céu escurecido, o botão de "barra lateral" (244, 26) e a busca em cápsula
// (9–262, 63–97); o "+" redondo fica no conteúdo (297, 26). No meio: cidade (20), temperatura fina (64), condição e
// máx./mín. Embaixo uma grade de 622 de largura, centrada: dois quadros de 143 e um largo de 304 por linha (141 de
// altura, 16 entre eles), vidro claro com cantos 18 e o título em maiúsculas pequenas.
float g_gScroll;
float g_gTotal;
IDWriteTextFormat *g_tfCityG, *g_tfTempG, *g_tfCondG, *g_tfHdr, *g_tfBigG, *g_tfSmallW, *g_tfSearchG, *g_tfCondTitle;

void SkyFill(D2D1_RECT_F r, const Wx& w, float darken) {
    D2D1_COLOR_F a, b; Sky(w, a, b);
    auto dk = [&](D2D1_COLOR_F c) { return D2D1::ColorF(c.r * (1 - darken), c.g * (1 - darken), c.b * (1 - darken), 1); };
    D2D1_GRADIENT_STOP gs[2] = { { 0, dk(a) }, { 1, dk(b) } };
    ID2D1GradientStopCollection* st = nullptr; rt->CreateGradientStopCollection(gs, 2, &st);
    ID2D1LinearGradientBrush* lg = nullptr;
    if (st) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, Size().height }), st, &lg);
    if (lg) { rt->FillRectangle(r, lg); lg->Release(); }
    if (st) st->Release();
}
void GlassCard(D2D1_RECT_F r) {
    Fill(r, Rgb(255, 255, 255, 0.15f), 18);
    br->SetColor(Rgb(255, 255, 255, 0.20f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.25f, r.top + 0.25f, r.right - 0.25f, r.bottom - 0.25f), 17.75f, 17.75f), br, 0.5f);
}
void CardHead(D2D1_RECT_F r, const wchar_t* title) { Text(title, g_tfHdr, D2D1::RectF(r.left + 14, r.top + 8, r.right - 10, r.top + 24), Rgb(255, 255, 255, 0.62f)); }

void PaintGoldenWx() {
    g_hits.clear();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F white = Rgb(255, 255, 255), dim = Rgb(255, 255, 255, 0.72f);
    const float SBG = g_side ? 270.f : 0.f;
    Place pl; bool have = g_sel >= 0 && g_sel < (int)g_places.size();
    if (have) { std::lock_guard<std::mutex> l(g_mx); pl = g_places[g_sel]; }
    Wx sky = have && pl.wx.ok ? pl.wx : Wx{};
    if (!have || !pl.wx.ok) { sky.day = true; sky.code = 1; }
    SkyFill(D2D1::RectF(0, 0, sz.width, sz.height), sky, 0);
    // ---- lateral (escondida: o botão dela fica na barra, logo depois dos três botões da janela)
    if (!g_side) {
        D2D1_RECT_F tb = D2D1::RectF(84, 8, 120, 44);
        Fill(tb, Rgb(255, 255, 255, 0.16f), 18); IconSidebar({ 102, 26 }, white); BarButton(tb);
        g_hits.push_back({ 3, 0, tb });
    } else {
    SkyFill(D2D1::RectF(0, 0, SBG, sz.height), sky, 0.28f);
    Fill(D2D1::RectF(SBG - 0.5f, 0, SBG, sz.height), Rgb(0, 0, 0, 0.18f));
    D2D1_RECT_F tb = D2D1::RectF(244 - 18, 8, 244 + 18, 44);
    Fill(tb, Rgb(255, 255, 255, 0.16f), 18); IconSidebar({ 244, 26 }, white); BarButton(tb);
    g_hits.push_back({ 3, 0, tb });
    D2D1_RECT_F sr = D2D1::RectF(9, 63, 262, 97);
    Fill(sr, Rgb(255, 255, 255, 0.16f), 17);
    br->SetColor(Rgb(255, 255, 255, 0.22f)); rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 0.25f, sr.top + 0.25f, sr.right - 0.25f, sr.bottom - 0.25f), 16.75f, 16.75f), br, 0.5f);
    IconSearch({ 25, 80 }, dim, 0.9f);
    Text(Tr(L"Buscar"), g_tfSearchG, D2D1::RectF(38, sr.top, sr.right - 10, sr.bottom), dim);
    g_hits.push_back({ 2, 0, sr });
    float y = 108;
    for (int i = 0; i < (int)g_places.size(); i++) {
        auto& p = g_places[i];
        D2D1_RECT_F r = D2D1::RectF(9, y, 262, y + 70);
        if (i == g_sel) Fill(r, Rgb(255, 255, 255, 0.18f), 14);
        Text(i == 0 ? std::wstring(Tr(L"Meu Local")) : p.name, g_tfSide, D2D1::RectF(r.left + 12, r.top + 8, r.right - 60, r.top + 30), white);
        if (i == 0) Text(p.name, g_tfSmall, D2D1::RectF(r.left + 12, r.top + 28, r.right - 60, r.top + 46), dim);
        if (p.wx.ok) {
            Text(CondText(p.wx.code), g_tfSmall, D2D1::RectF(r.left + 12, r.bottom - 24, r.right - 60, r.bottom - 6), dim);
            wchar_t t[16]; swprintf_s(t, L"%d°", (int)std::lround(p.wx.temp));
            Text(t, g_tfSideTemp, D2D1::RectF(r.right - 80, r.top + 4, r.right - 12, r.top + 44), white);
        }
        g_hits.push_back({ 1, i, r });
        y += 76;
    }
    if (!g_status.empty()) Text(g_status, g_tfSmall, D2D1::RectF(12, sz.height - 30, SBG - 10, sz.height - 8), dim);
    }  // g_side
    // "+" no conteúdo
    float addX = g_side ? 297.f : 146.f;
    D2D1_RECT_F add = D2D1::RectF(addX - 18, 8, addX + 18, 44);
    Fill(add, Rgb(255, 255, 255, 0.16f), 18); IconPlus({ addX, 26 }, white);
    g_hits.push_back({ 2, 0, add }); BarButton(add);
    if (!have) return;
    float X0 = SBG, cx = (SBG + sz.width) / 2;
    if (!pl.wx.ok) { Text(pl.loading ? Tr(L"Carregando…") : Tr(L"Sem conexão com o serviço do tempo."), g_tfCondG, D2D1::RectF(X0, 0, sz.width, sz.height), white); return; }
    const Wx& w = pl.wx;
    rt->PushAxisAlignedClip(D2D1::RectF(X0, 52, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float oy = -g_gScroll;
    wchar_t t[64];
    Text(g_sel == 0 ? std::wstring(Tr(L"Meu Local")) : pl.name, g_tfCityG, D2D1::RectF(X0, oy + 94, sz.width, oy + 120), white);
    swprintf_s(t, L" %d°", (int)std::lround(w.temp));
    Text(t, g_tfTempG, D2D1::RectF(X0, oy + 118, sz.width, oy + 182), white);
    Text(CondText(w.code), g_tfCondG, D2D1::RectF(X0, oy + 177, sz.width, oy + 195), white);
    if (!w.days.empty()) { swprintf_s(t, Tr(L"Máx.: %d°  Mín.: %d°"), (int)std::lround(w.days[0].hi), (int)std::lround(w.days[0].lo)); Text(t, g_tfCondG, D2D1::RectF(X0, oy + 194, sz.width, oy + 212), white); }
    float GW = 622, x0 = cx - GW / 2, y0 = oy + 266;
    if (x0 < X0 + 12) { x0 = X0 + 12; GW = sz.width - X0 - 24; }
    float sm = (GW - 32 - 304) / 2 > 100 ? 143.f : (GW - 16) / 2, wideX = x0 + 2 * sm + 32, wideW = x0 + GW - wideX;
    // linha 1: sensação, vento, condições por hora
    D2D1_RECT_F c1 = D2D1::RectF(x0, y0, x0 + sm, y0 + 141), c2 = D2D1::RectF(x0 + sm + 16, y0, x0 + 2 * sm + 16, y0 + 141);
    D2D1_RECT_F c3 = D2D1::RectF(wideX, y0, wideX + wideW, y0 + 141);
    GlassCard(c1); CardHead(c1, Tr(L"SENSAÇÃO"));
    swprintf_s(t, L"%d°", (int)std::lround(w.feels));
    Text(t, g_tfBigG, D2D1::RectF(c1.left + 14, c1.top + 28, c1.right, c1.top + 62), white);
    {
        IDWriteTextFormat* f = g_tfSmallW;
        const wchar_t* s = w.feels < w.temp - 1 ? Tr(L"O vento deixa mais frio do que a temperatura real.") : w.feels > w.temp + 1 ? Tr(L"A umidade deixa mais quente do que a temperatura real.") : Tr(L"Parecida com a temperatura real.");
        br->SetColor(white); rt->DrawTextW(s, (UINT32)wcslen(s), f, D2D1::RectF(c1.left + 14, c1.top + 66, c1.right - 12, c1.bottom - 8), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    GlassCard(c2); CardHead(c2, Tr(L"VENTO"));
    {   // bússola com a velocidade no meio
        D2D1_POINT_2F c{ (c2.left + c2.right) / 2, c2.top + 82 };
        br->SetColor(Rgb(255, 255, 255, 0.35f));
        for (int k = 0; k < 72; k++) {
            double a = k * 3.14159265 / 36; float r0 = k % 18 == 0 ? 36.f : 40.f;
            rt->DrawLine({ c.x + (float)sin(a) * r0, c.y - (float)cos(a) * r0 }, { c.x + (float)sin(a) * 44, c.y - (float)cos(a) * 44 }, br, 0.8f);
        }
        const wchar_t* L4[4] = { idioma::Compass(0), idioma::Compass(1), idioma::Compass(2), idioma::Compass(3) };
        for (int k = 0; k < 4; k++) { double a = k * 3.14159265 / 2; Text(L4[k], g_tfHdr, D2D1::RectF(c.x + (float)sin(a) * 30 - 8, c.y - (float)cos(a) * 30 - 8, c.x + (float)sin(a) * 30 + 8, c.y - (float)cos(a) * 30 + 8), white); }
        swprintf_s(t, L"%d", (int)std::lround(w.wind));
        Text(t, g_tfCondTitle, D2D1::RectF(c.x - 20, c.y - 12, c.x + 20, c.y + 6), white);
        Text(L"km/h", g_tfHdr, D2D1::RectF(c.x - 20, c.y + 3, c.x + 20, c.y + 15), white);
    }
    GlassCard(c3);
    Text(Tr(L"Condições"), g_tfCondTitle, D2D1::RectF(c3.left + 14, c3.top + 10, c3.right, c3.top + 28), white);
    Text(Tr(L"Temperatura (°C)"), g_tfHdr, D2D1::RectF(c3.left + 14, c3.top + 26, c3.right, c3.top + 40), dim);
    {
        time_t now = time(nullptr);
        int start = 0; for (int i = 0; i < (int)w.hours.size(); i++) if (w.hours[i].t <= now) start = i;
        int n = std::min(6, (int)w.hours.size() - start);
        float cw = (c3.right - c3.left - 16) / 6;
        for (int k = 0; k < n; k++) {
            auto& hh = w.hours[start + k];
            float x = c3.left + 8 + k * cw;
            Text(k == 0 ? std::wstring(Tr(L"Agora")) : HourOf(hh.t, w.off), g_tfHdrC, D2D1::RectF(x, c3.top + 50, x + cw, c3.top + 66), white);
            Text(CondEmoji(hh.code, hh.day), g_tfEmojiSmall, D2D1::RectF(x, c3.top + 70, x + cw, c3.top + 100), white);
            swprintf_s(t, L"%d°", (int)std::lround(hh.temp));
            Text(t, g_tfCardG, D2D1::RectF(x, c3.top + 104, x + cw, c3.top + 126), white);
        }
    }
    // linha 2: umidade (larga) e a previsão de 10 dias (alta, na coluna da direita)
    float y1 = y0 + 141 + 16;
    D2D1_RECT_F c4 = D2D1::RectF(x0, y1, x0 + 2 * sm + 16, y1 + 143);
    GlassCard(c4); CardHead(c4, Tr(L"UMIDADE"));
    swprintf_s(t, L"%d%%", (int)std::lround(w.hum));
    Text(t, g_tfBigG, D2D1::RectF(c4.left + 14, c4.top + 28, c4.right, c4.top + 62), white);
    {
        D2D1_RECT_F bar = D2D1::RectF(c4.left + 14, c4.top + 86, c4.right - 14, c4.top + 91);
        D2D1_GRADIENT_STOP gs[3] = { { 0, Rgb(255, 210, 120) }, { 0.5f, Rgb(120, 220, 255) }, { 1, Rgb(40, 110, 255) } };
        ID2D1GradientStopCollection* st = nullptr; rt->CreateGradientStopCollection(gs, 3, &st);
        ID2D1LinearGradientBrush* lg = nullptr;
        if (st) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ bar.left, 0 }, { bar.right, 0 }), st, &lg);
        if (lg) { rt->FillRoundedRectangle(D2D1::RoundedRect(bar, 2.5f, 2.5f), lg); lg->Release(); }
        if (st) st->Release();
        float px = bar.left + (bar.right - bar.left) * std::clamp(w.hum / 100.f, 0.f, 1.f);
        Circle({ px, (bar.top + bar.bottom) / 2 }, 4.5f, white, true);
        const wchar_t* s = w.hum >= 70 ? Tr(L"O ar está úmido agora.") : w.hum <= 35 ? Tr(L"O ar está seco agora.") : Tr(L"Umidade confortável agora.");
        Text(s, g_tfSmallW, D2D1::RectF(c4.left + 14, c4.top + 100, c4.right - 12, c4.bottom - 8), white);
    }
    float rows = (float)std::min<size_t>(10, w.days.size());
    D2D1_RECT_F c5 = D2D1::RectF(wideX, y1, wideX + wideW, y1 + 30 + rows * 42 + 8);
    GlassCard(c5);
    {
        float lo = 999, hi = -999; for (auto& d : w.days) { lo = std::min(lo, d.lo); hi = std::max(hi, d.hi); }
        for (int k = 0; k < (int)rows; k++) {
            auto& d = w.days[k];
            float yy = c5.top + 8 + k * 42;
            if (k) Fill(D2D1::RectF(c5.left + 14, yy, c5.right - 14, yy + 0.5f), Rgb(255, 255, 255, 0.22f));
            Text(k == 0 ? std::wstring(Tr(L"Hoje")) : std::wstring(WeekdayShort(WdayOf(d.t + 12 * 3600, w.off))), g_tfCondTitle, D2D1::RectF(c5.left + 14, yy, c5.left + 64, yy + 42), white);
            Text(CondEmoji(d.code, true), g_tfEmojiSmall, D2D1::RectF(c5.left + 64, yy, c5.left + 100, yy + 42), white);
            swprintf_s(t, L"%d°", (int)std::lround(d.lo)); Text(t, g_tfRowR, D2D1::RectF(c5.left + 100, yy, c5.left + 138, yy + 42), dim);
            float bx0 = c5.left + 146, bx1 = c5.right - 52, by = yy + 21;
            Fill(D2D1::RectF(bx0, by - 2.5f, bx1, by + 2.5f), Rgb(0, 0, 0, 0.16f), 2.5f);
            float span = std::max(1.f, hi - lo), a = bx0 + (d.lo - lo) / span * (bx1 - bx0), b = bx0 + (d.hi - lo) / span * (bx1 - bx0);
            D2D1_GRADIENT_STOP gs[2] = { { 0, Rgb(255, 196, 64) }, { 1, Rgb(255, 120, 40) } };
            ID2D1GradientStopCollection* st = nullptr; rt->CreateGradientStopCollection(gs, 2, &st);
            ID2D1LinearGradientBrush* lg = nullptr;
            if (st) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ a, 0 }, { std::max(a + 6, b), 0 }), st, &lg);
            if (lg) { rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a, by - 2.5f, std::max(a + 6, b), by + 2.5f), 2.5f, 2.5f), lg); lg->Release(); }
            if (st) st->Release();
            swprintf_s(t, L"%d°", (int)std::lround(d.hi)); Text(t, g_tfRowR, D2D1::RectF(c5.right - 50, yy, c5.right - 14, yy + 42), white);
        }
    }
    // linha 3: nascer e pôr do sol
    float y2 = y1 + 143 + 16;
    if (!w.days.empty()) {
        D2D1_RECT_F c6 = D2D1::RectF(x0, y2, x0 + sm, y2 + 141), c7 = D2D1::RectF(x0 + sm + 16, y2, x0 + 2 * sm + 16, y2 + 141);
        GlassCard(c6); CardHead(c6, Tr(L"NASCER DO SOL"));
        Text(HmOf(w.days[0].rise, w.off), g_tfBigG, D2D1::RectF(c6.left + 14, c6.top + 28, c6.right, c6.top + 62), white);
        GlassCard(c7); CardHead(c7, Tr(L"PÔR DO SOL"));
        Text(HmOf(w.days[0].set, w.off), g_tfBigG, D2D1::RectF(c7.left + 14, c7.top + 28, c7.right, c7.top + 62), white);
        for (D2D1_RECT_F c : { c6, c7 }) {  // a curva do sol
            br->SetColor(Rgb(255, 255, 255, 0.45f));
            ID2D1PathGeometry* g = nullptr; d2d->CreatePathGeometry(&g);
            if (g) {
                ID2D1GeometrySink* s = nullptr; g->Open(&s);
                float bx = c.left + 12, bw = c.right - c.left - 24, base = c.top + 112;
                s->BeginFigure({ bx, base + 6 }, D2D1_FIGURE_BEGIN_HOLLOW);
                for (int k = 1; k <= 30; k++) { float u = k / 30.f; s->AddLine({ bx + bw * u, base + 6 - (float)sin(u * 3.14159265) * 28 + 14 * (1 - (float)sin(u * 3.14159265)) * 0 }); }
                s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
                rt->DrawGeometry(g, br, 2); g->Release();
            }
            Fill(D2D1::RectF(c.left + 12, c.top + 112, c.right - 12, c.top + 112.6f), Rgb(255, 255, 255, 0.5f));
        }
    }
    // crédito do Open-Meteo, clicável
    float yc = std::max(c5.bottom, y2 + 141) + 14;
    D2D1_RECT_F credit = D2D1::RectF(x0, yc, x0 + std::min(GW, TextW(g_tfHdr, OpenMeteoCredit()) + 2), yc + 16);
    Text(OpenMeteoCredit(), g_tfHdr, credit, dim);
    if (credit.top >= 52) g_hits.push_back({ 4, 0, credit });  // escondido sob a barra: não clica
    g_gTotal = credit.bottom + g_gScroll + 20;
    rt->PopAxisAlignedClip();
}


void Paint() {
    if (!BeginPaint()) return;
    if (BAR > 0) { PaintGoldenWx(); EndPaint(); return; }  // macOS 27
    g_hits.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139), white = Rgb(255, 255, 255), dim = Rgb(255, 255, 255, 0.7f);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(242, 242, 245));
    const float SB = g_side ? SB_W : 0.f;
    D2D1_RECT_F tog = D2D1::RectF(10, 10, 38, 38);  // mostrar/esconder a lateral
    auto toggle = [&](D2D1_COLOR_F c) { wchar_t sg[2] = { 0xE700, 0 }; Text(sg, g_tfIcon, tog, c); g_hits.push_back({ 3, 0, tog }); };
    // ---- cidades
    if (g_side) {
    Fill(D2D1::RectF(0, 0, SB, sz.height), dark ? Rgb(40, 40, 42) : Rgb(236, 236, 240));
    toggle(text);
    D2D1_RECT_F add = D2D1::RectF(SB - 40, 10, SB - 10, 38);
    wchar_t p[2] = { 0xE710, 0 }; Text(p, g_tfIcon, add, text);
    g_hits.push_back({ 2, 0, add });
    float y = 48;
    for (int i = 0; i < (int)g_places.size(); i++) {
        auto& pl = g_places[i];
        D2D1_RECT_F r = D2D1::RectF(8, y, SB - 8, y + 70);
        if (pl.wx.ok) {
            D2D1_COLOR_F a, b; Sky(pl.wx, a, b);
            ID2D1GradientStopCollection* st = nullptr; D2D1_GRADIENT_STOP gs[2] = { { 0, a }, { 1, b } };
            rt->CreateGradientStopCollection(gs, 2, &st);
            ID2D1LinearGradientBrush* lg = nullptr;
            if (st) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ r.left, r.top }, { r.right, r.bottom }), st, &lg);
            if (lg) { rt->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), lg); lg->Release(); }
            if (st) st->Release();
        } else Fill(r, dark ? Rgb(58, 58, 60) : Rgb(255, 255, 255), 12);
        D2D1_COLOR_F tc = pl.wx.ok ? white : text;
        if (i == g_sel) { br->SetColor(Rgb(10, 132, 255)); rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 12, 12), br, 2.5f); }
        Text(i == 0 ? std::wstring(Tr(L"Meu Local")) : pl.name, g_tfSide, D2D1::RectF(r.left + 12, r.top + 8, r.right - 60, r.top + 30), tc);
        if (i == 0) Text(pl.name, g_tfSmall, D2D1::RectF(r.left + 12, r.top + 28, r.right - 60, r.top + 46), pl.wx.ok ? dim : gray);
        if (pl.wx.ok) {
            Text(CondText(pl.wx.code), g_tfSmall, D2D1::RectF(r.left + 12, r.bottom - 24, r.right - 60, r.bottom - 6), dim);
            wchar_t t[16]; swprintf_s(t, L"%d°", (int)std::lround(pl.wx.temp));
            Text(t, g_tfSideTemp, D2D1::RectF(r.right - 70, r.top + 4, r.right - 10, r.top + 44), white);
        }
        g_hits.push_back({ 1, i, r });
        y += 78;
    }
    if (!g_status.empty()) Text(g_status, g_tfSmall, D2D1::RectF(12, sz.height - 30, SB - 10, sz.height - 8), gray);
    }  // g_side
    // ---- cidade escolhida
    if (g_sel < 0 || g_sel >= (int)g_places.size()) { if (!g_side) toggle(text); EndPaint(); return; }
    Place pl; { std::lock_guard<std::mutex> l(g_mx); pl = g_places[g_sel]; }
    D2D1_RECT_F main = D2D1::RectF(SB, 0, sz.width, sz.height);
    if (!pl.wx.ok) { Text(pl.loading ? Tr(L"Carregando…") : Tr(L"Sem conexão com o serviço do tempo."), g_tfCond, main, gray); if (!g_side) toggle(text); EndPaint(); return; }
    const Wx& w = pl.wx;
    {
        D2D1_COLOR_F a, b; Sky(w, a, b);
        ID2D1GradientStopCollection* st = nullptr; D2D1_GRADIENT_STOP gs[2] = { { 0, a }, { 1, b } };
        rt->CreateGradientStopCollection(gs, 2, &st);
        ID2D1LinearGradientBrush* lg = nullptr;
        if (st) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, sz.height }), st, &lg);
        if (lg) { rt->FillRectangle(main, lg); lg->Release(); }
        if (st) st->Release();
    }
    if (!g_side) toggle(white);
    float cx = (SB + sz.width) / 2, W = sz.width - SB;
    Text(g_sel == 0 ? std::wstring(Tr(L"Meu Local")) : pl.name, g_tfCity, D2D1::RectF(SB, 24, sz.width, 62), white);
    if (g_sel == 0) Text(pl.name, g_tfSmallC, D2D1::RectF(SB, 58, sz.width, 76), dim);
    wchar_t t[32]; swprintf_s(t, L" %d°", (int)std::lround(w.temp));
    Text(t, g_tfTemp, D2D1::RectF(SB, 70, sz.width, 170), white);
    Text(CondText(w.code), g_tfCond, D2D1::RectF(SB, 168, sz.width, 194), white);
    if (!w.days.empty()) { swprintf_s(t, Tr(L"Máx.: %d°  Mín.: %d°"), (int)std::lround(w.days[0].hi), (int)std::lround(w.days[0].lo)); Text(t, g_tfCond, D2D1::RectF(SB, 192, sz.width, 218), white); }
    float pad = 20, y0 = 236;
    // por hora
    D2D1_RECT_F hr = D2D1::RectF(SB + pad, y0, sz.width - pad, y0 + 124);
    Card(hr);
    Text(Tr(L"PREVISÃO POR HORA"), g_tfSmall, D2D1::RectF(hr.left + 14, hr.top + 6, hr.right, hr.top + 26), dim);
    time_t now = time(nullptr);
    int start = 0; for (int i = 0; i < (int)w.hours.size(); i++) if (w.hours[i].t <= now) start = i;
    int n = std::min(24, (int)w.hours.size() - start);
    float cw = (hr.right - hr.left - 20) / std::max(1, std::min(n, (int)((hr.right - hr.left - 20) / 52)));
    int vis = (int)((hr.right - hr.left - 20) / cw);
    for (int k = 0; k < n && k < vis; k++) {
        auto& h = w.hours[start + k];
        float x = hr.left + 10 + k * cw;
        Text(k == 0 ? std::wstring(Tr(L"Agora")) : HourOf(h.t, w.off), g_tfCard, D2D1::RectF(x, hr.top + 30, x + cw, hr.top + 50), white);
        Text(CondEmoji(h.code, h.day), g_tfEmojiSmall, D2D1::RectF(x, hr.top + 52, x + cw, hr.top + 84), white);
        swprintf_s(t, L"%d°", (int)std::lround(h.temp));
        Text(t, g_tfCardBig, D2D1::RectF(x, hr.top + 88, x + cw, hr.top + 114), white);
    }
    // 10 dias
    float colW = (W - 3 * pad) / 2;
    D2D1_RECT_F dr = D2D1::RectF(SB + pad, hr.bottom + pad, SB + pad + colW, sz.height - pad);
    Card(dr);
    Text(Tr(L"PREVISÃO PARA 10 DIAS"), g_tfSmall, D2D1::RectF(dr.left + 14, dr.top + 6, dr.right, dr.top + 26), dim);
    float lo = 999, hi = -999; for (auto& d : w.days) { lo = std::min(lo, d.lo); hi = std::max(hi, d.hi); }
    float rowH = std::min(44.f, (dr.bottom - dr.top - 34) / std::max<size_t>(1, w.days.size()));
    for (int k = 0; k < (int)w.days.size(); k++) {
        auto& d = w.days[k];
        float yy = dr.top + 30 + k * rowH;
        if (yy + rowH > dr.bottom) break;
        Text(k == 0 ? std::wstring(Tr(L"Hoje")) : std::wstring(WeekdayShort(WdayOf(d.t + 12 * 3600, w.off))), g_tfCard, D2D1::RectF(dr.left + 14, yy, dr.left + 70, yy + rowH), white);
        Text(CondEmoji(d.code, true), g_tfEmojiSmall, D2D1::RectF(dr.left + 64, yy, dr.left + 100, yy + rowH), white);
        swprintf_s(t, L"%d°", (int)std::lround(d.lo)); Text(t, g_tfRowR, D2D1::RectF(dr.left + 96, yy, dr.left + 136, yy + rowH), dim);
        float bx0 = dr.left + 146, bx1 = dr.right - 56, by = yy + rowH / 2;
        Fill(D2D1::RectF(bx0, by - 2.5f, bx1, by + 2.5f), Rgb(0, 0, 0, 0.18f), 2.5f);
        float span = std::max(1.f, hi - lo), a = bx0 + (d.lo - lo) / span * (bx1 - bx0), b = bx0 + (d.hi - lo) / span * (bx1 - bx0);
        Fill(D2D1::RectF(a, by - 2.5f, std::max(a + 6, b), by + 2.5f), Rgb(255, 196, 64), 2.5f);
        swprintf_s(t, L"%d°", (int)std::lround(d.hi)); Text(t, g_tfRowR, D2D1::RectF(dr.right - 52, yy, dr.right - 12, yy + rowH), white);
    }
    // quadros
    float qx = dr.right + pad, qw = (sz.width - pad - qx - pad) / 2, qh = std::min(150.f, (dr.bottom - dr.top - pad) / 2);
    auto quad = [&](int i, const wchar_t* title, const std::wstring& big, const std::wstring& sub) {
        float x = qx + (i % 2) * (qw + pad), yy = dr.top + (i / 2) * (qh + pad);
        D2D1_RECT_F r = D2D1::RectF(x, yy, x + qw, yy + qh);
        Card(r);
        Text(title, g_tfSmall, D2D1::RectF(r.left + 14, r.top + 6, r.right, r.top + 26), dim);
        Text(big, g_tfCity, D2D1::RectF(r.left + 14, r.top + 30, r.right - 8, r.top + 70), white);
        IDWriteTextFormat* f = g_tfSmall; if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        Text(sub, g_tfSmall, D2D1::RectF(r.left + 14, r.top + 72, r.right - 10, r.bottom - 8), white);
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    if (g_tfCity) g_tfCity->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    swprintf_s(t, L"%d°", (int)std::lround(w.feels));
    quad(0, Tr(L"SENSAÇÃO"), t, w.feels < w.temp - 1 ? Tr(L"O vento deixa mais frio do que a temperatura real.") : w.feels > w.temp + 1 ? Tr(L"A umidade deixa mais quente do que a temperatura real.") : Tr(L"Parecida com a temperatura real."));
    swprintf_s(t, L"%d%%", (int)std::lround(w.hum));
    quad(1, Tr(L"UMIDADE"), t, L"");
    swprintf_s(t, L"%d km/h", (int)std::lround(w.wind));
    quad(2, Tr(L"VENTO"), t, L"");
    if (!w.days.empty()) quad(3, Tr(L"NASCER DO SOL"), HmOf(w.days[0].rise, w.off), std::wstring(Tr(L"Pôr do sol: ")) + HmOf(w.days[0].set, w.off));
    if (g_tfCity) g_tfCity->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    // crédito do Open-Meteo, clicável, na margem de baixo
    D2D1_RECT_F credit = D2D1::RectF(SB + pad, sz.height - pad + 2, SB + pad + TextW(g_tfSmall, OpenMeteoCredit()) + 2, sz.height - 2);
    Text(OpenMeteoCredit(), g_tfSmall, credit, gray);
    g_hits.push_back({ 4, 0, credit });
    (void)cx;
    EndPaint();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); return 0;
    case WM_DPICHANGED: OnDpi(w, l); return 0;
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_CTLCOLOREDIT:
        if (BAR > 0) { LRESULT r = FieldColors((HDC)w, RGB(60, 110, 170)); SetTextColor((HDC)w, RGB(255, 255, 255)); return r; }
        return FieldColors((HDC)w, Dark() ? RGB(40, 40, 42) : RGB(236, 236, 240));
    case WM_FIELDLOST: if (field.h && GetFocus() != field.h) FieldEnd(0); return 0;
    case WM_DATA: {
        int i = (int)w;
        if (i == -1) {  // cidade procurada
            auto* p = (Place*)l;
            if (!p->name.empty()) { g_places.push_back(*p); g_sel = (int)g_places.size() - 1; Save(); Fetch(g_sel); g_status.clear(); }
            else g_status = Tr(L"Cidade não encontrada.");
            delete p;
        } else {
            auto* wx = (Wx*)l;
            if (i >= 0 && i < (int)g_places.size()) { std::lock_guard<std::mutex> lk(g_mx); g_places[i].loading = false; if (wx->ok) g_places[i].wx = *wx; }
            delete wx;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER: for (int i = 0; i < (int)g_places.size(); i++) Fetch(i); return 0;  // a cada 15 min
    case WM_MOUSEWHEEL: if (BAR > 0) {  // macOS 27: o conteúdo rola
        g_gScroll = std::clamp(g_gScroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 60, 0.f, std::max(0.f, g_gTotal - Size().height));
        InvalidateRect(h, nullptr, FALSE);
    } return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONUP: {
        if (field.h) FieldEnd(0);
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        for (auto& ht : g_hits) if (In(ht.r, x, y)) {
            if (ht.kind == 3) { g_side = !g_side; InvalidateRect(h, nullptr, FALSE); return 0; }
            if (ht.kind == 4) { if (m == WM_LBUTTONDOWN) OpenMeteoSite(); return 0; }
            if (ht.kind == 2) {
                if (!g_side) { g_side = true; Paint(); }  // a busca fica na lateral
                D2D1_RECT_F fr = BAR > 0 ? D2D1::RectF(38, 71, 256, 89) : D2D1::RectF(12, 12, SB_W - 46, 36);
                FieldOpen(fr, L"", 13, false, [](const std::wstring& t, int key) { if (key == VK_RETURN) Search(t); });
                return 0;
            }
            if (ht.kind == 1) {
                if (m == WM_RBUTTONUP) {
                    POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
                    HMENU mm = CreatePopupMenu();
                    if (ht.arg > 0) AppendMenuW(mm, MF_STRING, 2, Tr(L"Usar como Meu Local"));
                    AppendMenuW(mm, MF_STRING | (g_places.size() > 1 ? 0 : MF_GRAYED), 1, Tr(L"Apagar"));
                    int k = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr);
                    DestroyMenu(mm);
                    if (k == 2) { std::lock_guard<std::mutex> lk(g_mx); std::swap(g_places[0], g_places[ht.arg]); SetMyPlace(g_places[0]); g_sel = 0; Save(); }
                    else if (k == 1 && g_places.size() > 1) {
                        { std::lock_guard<std::mutex> lk(g_mx); g_places.erase(g_places.begin() + ht.arg); }
                        if (ht.arg == 0) SetMyPlace(g_places[0]);  // o "Meu Local" saiu: a próxima cidade fica no lugar
                        g_sel = 0; Save();
                    }
                } else g_sel = ht.arg;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && w == 'W') DestroyWindow(h);
        if (GetKeyState(VK_CONTROL) < 0 && w == 'R') for (int i = 0; i < (int)g_places.size(); i++) Fetch(i);
        if (w == VK_UP && g_sel > 0) { g_sel--; InvalidateRect(h, nullptr, FALSE); }
        if (w == VK_DOWN && g_sel + 1 < (int)g_places.size()) { g_sel++; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_file = DataPath(L"clima.txt");
    Load();
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfCity = Fmt(30, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, false, disp.c_str());
    g_tfTemp = Fmt(92, DWRITE_FONT_WEIGHT_THIN, DWRITE_TEXT_ALIGNMENT_CENTER, false, disp.c_str());
    g_tfCond = Fmt(18, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfSmall = Fmt(12, DWRITE_FONT_WEIGHT_MEDIUM);
    g_tfSmallC = Fmt(13, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfCard = Fmt(14, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfCardBig = Fmt(18, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfRowR = Fmt(16, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfEmojiSmall = nullptr; dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"", &g_tfEmojiSmall);
    if (g_tfEmojiSmall) { g_tfEmojiSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g_tfEmojiSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    g_tfSide = Fmt(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfSideTemp = Fmt(30, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_TRAILING, false, disp.c_str());
    g_tfIcon = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfCityG = Fmt(22, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, false, disp.c_str());
    g_tfTempG = Fmt(64, DWRITE_FONT_WEIGHT_THIN, DWRITE_TEXT_ALIGNMENT_CENTER, false, disp.c_str());
    g_tfCondG = Fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfHdr = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfHdrC = Fmt(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfBigG = Fmt(26, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfSmallW = Fmt(11.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    if (g_tfSmallW) g_tfSmallW->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_tfSearchG = Fmt(14);
    g_tfCondTitle = Fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfCardG = Fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    barWanted = 52;  // macOS 27: o céu atrás da barra
    if (!CreateMain(hi, L"ClimaJanela", Tr(L"Clima"), Proc, semaforo::Aqua() ? 1070 : 1080, semaforo::Aqua() ? 768 : 760)) return 1;
    for (int i = 0; i < (int)g_places.size(); i++) Fetch(i);
    SetTimer(wnd, 1, 15 * 60 * 1000, nullptr);
    ShowWindow(wnd, show);
    return Run();
}
