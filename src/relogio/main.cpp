// Relógio (Clock do Mac): Relógio Mundial, Alarmes, Cronômetro e Timer, nas abas do alto (Ctrl+1..4).
//  - Mundial: cidades (fusos do Windows), hora, "Hoje/Amanhã" e a diferença; + adiciona, botão direito remove.
//  - Alarmes: hora grande, nome, dias e o interruptor; + cria, clique edita a hora, botão direito apaga/renomeia.
//    Quem toca é o Shell (banner do Mac + som), mesmo com o app fechado: %APPDATA%\Shell\relogio.txt. Sem o Shell
//    (modo só apps), o próprio app toca enquanto estiver aberto.
//  - "Outra Cidade…" no +: qualquer cidade pelo nome (Open-Meteo), com o fuso convertido pelo ICU do Windows.
//  - Cronômetro: Iniciar/Parar, Volta/Zerar e a lista de voltas (a melhor em verde, a pior em vermelho).
//  - Timer: escolher minutos (atalhos e − / +), Iniciar/Pausar/Cancelar; ao acabar toca e a janela vem para frente.
#include "../comum/app2d.h"
#include <mmsystem.h>
#include <thread>
#include <winhttp.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "winhttp.lib")

using namespace app2d;

namespace {

float TB = 56;  // macOS 27: 52 (barra unificada)
const UINT WM_FIELDLOST = WM_APP + 77, WM_CITYFOUND = WM_APP + 8;
HWND g_cityField;  // o campo do "Outra Cidade…" enquanto aberto (o crédito do Open-Meteo aparece embaixo dele)
int g_tab = 0;  // 0 mundial, 1 alarmes, 2 cronômetro, 3 timer
IDWriteTextFormat *g_tfSeg, *g_tfBig, *g_tfBigR, *g_tfHuge, *g_tfRow, *g_tfSub, *g_tfBtn, *g_tfIcon, *g_tfMono;

// ---- relógio mundial (fusos do Windows por cidade)
struct City { const wchar_t* name; const wchar_t* tz; };
const City CITIES[] = {
    { L"São Paulo", L"E. South America Standard Time" }, { L"Rio de Janeiro", L"E. South America Standard Time" }, { L"Manaus", L"SA Western Standard Time" },
    { L"Fortaleza", L"SA Eastern Standard Time" }, { L"Lisboa", L"GMT Standard Time" }, { L"Londres", L"GMT Standard Time" }, { L"Paris", L"Romance Standard Time" },
    { L"Berlim", L"W. Europe Standard Time" }, { L"Madri", L"Romance Standard Time" }, { L"Roma", L"W. Europe Standard Time" }, { L"Atenas", L"GTB Standard Time" },
    { L"Istambul", L"Turkey Standard Time" }, { L"Moscou", L"Russian Standard Time" }, { L"Dubai", L"Arabian Standard Time" }, { L"Mumbai", L"India Standard Time" },
    { L"Singapura", L"Singapore Standard Time" }, { L"Pequim", L"China Standard Time" }, { L"Tóquio", L"Tokyo Standard Time" }, { L"Seul", L"Korea Standard Time" },
    { L"Sydney", L"AUS Eastern Standard Time" }, { L"Auckland", L"New Zealand Standard Time" }, { L"Honolulu", L"Hawaiian Standard Time" },
    { L"Los Angeles", L"Pacific Standard Time" }, { L"Denver", L"Mountain Standard Time" }, { L"Chicago", L"Central Standard Time" },
    { L"Nova York", L"Eastern Standard Time" }, { L"Cidade do México", L"Central Standard Time (Mexico)" }, { L"Bogotá", L"SA Pacific Standard Time" },
    { L"Buenos Aires", L"Argentina Standard Time" }, { L"Santiago", L"Pacific SA Standard Time" }, { L"Joanesburgo", L"South Africa Standard Time" },
    { L"Cairo", L"Egypt Standard Time" }, { L"Nairóbi", L"E. Africa Standard Time" }, { L"UTC", L"UTC" },
};
const int NCITIES = (int)(sizeof(CITIES) / sizeof(CITIES[0]));
// cidades que o usuário procurou pelo nome (Outra Cidade…): no g_world como 1000 + índice aqui
struct Custom { std::wstring name, tz; };
std::vector<Custom> g_custom;
const int CUSTOM0 = 1000;
std::vector<int> g_world;  // índices em CITIES (ou CUSTOM0 + índice em g_custom)
const wchar_t* CityTz(int i) { return i >= CUSTOM0 ? g_custom[i - CUSTOM0].tz.c_str() : CITIES[i].tz; }
std::wstring CityName(int i) { return i >= CUSTOM0 ? g_custom[i - CUSTOM0].name : std::wstring(Tr(CITIES[i].name)); }

bool CityTime(int i, SYSTEMTIME& out) {
    DYNAMIC_TIME_ZONE_INFORMATION d{};
    for (DWORD k = 0; EnumDynamicTimeZoneInformation(k, &d) == ERROR_SUCCESS; k++)
        if (!wcscmp(d.TimeZoneKeyName, CityTz(i))) {
            SYSTEMTIME utc; GetSystemTime(&utc);
            return SystemTimeToTzSpecificLocalTimeEx(&d, &utc, &out) != FALSE;
        }
    return false;
}

// ---- alarmes
struct Alarm { int id; int hh, mm; bool on; std::wstring label; int days; };  // days: bits 0 = dom ... 6 = sáb; 0 = uma vez
std::vector<Alarm> g_alarms;
int g_nextId = 1;
std::wstring g_file;

void Save() {
    std::vector<std::wstring> out{ L"# Relógio do Liquinix: W = cidade do Relógio Mundial, C = cidade própria (nome, fuso do Windows), A = alarme (id, hora, minuto, ligado, nome, dias)" };
    for (auto& c : g_custom) out.push_back(L"C\t" + Esc(c.name) + L"\t" + c.tz);
    for (int i : g_world) out.push_back(L"W\t" + std::to_wstring(i));
    for (auto& a : g_alarms) out.push_back(L"A\t" + std::to_wstring(a.id) + L"\t" + std::to_wstring(a.hh) + L"\t" + std::to_wstring(a.mm) + L"\t" + (a.on ? L"1" : L"0") + L"\t" + Esc(a.label) + L"\t" + std::to_wstring(a.days));
    WriteLines(g_file, out);
}
void Load() {
    g_world.clear(); g_alarms.clear(); g_custom.clear();
    bool any = false;
    auto lines = ReadLines(g_file);
    for (auto& l : lines) { auto f = Split(l); if (f.size() >= 3 && f[0] == L"C") g_custom.push_back({ Unesc(f[1]), f[2] }); }
    for (auto& l : lines) {
        auto f = Split(l);
        if (f.size() >= 2 && f[0] == L"W") { int i = _wtoi(f[1].c_str()); if ((i >= 0 && i < NCITIES) || (i >= CUSTOM0 && i - CUSTOM0 < (int)g_custom.size())) g_world.push_back(i); any = true; }
        else if (f.size() >= 7 && f[0] == L"A") g_alarms.push_back({ _wtoi(f[1].c_str()), _wtoi(f[2].c_str()), _wtoi(f[3].c_str()), f[4] == L"1", Unesc(f[5]), _wtoi(f[6].c_str()) });
    }
    if (!any && g_world.empty()) g_world = { 0, 5, 25, 17 };  // São Paulo, Londres, Nova York, Tóquio
    for (auto& a : g_alarms) g_nextId = std::max(g_nextId, a.id + 1);
}

// ---- cronômetro
bool g_swRun = false;
ULONGLONG g_swStart = 0, g_swAcc = 0;
std::vector<ULONGLONG> g_laps;  // duração de cada volta
ULONGLONG SwNow() { return g_swAcc + (g_swRun ? GetTickCount64() - g_swStart : 0); }
std::wstring FmtTime(ULONGLONG ms, bool cs = true) {
    wchar_t b[32];
    ULONGLONG s = ms / 1000;
    if (s >= 3600) swprintf_s(b, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else if (cs) swprintf_s(b, L"%02llu:%02llu,%02llu", s / 60, s % 60, ms / 10 % 100);
    else swprintf_s(b, L"%02llu:%02llu", s / 60, s % 60);
    return b;
}

// ---- timer
int g_tmMin = 5;               // escolhido
bool g_tmRun = false, g_tmPaused = false;
ULONGLONG g_tmEnd = 0, g_tmLeft = 0;  // fim (tick) / restante quando pausado
ULONGLONG TmLeft() { if (!g_tmRun) return (ULONGLONG)g_tmMin * 60000; if (g_tmPaused) return g_tmLeft; ULONGLONG n = GetTickCount64(); return g_tmEnd > n ? g_tmEnd - n : 0; }

struct Hit { int kind, arg; D2D1_RECT_F r; };
std::vector<Hit> g_hits;
enum { H_TAB = 1, H_ADD, H_WORLD, H_ALARM, H_ALARMTIME, H_SWITCH, H_SW_A, H_SW_B, H_TM_PRESET, H_TM_MINUS, H_TM_PLUS, H_TM_A, H_TM_B };

void Button(D2D1_RECT_F r, const std::wstring& t, D2D1_COLOR_F bg, D2D1_COLOR_F fg, int kind, int arg = 0) {
    float rad = (r.bottom - r.top) / 2;
    Fill(r, bg, rad);
    Text(t, g_tfBtn, r, fg);
    g_hits.push_back({ kind, arg, r });
}

// ---- macOS 27 (captura Clock do 512 Pixels, 2x, medida em pt): abas num controle de vidro no meio da barra de 52
// e o "+" redondo à direita; o mapa-múndi ocupa a largura toda logo abaixo (equirretangular, 360° na largura,
// do polo norte até ~-86°), continentes cinza (63) sobre o preto, a parte da noite escurecida (25) e contornada por
// uma linha clara, meridianos de 15° bem fracos; as cidades num ponto laranja com o nome em negrito e a hora.
// Embaixo, cartões de 170 × 180 com o relógio analógico (mostrador branco de dia, preto de noite).
#include "mapa_mundi.inc"
struct LatLon { double lat, lon; };
const LatLon COORDS[] = {
    { -23.55, -46.63 }, { -22.91, -43.17 }, { -3.12, -60.02 }, { -3.73, -38.53 }, { 38.72, -9.14 }, { 51.51, -0.13 }, { 48.86, 2.35 },
    { 52.52, 13.40 }, { 40.42, -3.70 }, { 41.90, 12.50 }, { 37.98, 23.73 }, { 41.01, 28.98 }, { 55.76, 37.62 }, { 25.20, 55.27 }, { 19.08, 72.88 },
    { 1.35, 103.82 }, { 39.90, 116.41 }, { 35.68, 139.69 }, { 37.57, 126.98 }, { -33.87, 151.21 }, { -36.85, 174.76 }, { 21.31, -157.86 },
    { 34.05, -118.24 }, { 39.74, -104.99 }, { 41.88, -87.63 }, { 40.71, -74.01 }, { 19.43, -99.13 }, { 4.71, -74.07 }, { -34.60, -58.38 },
    { -33.45, -70.67 }, { -26.20, 28.05 }, { 30.04, 31.24 }, { -1.29, 36.82 }, { 51.48, 0.0 },
};
ID2D1Bitmap* g_map; ID2D1RenderTarget* g_mapRt;
IDWriteTextFormat *g_tfCityB, *g_tfCityT, *g_tfDial, *g_tfCard, *g_tfCardSub, *g_tfCardSubL;

ID2D1Bitmap* MapBitmap() {
    if (g_mapRt != rt && g_map) { g_map->Release(); g_map = nullptr; }
    if (g_map) return g_map;
    std::vector<BYTE> px((size_t)MAPA_W * MAPA_H * 4, 0);
    for (int y = 0; y < MAPA_H; y++)
        for (int x = 0; x < MAPA_W; x++)
            if (MAPA_BITS[y * (MAPA_W / 8) + x / 8] & (0x80 >> (x % 8))) { BYTE* p = &px[((size_t)y * MAPA_W + x) * 4]; p[0] = p[1] = p[2] = 63; p[3] = 255; }
    rt->CreateBitmap(D2D1::SizeU(MAPA_W, MAPA_H), px.data(), MAPA_W * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &g_map);
    g_mapRt = rt;
    return g_map;
}

// ponto do sol a pino agora (declinação e longitude), aproximado (sem a equação do tempo)
void SubSolar(double& decl, double& lon) {
    SYSTEMTIME u; GetSystemTime(&u);
    FILETIME f; SystemTimeToFileTime(&u, &f);
    SYSTEMTIME j{ u.wYear, 1, 0, 1, 0, 0, 0, 0 }; FILETIME fj; SystemTimeToFileTime(&j, &fj);
    double day = (((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) - ((ULONGLONG)fj.dwHighDateTime << 32 | fj.dwLowDateTime)) / 864e9;
    decl = 23.44 * sin(2 * 3.14159265 * (284 + day) / 365.0);
    double h = u.wHour + u.wMinute / 60.0 + u.wSecond / 3600.0;
    lon = -15 * (h - 12);
}

// nascer e pôr do sol (hora local da cidade, em horas; false nos dias sem nascer ou sem pôr, perto dos polos):
// declinação do dia, ângulo horário do nascer (-0,83° pela refração e o raio do sol) e a equação do tempo
bool SunTimes(double lat, double lon, double utcOffsetH, double& rise, double& set) {
    const double PI = 3.14159265358979;
    SYSTEMTIME u; GetSystemTime(&u);
    SYSTEMTIME j{ u.wYear, 1, 0, 1, 0, 0, 0, 0 }; FILETIME fu, fj; SystemTimeToFileTime(&u, &fu); SystemTimeToFileTime(&j, &fj);
    double n = (((ULONGLONG)fu.dwHighDateTime << 32 | fu.dwLowDateTime) - ((ULONGLONG)fj.dwHighDateTime << 32 | fj.dwLowDateTime)) / 864e9 + 1;
    double decl = 23.44 * sin(2 * PI * (284 + n) / 365.0) * PI / 180;
    double B = 2 * PI * (n - 81) / 364, eot = 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B);
    double c = (sin(-0.83 * PI / 180) - sin(lat * PI / 180) * sin(decl)) / (cos(lat * PI / 180) * cos(decl));
    if (c < -1 || c > 1) return false;
    double h0 = acos(c) * 180 / PI / 15;  // horas
    double noon = 12 - lon / 15 - eot / 60 + utcOffsetH;
    rise = fmod(noon - h0 + 48, 24); set = fmod(noon + h0 + 48, 24);
    return true;
}

void PaintWorldGolden(D2D1_SIZE_F sz, bool dark) {
    const double PI = 3.14159265358979;
    float mapH = sz.width * MAPA_H / (float)MAPA_W, top = TB;
    Fill(D2D1::RectF(0, top, sz.width, top + mapH), Rgb(0, 0, 0));
    if (ID2D1Bitmap* b = MapBitmap()) rt->DrawBitmap(b, D2D1::RectF(0, top, sz.width, top + mapH), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    auto X = [&](double lon) { return (float)((lon + 180) / 360 * sz.width); };
    double latBottom = MAPA_LAT_BAIXO;
    auto Y = [&](double lat) { return (float)(top + (90 - lat) / (90 - latBottom) * mapH); };
    for (int k = 1; k < 24; k++) Fill(D2D1::RectF(k * sz.width / 24, top, k * sz.width / 24 + 0.5f, top + mapH), Rgb(255, 255, 255, 0.06f));
    // noite
    double decl, slon; SubSolar(decl, slon);
    if (fabs(decl) < 0.1) decl = decl < 0 ? -0.1 : 0.1;
    ID2D1PathGeometry* g = nullptr; d2d->CreatePathGeometry(&g);
    ID2D1PathGeometry* line = nullptr; d2d->CreatePathGeometry(&line);
    if (g && line) {
        ID2D1GeometrySink *s = nullptr, *ls = nullptr; g->Open(&s); line->Open(&ls);
        bool northLit = decl > 0;
        float edge = northLit ? top + mapH : top;
        int N = 180;
        for (int i = 0; i <= N; i++) {
            double lon = -180 + 360.0 * i / N, H = (lon - slon) * PI / 180;
            double lat = atan(-cos(H) / tan(decl * PI / 180)) * 180 / PI;
            D2D1_POINT_2F p{ X(lon), std::clamp(Y(lat), top, top + mapH) };
            if (!i) { s->BeginFigure({ p.x, edge }, D2D1_FIGURE_BEGIN_FILLED); ls->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW); }
            s->AddLine(p);
            if (i) ls->AddLine(p);
        }
        s->AddLine({ sz.width, edge });
        s->EndFigure(D2D1_FIGURE_END_CLOSED); ls->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close(); ls->Close(); s->Release(); ls->Release();
        rt->PushAxisAlignedClip(D2D1::RectF(0, top, sz.width, top + mapH), D2D1_ANTIALIAS_MODE_ALIASED);
        br->SetColor(Rgb(0, 0, 0, 0.6f)); rt->FillGeometry(g, br);
        br->SetColor(Rgb(255, 255, 255, 0.55f)); rt->DrawGeometry(line, br, 1);
        rt->PopAxisAlignedClip();
    }
    if (g) g->Release();
    if (line) line->Release();
    // cidades
    for (int k = 0; k < (int)g_world.size(); k++) {
        int i = g_world[k];
        if (i < 0 || i >= (int)(sizeof(COORDS) / sizeof(COORDS[0]))) continue;
        SYSTEMTIME t{}; if (!CityTime(i, t)) continue;
        float x = X(COORDS[i].lon), y = Y(COORDS[i].lat);
        Circle({ x, y }, 4.2f, Rgb(255, 255, 255, 0.9f), true);
        Circle({ x, y }, 3.2f, Rgb(255, 149, 0), true);
        wchar_t hm[24]; swprintf_s(hm, L"%d:%02d", t.wHour, t.wMinute);
        Text(CityName(i), g_tfCityB, D2D1::RectF(x + 7, y - 9, x + 160, y + 5), Rgb(255, 255, 255));
        Text(hm, g_tfCityT, D2D1::RectF(x + 7, y + 4, x + 160, y + 15), Rgb(255, 255, 255, 0.85f));
    }
    // cartões com o relógio analógico
    SYSTEMTIME here; GetLocalTime(&here);
    float cy0 = top + mapH + 14;
    for (int k = 0; k < (int)g_world.size(); k++) {
        int i = g_world[k];
        SYSTEMTIME t{}; if (!CityTime(i, t)) continue;
        float x0 = 14 + k * 184.f;
        if (x0 + 170 > sz.width) break;
        D2D1_RECT_F card = D2D1::RectF(x0, cy0, x0 + 170, cy0 + 196);  // macOS 27: com a linha do nascer e pôr do sol
        Fill(card, dark ? Rgb(255, 255, 255, 0.08f) : Rgb(0, 0, 0, 0.05f), 12);
        bool day = t.wHour >= 6 && t.wHour < 18;
        D2D1_POINT_2F c{ x0 + 85, cy0 + 72 };
        Circle(c, 55, day ? Rgb(255, 255, 255) : Rgb(0, 0, 0), true);
        if (!day) { br->SetColor(Rgb(255, 255, 255, 0.25f)); rt->DrawEllipse(D2D1::Ellipse(c, 55, 55), br, 0.75f); }
        D2D1_COLOR_F ink = day ? Rgb(0, 0, 0) : Rgb(255, 255, 255);
        for (int n = 1; n <= 12; n++) {
            double a = n * PI / 6;
            float nx = c.x + (float)sin(a) * 43, ny = c.y - (float)cos(a) * 43;
            Text(std::to_wstring(n), g_tfDial, D2D1::RectF(nx - 10, ny - 8, nx + 10, ny + 8), ink);
        }
        double hm = (t.wHour % 12 + t.wMinute / 60.0) * PI / 6, mm = (t.wMinute + t.wSecond / 60.0) * PI / 30, ss = t.wSecond * PI / 30;
        br->SetColor(ink);
        rt->DrawLine(c, { c.x + (float)sin(hm) * 26, c.y - (float)cos(hm) * 26 }, br, 3.2f);
        rt->DrawLine(c, { c.x + (float)sin(mm) * 40, c.y - (float)cos(mm) * 40 }, br, 2.4f);
        br->SetColor(Rgb(255, 149, 0));
        rt->DrawLine({ c.x - (float)sin(ss) * 8, c.y + (float)cos(ss) * 8 }, { c.x + (float)sin(ss) * 46, c.y - (float)cos(ss) * 46 }, br, 1);
        Circle(c, 3, Rgb(255, 149, 0), true);
        Circle(c, 1.4f, day ? Rgb(255, 255, 255) : Rgb(0, 0, 0), true);
        wchar_t nm[96]; swprintf_s(nm, L"%ls, %d:%02d", CityName(i).c_str(), t.wHour, t.wMinute);
        Text(nm, g_tfCard, D2D1::RectF(x0 + 6, cy0 + 136, x0 + 164, cy0 + 154), dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31));
        FILETIME a, b2; SystemTimeToFileTime(&here, &a); SystemTimeToFileTime(&t, &b2);
        long long diffMin = ((long long)(((ULONGLONG)b2.dwHighDateTime << 32 | b2.dwLowDateTime) - ((ULONGLONG)a.dwHighDateTime << 32 | a.dwLowDateTime))) / 600000000LL;
        long long dh = (diffMin + (diffMin >= 0 ? 15 : -15)) / 30;
        std::wstring dayS = t.wDay == here.wDay ? Tr(L"Hoje") : (diffMin > 0 ? Tr(L"Amanhã") : Tr(L"Ontem"));
        wchar_t off[32] = L"";
        if (dh % 2) swprintf_s(off, L", %+lld h 30", dh / 2); else if (dh) swprintf_s(off, L", %+lld h", dh / 2);
        Text(dayS + off, g_tfCardSub, D2D1::RectF(x0 + 6, cy0 + 154, x0 + 164, cy0 + 170), dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139));
        {   // ⤒ nascer  ⤓ pôr (sol pela metade com a seta), como na captura
            SYSTEMTIME ut; GetSystemTime(&ut);
            FILETIME fu, fc; SystemTimeToFileTime(&ut, &fu); SystemTimeToFileTime(&t, &fc);
            double offH = (double)((long long)(((ULONGLONG)fc.dwHighDateTime << 32 | fc.dwLowDateTime) - ((ULONGLONG)fu.dwHighDateTime << 32 | fu.dwLowDateTime))) / 36e9;
            offH = std::round(offH * 4) / 4;
            double rise, set;
            if (i < (int)(sizeof(COORDS) / sizeof(COORDS[0])) && SunTimes(COORDS[i].lat, COORDS[i].lon, offH, rise, set)) {
                D2D1_COLOR_F g2 = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
                auto hm2 = [](double h) { int m = (int)std::lround(h * 60) % 1440; wchar_t b[16]; swprintf_s(b, L"%d:%02d", m / 60, m % 60); return std::wstring(b); };
                std::wstring a1 = hm2(rise), a2 = hm2(set);
                float w1 = TextW(g_tfCardSub, a1), w2 = TextW(g_tfCardSub, a2), gap = 14, iw = 12;
                float total = iw + 3 + w1 + gap + iw + 3 + w2, sx = x0 + 85 - total / 2, yy = cy0 + 178;
                auto sun = [&](float x, bool up) {
                    br->SetColor(g2);
                    rt->DrawLine({ x, yy + 4 }, { x + iw, yy + 4 }, br, 1.1f);
                    rt->PushAxisAlignedClip(D2D1::RectF(x - 1, yy - 1, x + iw + 1, yy + 4), D2D1_ANTIALIAS_MODE_ALIASED);  // só a metade de cima do sol
                    rt->DrawEllipse(D2D1::Ellipse({ x + iw / 2, yy + 4 }, 3, 3), br, 1.1f);
                    rt->PopAxisAlignedClip();
                    float ax = x + iw / 2, ay0 = yy - 4.5f, ay1 = yy - 1;
                    if (up) { rt->DrawLine({ ax, ay1 }, { ax, ay0 }, br, 1); rt->DrawLine({ ax - 1.8f, ay0 + 1.8f }, { ax, ay0 }, br, 1); rt->DrawLine({ ax + 1.8f, ay0 + 1.8f }, { ax, ay0 }, br, 1); }
                    else { rt->DrawLine({ ax, ay0 }, { ax, ay1 }, br, 1); rt->DrawLine({ ax - 1.8f, ay1 - 1.8f }, { ax, ay1 }, br, 1); rt->DrawLine({ ax + 1.8f, ay1 - 1.8f }, { ax, ay1 }, br, 1); }
                };
                sun(sx, true);
                Text(a1, g_tfCardSubL, D2D1::RectF(sx + iw + 3, yy - 8, sx + iw + 3 + w1 + 2, yy + 8), g2);
                float sx2 = sx + iw + 3 + w1 + gap;
                sun(sx2, false);
                Text(a2, g_tfCardSubL, D2D1::RectF(sx2 + iw + 3, yy - 8, sx2 + iw + 3 + w2 + 2, yy + 8), g2);
            }
        }
        g_hits.push_back({ H_WORLD, k, card });
    }
}

void PaintTabsGolden(D2D1_SIZE_F sz, bool dark) {
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31);
    const wchar_t* tabs[] = { L"Relógio Mundial", L"Alarmes", L"Cronômetro", L"Timer" };
    float ws[4], total = 8;
    for (int i = 0; i < 4; i++) { ws[i] = TextW(g_tfSeg, Tr(tabs[i])) + 34; total += ws[i]; }
    D2D1_RECT_F seg = D2D1::RectF(sz.width / 2 - total / 2, 10, sz.width / 2 + total / 2, 42);
    GlassButton(seg, dark);
    BarButton(seg);
    float x = seg.left + 4;
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = D2D1::RectF(x, seg.top + 3, x + ws[i], seg.bottom - 3);
        if (g_tab == i) Fill(r, dark ? Rgb(255, 255, 255, 0.14f) : Rgb(0, 0, 0, 0.08f), 13);
        Text(Tr(tabs[i]), g_tfSeg, r, text);
        g_hits.push_back({ H_TAB, i, r });
        x += ws[i];
    }
    if (g_tab <= 1) {
        D2D1_RECT_F add = D2D1::RectF(sz.width - 8 - 36, 8, sz.width - 8, 44);
        GlassButton(add, dark); IconPlus({ (add.left + add.right) / 2, 26 }, text);
        g_hits.push_back({ H_ADD, 0, add }); BarButton(add);
    }
}

void Paint() {
    if (!BeginPaint()) return;
    g_hits.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    D2D1_COLOR_F line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    const bool gg = BAR > 0;
    if (gg) PaintTabsGolden(sz, dark);
    // abas
    if (!gg) {
    const wchar_t* tabs[] = { L"Relógio Mundial", L"Alarmes", L"Cronômetro", L"Timer" };
    float segW = 120, sx = sz.width / 2 - segW * 2;
    Fill(D2D1::RectF(sx, 14, sx + segW * 4, 40), dark ? Rgb(58, 58, 60) : Rgb(232, 232, 237), 7);
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = D2D1::RectF(sx + i * segW + 2, 16, sx + (i + 1) * segW - 2, 38);
        if (g_tab == i) Fill(r, dark ? Rgb(99, 99, 102) : Rgb(255, 255, 255), 6);
        Text(Tr(tabs[i]), g_tfSeg, r, text);
        g_hits.push_back({ H_TAB, i, r });
    }
    if (g_tab <= 1) {
        D2D1_RECT_F add = D2D1::RectF(sz.width - 46, 12, sz.width - 14, 42);
        wchar_t p[2] = { 0xE710, 0 };
        Text(p, g_tfIcon, add, text);
        g_hits.push_back({ H_ADD, 0, add });
    }
    }  // !gg
    Fill(D2D1::RectF(0, TB - 1, sz.width, TB), line);
    float X = 28, W = sz.width - 28;
    if (g_tab == 0 && gg) PaintWorldGolden(sz, dark);
    else if (g_tab == 0) {
        SYSTEMTIME here; GetLocalTime(&here);
        float y = TB + 8;
        for (int k = 0; k < (int)g_world.size(); k++) {
            int i = g_world[k];
            SYSTEMTIME t{};
            if (!CityTime(i, t)) continue;
            FILETIME a, b; SystemTimeToFileTime(&here, &a); SystemTimeToFileTime(&t, &b);
            long long diffMin = ((long long)(((ULONGLONG)b.dwHighDateTime << 32 | b.dwLowDateTime) - ((ULONGLONG)a.dwHighDateTime << 32 | a.dwLowDateTime))) / 600000000LL;
            long long dh = (diffMin + (diffMin >= 0 ? 15 : -15)) / 30;  // em meias horas
            std::wstring day = t.wDay == here.wDay ? Tr(L"Hoje") : (diffMin > 0 ? Tr(L"Amanhã") : Tr(L"Ontem"));
            wchar_t off[32];
            if (dh == 0) wcscpy_s(off, L"");
            else if (dh % 2) swprintf_s(off, L", %+lld h 30", dh / 2);
            else swprintf_s(off, L", %+lld h", dh / 2);
            D2D1_RECT_F row = D2D1::RectF(X, y, W, y + 74);
            Text(day + off, g_tfSub, D2D1::RectF(X, y + 10, W - 200, y + 30), gray);
            Text(CityName(i), g_tfRow, D2D1::RectF(X, y + 30, W - 200, y + 60), text);
            wchar_t hm[16]; swprintf_s(hm, L"%d:%02d", t.wHour, t.wMinute);
            Text(hm, g_tfBigR, D2D1::RectF(W - 220, y, W, y + 74), text);
            Fill(D2D1::RectF(X, y + 74, W, y + 75), line);
            g_hits.push_back({ H_WORLD, k, row });
            y += 75;
        }
    } else if (g_tab == 1) {
        float y = TB + 8;
        if (g_alarms.empty()) Text(Tr(L"Nenhum Alarme"), g_tfRow, D2D1::RectF(0, TB, sz.width, sz.height * 0.6f), gray);
        for (int k = 0; k < (int)g_alarms.size(); k++) {
            auto& a = g_alarms[k];
            wchar_t hm[16]; swprintf_s(hm, L"%d:%02d", a.hh, a.mm);
            D2D1_RECT_F tr = D2D1::RectF(X, y, X + 220, y + 60);
            Text(hm, g_tfBig, tr, a.on ? text : gray);
            std::wstring sub = a.label.empty() ? Tr(L"Alarme") : a.label;
            if (a.days == 0x7F) sub += L", " + std::wstring(Tr(L"todos os dias"));
            else if (a.days == 0x3E) sub += L", " + std::wstring(Tr(L"dias de semana"));
            else if (a.days == 0x41) sub += L", " + std::wstring(Tr(L"fins de semana"));
            Text(sub, g_tfSub, D2D1::RectF(X, y + 56, W - 100, y + 76), gray);
            g_hits.push_back({ H_ALARMTIME, k, tr });
            // interruptor
            D2D1_RECT_F sw = D2D1::RectF(W - 60, y + 26, W - 10, y + 54);
            Fill(sw, a.on ? Rgb(48, 209, 88) : (dark ? Rgb(72, 72, 74) : Rgb(229, 229, 234)), 14);
            Circle({ a.on ? sw.right - 14 : sw.left + 14, (sw.top + sw.bottom) / 2 }, 12, Rgb(255, 255, 255), true);
            g_hits.push_back({ H_SWITCH, k, sw });
            g_hits.push_back({ H_ALARM, k, D2D1::RectF(X, y, W - 70, y + 84) });
            Fill(D2D1::RectF(X, y + 84, W, y + 85), line);
            y += 86;
        }
    } else if (g_tab == 2) {
        ULONGLONG now = SwNow();
        Text(FmtTime(now), g_tfHuge, D2D1::RectF(0, TB + 20, sz.width, TB + 150), text);
        float by = TB + 170, cx = sz.width / 2;
        bool running = g_swRun;
        Button(D2D1::RectF(cx - 190, by, cx - 30, by + 44), running ? Tr(L"Volta") : Tr(L"Zerar"), dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), text, H_SW_A);
        Button(D2D1::RectF(cx + 30, by, cx + 190, by + 44), running ? Tr(L"Parar") : Tr(L"Iniciar"), running ? Rgb(255, 69, 58, 0.25f) : Rgb(48, 209, 88, 0.25f), running ? Rgb(255, 69, 58) : Rgb(48, 209, 88), H_SW_B);
        // voltas (a atual em cima)
        std::vector<ULONGLONG> laps = g_laps;
        ULONGLONG done = 0; for (auto l : g_laps) done += l;
        if (now || !laps.empty()) laps.push_back(now - done);
        int best = -1, worst = -1;
        if (g_laps.size() >= 2) for (int i = 0; i < (int)g_laps.size(); i++) { if (best < 0 || g_laps[i] < g_laps[best]) best = i; if (worst < 0 || g_laps[i] > g_laps[worst]) worst = i; }
        float y = by + 70;
        for (int i = (int)laps.size() - 1; i >= 0 && y < sz.height - 20; i--) {
            D2D1_COLOR_F c = i == best ? Rgb(48, 209, 88) : i == worst ? Rgb(255, 69, 58) : text;
            wchar_t n[32]; swprintf_s(n, Tr(L"Volta %d"), i + 1);
            Text(n, g_tfRow, D2D1::RectF(X + 60, y, W, y + 34), c);
            Text(FmtTime(laps[i]), g_tfMono, D2D1::RectF(X, y, W - 60, y + 34), c);
            Fill(D2D1::RectF(X + 60, y + 34, W - 60, y + 35), line);
            y += 35;
        }
    } else {
        ULONGLONG left = TmLeft();
        Text(FmtTime(left + 999, false), g_tfHuge, D2D1::RectF(0, TB + 20, sz.width, TB + 150), g_tmRun && !g_tmPaused ? Rgb(255, 159, 10) : text);
        float cx = sz.width / 2, y = TB + 160;
        if (!g_tmRun) {
            int presets[] = { 1, 3, 5, 10, 15, 30, 60 };
            float pw = 64, px = cx - pw * 3.5f - 9;
            for (int i = 0; i < 7; i++) {
                D2D1_RECT_F r = D2D1::RectF(px + i * (pw + 3), y, px + i * (pw + 3) + pw, y + 34);
                bool on = g_tmMin == presets[i];
                wchar_t b[16]; swprintf_s(b, presets[i] >= 60 ? L"%d h" : L"%d min", presets[i] >= 60 ? presets[i] / 60 : presets[i]);
                Fill(r, on ? Rgb(255, 159, 10, 0.3f) : (dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234)), 8);
                Text(b, g_tfBtn, r, on ? Rgb(255, 159, 10) : text);
                g_hits.push_back({ H_TM_PRESET, presets[i], r });
            }
            Button(D2D1::RectF(cx - 90, y + 50, cx - 50, y + 84), L"−", dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), text, H_TM_MINUS);
            Button(D2D1::RectF(cx + 50, y + 50, cx + 90, y + 84), L"+", dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), text, H_TM_PLUS);
            wchar_t m[16]; swprintf_s(m, L"%d min", g_tmMin);
            Text(m, g_tfRow, D2D1::RectF(cx - 50, y + 50, cx + 50, y + 84), text);
            y += 110;
        }
        Button(D2D1::RectF(cx - 190, y, cx - 30, y + 44), Tr(L"Cancelar"), dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), g_tmRun ? text : gray, H_TM_A);
        const wchar_t* b = !g_tmRun ? Tr(L"Iniciar") : g_tmPaused ? Tr(L"Retomar") : Tr(L"Pausar");
        Button(D2D1::RectF(cx + 30, y, cx + 190, y + 44), b, g_tmRun && !g_tmPaused ? Rgb(255, 159, 10, 0.25f) : Rgb(48, 209, 88, 0.25f), g_tmRun && !g_tmPaused ? Rgb(255, 159, 10) : Rgb(48, 209, 88), H_TM_B);
    }
    if (field.h && field.h == g_cityField) {  // crédito do Open-Meteo (CC BY 4.0) embaixo da busca de cidade
        std::wstring c = Tr(L"Busca de cidades por Open-Meteo.com");
        float cw = TextW(g_tfSub, c) + 16, right = sz.width - 24;
        D2D1_RECT_F r = D2D1::RectF(right - cw, TB + 40, right, TB + 60);
        Fill(r, dark || gg ? Rgb(44, 44, 46, 0.92f) : Rgb(242, 242, 247, 0.96f), 6);
        Text(c, g_tfSub, D2D1::RectF(r.left + 8, r.top, r.right, r.bottom), dark || gg ? Rgb(235, 235, 245, 0.75f) : gray);
    }
    EndPaint();
}

// Sem o Shell (modo só apps) ninguém tocava os alarmes: o app toca enquanto estiver aberto
void CheckAlarms() {
    SYSTEMTIME t; GetLocalTime(&t);
    static int last = -1;
    int now = t.wHour * 60 + t.wMinute;
    if (now == last) return;
    bool first = last < 0;
    last = now;
    if (first) return;  // ao abrir não toca o minuto que já está correndo
    if (FindWindowExW(HWND_MESSAGE, nullptr, L"ShellMensagens", nullptr) || FindWindowW(L"ShellMensagens", nullptr)) return;  // o Shell toca
    bool changed = false;
    for (auto& a : g_alarms) {
        if (!a.on || a.hh != t.wHour || a.mm != t.wMinute) continue;
        if (a.days && !(a.days & (1 << t.wDayOfWeek))) continue;
        if (!a.days) { a.on = false; changed = true; }  // uma vez só: desliga (como no Mac)
        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
        FLASHWINFO fw{ sizeof(fw), wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 6, 0 }; FlashWindowEx(&fw);
        wchar_t msg[160]; swprintf_s(msg, L"%ls\n%d:%02d", a.label.empty() ? Tr(L"Alarme") : a.label.c_str(), a.hh, a.mm);
        if (changed) Save();
        MessageBoxW(wnd, msg, Tr(L"Relógio"), MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }
    if (changed) { Save(); InvalidateRect(wnd, nullptr, FALSE); }
}

void Tick() {
    CheckAlarms();
    if (g_tmRun && !g_tmPaused && TmLeft() == 0) {  // timer acabou
        g_tmRun = false;
        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
        FLASHWINFO fw{ sizeof(fw), wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 6, 0 }; FlashWindowEx(&fw);
        ShowWindow(wnd, SW_RESTORE); SetForegroundWindow(wnd);
        MessageBoxW(wnd, Tr(L"O timer terminou."), Tr(L"Relógio"), MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }
    static int n;
    if (g_swRun || (g_tmRun && !g_tmPaused) || ++n % 20 == 0) InvalidateRect(wnd, nullptr, FALSE);
}

// Fuso IANA ("Europe/Warsaw", o que o Open-Meteo devolve) -> nome do fuso no Windows ("Central European Standard
// Time"), pelo ICU que vem no Windows 10 1903+ (icu.dll)
std::wstring WindowsTz(const std::wstring& iana) {
    typedef int (*GetWinId)(const wchar_t*, int, wchar_t*, int, int*);
    static HMODULE icu = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static GetWinId f = icu ? (GetWinId)GetProcAddress(icu, "ucal_getWindowsTimeZoneID") : nullptr;
    if (!f) return L"";
    wchar_t out[128] = {}; int st = 0;
    int n = f(iana.c_str(), (int)iana.size(), out, 127, &st);
    return st > 0 || n <= 0 ? L"" : std::wstring(out, n);
}
std::string Utf8Of(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr); return s; }
std::wstring WideOf(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w; }
std::string JsonField(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    size_t p = j.find(k); if (p == std::string::npos) return "";
    p += k.size(); size_t e = j.find('"', p);
    return e == std::string::npos ? "" : j.substr(p, e - p);
}
void FindCity(const std::wstring& q) {
    if (q.empty()) return;
    HWND h = wnd;
    std::thread([q, h] {
        std::string u = Utf8Of(q), enc;
        for (unsigned char c : u) { if (isalnum(c) || c == '-' || c == '.') enc += (char)c; else { char b[4]; sprintf_s(b, "%%%02X", c); enc += b; } }
        std::wstring path = L"/v1/search?count=1&language=" + std::wstring(idioma::Code()) + L"&name=" + WideOf(enc);
        std::string body;
        HINTERNET ses = WinHttpOpen(L"Liquinix-Clock/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (ses) {
            WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);
            HINTERNET con = WinHttpConnect(ses, L"geocoding-api.open-meteo.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
            if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 64 * 1024) {
                    std::string chunk(avail, '\0'); DWORD got = 0;
                    if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
                    body.append(chunk.data(), got);
                }
            }
            if (req) WinHttpCloseHandle(req);
            if (con) WinHttpCloseHandle(con);
            WinHttpCloseHandle(ses);
        }
        auto* c = new Custom;
        if (body.find("\"results\"") != std::string::npos) { c->name = WideOf(JsonField(body, "name")); c->tz = WideOf(JsonField(body, "timezone")); }
        if (!PostMessageW(h, WM_CITYFOUND, 0, (LPARAM)c)) delete c;
    }).detach();
}
void CityFound(Custom* c) {
    std::wstring tz = c->tz.empty() ? L"" : WindowsTz(c->tz);
    if (c->name.empty() || tz.empty()) MessageBoxW(wnd, Tr(L"Cidade não encontrada."), Tr(L"Relógio"), MB_OK | MB_ICONINFORMATION);
    else {
        int idx = -1;
        for (int k = 0; k < (int)g_custom.size(); k++) if (g_custom[k].name == c->name && g_custom[k].tz == tz) idx = k;
        if (idx < 0) { g_custom.push_back({ c->name, tz }); idx = (int)g_custom.size() - 1; }
        if (std::find(g_world.begin(), g_world.end(), CUSTOM0 + idx) == g_world.end()) g_world.push_back(CUSTOM0 + idx);
        Save();
    }
    delete c;
    InvalidateRect(wnd, nullptr, FALSE);
}
void AddCity(POINT scr) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 9999, Tr(L"Outra Cidade…"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < NCITIES; i++) AppendMenuW(m, MF_STRING | (std::find(g_world.begin(), g_world.end(), i) != g_world.end() ? MF_CHECKED : 0), i + 1, Tr(CITIES[i].name));
    int c = TrackPopupMenu(m, TPM_RETURNCMD, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c == 9999) {  // digita o nome; Enter procura
        D2D1_SIZE_F sz = Size();
        FieldOpen(D2D1::RectF(sz.width - 300, TB + 12, sz.width - 24, TB + 36), L"", 14, false, [](const std::wstring& t, int key) { if (key == VK_RETURN) FindCity(t); });
        g_cityField = field.h;
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
    if (c > 0 && c <= NCITIES && std::find(g_world.begin(), g_world.end(), c - 1) == g_world.end()) { g_world.push_back(c - 1); Save(); }
}

// hora do alarme digitada ("7:30", "0730", "19h")
bool ParseTime(const std::wstring& s, int& hh, int& mm) {
    std::wstring d; for (wchar_t c : s) if (iswdigit(c)) d += c;
    if (d.empty()) return false;
    if (d.size() <= 2) { hh = _wtoi(d.c_str()); mm = 0; }
    else { hh = _wtoi(d.substr(0, d.size() - 2).c_str()); mm = _wtoi(d.substr(d.size() - 2).c_str()); }
    return hh >= 0 && hh < 24 && mm >= 0 && mm < 60;
}
void EditAlarmTime(int k) {
    for (auto& h : g_hits) if (h.kind == H_ALARMTIME && h.arg == k) {
        int id = g_alarms[k].id;
        wchar_t cur[16]; swprintf_s(cur, L"%d:%02d", g_alarms[k].hh, g_alarms[k].mm);
        FieldOpen(D2D1::RectF(h.r.left, h.r.top + 10, h.r.left + 160, h.r.bottom - 10), cur, 30, false, [id](const std::wstring& t, int key) {
            if (key == VK_ESCAPE) return;
            for (auto& a : g_alarms) if (a.id == id) { int hh, mm; if (ParseTime(t, hh, mm)) { a.hh = hh; a.mm = mm; a.on = true; Save(); } }
        });
    }
}
void AddAlarm() {
    SYSTEMTIME t; GetLocalTime(&t);
    g_alarms.push_back({ g_nextId++, (t.wHour + 1) % 24, 0, true, L"", 0 });
    Save();
    Paint();
    EditAlarmTime((int)g_alarms.size() - 1);
}
void AlarmMenu(int k, POINT scr) {
    HMENU m = CreatePopupMenu(), rep = CreatePopupMenu();
    int d = g_alarms[k].days;
    AppendMenuW(rep, MF_STRING | (d == 0 ? MF_CHECKED : 0), 10, Tr(L"Nunca"));
    AppendMenuW(rep, MF_STRING | (d == 0x7F ? MF_CHECKED : 0), 11, Tr(L"Todos os Dias"));
    AppendMenuW(rep, MF_STRING | (d == 0x3E ? MF_CHECKED : 0), 12, Tr(L"Dias de Semana"));
    AppendMenuW(rep, MF_STRING | (d == 0x41 ? MF_CHECKED : 0), 13, Tr(L"Fins de Semana"));
    AppendMenuW(m, MF_STRING, 1, Tr(L"Editar Hora"));
    AppendMenuW(m, MF_STRING, 2, Tr(L"Renomear"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rep, Tr(L"Repetir"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, Tr(L"Apagar"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c == 1) EditAlarmTime(k);
    else if (c == 2) {
        for (auto& h : g_hits) if (h.kind == H_ALARM && h.arg == k) {
            int id = g_alarms[k].id;
            FieldOpen(D2D1::RectF(h.r.left, h.r.top + 56, h.r.right, h.r.top + 76), g_alarms[k].label, 13, false, [id](const std::wstring& t, int key) {
                if (key == VK_ESCAPE) return; for (auto& a : g_alarms) if (a.id == id) { a.label = t; Save(); } });
        }
    } else if (c == 3) { g_alarms.erase(g_alarms.begin() + k); Save(); }
    else if (c >= 10) { static const int v[] = { 0, 0x7F, 0x3E, 0x41 }; g_alarms[k].days = v[c - 10]; Save(); }
    InvalidateRect(wnd, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); return 0;
    case WM_DPICHANGED: OnDpi(w, l); return 0;
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_CTLCOLOREDIT: return FieldColors((HDC)w, Dark() ? RGB(30, 30, 30) : RGB(255, 255, 255));
    case WM_FIELDLOST: if (field.h && GetFocus() != field.h) FieldEnd(0); return 0;
    case WM_TIMER: Tick(); return 0;
    case WM_CITYFOUND: CityFound((Custom*)l); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONUP: {
        if (field.h) FieldEnd(0);
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
        bool right = m == WM_RBUTTONUP;
        for (auto& ht : g_hits) {
            if (!In(ht.r, x, y)) continue;
            switch (ht.kind) {
            case H_TAB: g_tab = ht.arg; break;
            case H_ADD: if (g_tab == 0) AddCity(scr); else AddAlarm(); break;
            case H_WORLD:
                if (right) {
                    HMENU mm = CreatePopupMenu(); AppendMenuW(mm, MF_STRING, 1, Tr(L"Apagar"));
                    if (TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr) == 1) { g_world.erase(g_world.begin() + ht.arg); Save(); }
                    DestroyMenu(mm);
                }
                break;
            case H_SWITCH: g_alarms[ht.arg].on = !g_alarms[ht.arg].on; Save(); break;
            case H_ALARMTIME: if (right) AlarmMenu(ht.arg, scr); else EditAlarmTime(ht.arg); break;
            case H_ALARM: if (right) AlarmMenu(ht.arg, scr); break;
            case H_SW_A: if (g_swRun) { ULONGLONG done = 0; for (auto v : g_laps) done += v; g_laps.push_back(SwNow() - done); } else { g_swAcc = 0; g_laps.clear(); } break;
            case H_SW_B: if (g_swRun) { g_swAcc = SwNow(); g_swRun = false; } else { g_swStart = GetTickCount64(); g_swRun = true; } break;
            case H_TM_PRESET: g_tmMin = ht.arg; break;
            case H_TM_MINUS: g_tmMin = std::max(1, g_tmMin - 1); break;
            case H_TM_PLUS: g_tmMin = std::min(24 * 60, g_tmMin + 1); break;
            case H_TM_A: g_tmRun = false; g_tmPaused = false; break;
            case H_TM_B:
                if (!g_tmRun) { g_tmRun = true; g_tmPaused = false; g_tmEnd = GetTickCount64() + (ULONGLONG)g_tmMin * 60000; }
                else if (g_tmPaused) { g_tmPaused = false; g_tmEnd = GetTickCount64() + g_tmLeft; }
                else { g_tmLeft = TmLeft(); g_tmPaused = true; }
                break;
            default: continue;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && w >= '1' && w <= '4') { g_tab = (int)(w - '1'); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (ctrl && w == 'W') { DestroyWindow(h); return 0; }
        if (w == VK_SPACE && g_tab == 2) { { if (g_swRun) { g_swAcc = SwNow(); g_swRun = false; } else { g_swStart = GetTickCount64(); g_swRun = true; } } InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_file = DataPath(L"relogio.txt");
    Load();
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfSeg = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfBig = Fmt(44, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_TRAILING, false, disp.c_str());
    g_tfHuge = Fmt(84, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_CENTER, false, disp.c_str());
    if (g_tfHuge) { DWRITE_FONT_FEATURE f{ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 }; (void)f; }
    g_tfRow = Fmt(16);
    g_tfSub = Fmt(13);
    g_tfBtn = Fmt(15, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfMono = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfCityB = Fmt(11, DWRITE_FONT_WEIGHT_BOLD);
    g_tfCityT = Fmt(9);
    g_tfDial = Fmt(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfCard = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfCardSub = Fmt(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfCardSubL = Fmt(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    barWanted = 52;  // macOS 27: abas na barra unificada
    if (!CreateMain(hi, L"RelogioJanela", Tr(L"Relógio"), Proc, semaforo::Aqua() ? 1046 : 720, semaforo::Aqua() ? 768 : 640)) return 1;
    if (BAR > 0) TB = 52;
    // alarmes: o tempo do Mac é a hora grande alinhada à esquerda
    if (g_tfBig) g_tfBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_tfBigR = Fmt(44, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_TRAILING, false, disp.c_str());
    SetTimer(wnd, 1, 50, nullptr);
    ShowWindow(wnd, show);
    return Run();
}
