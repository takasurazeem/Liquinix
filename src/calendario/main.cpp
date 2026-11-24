// Calendário (Calendar do Mac): Dia, Semana e Mês (Ctrl+1/2/3), "‹ Hoje ›" no alto à direita, o mês grande à
// esquerda. Duplo clique cria um evento (no Mês: o dia inteiro; na Semana/Dia: uma hora a partir dali) e o nome
// já fica para digitar. Botão direito no evento: horário, duração, cor, apagar. Ctrl+I importa um .ics (Google,
// Outlook, iCloud exportam). Os lembretes com data (app Lembretes) aparecem como itens com quadradinho.
// Dados: %APPDATA%\Shell\calendario.txt. O Shell avisa (banner do Mac) quando o evento começa.
#include "../comum/app2d.h"
#include <shobjidl.h>
#include <map>
#include <set>
#include <thread>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using namespace app2d;

namespace {

float TB = 64;  // macOS 27: 112 (barra de 52 + a linha do título)
const float WEEKHEAD = 30, HOUR = 48, ALLDAY = 26, LEFTCOL = 56;
const UINT WM_FIELDLOST = WM_APP + 77;

struct Ev { int id; long long start, end; bool allDay; std::wstring title; int color; std::wstring notes; int cal = 1; };
std::vector<Ev> g_ev;
// calendários (barra lateral): cada evento é de um; esconder um calendário esconde os eventos dele
struct Cal { int id; std::wstring name; int color; bool on; };
std::vector<Cal> g_cals;
bool g_side = true;
float SIDE = 200;  // macOS 27: 218,5
std::wstring g_search;  // macOS 27: busca (Enter vai ao próximo evento com o texto)
Cal* FindCal(int id) { for (auto& c : g_cals) if (c.id == id) return &c; return nullptr; }
bool CalOn(const Ev& e) { Cal* c = FindCal(e.cal); return !c || c->on; }
int g_nextId = 1;
std::wstring g_file, g_remFile;
enum View { V_DAY, V_WEEK, V_MONTH, V_YEAR };
View g_view = V_MONTH;
long long g_anchor;  // um dia (minutos) dentro do período mostrado
long long g_selDay;  // dia escolhido
int g_selEv = -1;
float g_scrollH = 8 * 48;  // Semana/Dia: rolagem (começa às 8h)
int g_editing = -1;
IDWriteTextFormat *g_tfTitleG, *g_tfTitleYG, *g_tfSeg, *g_tfSideG, *g_tfSideBold, *g_tfSecG, *g_tfMiniT, *g_tfMiniD, *g_tfMiniW;
IDWriteTextFormat *g_tfYearDay, *g_tfMonth, *g_tfYear, *g_tfBtn, *g_tfDay, *g_tfDayNum, *g_tfEv, *g_tfEvSmall, *g_tfHour, *g_tfHeadDay, *g_tfIcon;

D2D1_COLOR_F EvColor(int i) {
    static const D2D1_COLOR_F c[] = { Rgb(10, 132, 255), Rgb(255, 69, 58), Rgb(255, 159, 10), Rgb(48, 209, 88), Rgb(191, 90, 242), Rgb(255, 214, 10), Rgb(172, 142, 104) };
    return c[((i % 7) + 7) % 7];
}
const wchar_t* COLOR_NAMES[] = { L"Azul", L"Vermelho", L"Laranja", L"Verde", L"Roxo", L"Amarelo", L"Marrom" };

// Feriados nacionais (pedido no Reddit): o calendário "Feriados", só de leitura, com os feriados do país da região do
// Windows, da Nager.Date (date.nager.at, sem chave). Os eventos têm id negativo e não vão para o calendario.txt;
// cada ano baixado fica em feriados.txt (não baixa de novo). Desliga no quadradinho da lateral.
const int HOL_CAL = -5;
const UINT WM_HOLIDAYS = WM_APP + 6;
bool g_holOn = true;
std::wstring g_holCC;           // país (ISO de 2 letras)
std::set<int> g_holYears;       // anos já carregados ou pedidos
int g_holNextId = -1000;
int g_firstDow = 0;             // primeiro dia da semana (WeekStart, mais abaixo): 0 = domingo, 1 = segunda
bool g_firstDowSet = false;     // escolhido pelo usuário (senão vale a região do Windows)

void Save() {
    std::vector<std::wstring> out{ L"# Calendário do WacOS (E = evento: id, início, fim em minutos desde 1970, dia inteiro, título, cor, notas)" };
    if (Cal* hc = FindCal(HOL_CAL)) g_holOn = hc->on;
    out.push_back(L"S\tsemana\t" + std::to_wstring(g_firstDowSet ? g_firstDow : -1));
    out.push_back(L"S\tferiados\t" + std::wstring(g_holOn ? L"1" : L"0"));
    for (auto& c : g_cals) if (c.id != HOL_CAL) out.push_back(L"C\t" + std::to_wstring(c.id) + L"\t" + Esc(c.name) + L"\t" + std::to_wstring(c.color) + L"\t" + (c.on ? L"1" : L"0"));
    for (auto& e : g_ev) if (e.id >= 0) out.push_back(L"E\t" + std::to_wstring(e.id) + L"\t" + std::to_wstring(e.start) + L"\t" + std::to_wstring(e.end) + L"\t" + (e.allDay ? L"1" : L"0") + L"\t" +
                                       Esc(e.title) + L"\t" + std::to_wstring(e.color) + L"\t" + Esc(e.notes) + L"\t" + std::to_wstring(e.cal));
    WriteLines(g_file, out);
}
void Load() {
    g_ev.clear(); g_cals.clear();
    for (auto& line : ReadLines(g_file)) {
        auto f = Split(line);
        if (f.size() >= 7 && f[0] == L"E") {
            Ev e{ _wtoi(f[1].c_str()), _wtoi64(f[2].c_str()), _wtoi64(f[3].c_str()), f[4] == L"1", Unesc(f[5]), _wtoi(f[6].c_str()), f.size() > 7 ? Unesc(f[7]) : L"" };
            if (f.size() > 8) e.cal = _wtoi(f[8].c_str());
            g_ev.push_back(e);
        } else if (f.size() >= 5 && f[0] == L"C") g_cals.push_back({ _wtoi(f[1].c_str()), Unesc(f[2]), _wtoi(f[3].c_str()), f[4] == L"1" });
        else if (f.size() >= 3 && f[0] == L"S" && f[1] == L"semana") { int v = _wtoi(f[2].c_str()); if (v == 0 || v == 1) { g_firstDow = v; g_firstDowSet = true; } }
        else if (f.size() >= 3 && f[0] == L"S" && f[1] == L"feriados") g_holOn = f[2] != L"0";
    }
    if (g_cals.empty()) g_cals = { { 1, Tr(L"Pessoal"), 0, true }, { 2, Tr(L"Trabalho"), 2, true }, { 3, Tr(L"Família"), 3, true } };
    if (!g_firstDowSet) {  // a região do Windows: LOCALE_IFIRSTDAYOFWEEK 0 = segunda ... 6 = domingo
        wchar_t b[4] = {};
        if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, b, 4)) g_firstDow = (_wtoi(b) + 1) % 7 == 1 ? 1 : 0;
    }
    { wchar_t cc[8] = {}; if (GetUserDefaultGeoName(cc, 8) && wcslen(cc) == 2) g_holCC = cc; }
    if (!g_holCC.empty()) g_cals.push_back({ HOL_CAL, Tr(L"Feriados"), 1, g_holOn });
    for (auto& e : g_ev) g_nextId = std::max(g_nextId, e.id + 1);
    for (auto& c : g_cals) g_nextId = std::max(g_nextId, c.id + 1);
}
Ev* FindEv(int id) { for (auto& e : g_ev) if (e.id == id) return &e; return nullptr; }

// lembretes com data (só leitura): "R id lista feito data título notas sinal"
struct RemItem { long long due; std::wstring title; bool done; };
std::vector<RemItem> Reminders() {
    std::vector<RemItem> v;
    for (auto& line : ReadLines(g_remFile)) {
        auto f = Split(line);
        if (f.size() >= 8 && f[0] == L"R" && _wtoi64(f[4].c_str())) v.push_back({ _wtoi64(f[4].c_str()), Unesc(f[5]), f[3] == L"1" });
    }
    return v;
}

long long AddDays(long long day, int n) { SYSTEMTIME s = FromMin(day); s.wHour = 12; long long m = ToMin(s) + n * 24LL * 60; return DayStart(m); }
long long MonthStart(long long m) { SYSTEMTIME s = FromMin(m); s.wDay = 1; s.wHour = s.wMinute = 0; return ToMin(s); }
long long AddMonths(long long m, int n) { SYSTEMTIME s = FromMin(MonthStart(m)); int mo = s.wMonth - 1 + n; s.wYear = (WORD)(s.wYear + (mo >= 0 ? mo / 12 : (mo - 11) / 12)); s.wMonth = (WORD)(((mo % 12) + 12) % 12 + 1); return ToMin(s); }
// primeiro dia da semana: 0 = domingo, 1 = segunda. Padrão: o da região do Windows (como o Mac, que segue a
// região); o usuário troca com o botão direito nos nomes dos dias (pedido no Reddit); g_firstDow fica lá em cima
long long WeekStart(long long m) { long long d = DayStart(m); return AddDays(d, -(((int)FromMin(d).wDayOfWeek - g_firstDow + 7) % 7)); }
int DowAt(int col) { return (col + g_firstDow) % 7; }  // coluna 0..6 -> dia da semana

// ---- feriados
struct HolDay { int y, m, d; std::wstring name; };
void AddHoliday(const HolDay& hd) {
    SYSTEMTIME st{}; st.wYear = (WORD)hd.y; st.wMonth = (WORD)hd.m; st.wDay = (WORD)hd.d;
    long long day = ToMin(st);
    for (auto& e : g_ev) if (e.cal == HOL_CAL && e.start == day && e.title == hd.name) return;
    g_ev.push_back({ g_holNextId--, day, AddDays(day, 1), true, hd.name, 1, L"", HOL_CAL });
}
std::wstring HolFile() { return DataPath(L"feriados.txt"); }
void LoadHolidayCache() {  // H  país  ano  aaaammdd  nome
    for (auto& line : ReadLines(HolFile())) {
        auto f = Split(line);
        if (f.size() < 5 || f[0] != L"H" || f[1] != g_holCC) continue;
        int y = _wtoi(f[2].c_str()), v = _wtoi(f[3].c_str());
        g_holYears.insert(y);
        if (v > 0) AddHoliday({ v / 10000, v / 100 % 100, v % 100, Unesc(f[4]) });
    }
}
std::wstring JsonStr(const std::string& j, size_t& p, const char* key) {  // o próximo "key":"valor" a partir de p
    std::string k = std::string("\"") + key + "\":\"";
    size_t a = j.find(k, p);
    if (a == std::string::npos) return L"";
    a += k.size();
    std::wstring out;
    size_t i = a;
    for (; i < j.size() && j[i] != '"'; i++) {
        if (j[i] == '\\' && i + 1 < j.size()) {
            char c = j[++i];
            if (c == 'u' && i + 4 < j.size()) { out += (wchar_t)strtol(j.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; }
            else out += (wchar_t)(c == 'n' ? L' ' : c);
            continue;
        }
        // UTF-8: junta os bytes do caractere
        unsigned char c = (unsigned char)j[i];
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        wchar_t w[4] = {}; int got = MultiByteToWideChar(CP_UTF8, 0, j.data() + i, std::min<int>(n, (int)(j.size() - i)), w, 4);
        out.append(w, got > 0 ? got : 0);
        i += n - 1;
    }
    p = i;
    return out;
}
void RequestHolidays(int year) {
    if (g_holCC.empty() || !g_holOn || year < 1970 || g_holYears.count(year)) return;
    g_holYears.insert(year);
    std::wstring cc = g_holCC; HWND h = wnd;
    std::thread([year, cc, h] {
        std::string body;
        HINTERNET ses = WinHttpOpen(L"WacOS-Calendar/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (ses) {
            WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);
            HINTERNET con = WinHttpConnect(ses, L"date.nager.at", INTERNET_DEFAULT_HTTPS_PORT, 0);
            std::wstring path = L"/api/v3/PublicHolidays/" + std::to_wstring(year) + L"/" + cc;
            HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
            if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, sz = sizeof(status), avail = 0;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
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
        }
        auto* list = new std::vector<HolDay>;
        size_t p = 0;
        for (;;) {
            std::wstring d = JsonStr(body, p, "date");
            if (d.size() < 10) break;
            std::wstring n = JsonStr(body, p, "localName");
            list->push_back({ _wtoi(d.substr(0, 4).c_str()), _wtoi(d.substr(5, 2).c_str()), _wtoi(d.substr(8, 2).c_str()), n });
        }
        bool ok = !body.empty();
        if (!PostMessageW(h, WM_HOLIDAYS, (WPARAM)(ok ? year : -year), (LPARAM)list)) delete list;
    }).detach();
}
void HolidaysArrived(int year, std::vector<HolDay>* list) {
    if (year < 0) { g_holYears.erase(-year); delete list; return; }  // sem rede: tenta de novo depois
    auto lines = ReadLines(HolFile());
    if (lines.empty()) lines.push_back(L"# Feriados baixados pelo Calendário do WacOS (Nager.Date): H, país, ano, aaaammdd, nome");
    lines.push_back(L"H\t" + g_holCC + L"\t" + std::to_wstring(year) + L"\t0\t");  // ano baixado (mesmo sem feriados)
    for (auto& hd : *list) {
        wchar_t v[16]; swprintf_s(v, L"%04d%02d%02d", hd.y, hd.m, hd.d);
        lines.push_back(L"H\t" + g_holCC + L"\t" + std::to_wstring(year) + L"\t" + v + L"\t" + Esc(hd.name));
        AddHoliday(hd);
    }
    WriteLines(HolFile(), lines);
    delete list;
}

bool OnDay(const Ev& e, long long day) { long long next = AddDays(day, 1); return e.start < next && (e.end > day || (e.end <= e.start && e.start >= day)); }

std::wstring Hm(long long m) { SYSTEMTIME s = FromMin(m); wchar_t b[16]; swprintf_s(b, L"%d:%02d", s.wHour, s.wMinute); return b; }

// ---- desenho
struct Hit { int kind, ev; long long day; D2D1_RECT_F r; };  // kind: 1 evento, 2 dia (célula/coluna), 3 botão, 4 visão
std::vector<Hit> g_hits;

void Toolbar(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray) {
    SYSTEMTIME a = FromMin(g_anchor);
    std::wstring mon = MonthName(a.wMonth);
    if (!idioma::En() && !mon.empty()) mon[0] = towupper(mon[0]);
    float mw = TextW(g_tfMonth, mon);
    Text(mon, g_tfMonth, D2D1::RectF(20, 12, 20 + mw + 4, 52), text);
    Text(std::to_wstring(a.wYear), g_tfYear, D2D1::RectF(26 + mw, 12, 26 + mw + 120, 52), gray);
    // Dia | Semana | Mês no meio
    const wchar_t* names[] = { L"Dia", L"Semana", L"Mês", L"Ano" };
    float segW = 70, sx = sz.width / 2 - segW * 2;
    Fill(D2D1::RectF(sx, 18, sx + segW * 4, 44), dark ? Rgb(58, 58, 60) : Rgb(232, 232, 237), 7);
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = D2D1::RectF(sx + i * segW + 2, 20, sx + (i + 1) * segW - 2, 42);
        if (g_view == i) Fill(r, dark ? Rgb(99, 99, 102) : Rgb(255, 255, 255), 6);
        Text(Tr(names[i]), g_tfBtn, r, text);
        g_hits.push_back({ 4, i, 0, r });
    }
    // ‹ Hoje ›
    D2D1_RECT_F prev = D2D1::RectF(sz.width - 150, 18, sz.width - 122, 44), today = D2D1::RectF(sz.width - 120, 18, sz.width - 50, 44), next = D2D1::RectF(sz.width - 48, 18, sz.width - 20, 44);
    Fill(today, dark ? Rgb(58, 58, 60) : Rgb(232, 232, 237), 7);
    Text(Tr(L"Hoje"), g_tfBtn, today, text);
    Text(L"‹", g_tfMonth, prev, text);
    Text(L"›", g_tfMonth, next, text);
    g_hits.push_back({ 3, -1, 0, prev }); g_hits.push_back({ 3, 0, 0, today }); g_hits.push_back({ 3, 1, 0, next });
}

void PaintMonth(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray, const std::vector<RemItem>& rems) {
    long long ms = MonthStart(g_anchor), first = WeekStart(ms);
    SYSTEMTIME am = FromMin(ms);
    float top = TB + WEEKHEAD, cw = sz.width / 7, rows = 6, ch = (sz.height - top) / rows;
    for (int d = 0; d < 7; d++) Text(WeekdayShort(DowAt(d)), g_tfHeadDay, D2D1::RectF(d * cw, TB, (d + 1) * cw - 8, TB + WEEKHEAD), gray);
    g_hits.push_back({ 13, 0, 0, D2D1::RectF(0, TB, 7 * cw, TB + WEEKHEAD) });  // botão direito: começar a semana em...
    long long today = DayStart(NowMin());
    D2D1_COLOR_F line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    for (int i = 0; i < 42; i++) {
        long long day = AddDays(first, i);
        SYSTEMTIME s = FromMin(day);
        D2D1_RECT_F c = D2D1::RectF((i % 7) * cw, top + (i / 7) * ch, (i % 7 + 1) * cw, top + (i / 7 + 1) * ch);
        bool other = s.wMonth != am.wMonth;
        if (s.wDayOfWeek == 0 || s.wDayOfWeek == 6) Fill(c, dark ? Rgb(255, 255, 255, 0.025f) : Rgb(0, 0, 0, 0.025f));
        if (day == g_selDay) Fill(c, dark ? Rgb(10, 132, 255, 0.16f) : Rgb(10, 132, 255, 0.08f));
        Fill(D2D1::RectF(c.left, c.top, c.right, c.top + 1), line);
        Fill(D2D1::RectF(c.right - 1, c.top, c.right, c.bottom), line);
        // número do dia (hoje: no círculo vermelho)
        std::wstring n = std::to_wstring(s.wDay);
        if (s.wDay == 1) { std::wstring mn = MonthName(s.wMonth); n = n + L" " + (idioma::Pt() || idioma::En() ? mn.substr(0, 3) + L"." : idioma::MonthName(s.wMonth, true)); }  // "1 nov.", "1 11月"
        D2D1_RECT_F nr = D2D1::RectF(c.right - 70, c.top + 4, c.right - 8, c.top + 26);
        if (day == today) {
            float w = TextW(g_tfDayNum, n) + 12;
            Fill(D2D1::RectF(c.right - 8 - w, c.top + 4, c.right - 8, c.top + 26), Rgb(255, 59, 48), 11);
            Text(n, g_tfDayNum, D2D1::RectF(c.right - 8 - w, c.top + 4, c.right - 14, c.top + 26), Rgb(255, 255, 255));
        } else Text(n, g_tfDayNum, nr, other ? gray : text);
        g_hits.push_back({ 2, -1, day, c });
        // eventos do dia
        float y = c.top + 30, lineH = 18;
        int shown = 0, total = 0;
        std::vector<const Ev*> list;
        for (auto& e : g_ev) if (OnDay(e, day) && CalOn(e)) list.push_back(&e);
        std::sort(list.begin(), list.end(), [](const Ev* a, const Ev* b) { return a->allDay != b->allDay ? a->allDay : a->start < b->start; });
        std::vector<const RemItem*> rl;
        for (auto& r : rems) if (DayStart(r.due) == day) rl.push_back(&r);
        total = (int)(list.size() + rl.size());
        int capacity = std::max(0, (int)((c.bottom - y - 4) / lineH));
        for (auto* e : list) {
            if (shown >= capacity - (total > capacity ? 1 : 0)) break;
            D2D1_RECT_F er = D2D1::RectF(c.left + 4, y, c.right - 4, y + lineH - 2);
            D2D1_COLOR_F col = EvColor(e->color);
            bool sel = e->id == g_selEv;
            if (e->allDay || sel) Fill(er, sel ? col : D2D1::ColorF(col.r, col.g, col.b, dark ? 0.35f : 0.22f), 4);
            else Circle({ er.left + 6, (er.top + er.bottom) / 2 }, 3.5f, col, true);
            std::wstring t = e->allDay ? e->title : Hm(e->start) + L"  " + e->title;
            if (e->id != g_editing) Text(t, g_tfEv, D2D1::RectF(er.left + (e->allDay || sel ? 6 : 14), er.top, er.right - 2, er.bottom), sel ? Rgb(255, 255, 255) : text);
            g_hits.push_back({ 1, e->id, day, er });
            y += lineH; shown++;
        }
        for (auto* r : rl) {
            if (shown >= capacity - (total > capacity ? 1 : 0)) break;
            D2D1_RECT_F er = D2D1::RectF(c.left + 4, y, c.right - 4, y + lineH - 2);
            Text(std::wstring(r->done ? L"☑ " : L"☐ ") + Hm(r->due) + L"  " + r->title, g_tfEv, D2D1::RectF(er.left + 4, er.top, er.right, er.bottom), gray);
            y += lineH; shown++;
        }
        if (total > shown) { wchar_t b[32]; swprintf_s(b, Tr(L"mais %d…"), total - shown); Text(b, g_tfEvSmall, D2D1::RectF(c.left + 8, y, c.right - 4, y + lineH - 2), gray); }
    }
}

void PaintGrid(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray, const std::vector<RemItem>& rems) {
    int cols = g_view == V_DAY ? 1 : 7;
    long long first = g_view == V_DAY ? DayStart(g_anchor) : WeekStart(g_anchor), today = DayStart(NowMin());
    float cw = (sz.width - LEFTCOL) / cols;
    D2D1_COLOR_F line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    // cabeçalho com os dias e a faixa de "dia inteiro"
    for (int d = 0; d < cols; d++) {
        long long day = AddDays(first, d);
        SYSTEMTIME s = FromMin(day);
        D2D1_RECT_F h = D2D1::RectF(LEFTCOL + d * cw, TB, LEFTCOL + (d + 1) * cw, TB + WEEKHEAD);
        wchar_t b[48]; swprintf_s(b, L"%ls %d", WeekdayShort(s.wDayOfWeek), s.wDay);
        if (day == today) Text(b, g_tfHeadDay, h, Rgb(255, 59, 48)); else Text(b, g_tfHeadDay, h, text);
        g_hits.push_back({ 2, -1, day, D2D1::RectF(h.left, TB + WEEKHEAD, h.right, TB + WEEKHEAD + ALLDAY) });
        float x = h.left + 2;
        for (auto& e : g_ev) if (e.allDay && OnDay(e, day) && CalOn(e)) {
            D2D1_RECT_F er = D2D1::RectF(x, TB + WEEKHEAD + 3, h.right - 2, TB + WEEKHEAD + ALLDAY - 3);
            D2D1_COLOR_F col = EvColor(e.color);
            Fill(er, e.id == g_selEv ? col : D2D1::ColorF(col.r, col.g, col.b, 0.3f), 4);
            if (e.id != g_editing) Text(e.title, g_tfEv, D2D1::RectF(er.left + 6, er.top, er.right, er.bottom), e.id == g_selEv ? Rgb(255, 255, 255) : text);
            g_hits.push_back({ 1, e.id, day, er });
            break;  // uma por coluna na faixa (cabe pouco)
        }
    }
    float top = TB + WEEKHEAD + ALLDAY;
    Fill(D2D1::RectF(0, top - 1, sz.width, top), line);
    rt->PushAxisAlignedClip(D2D1::RectF(0, top, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float y0 = top - g_scrollH;
    for (int hr = 0; hr < 24; hr++) {
        float y = y0 + hr * HOUR;
        Fill(D2D1::RectF(LEFTCOL, y, sz.width, y + 1), line);
        if (hr) { wchar_t b[8]; swprintf_s(b, L"%d:00", hr); Text(b, g_tfHour, D2D1::RectF(0, y - 9, LEFTCOL - 8, y + 9), gray); }
    }
    for (int d = 0; d < cols; d++) {
        long long day = AddDays(first, d);
        float x = LEFTCOL + d * cw;
        Fill(D2D1::RectF(x, top, x + 1, sz.height), line);
        g_hits.push_back({ 2, -1, day, D2D1::RectF(x, top, x + cw, sz.height) });
        for (auto& e : g_ev) {
            if (e.allDay || !OnDay(e, day) || !CalOn(e)) continue;
            long long s = std::max(e.start, day), en = std::min(std::max(e.end, e.start + 30), AddDays(day, 1));
            float ya = y0 + (s - day) / 60.f * HOUR, yb = y0 + (en - day) / 60.f * HOUR;
            D2D1_RECT_F er = D2D1::RectF(x + 2, ya + 1, x + cw - 3, std::max(ya + 18, yb - 1));
            D2D1_COLOR_F col = EvColor(e.color);
            bool sel = e.id == g_selEv;
            Fill(er, sel ? col : D2D1::ColorF(col.r, col.g, col.b, dark ? 0.35f : 0.2f), 5);
            Fill(D2D1::RectF(er.left, er.top, er.left + 3, er.bottom), col, 1.5f);
            if (e.id != g_editing) {
                Text(e.title, g_tfEv, D2D1::RectF(er.left + 8, er.top + 2, er.right - 2, er.top + 18), sel ? Rgb(255, 255, 255) : text);
                if (er.bottom - er.top > 34) Text(Hm(e.start) + L" – " + Hm(e.end), g_tfEvSmall, D2D1::RectF(er.left + 8, er.top + 18, er.right - 2, er.top + 34), sel ? Rgb(255, 255, 255) : gray);
            }
            g_hits.push_back({ 1, e.id, day, er });
        }
        for (auto& r : rems) if (DayStart(r.due) == day) {
            float ya = y0 + (r.due - day) / 60.f * HOUR;
            Text(std::wstring(r.done ? L"☑ " : L"☐ ") + r.title, g_tfEv, D2D1::RectF(x + 6, ya, x + cw - 4, ya + 18), gray);
        }
        if (day == today) {  // linha vermelha da hora atual
            long long now = NowMin();
            float y = y0 + (now - day) / 60.f * HOUR;
            Fill(D2D1::RectF(x, y - 1, x + cw, y + 1), Rgb(255, 59, 48));
            Circle({ x + 2, y }, 4, Rgb(255, 59, 48), true);
        }
    }
    rt->PopAxisAlignedClip();
}

// Ano: os 12 meses em miniatura (4 × 3); hoje no círculo vermelho; um ponto nos dias com eventos. Clique: o Dia.
void PaintYear(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray) {
    SYSTEMTIME a = FromMin(g_anchor);
    float top = TB + 8, mw = (sz.width - 40) / 4, mh = (sz.height - top - 8) / 3;
    long long today = DayStart(NowMin());
    SYSTEMTIME tn = FromMin(today);
    for (int m = 0; m < 12; m++) {
        float x0 = 20 + (m % 4) * mw, y0 = top + (m / 4) * mh;
        std::wstring mn = MonthName(m + 1);
        if (!idioma::En() && !mn.empty()) mn[0] = towupper(mn[0]);
        bool cur = a.wYear == tn.wYear && m + 1 == tn.wMonth;
        Text(mn, g_tfHeadDay, D2D1::RectF(x0 + 6, y0 + 4, x0 + mw - 10, y0 + 26), cur ? Rgb(255, 59, 48) : text);
        if (g_tfHeadDay) {}
        SYSTEMTIME s1{}; s1.wYear = a.wYear; s1.wMonth = (WORD)(m + 1); s1.wDay = 1;
        long long first = ToMin(s1), next = AddMonths(first, 1);
        int wd = ((int)FromMin(first).wDayOfWeek - g_firstDow + 7) % 7, days = (int)((DayStart(next + 12 * 60) - first + 12 * 60) / (24 * 60));
        float cw = (mw - 20) / 7, ch = std::min((mh - 50) / 6, cw);
        for (int d = 0; d < 7; d++) Text(std::wstring(1, WeekdayShort(DowAt(d))[0]), g_tfYearDay, D2D1::RectF(x0 + d * cw, y0 + 26, x0 + (d + 1) * cw, y0 + 42), gray);
        for (int d = 1; d <= days; d++) {
            int idx = wd + d - 1;
            D2D1_RECT_F c = D2D1::RectF(x0 + (idx % 7) * cw, y0 + 44 + (idx / 7) * ch, x0 + (idx % 7 + 1) * cw, y0 + 44 + (idx / 7 + 1) * ch);
            long long day = AddDays(first, d - 1);
            bool has = false; for (auto& e : g_ev) if (CalOn(e) && OnDay(e, day)) { has = true; break; }
            if (day == today) Circle({ (c.left + c.right) / 2, (c.top + c.bottom) / 2 }, std::min(cw, ch) * 0.42f, Rgb(255, 59, 48), true);
            Text(std::to_wstring(d), g_tfYearDay, c, day == today ? Rgb(255, 255, 255) : text);
            if (has && day != today) Circle({ (c.left + c.right) / 2, c.bottom - 2 }, 1.8f, Rgb(10, 132, 255), true);
            g_hits.push_back({ 8, -1, day, c });
        }
    }
}

// ================= macOS 27 (captura Calendar do 512 Pixels, 2x, medida em pt)
// Barra unificada de 52: na barra lateral (218,5, tingida) os botões de vidro calendários/caixa de entrada (77,5 × 36 a
// 133,5); no conteúdo o "+" redondo de 37 a 6 da lateral, a cápsula Dia/Semana/Mês/Ano de 241 × 36 no meio do conteúdo
// e a busca redonda de 36 a 8 da borda. Embaixo da barra o título (mês em negrito de 34 + ano normal) a 16,5 do
// conteúdo, centro a 79, e "‹ Hoje ›" (cápsulas cinza de 24) à direita. Lateral: seção a 68, linhas de 31,5 com a
// caixinha de 13 a 17,5 e o nome a 41,5; o mini mês embaixo (título a 190 do fundo, letras a 157,5, dias a cada
// 28 × 23,5 a partir de 133,5 do fundo).
void MiniMonth(float x0, float bottom, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray) {
    D2D1_COLOR_F soft = dark ? Rgb(255, 255, 255, 0.5f) : Rgb(0, 0, 0, 0.45f), faint = dark ? Rgb(255, 255, 255, 0.28f) : Rgb(0, 0, 0, 0.25f);
    Fill(D2D1::RectF(x0 + 19, bottom - 223.5f, x0 + 199, bottom - 223), dark ? Rgb(255, 255, 255, 0.12f) : Rgb(0, 0, 0, 0.10f));
    long long ms = MonthStart(g_selDay ? g_selDay : g_anchor), first = WeekStart(ms);
    SYSTEMTIME am = FromMin(ms);
    std::wstring mn = MonthName(am.wMonth);
    if (!idioma::En() && !mn.empty()) mn[0] = towupper(mn[0]);
    float ty = bottom - 190;
    Text(mn + L" " + std::to_wstring(am.wYear), g_tfMiniT, D2D1::RectF(x0 + 40, ty - 10, x0 + 180, ty + 10), soft);
    D2D1_RECT_F pv = D2D1::RectF(x0 + 16, ty - 10, x0 + 36, ty + 10), nx = D2D1::RectF(x0 + 184.5f, ty - 10, x0 + 204.5f, ty + 10);
    Text(L"‹", g_tfMonth, D2D1::RectF(pv.left, pv.top - 8, pv.right, pv.bottom + 4), soft);
    Text(L"›", g_tfMonth, D2D1::RectF(nx.left, nx.top - 8, nx.right, nx.bottom + 4), soft);
    g_hits.push_back({ 10, -1, 0, pv }); g_hits.push_back({ 10, 1, 0, nx });
    for (int d = 0; d < 7; d++) {
        float cx = x0 + 26.5f + d * 28;
        Text(std::wstring(1, WeekdayShort(DowAt(d))[0]), g_tfMiniW, D2D1::RectF(cx - 12, bottom - 167.5f, cx + 12, bottom - 147.5f), soft);
    }
    long long today = DayStart(NowMin());
    for (int i = 0; i < 42; i++) {
        long long day = AddDays(first, i);
        SYSTEMTIME s = FromMin(day);
        float cx = x0 + 26.5f + (i % 7) * 28, cy = bottom - 133.5f + (i / 7) * 23.5f;
        D2D1_RECT_F c = D2D1::RectF(cx - 12, cy - 11, cx + 12, cy + 11);
        bool other = s.wMonth != am.wMonth, wk = s.wDayOfWeek == 0 || s.wDayOfWeek == 6;
        if (day == today) Circle({ cx, cy }, 9.5f, Rgb(255, 59, 48), true);
        else if (day == g_selDay) Circle({ cx, cy }, 9.5f, dark ? Rgb(255, 255, 255, 0.14f) : Rgb(0, 0, 0, 0.08f), true);
        Text(std::to_wstring(s.wDay), g_tfMiniD, c, day == today ? Rgb(255, 255, 255) : other ? faint : wk ? soft : text);
        g_hits.push_back({ 11, -1, day, c });
    }
}

void PaintGolden() {
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    auto rems = Reminders();
    float X0 = g_side ? SIDE : 0;
    D2D1_SIZE_F csz{ sz.width - X0, sz.height };
    rt->SetTransform(D2D1::Matrix3x2F::Translation(X0, 0));
    size_t h0 = g_hits.size();
    if (g_view == V_YEAR) PaintYear(csz, dark, text, gray);
    else if (g_view == V_MONTH) PaintMonth(csz, dark, text, gray, rems); else PaintGrid(csz, dark, text, gray, rems);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    for (size_t i = h0; i < g_hits.size(); i++) { g_hits[i].r.left += X0; g_hits[i].r.right += X0; }
    // a faixa de cima do conteúdo (barra + título) por cima da grade
    Fill(D2D1::RectF(X0, 0, sz.width, TB), dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    if (g_side) {
        Fill(D2D1::RectF(0, 0, SIDE, sz.height), dark ? Rgb(43, 41, 40) : Rgb(237, 237, 236));
        Fill(D2D1::RectF(SIDE - 0.5f, 0, SIDE, sz.height), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.08f));
        // calendários / caixa de entrada (vidro): o primeiro esconde a lateral
        D2D1_RECT_F grp = D2D1::RectF(133.5f, 8, 211, 44);
        GlassButton(grp, dark);
        D2D1_RECT_F b1 = D2D1::RectF(grp.left + 4, 12, grp.left + 35, 40), b2 = D2D1::RectF(grp.right - 35, 12, grp.right - 4, 40);
        Fill(b1, dark ? Rgb(255, 255, 255, 0.14f) : Rgb(0, 0, 0, 0.08f), 14);
        IconSidebar({ (b1.left + b1.right) / 2, (b1.top + b1.bottom) / 2 }, text);
        {   // bandeja (caixa de entrada)
            float cx = (b2.left + b2.right) / 2, cy = (b2.top + b2.bottom) / 2;
            br->SetColor(text);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8, cy - 5, cx + 8, cy + 6), 2.5f, 2.5f), br, 1.4f);
            rt->DrawLine({ cx - 8, cy + 0.5f }, { cx - 3, cy + 0.5f }, br, 1.4f); rt->DrawLine({ cx + 3, cy + 0.5f }, { cx + 8, cy + 0.5f }, br, 1.4f);
            rt->DrawLine({ cx - 3, cy + 0.5f }, { cx - 2, cy + 2.5f }, br, 1.4f); rt->DrawLine({ cx - 2, cy + 2.5f }, { cx + 2, cy + 2.5f }, br, 1.4f); rt->DrawLine({ cx + 2, cy + 2.5f }, { cx + 3, cy + 0.5f }, br, 1.4f);
        }
        g_hits.push_back({ 5, 0, 0, b1 }); BarButton(grp);
        Text(Tr(L"Calendários"), g_tfSecG, D2D1::RectF(20, 58, SIDE - 14, 78), gray);
        float y = 84.5f;
        for (int i = 0; i < (int)g_cals.size(); i++) {
            auto& c = g_cals[i];
            D2D1_RECT_F r = D2D1::RectF(10, y, SIDE - 10, y + 31.5f);
            D2D1_RECT_F box = D2D1::RectF(17.5f, y + 9.25f, 30.5f, y + 22.25f);
            D2D1_COLOR_F col = EvColor(c.color);
            if (c.on) {
                Fill(box, col, 3);
                br->SetColor(Rgb(255, 255, 255));
                rt->DrawLine({ box.left + 3, box.top + 6.8f }, { box.left + 5.5f, box.top + 9.3f }, br, 1.6f);
                rt->DrawLine({ box.left + 5.5f, box.top + 9.3f }, { box.left + 10, box.top + 3.8f }, br, 1.6f);
            } else { br->SetColor(col); rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3, 3), br, 1.3f); }
            Text(c.name, g_tfSideG, D2D1::RectF(41.5f, r.top, r.right, r.bottom), text);
            g_hits.push_back({ 6, i, 0, r });
            y += 31.5f;
        }
        D2D1_RECT_F add = D2D1::RectF(17.5f, y + 2, SIDE - 14, y + 30);
        Text(L"+  " + std::wstring(Tr(L"Novo Calendário")), g_tfSideG, add, gray);
        g_hits.push_back({ 7, 0, 0, add });
        MiniMonth(0, sz.height, dark, text, gray);
    }
    // barra: "+" redondo, Dia/Semana/Mês/Ano, busca
    float cl = X0, cr = sz.width;
    float plusL = g_side ? cl + 6 : 84;
    if (!g_side) {  // lateral escondida: o botão dela vem para a barra, logo depois dos três botões (senão não voltava)
        D2D1_RECT_F sb = D2D1::RectF(84, 7.5f, 121, 44.5f);
        GlassButton(sb, dark);
        IconSidebar({ (sb.left + sb.right) / 2, 26 }, text);
        g_hits.push_back({ 5, 0, 0, sb }); BarButton(sb);
        plusL = sb.right + 8;
    }
    D2D1_RECT_F plus = D2D1::RectF(plusL, 7.5f, plusL + 37, 44.5f);
    GlassButton(plus, dark);
    br->SetColor(text);
    { float cx = (plus.left + plus.right) / 2, cy = 26; rt->DrawLine({ cx - 7, cy }, { cx + 7, cy }, br, 1.6f); rt->DrawLine({ cx, cy - 7 }, { cx, cy + 7 }, br, 1.6f); }
    g_hits.push_back({ 9, 0, 0, plus }); BarButton(plus);
    const wchar_t* names[] = { L"Dia", L"Semana", L"Mês", L"Ano" };
    float segW = 241, mid = (cl + cr) / 2;
    D2D1_RECT_F seg = D2D1::RectF(mid - segW / 2, 8, mid + segW / 2, 44);
    GlassButton(seg, dark);
    BarButton(seg);
    float iw = (segW - 8) / 4;
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = D2D1::RectF(seg.left + 4 + i * iw, 12, seg.left + 4 + (i + 1) * iw, 40);
        if (g_view == i) Fill(r, dark ? Rgb(255, 255, 255, 0.16f) : Rgb(0, 0, 0, 0.075f), 14);
        Text(Tr(names[i]), g_tfSeg, r, text);
        g_hits.push_back({ 4, i, 0, r });
    }
    D2D1_RECT_F srch = D2D1::RectF(cr - 8 - 36, 8, cr - 8, 44);
    GlassButton(srch, dark);
    IconSearch({ (srch.left + srch.right) / 2, 26 }, text);
    g_hits.push_back({ 12, 0, 0, srch }); BarButton(srch);
    // título
    SYSTEMTIME a = FromMin(g_anchor);
    std::wstring mon = g_view == V_YEAR ? L"" : MonthName(a.wMonth);
    if (!idioma::En() && !mon.empty()) mon[0] = towupper(mon[0]);
    float tx = cl + 16.5f, mw = mon.empty() ? 0 : TextW(g_tfTitleG, mon) + 10;
    if (!mon.empty()) Text(mon, g_tfTitleG, D2D1::RectF(tx, 58, tx + mw + 4, 100), text);
    Text(std::to_wstring(a.wYear), g_tfTitleYG, D2D1::RectF(tx + mw, 58, tx + mw + 160, 100), text);
    // ‹ Hoje ›
    D2D1_COLOR_F pill = dark ? Rgb(255, 255, 255, 0.12f) : Rgb(234, 234, 234);
    D2D1_RECT_F nx = D2D1::RectF(cr - 34.5f, 69, cr - 10.5f, 93), td = D2D1::RectF(nx.left - 71, 69, nx.left - 6, 93), pv = D2D1::RectF(td.left - 29.5f, 69, td.left - 5.5f, 93);
    Fill(pv, pill, 12); Fill(td, pill, 12); Fill(nx, pill, 12);
    Text(Tr(L"Hoje"), g_tfSeg, td, text);
    br->SetColor(text);
    { float cx = (pv.left + pv.right) / 2, cy = 81; rt->DrawLine({ cx + 2, cy - 5 }, { cx - 3, cy }, br, 1.6f); rt->DrawLine({ cx - 3, cy }, { cx + 2, cy + 5 }, br, 1.6f); }
    { float cx = (nx.left + nx.right) / 2, cy = 81; rt->DrawLine({ cx - 2, cy - 5 }, { cx + 3, cy }, br, 1.6f); rt->DrawLine({ cx + 3, cy }, { cx - 2, cy + 5 }, br, 1.6f); }
    g_hits.push_back({ 3, -1, 0, pv }); g_hits.push_back({ 3, 0, 0, td }); g_hits.push_back({ 3, 1, 0, nx });
}

void Paint() {
    if (g_holOn) { int y = FromMin(g_anchor).wYear; RequestHolidays(y); if (FromMin(g_anchor).wMonth == 12) RequestHolidays(y + 1); }  // o ano à vista
    if (!BeginPaint()) return;
    g_hits.clear();
    barButtons.clear();
    if (BAR > 0) { PaintGolden(); EndPaint(); return; }
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    auto rems = Reminders();
    // conteúdo deslocado pela barra lateral (os retângulos clicáveis também)
    float X0 = g_side ? SIDE : 0;
    D2D1_SIZE_F csz{ sz.width - X0, sz.height };
    rt->SetTransform(D2D1::Matrix3x2F::Translation(X0, 0));
    size_t h0 = g_hits.size();
    if (g_view == V_YEAR) PaintYear(csz, dark, text, gray);
    else if (g_view == V_MONTH) PaintMonth(csz, dark, text, gray, rems); else PaintGrid(csz, dark, text, gray, rems);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    for (size_t i = h0; i < g_hits.size(); i++) { g_hits[i].r.left += X0; g_hits[i].r.right += X0; }
    if (g_side) {  // calendários
        Fill(D2D1::RectF(0, TB, SIDE, sz.height), SidebarColor());
        Fill(D2D1::RectF(SIDE - 1, TB, SIDE, sz.height), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.08f));
        Text(Tr(L"Calendários"), g_tfEv, D2D1::RectF(14, TB + 10, SIDE - 14, TB + 30), gray);
        if (g_tfHeadDay) {}
        float y = TB + 36;
        for (int i = 0; i < (int)g_cals.size(); i++) {
            auto& c = g_cals[i];
            D2D1_RECT_F r = D2D1::RectF(8, y, SIDE - 8, y + 28);
            D2D1_RECT_F box = D2D1::RectF(r.left + 8, y + 7, r.left + 22, y + 21);
            D2D1_COLOR_F col = EvColor(c.color);
            if (c.on) { Fill(box, col, 3.5f); Text(L"✓", g_tfEvSmall, box, Rgb(255, 255, 255)); }
            else { br->SetColor(col); rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.5f, 3.5f), br, 1.5f); }
            Text(c.name, g_tfEv, D2D1::RectF(r.left + 32, r.top, r.right, r.bottom), text);
            g_hits.push_back({ 6, i, 0, r });
            y += 30;
        }
        D2D1_RECT_F add = D2D1::RectF(14, y + 6, SIDE - 14, y + 30);
        Text(L"⊕  " + std::wstring(Tr(L"Novo Calendário")), g_tfEv, add, gray);
        g_hits.push_back({ 7, 0, 0, add });
    }
    Fill(D2D1::RectF(0, 0, sz.width, TB), dark ? Rgb(40, 40, 42) : Rgb(246, 246, 248));
    Fill(D2D1::RectF(0, TB - 1, sz.width, TB), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.1f));
    D2D1_RECT_F tog = D2D1::RectF(10, 18, 40, 44);  // mostrar/esconder a barra lateral
    wchar_t sg[2] = { 0xE700, 0 }; Text(sg, g_tfIcon, tog, g_side ? Rgb(10, 132, 255) : text);
    g_hits.push_back({ 5, 0, 0, tog });
    rt->SetTransform(D2D1::Matrix3x2F::Translation(36, 0));
    size_t t0 = g_hits.size();
    Toolbar(D2D1::SizeF(sz.width - 36, sz.height), dark, text, gray);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    for (size_t i = t0; i < g_hits.size(); i++) { g_hits[i].r.left += 36; g_hits[i].r.right += 36; }
    EndPaint();
}

// ---- ações
void Rename(int id) {
    for (auto& h : g_hits) if (h.kind == 1 && h.ev == id) {
        Ev* e = FindEv(id);
        g_editing = id;
        FieldOpen(D2D1::RectF(h.r.left + 4, h.r.top, std::max(h.r.right, h.r.left + 160), h.r.top + 18), e ? e->title : L"", 12, false, [id](const std::wstring& t, int key) {
            g_editing = -1;
            Ev* e = FindEv(id);
            if (!e) return;
            if (t.empty() && key != VK_ESCAPE) { g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& x) { return x.id == id; }), g_ev.end()); g_selEv = -1; }
            else if (!t.empty()) e->title = t;
            Save();
        });
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
}

void NewEvent(long long day, long long at, bool allDay) {
    Ev e{ g_nextId++, allDay ? day : at, allDay ? AddDays(day, 1) : at + 60, allDay, Tr(L"Novo Evento"), 0, L"" };
    for (auto& c : g_cals) if (c.on && c.id != HOL_CAL) { e.cal = c.id; e.color = c.color; break; }  // no primeiro calendário à vista
    g_ev.push_back(e);
    g_selEv = e.id; g_selDay = day;
    Save();
    Paint();
    Rename(e.id);
}

void EvMenu(int id, POINT scr) {
    Ev* e = FindEv(id);
    if (!e) return;
    HMENU m = CreatePopupMenu(), tm = CreatePopupMenu(), dur = CreatePopupMenu(), col = CreatePopupMenu();
    AppendMenuW(tm, MF_STRING | (e->allDay ? MF_CHECKED : 0), 100, Tr(L"Dia Inteiro"));
    AppendMenuW(tm, MF_SEPARATOR, 0, nullptr);
    for (int h = 7; h <= 21; h++) { wchar_t b[16]; swprintf_s(b, L"%d:00", h); AppendMenuW(tm, MF_STRING, 100 + h, b); }
    int mins[] = { 15, 30, 60, 90, 120, 180 };
    const wchar_t* dn[] = { L"15 min", L"30 min", L"1 h", L"1 h 30", L"2 h", L"3 h" };
    for (int i = 0; i < 6; i++) AppendMenuW(dur, MF_STRING | (e->allDay ? MF_GRAYED : 0), 200 + i, dn[i]);
    for (int i = 0; i < 7; i++) AppendMenuW(col, MF_STRING | (e->color == i ? MF_CHECKED : 0), 300 + i, Tr(COLOR_NAMES[i]));
    HMENU cal = CreatePopupMenu();
    for (int i = 0; i < (int)g_cals.size(); i++) AppendMenuW(cal, MF_STRING | (e->cal == g_cals[i].id ? MF_CHECKED : 0), 500 + i, g_cals[i].name.c_str());
    AppendMenuW(m, MF_STRING, 1, Tr(L"Renomear"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, Tr(L"Horário"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dur, Tr(L"Duração"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)cal, Tr(L"Calendário"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)col, Tr(L"Cor"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, Tr(L"Apagar"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    long long day = DayStart(e->start);
    if (c == 100) { e->allDay = true; e->start = day; e->end = AddDays(day, 1); }
    else if (c > 100 && c < 200) { long long len = e->allDay ? 60 : std::max(15LL, e->end - e->start); e->allDay = false; e->start = day + (c - 100) * 60; e->end = e->start + len; }
    else if (c >= 200 && c < 206) e->end = e->start + mins[c - 200];
    else if (c >= 300 && c < 307) e->color = c - 300;
    else if (c >= 500 && c < 500 + (int)g_cals.size()) { e->cal = g_cals[c - 500].id; e->color = g_cals[c - 500].color; }
    else if (c == 2) { g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& x) { return x.id == id; }), g_ev.end()); g_selEv = -1; }
    else if (c == 1) { Paint(); Rename(id); return; }
    if (c) Save();
    InvalidateRect(wnd, nullptr, FALSE);
}

void Nav(int dir) {
    if (dir == 0) { g_anchor = g_selDay = DayStart(NowMin()); }
    else if (g_view == V_YEAR) g_anchor = AddMonths(g_anchor, dir * 12);
    else if (g_view == V_MONTH) g_anchor = AddMonths(g_anchor, dir);
    else g_anchor = AddDays(DayStart(g_anchor), dir * (g_view == V_WEEK ? 7 : 1));
    InvalidateRect(wnd, nullptr, FALSE);
}

// ---- .ics (o básico: DTSTART, DTEND, SUMMARY, DESCRIPTION; datas locais ou UTC "Z")
long long IcsTime(const std::wstring& v, bool& allDay) {
    allDay = v.size() == 8;
    if (v.size() < 8) return 0;
    SYSTEMTIME s{}; s.wYear = (WORD)_wtoi(v.substr(0, 4).c_str()); s.wMonth = (WORD)_wtoi(v.substr(4, 2).c_str()); s.wDay = (WORD)_wtoi(v.substr(6, 2).c_str());
    if (v.size() >= 15) { s.wHour = (WORD)_wtoi(v.substr(9, 2).c_str()); s.wMinute = (WORD)_wtoi(v.substr(11, 2).c_str()); }
    if (!v.empty() && v.back() == L'Z') { SYSTEMTIME loc; SystemTimeToTzSpecificLocalTime(nullptr, &s, &loc); s = loc; }
    return ToMin(s);
}
void ImportIcs() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"iCalendar (.ics)", L"*.ics" } };
    d->SetFileTypes(1, f);
    std::wstring path;
    if (SUCCEEDED(d->Show(wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); } it->Release(); } }
    d->Release();
    if (path.empty()) return;
    auto lines = ReadLines(path);
    // linhas dobradas (começam com espaço) continuam a anterior
    std::vector<std::wstring> un;
    for (auto& l : lines) { if (!l.empty() && (l[0] == L' ' || l[0] == L'\t') && !un.empty()) un.back() += l.substr(1); else un.push_back(l); }
    int n = 0; Ev cur{}; bool in = false;
    for (auto& l : un) {
        size_t c = l.find(L':'); std::wstring key = c == std::wstring::npos ? l : l.substr(0, c), val = c == std::wstring::npos ? L"" : l.substr(c + 1);
        std::wstring k = key.substr(0, key.find(L';'));
        if (l == L"BEGIN:VEVENT") { cur = Ev{ 0, 0, 0, false, L"", 0, L"" }; in = true; }
        else if (l == L"END:VEVENT" && in) {
            in = false;
            if (cur.start) { cur.id = g_nextId++; if (!cur.end || cur.end <= cur.start) cur.end = cur.allDay ? AddDays(cur.start, 1) : cur.start + 60; g_ev.push_back(cur); n++; }
        } else if (in && k == L"DTSTART") { bool ad; cur.start = IcsTime(val, ad); cur.allDay = ad || key.find(L"VALUE=DATE") != std::wstring::npos; }
        else if (in && k == L"DTEND") { bool ad; cur.end = IcsTime(val, ad); }
        else if (in && k == L"SUMMARY") { std::wstring t; for (size_t i = 0; i < val.size(); i++) { if (val[i] == L'\\' && i + 1 < val.size()) { i++; t += val[i] == L'n' ? L' ' : val[i]; } else t += val[i]; } cur.title = t; }
    }
    Save();
    wchar_t b[96]; swprintf_s(b, Tr(L"%d eventos importados."), n);
    MessageBoxW(wnd, b, Tr(L"Calendário"), MB_OK | MB_ICONINFORMATION);
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
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;  // a linha da hora atual e os lembretes
    case WM_HOLIDAYS: HolidaysArrived((int)w, (std::vector<HolDay>*)l); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEWHEEL: {
        if (field.h) FieldEnd(0);
        float d = GET_WHEEL_DELTA_WPARAM(w) / 120.f;
        if (g_view == V_MONTH) Nav(d > 0 ? -1 : 1);
        else { g_scrollH = std::clamp(g_scrollH - d * 60, 0.f, std::max(0.f, 24 * HOUR - (Size().height - TB - WEEKHEAD - ALLDAY))); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONUP: {
        if (field.h) FieldEnd(0);
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
        bool dbl = m == WM_LBUTTONDBLCLK, right = m == WM_RBUTTONUP;
        for (auto& ht : g_hits) if (ht.kind == 4 && In(ht.r, x, y)) { g_view = (View)ht.ev; InvalidateRect(h, nullptr, FALSE); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 5 && In(ht.r, x, y)) { g_side = !g_side; InvalidateRect(h, nullptr, FALSE); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 8 && In(ht.r, x, y)) { g_anchor = g_selDay = ht.day; g_view = V_DAY; InvalidateRect(h, nullptr, FALSE); return 0; }  // Ano: o dia
        for (auto& ht : g_hits) if (ht.kind == 7 && In(ht.r, x, y)) {
            Cal c{ g_nextId++, Tr(L"Novo Calendário"), (int)g_cals.size() % 7, true };
            g_cals.push_back(c); Save(); Paint();
            for (auto& k : g_hits) if (k.kind == 6 && k.ev == (int)g_cals.size() - 1) {
                int id = c.id;
                FieldOpen(D2D1::RectF(k.r.left + 32, k.r.top, k.r.right, k.r.bottom), c.name, 12, false, [id](const std::wstring& t, int key) { if (key != VK_ESCAPE && !t.empty()) if (Cal* x = FindCal(id)) { x->name = t; Save(); } });
            }
            return 0;
        }
        for (auto& ht : g_hits) if (ht.kind == 6 && In(ht.r, x, y)) {
            Cal& c = g_cals[ht.ev];
            if (!right) { c.on = !c.on; Save(); if (c.id == HOL_CAL && c.on) RequestHolidays(FromMin(g_anchor).wYear); InvalidateRect(h, nullptr, FALSE); return 0; }
            if (c.id == HOL_CAL) return 0;  // feriados: só ligar e desligar
            HMENU mm = CreatePopupMenu(), col = CreatePopupMenu();
            for (int i = 0; i < 7; i++) AppendMenuW(col, MF_STRING | (c.color == i ? MF_CHECKED : 0), 10 + i, Tr(COLOR_NAMES[i]));
            AppendMenuW(mm, MF_STRING, 1, Tr(L"Renomear"));
            AppendMenuW(mm, MF_POPUP, (UINT_PTR)col, Tr(L"Cor"));
            AppendMenuW(mm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(mm, MF_STRING | (g_cals.size() > 1 ? 0 : MF_GRAYED), 2, Tr(L"Apagar Calendário"));
            int k = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr);
            DestroyMenu(mm);
            int id = c.id;
            if (k >= 10) { c.color = k - 10; for (auto& e : g_ev) if (e.cal == id) e.color = c.color; Save(); }
            else if (k == 1) FieldOpen(D2D1::RectF(ht.r.left + 32, ht.r.top, ht.r.right, ht.r.bottom), c.name, 12, false, [id](const std::wstring& t, int key) { if (key != VK_ESCAPE && !t.empty()) if (Cal* x = FindCal(id)) { x->name = t; Save(); } });
            else if (k == 2) {
                int n = 0; for (auto& e : g_ev) if (e.cal == id) n++;
                wchar_t q[200]; swprintf_s(q, Tr(L"Apagar o calendário “%ls” e os %d eventos dele?"), c.name.c_str(), n);
                if (MessageBoxW(h, q, Tr(L"Calendário"), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                    g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& e) { return e.cal == id; }), g_ev.end());
                    g_cals.erase(g_cals.begin() + ht.ev); Save();
                }
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (auto& ht : g_hits) if (ht.kind == 3 && In(ht.r, x, y)) { Nav(ht.ev); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 9 && In(ht.r, x, y)) { NewEvent(g_selDay, g_selDay + 9 * 60, g_view == V_MONTH); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 10 && In(ht.r, x, y)) { g_selDay = AddMonths(MonthStart(g_selDay), ht.ev); InvalidateRect(h, nullptr, FALSE); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 13 && right && In(ht.r, x, y)) {  // nomes dos dias: começar a semana em...
            HMENU mm = CreatePopupMenu();
            AppendMenuW(mm, MF_STRING | (g_firstDow == 0 ? MF_CHECKED : 0), 1, Tr(L"Começar a Semana no Domingo"));
            AppendMenuW(mm, MF_STRING | (g_firstDow == 1 ? MF_CHECKED : 0), 2, Tr(L"Começar a Semana na Segunda-feira"));
            if (!g_holCC.empty()) {
                AppendMenuW(mm, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(mm, MF_STRING | (g_holOn ? MF_CHECKED : 0), 3, Tr(L"Mostrar Feriados"));
            }
            int k = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr);
            DestroyMenu(mm);
            if (k == 1 || k == 2) { g_firstDow = k - 1; g_firstDowSet = true; Save(); }
            if (k == 3) { if (Cal* hc = FindCal(HOL_CAL)) { hc->on = !hc->on; Save(); if (hc->on) RequestHolidays(FromMin(g_anchor).wYear); } }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (auto& ht : g_hits) if (ht.kind == 11 && In(ht.r, x, y)) { g_anchor = g_selDay = ht.day; InvalidateRect(h, nullptr, FALSE); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 12 && In(ht.r, x, y)) {  // busca: digita e Enter vai ao próximo evento com o texto
            D2D1_RECT_F fr = D2D1::RectF(ht.r.left - 200, ht.r.top + 8, ht.r.left - 8, ht.r.bottom - 8);
            FieldOpen(fr, g_search, 13, false, [](const std::wstring& q, int key) {
                g_search = q;
                if (key != VK_RETURN || q.empty()) return;
                std::wstring lq = q; for (auto& c : lq) c = towlower(c);
                const Ev* best = nullptr;
                for (auto& e : g_ev) {
                    std::wstring tt = e.title; for (auto& c : tt) c = towlower(c);
                    if (tt.find(lq) == std::wstring::npos) continue;
                    if (!best || (e.start >= g_selDay && (best->start < g_selDay || e.start < best->start))) best = &e;
                }
                if (best) { g_anchor = g_selDay = DayStart(best->start); g_selEv = best->id; }
                else MessageBeep(MB_ICONASTERISK);
                InvalidateRect(wnd, nullptr, FALSE);
            });
            return 0;
        }
        for (int i = (int)g_hits.size() - 1; i >= 0; i--) {
            auto& ht = g_hits[i];
            if (ht.kind != 1 || !In(ht.r, x, y)) continue;
            g_selEv = ht.ev; g_selDay = ht.day;
            if (ht.ev < 0) { g_selEv = -1; InvalidateRect(h, nullptr, FALSE); return 0; }  // feriado: só de leitura
            if (right) { InvalidateRect(h, nullptr, FALSE); EvMenu(ht.ev, scr); return 0; }
            if (dbl) { Rename(ht.ev); return 0; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (auto& ht : g_hits) if (ht.kind == 2 && In(ht.r, x, y)) {
            g_selDay = ht.day; g_selEv = -1;
            if (dbl) {
                if (g_view == V_MONTH || y < TB + WEEKHEAD + ALLDAY) NewEvent(ht.day, 0, true);
                else {
                    float top = TB + WEEKHEAD + ALLDAY;
                    int mins = (int)((y - top + g_scrollH) / HOUR * 60) / 30 * 30;
                    NewEvent(ht.day, ht.day + mins, false);
                }
                return 0;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && w >= '1' && w <= '4') { g_view = (View)(w - '1'); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (ctrl && w == 'I') { ImportIcs(); return 0; }
        if (ctrl && w == 'N') { NewEvent(g_selDay, g_selDay + 9 * 60, g_view == V_MONTH); return 0; }
        if (ctrl && w == 'W') { DestroyWindow(h); return 0; }
        if (ctrl && w == 'T') { Nav(0); return 0; }
        if (w == VK_LEFT) Nav(-1);
        else if (w == VK_RIGHT) Nav(1);
        else if ((w == VK_DELETE || w == VK_BACK) && g_selEv >= 0) {
            int id = g_selEv;
            g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& x) { return x.id == id; }), g_ev.end());
            g_selEv = -1; Save(); InvalidateRect(h, nullptr, FALSE);
        } else if (w == VK_RETURN && g_selEv >= 0) Rename(g_selEv);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_file = DataPath(L"calendario.txt");
    g_remFile = DataPath(L"lembretes.txt");
    Load();
    g_anchor = g_selDay = DayStart(NowMin());
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfMonth = Fmt(26, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfYear = Fmt(26, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfBtn = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfHeadDay = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfDayNum = Fmt(13, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfEv = Fmt(11.5f);
    g_tfEvSmall = Fmt(10.5f);
    g_tfYearDay = Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfHour = Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfIcon = Fmt(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    if (g_tfMonth) { g_tfMonth->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); }
    g_tfTitleG = Fmt(34, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfTitleYG = Fmt(34, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfSeg = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfSideG = Fmt(13);
    g_tfSideBold = Fmt(13, DWRITE_FONT_WEIGHT_BOLD);
    g_tfSecG = Fmt(11, DWRITE_FONT_WEIGHT_BOLD);
    g_tfMiniT = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfMiniD = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfMiniW = Fmt(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    barWanted = 52;  // macOS 27: barra unificada
    if (semaforo::Aqua()) { TB = 112; SIDE = 218.5f; }
    if (!CreateMain(hi, L"CalendarioJanela", Tr(L"Calendário"), Proc, semaforo::Aqua() ? 1137 : 1100, semaforo::Aqua() ? 760 : 760)) return 1;
    SetTimer(wnd, 1, 30000, nullptr);
    LoadHolidayCache();
    ShowWindow(wnd, show);
    return Run();
}
