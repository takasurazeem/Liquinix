// Lembretes (Reminders do Mac). À esquerda: Hoje, Agendados, Todos e Concluídos (quadrinhos com a contagem) e
// "Minhas Listas"; à direita os lembretes da lista escolhida, com o círculo para concluir, o nome, as notas e a
// data. Clique no nome edita; Enter cria o próximo (como no Mac); vazio apaga. Botão direito: data, sinalizar,
// mover e apagar. Os dados ficam em %APPDATA%\Shell\lembretes.txt; o Shell lê o arquivo e mostra o aviso
// (banner do Mac) quando a hora chega, mesmo com o app fechado.
#include "../comum/app2d.h"
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <map>
#include <set>

using namespace app2d;

namespace {

const float ROW = 40, ROW2 = 54, PADX = 22;
float SB = 250, HEAD = 76;  // macOS 27: 358 e 143
bool g_hideSide;            // macOS 27: o botão da barra lateral a esconde
std::wstring g_filter;      // macOS 27: a busca da barra filtra pelo nome
IDWriteTextFormat *g_tfCardG, *g_tfCardNumG, *g_tfTitleG, *g_tfTitleRG, *g_tfRowG, *g_tfSubG, *g_tfListG, *g_tfListBoldG, *g_tfHeadG, *g_tfInfoG, *g_tfInfoRG;
const UINT WM_FIELDLOST = WM_APP + 77;

struct List { int id; std::wstring name; int color; };
// repeat: 0 não, 1 dia, 2 dias de semana, 3 semana, 4 mês, 5 ano; parent: é subtarefa deste id
struct Rem { int id; int list; bool done; long long due; std::wstring title, notes; bool flag; long long doneAt = 0; int repeat = 0; int parent = 0; };
std::vector<List> g_lists;
std::vector<Rem> g_rems;
int g_nextId = 1;
// seleção: >= 0 = lista; negativos = listas inteligentes
enum { S_TODAY = -1, S_SCHED = -2, S_ALL = -3, S_DONE = -4, S_FLAG = -5 };
int g_sel = 0;
bool g_showDone = false;
float g_scroll = 0;
int g_editing = -1;  // id do lembrete em edição
std::wstring g_file;
IDWriteTextFormat *g_tfTitle, *g_tfBig, *g_tfBigR, *g_tfRow, *g_tfSub, *g_tfSide, *g_tfSideBold, *g_tfTile, *g_tfTileNum, *g_tfIcon, *g_tfHead, *g_tfSmall;

// cores das listas (as do Mac)
D2D1_COLOR_F ListColor(int i) {
    static const D2D1_COLOR_F c[] = { Rgb(10, 132, 255), Rgb(255, 69, 58), Rgb(255, 159, 10), Rgb(255, 214, 10), Rgb(48, 209, 88),
                                      Rgb(100, 210, 255), Rgb(191, 90, 242), Rgb(255, 55, 95), Rgb(172, 142, 104), Rgb(142, 142, 147) };
    return c[((i % 10) + 10) % 10];
}
const wchar_t* COLOR_NAMES[] = { L"Azul", L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Ciano", L"Roxo", L"Rosa", L"Marrom", L"Cinza" };

void Save() {
    std::vector<std::wstring> out;
    out.push_back(L"# Lembretes do WacOS (L = lista, R = lembrete)");
    for (auto& l : g_lists) out.push_back(L"L\t" + std::to_wstring(l.id) + L"\t" + Esc(l.name) + L"\t" + std::to_wstring(l.color));
    for (auto& r : g_rems)
        out.push_back(L"R\t" + std::to_wstring(r.id) + L"\t" + std::to_wstring(r.list) + L"\t" + (r.done ? L"1" : L"0") + L"\t" + std::to_wstring(r.due) + L"\t" +
                      Esc(r.title) + L"\t" + Esc(r.notes) + L"\t" + (r.flag ? L"1" : L"0") + L"\t" + std::to_wstring(r.repeat) + L"\t" + std::to_wstring(r.parent));
    WriteLines(g_file, out);
}

void Load() {
    g_lists.clear(); g_rems.clear();
    for (auto& line : ReadLines(g_file)) {
        auto f = Split(line);
        if (f.size() >= 4 && f[0] == L"L") g_lists.push_back({ _wtoi(f[1].c_str()), Unesc(f[2]), _wtoi(f[3].c_str()) });
        else if (f.size() >= 8 && f[0] == L"R") {
            Rem r{ _wtoi(f[1].c_str()), _wtoi(f[2].c_str()), f[3] == L"1", _wtoi64(f[4].c_str()), Unesc(f[5]), Unesc(f[6]), f[7] == L"1" };
            if (f.size() >= 10) { r.repeat = _wtoi(f[8].c_str()); r.parent = _wtoi(f[9].c_str()); }
            g_rems.push_back(r);
        }
    }
    if (g_lists.empty()) g_lists.push_back({ 1, Tr(L"Lembretes"), 0 });
    for (auto& l : g_lists) g_nextId = std::max(g_nextId, l.id + 1);
    for (auto& r : g_rems) g_nextId = std::max(g_nextId, r.id + 1);
    bool ok = false; for (auto& l : g_lists) if (l.id == g_sel) ok = true;
    if (g_sel >= 0 && !ok) g_sel = g_lists[0].id;
}

List* FindList(int id) { for (auto& l : g_lists) if (l.id == id) return &l; return nullptr; }
Rem* FindRem(int id) { for (auto& r : g_rems) if (r.id == id) return &r; return nullptr; }

bool IsToday(const Rem& r) { return r.due && DayStart(r.due) <= DayStart(NowMin()); }
bool Visible(const Rem& r, int sel) {
    bool fresh = r.done && r.doneAt && GetTickCount64() / 60000 - r.doneAt < 1;  // acabou de concluir: fica um instante
    bool open = !r.done || fresh || (g_showDone && sel >= 0);
    switch (sel) {
    case S_TODAY: return open && IsToday(r);
    case S_SCHED: return open && r.due;
    case S_ALL: return open;
    case S_DONE: return r.done;
    case S_FLAG: return open && r.flag;
    default: return open && r.list == sel;
    }
}
int Count(int sel) { int n = 0; for (auto& r : g_rems) if ((sel == S_DONE ? r.done : !r.done) && Visible(r, sel)) n++; return n; }

std::vector<Rem*> Shown() {
    std::vector<Rem*> v;
    std::wstring lf = g_filter; for (auto& c : lf) c = towlower(c);
    for (auto& r : g_rems) {
        if (!Visible(r, g_sel)) continue;
        if (!lf.empty()) { std::wstring tt = r.title + L" " + r.notes; for (auto& c : tt) c = towlower(c); if (tt.find(lf) == std::wstring::npos) continue; }
        v.push_back(&r);
    }
    if (g_sel == S_SCHED || g_sel == S_TODAY) { std::stable_sort(v.begin(), v.end(), [](Rem* a, Rem* b) { return a->due < b->due; }); return v; }
    // subtarefas logo abaixo da tarefa de cima (como no Mac); sem a de cima à vista, ficam soltas
    std::vector<Rem*> o;
    auto shown = [&](int id) { for (auto* x : v) if (x->id == id) return true; return false; };
    for (auto* r : v) {
        if (r->parent && shown(r->parent)) continue;
        o.push_back(r);
        for (auto* c : v) if (c->parent == r->id) o.push_back(c);
    }
    return o;
}

std::wstring SelName() {
    switch (g_sel) {
    case S_TODAY: return Tr(L"Hoje");
    case S_SCHED: return Tr(L"Agendados");
    case S_ALL: return TrPick(L"Todos", L"All");
    case S_DONE: return Tr(L"Concluídos");
    case S_FLAG: return Tr(L"Sinalizados");
    }
    List* l = FindList(g_sel); return l ? l->name : L"";
}
D2D1_COLOR_F SelColor() {
    switch (g_sel) {
    case S_TODAY: return Rgb(10, 132, 255);
    case S_SCHED: return Rgb(255, 69, 58);
    case S_ALL: return Dark() ? Rgb(230, 230, 230) : Rgb(60, 60, 67);
    case S_DONE: return Rgb(142, 142, 147);
    case S_FLAG: return Rgb(255, 159, 10);
    }
    List* l = FindList(g_sel); return l ? ListColor(l->color) : Rgb(10, 132, 255);
}

// ---- geometria
struct Hit { int kind; int arg; D2D1_RECT_F r; };  // kind: 1 tile, 2 lista, 3 adicionar lista, 4 círculo, 5 nome, 6 + novo, 7 mostrar concluídos
std::vector<Hit> g_hits;
struct RowPos { int id; float top, bottom; D2D1_RECT_F circle, title; };
std::vector<RowPos> g_rows;

// ================= macOS 27 (captura Reminders do 512 Pixels, 2x, medida em pt)
// Barra unificada de 52. Lateral de 358 (tingida): botões de vidro redondos de 36 (nova lista e esconder a lateral) a
// 271 e 315; cartões de 162,5 × 59 em degradê (cantos 10) a cada 66 a partir de 53,5, a 17,5 e 186,5 da borda, com
// o símbolo e o número (negrito de 18) em cima e o nome (negrito de 15) embaixo; "Minhas Listas" e as listas em
// linhas de 36 com o círculo de 24 a 38 e o nome a 63, a contagem à direita; a escolhida numa pílula cinza.
// Conteúdo: cápsula de vidro (compartilhar, formato, +) de 110 e a busca de 224,5 a 8 da borda; título em negrito de
// 31 na cor da lista a 17 do conteúdo (centro 78,5) com a contagem à direita; "N Concluídos • Limpar" e "Mostrar" a
// 125,5; linha a 142; lembretes em linhas de 32 (com notas, 49,5) com o círculo de 17 a 26,5 e o nome a 47.
void PaintGolden() {
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    float S0 = g_hideSide ? 0.f : SB;
    if (!g_hideSide) {
        Fill(D2D1::RectF(0, 0, SB, sz.height), dark ? Rgb(43, 41, 40) : Rgb(237, 237, 236));
        Fill(D2D1::RectF(SB - 0.5f, 0, SB, sz.height), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.08f));
        D2D1_RECT_F nl = D2D1::RectF(271, 8, 307, 44), tg = D2D1::RectF(315, 8, 351, 44);
        GlassButton(nl, dark); GlassButton(tg, dark);
        {   // nova lista: a lista com o "+" no canto
            float cx = 289, cy = 26;
            br->SetColor(text);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8, cy - 7, cx + 7, cy + 7), 2.5f, 2.5f), br, 1.4f);
            rt->DrawEllipse(D2D1::Ellipse({ cx - 4, cy - 2.5f }, 1.2f, 1.2f), br, 1.1f); rt->DrawLine({ cx - 1.5f, cy - 2.5f }, { cx + 4, cy - 2.5f }, br, 1.3f);
            rt->DrawEllipse(D2D1::Ellipse({ cx - 4, cy + 2.5f }, 1.2f, 1.2f), br, 1.1f); rt->DrawLine({ cx - 1.5f, cy + 2.5f }, { cx + 2, cy + 2.5f }, br, 1.3f);
            Circle({ cx + 6, cy + 5.5f }, 4.6f, text, true);
            br->SetColor(dark ? Rgb(60, 58, 57) : Rgb(245, 245, 245));
            rt->DrawLine({ cx + 3.6f, cy + 5.5f }, { cx + 8.4f, cy + 5.5f }, br, 1.3f); rt->DrawLine({ cx + 6, cy + 3.1f }, { cx + 6, cy + 7.9f }, br, 1.3f);
        }
        IconSidebar({ 333, 26 }, text);
        g_hits.push_back({ 3, 0, nl }); g_hits.push_back({ 9, 0, tg }); BarButton(nl); BarButton(tg);
        struct T { int sel; const wchar_t* name; wchar_t glyph; UINT32 top, bot; } tiles[] = {
            { S_TODAY, L"Hoje", 0xE787, 0x6EB6EB, 0x4A96DE }, { S_SCHED, L"Agendados", 0xE787, 0xEE8C8C, 0xE1696E },
            { S_ALL, L"Todos", 0xE8FD, 0x737373, 0x505050 }, { S_FLAG, L"Sinalizados", 0xE7C1, 0xF3BE8C, 0xE8A064 },
            { S_DONE, L"Concluídos", 0xE73E, 0xA5AAAF, 0x82878E } };
        for (int i = 0; i < 5; i++) {
            float x = i % 2 ? 186.5f : 17.5f, y = 53.5f + (i / 2) * 66.f;
            D2D1_RECT_F r = D2D1::RectF(x, y, x + 162.5f, y + 59);
            auto col = [](UINT32 c) { return Rgb((c >> 16) & 255, (c >> 8) & 255, c & 255); };
            D2D1_GRADIENT_STOP st[2] = { { 0, col(tiles[i].top) }, { 1, col(tiles[i].bot) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) && SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ r.left, r.top }, { r.right, r.bottom }), gs, &lb)))
                rt->FillRoundedRectangle(D2D1::RoundedRect(r, 10, 10), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            if (g_sel == tiles[i].sel) { br->SetColor(Rgb(255, 255, 255, 0.85f)); rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), 9, 9), br, 2); }
            wchar_t g[2] = { tiles[i].glyph, 0 };
            Text(g, g_tfIcon, D2D1::RectF(x + 8, y + 6, x + 32, y + 30), Rgb(255, 255, 255));
            Text(std::to_wstring(Count(tiles[i].sel)), g_tfCardNumG, D2D1::RectF(x, y + 6, x + 162.5f - 12, y + 32), Rgb(255, 255, 255));
            Text(tiles[i].sel == S_ALL ? TrPick(L"Todos", L"All") : Tr(tiles[i].name), g_tfCardG, D2D1::RectF(x + 10, y + 28, x + 158, y + 52), Rgb(255, 255, 255));
            g_hits.push_back({ 1, tiles[i].sel, r });
        }
        float y = 53.5f + 3 * 66.f + 12;
        Text(Tr(L"Minhas Listas"), g_tfHeadG, D2D1::RectF(25, y, SB - 10, y + 22), gray);
        y += 26;
        for (auto& l : g_lists) {
            D2D1_RECT_F r = D2D1::RectF(13, y, SB - 6, y + 36);
            bool on = g_sel == l.id;
            if (on) Fill(r, dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.075f), 10);
            Circle({ 38, y + 18 }, 12, ListColor(l.color), true);
            br->SetColor(Rgb(255, 255, 255));
            for (int k = -1; k <= 1; k++) { rt->FillEllipse(D2D1::Ellipse({ 33.5f, y + 18 + k * 4.f }, 1.2f, 1.2f), br); rt->DrawLine({ 36, y + 18 + k * 4.f }, { 43.5f, y + 18 + k * 4.f }, br, 1.4f); }
            Text(l.name, on ? g_tfListBoldG : g_tfListG, D2D1::RectF(63, r.top, r.right - 40, r.bottom), text);
            Text(std::to_wstring(Count(l.id)), g_tfInfoRG, D2D1::RectF(r.right - 60, r.top, r.right - 14, r.bottom), gray);
            g_hits.push_back({ 2, l.id, r });
            y += 36;
        }
        D2D1_RECT_F add = D2D1::RectF(17.5f, sz.height - 40, SB - 10, sz.height - 10);
        Text(L"+  " + std::wstring(Tr(L"Adicionar Lista")), g_tfInfoG, add, gray);
        g_hits.push_back({ 3, 0, add });
    }
    // ---- barra do conteúdo
    float W = sz.width;
    if (g_hideSide) {  // a lateral escondida: o botão de mostrar vai para a barra, depois dos três botões
        D2D1_RECT_F tg = D2D1::RectF(84, 8, 120, 44);
        GlassButton(tg, dark); IconSidebar({ 102, 26 }, text);
        g_hits.push_back({ 9, 0, tg }); BarButton(tg);
    }
    D2D1_RECT_F srch = D2D1::RectF(W - 8 - 224.5f, 8, W - 8, 44);
    D2D1_RECT_F grp = D2D1::RectF(srch.left - 7 - 110, 8, srch.left - 7, 44);
    GlassButton(grp, dark); GlassButton(srch, dark);
    BarButton(grp); BarButton(srch);
    br->SetColor(text);
    {   // compartilhar
        float cx = grp.left + 20, cy = 27;
        rt->DrawLine({ cx - 2.5f, cy - 3 }, { cx - 6, cy - 3 }, br, 1.4f); rt->DrawLine({ cx - 6, cy - 3 }, { cx - 6, cy + 8 }, br, 1.4f);
        rt->DrawLine({ cx - 6, cy + 8 }, { cx + 6, cy + 8 }, br, 1.4f); rt->DrawLine({ cx + 6, cy + 8 }, { cx + 6, cy - 3 }, br, 1.4f); rt->DrawLine({ cx + 6, cy - 3 }, { cx + 2.5f, cy - 3 }, br, 1.4f);
        rt->DrawLine({ cx, cy + 3 }, { cx, cy - 9 }, br, 1.4f); rt->DrawLine({ cx - 3.5f, cy - 5.5f }, { cx, cy - 9 }, br, 1.4f); rt->DrawLine({ cx, cy - 9 }, { cx + 3.5f, cy - 5.5f }, br, 1.4f);
        g_hits.push_back({ 10, 0, D2D1::RectF(grp.left + 2, 10, grp.left + 38, 42) });
    }
    {   // mostrar/esconder concluídos (o símbolo da lista com o "+")
        float cx = grp.left + 55, cy = 26;
        for (int k = 0; k < 3; k++) rt->DrawLine({ cx - 7 + k * 1.f, cy - 4 + k * 4.f }, { cx + (k == 0 ? 3.f : 7.f), cy - 4 + k * 4.f }, br, 1.4f);
        Circle({ cx + 6.5f, cy - 5 }, 4, g_showDone ? SelColor() : text, true);
        g_hits.push_back({ 7, 0, D2D1::RectF(grp.left + 38, 10, grp.left + 72, 42) });
    }
    {   // + novo lembrete
        float cx = grp.right - 20, cy = 26;
        rt->DrawLine({ cx - 7, cy }, { cx + 7, cy }, br, 1.6f); rt->DrawLine({ cx, cy - 7 }, { cx, cy + 7 }, br, 1.6f);
        if (g_sel != S_DONE) g_hits.push_back({ 6, 0, D2D1::RectF(grp.right - 38, 10, grp.right - 2, 42) });
    }
    IconSearch({ srch.left + 18, 26 }, gray);
    Text(g_filter.empty() ? std::wstring(Tr(L"Buscar")) : g_filter, g_tfInfoG, D2D1::RectF(srch.left + 32, 8, srch.right - 12, 44), g_filter.empty() ? gray : text);
    g_hits.push_back({ 11, 0, srch });
    // ---- título, concluídos e a lista
    float X = S0 + 17, R = W - 15.5f;
    D2D1_COLOR_F sc = SelColor();
    int n = 0; for (auto* r : Shown()) if (!r->done || g_sel == S_DONE) n++;
    Text(SelName(), g_tfTitleG, D2D1::RectF(X, 58, R - 90, 99), sc);
    Text(std::to_wstring(n), g_tfTitleRG, D2D1::RectF(R - 90, 58, R, 99), sc);
    if (g_sel >= 0) {
        int done = 0; for (auto& r : g_rems) if (r.list == g_sel && r.done) done++;
        wchar_t b[64]; swprintf_s(b, Tr(L"%d Concluídos"), done);
        std::wstring s = b;
        float w1 = TextW(g_tfInfoG, s);
        Text(s, g_tfInfoG, D2D1::RectF(X, 115, X + w1 + 4, 136), gray);
        if (done) {
            Text(L"•", g_tfInfoG, D2D1::RectF(X + w1 + 8, 115, X + w1 + 20, 136), gray);
            D2D1_RECT_F clr = D2D1::RectF(X + w1 + 22, 115, X + w1 + 22 + TextW(g_tfInfoG, Tr(L"Limpar")) + 4, 136);
            Text(Tr(L"Limpar"), g_tfInfoG, clr, Rgb(10, 132, 255));
            g_hits.push_back({ 12, 0, clr });
        }
        std::wstring sh = g_showDone ? Tr(L"Ocultar") : Tr(L"Mostrar");
        D2D1_RECT_F shr = D2D1::RectF(R - TextW(g_tfInfoG, sh) - 4, 115, R, 136);
        Text(sh, g_tfInfoRG, shr, Rgb(10, 132, 255));
        g_hits.push_back({ 7, 0, shr });
    }
    Fill(D2D1::RectF(X, 141.5f, R + 3, 142), dark ? Rgb(255, 255, 255, 0.12f) : Rgb(0, 0, 0, 0.12f));
    rt->PushAxisAlignedClip(D2D1::RectF(S0, HEAD, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float ry = HEAD + 2 - g_scroll;
    for (Rem* r : Shown()) {
        bool sub = !r->notes.empty() || r->due || r->repeat || (g_sel < 0 && g_sel != S_DONE);
        float h = sub ? 49.5f : 32;
        RowPos rp{ r->id, ry, ry + h };
        D2D1_COLOR_F lc = g_sel >= 0 ? sc : (FindList(r->list) ? ListColor(FindList(r->list)->color) : sc);
        float ind = r->parent && FindRem(r->parent) ? 30.f : 0.f;
        D2D1_POINT_2F cc{ X + 9.5f + ind, ry + 16 };
        rp.circle = D2D1::RectF(cc.x - 12, cc.y - 12, cc.x + 12, cc.y + 12);
        if (r->done) { Circle(cc, 8.5f, lc, false, 1.3f); Circle(cc, 5.5f, lc, true); }
        else Circle(cc, 8.5f, dark ? Rgb(110, 110, 114) : Rgb(190, 190, 195), false, 1.3f);
        rp.title = D2D1::RectF(X + 30 + ind, ry + 4, R - 30, ry + 28);
        if (r->id != g_editing) Text(r->title.empty() ? L" " : r->title, g_tfRowG, rp.title, r->done ? gray : text);
        if (r->flag) { wchar_t f[2] = { 0xE7C1, 0 }; Text(f, g_tfSmall, D2D1::RectF(R - 24, ry + 4, R, ry + 28), Rgb(255, 159, 10)); }
        if (sub) {
            std::wstring s;
            if (r->due) s = DueText(r->due, true);
            if (r->repeat) { static const wchar_t* rn[] = { L"", L"Diariamente", L"Dias de Semana", L"Semanalmente", L"Mensalmente", L"Anualmente" }; s += L"  ↻ " + std::wstring(Tr(rn[std::clamp(r->repeat, 0, 5)])); }
            if (g_sel < 0 && g_sel != S_DONE) { List* l = FindList(r->list); if (l) s = s.empty() ? l->name : s + L"  ·  " + l->name; }
            if (!r->notes.empty()) s = s.empty() ? r->notes : r->notes + L"  ·  " + s;
            bool late = r->due && r->due < NowMin() && !r->done;
            Text(s, g_tfSubG, D2D1::RectF(X + 30 + ind, ry + 26, R - 30, ry + 44), late ? Rgb(255, 69, 58) : gray);
        }
        g_rows.push_back(rp);
        ry += h;
    }
    if (g_rows.empty() && g_editing < 0) {
        const wchar_t* tx = g_sel == S_DONE ? Tr(L"Nenhum Lembrete Concluído") : Tr(L"Nenhum Lembrete");
        static IDWriteTextFormat* tfC = Fmt(17, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(tx, tfC, D2D1::RectF(S0, HEAD, sz.width, sz.height * 0.7f), gray);  // no meio, como no Mac
    }
    rt->PopAxisAlignedClip();
}

void Paint() {
    if (!BeginPaint()) return;
    g_hits.clear(); g_rows.clear();
    barButtons.clear();
    if (BAR > 0) { PaintGolden(); EndPaint(); return; }
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    // ---- barra lateral
    Fill(D2D1::RectF(0, 0, SB, sz.height), SidebarColor());
    Fill(D2D1::RectF(SB - 1, 0, SB, sz.height), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.08f));
    struct T { int sel; const wchar_t* name; wchar_t glyph; D2D1_COLOR_F c; } tiles[] = {
        { S_TODAY, L"Hoje", 0xE787, Rgb(10, 132, 255) }, { S_SCHED, L"Agendados", 0xE787, Rgb(255, 69, 58) },
        { S_ALL, L"Todos", 0xE8FD, dark ? Rgb(99, 99, 102) : Rgb(60, 60, 67) }, { S_FLAG, L"Sinalizados", 0xE7C1, Rgb(255, 159, 10) },
        { S_DONE, L"Concluídos", 0xE73E, Rgb(142, 142, 147) } };
    float tw = (SB - 3 * 10) / 2, th = 62;
    for (int i = 0; i < 5; i++) {
        float x = 10 + (i % 2) * (tw + 10), y = 14 + (i / 2) * (th + 10);
        D2D1_RECT_F r = D2D1::RectF(x, y, x + tw, y + th);
        bool on = g_sel == tiles[i].sel;
        if (semaforo::Aqua()) {
            // macOS 27 (captura Reminders): os cartões são da cor da lista, cheios, com o símbolo, o número e o nome
            // brancos; o escolhido fica um pouco mais escuro
            D2D1_COLOR_F c = tiles[i].c;
            if (on) { c.r *= 0.85f; c.g *= 0.85f; c.b *= 0.85f; }
            Fill(r, c, 12);
            wchar_t g[2] = { tiles[i].glyph, 0 };
            Text(g, g_tfIcon, D2D1::RectF(x + 6, y + 6, x + 30, y + 30), Rgb(255, 255, 255));
            Text(std::to_wstring(Count(tiles[i].sel)), g_tfTileNum, D2D1::RectF(x, y + 4, x + tw - 10, y + 32), Rgb(255, 255, 255));
            Text(tiles[i].sel == S_ALL ? TrPick(L"Todos", L"All") : Tr(tiles[i].name), g_tfTile, D2D1::RectF(x + 10, y + 36, x + tw - 6, y + th - 4), Rgb(255, 255, 255));
            g_hits.push_back({ 1, tiles[i].sel, r });
            continue;
        }
        Fill(r, on ? tiles[i].c : (dark ? Rgb(58, 58, 60) : Rgb(255, 255, 255)), 10);
        Circle({ x + 18, y + 18 }, 12, on ? Rgb(255, 255, 255) : tiles[i].c, true);
        wchar_t g[2] = { tiles[i].glyph, 0 };
        Text(g, g_tfIcon, D2D1::RectF(x + 6, y + 6, x + 30, y + 30), on ? tiles[i].c : Rgb(255, 255, 255));
        Text(std::to_wstring(Count(tiles[i].sel)), g_tfTileNum, D2D1::RectF(x, y + 4, x + tw - 10, y + 32), on ? Rgb(255, 255, 255) : text);
        Text(tiles[i].sel == S_ALL ? TrPick(L"Todos", L"All") : Tr(tiles[i].name), g_tfTile, D2D1::RectF(x + 10, y + 36, x + tw - 6, y + th - 4), on ? Rgb(255, 255, 255) : gray);
        g_hits.push_back({ 1, tiles[i].sel, r });
    }
    float y = 14 + 3 * (th + 10) + 6;
    Text(Tr(L"Minhas Listas"), g_tfHead, D2D1::RectF(16, y, SB - 10, y + 22), gray);
    y += 26;
    for (auto& l : g_lists) {
        D2D1_RECT_F r = D2D1::RectF(8, y, SB - 8, y + 34);
        if (g_sel == l.id) Fill(r, dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.07f), 7);
        Circle({ r.left + 20, (r.top + r.bottom) / 2 }, 12, ListColor(l.color), true);
        wchar_t g[2] = { 0xE8FD, 0 };
        Text(g, g_tfSmall, D2D1::RectF(r.left + 8, r.top, r.left + 32, r.bottom), Rgb(255, 255, 255));
        Text(l.name, g_tfSide, D2D1::RectF(r.left + 40, r.top, r.right - 36, r.bottom), text);
        Text(std::to_wstring(Count(l.id)), g_tfSide, D2D1::RectF(r.right - 34, r.top, r.right - 10, r.bottom), gray);
        g_hits.push_back({ 2, l.id, r });
        y += 36;
    }
    D2D1_RECT_F add = D2D1::RectF(10, sz.height - 40, SB - 10, sz.height - 10);
    Text(L"⊕  " + std::wstring(Tr(L"Adicionar Lista")), g_tfSide, add, gray);
    g_hits.push_back({ 3, 0, add });
    // ---- lista escolhida
    float X = SB + PADX, W = sz.width - PADX;
    D2D1_COLOR_F sc = SelColor();
    Text(SelName(), g_tfBig, D2D1::RectF(X, 18, W - 120, 58), sc);
    int n = 0; for (auto* r : Shown()) if (!r->done || g_sel == S_DONE) n++;
    Text(std::to_wstring(n), g_tfBigR, D2D1::RectF(W - 110, 18, W - 6, 58), sc);
    // botões: + (novo) e mostrar concluídos
    D2D1_RECT_F plus = D2D1::RectF(W - 150, 14, W - 122, 42), eye = D2D1::RectF(W - 186, 14, W - 158, 42);
    if (g_sel != S_DONE) {
        wchar_t p[2] = { 0xE710, 0 };
        Text(p, g_tfIcon, plus, gray);
        g_hits.push_back({ 6, 0, plus });
    }
    if (g_sel >= 0) {
        wchar_t e[2] = { g_showDone ? (wchar_t)0xE7B3 : (wchar_t)0xED1A, 0 };
        Text(e, g_tfIcon, eye, g_showDone ? sc : gray);
        g_hits.push_back({ 7, 0, eye });
    }
    rt->PushAxisAlignedClip(D2D1::RectF(SB, HEAD, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float ry = HEAD - g_scroll;
    for (Rem* r : Shown()) {
        bool sub = !r->notes.empty() || r->due || r->repeat || (g_sel < 0 && g_sel != S_DONE);
        float h = sub ? ROW2 : ROW;
        RowPos rp{ r->id, ry, ry + h };
        D2D1_COLOR_F lc = g_sel >= 0 ? sc : (FindList(r->list) ? ListColor(FindList(r->list)->color) : sc);
        float ind = r->parent && FindRem(r->parent) ? 30.f : 0.f;  // subtarefa: recuada
        D2D1_POINT_2F cc{ X + 10 + ind, ry + 20 };
        rp.circle = D2D1::RectF(cc.x - 12, cc.y - 12, cc.x + 12, cc.y + 12);
        if (r->done) { Circle(cc, 9.5f, lc, false, 1.5f); Circle(cc, 6.5f, lc, true); }
        else Circle(cc, 9.5f, dark ? Rgb(99, 99, 102) : Rgb(199, 199, 204), false, 1.5f);
        rp.title = D2D1::RectF(X + 30 + ind, ry + 8, W - 40, ry + 32);
        if (r->id != g_editing) Text(r->title.empty() ? L" " : r->title, g_tfRow, rp.title, r->done ? gray : text);
        if (r->flag) { wchar_t f[2] = { 0xE7C1, 0 }; Text(f, g_tfSmall, D2D1::RectF(W - 34, ry + 8, W - 10, ry + 32), Rgb(255, 159, 10)); }
        if (sub) {
            std::wstring s;
            if (r->due) s = DueText(r->due, true);
            if (r->repeat) { static const wchar_t* rn[] = { L"", L"Diariamente", L"Dias de Semana", L"Semanalmente", L"Mensalmente", L"Anualmente" }; s += L"  ↻ " + std::wstring(Tr(rn[std::clamp(r->repeat, 0, 5)])); }
            if (g_sel < 0 && g_sel != S_DONE) { List* l = FindList(r->list); if (l) s = s.empty() ? l->name : s + L"  ·  " + l->name; }
            if (!r->notes.empty()) s = s.empty() ? r->notes : r->notes + L"  ·  " + s;
            bool late = r->due && r->due < NowMin() && !r->done;
            Text(s, g_tfSub, D2D1::RectF(X + 30 + ind, ry + 30, W - 40, ry + 48), late ? Rgb(255, 69, 58) : gray);
        }
        Fill(D2D1::RectF(X + 30, ry + h - 0.5f, W, ry + h), dark ? Rgb(255, 255, 255, 0.08f) : Rgb(0, 0, 0, 0.08f));
        g_rows.push_back(rp);
        ry += h;
    }
    if (g_rows.empty() && g_editing < 0) {
        const wchar_t* t = g_sel == S_DONE ? Tr(L"Nenhum Lembrete Concluído") : Tr(L"Nenhum Lembrete");
        Text(t, g_tfRow, D2D1::RectF(SB, HEAD, sz.width, sz.height * 0.7f), gray);
    }
    rt->PopAxisAlignedClip();
    EndPaint();
}

// ---- edição
void Edit(int id) {
    for (auto& rp : g_rows) if (rp.id == id) {
        g_editing = id;
        Rem* r = FindRem(id);
        FieldOpen(rp.title, r ? r->title : L"", 14, false, [id](const std::wstring& t, int key) {
            g_editing = -1;
            Rem* r = FindRem(id);
            if (!r) return;
            std::wstring s = t;
            while (!s.empty() && s.back() == L' ') s.pop_back();
            if (s.empty()) { g_rems.erase(std::remove_if(g_rems.begin(), g_rems.end(), [id](const Rem& x) { return x.id == id; }), g_rems.end()); Save(); return; }
            r->title = s;
            if (key == VK_TAB && !r->parent) {  // Tab: vira subtarefa da de cima (como no Mac)
                int prev = 0;
                for (auto& rp : g_rows) { if (rp.id == id) break; Rem* x = FindRem(rp.id); if (x && !x->parent) prev = x->id; }
                bool hasKids = false; for (auto& x : g_rems) if (x.parent == id) hasKids = true;
                if (prev && !hasKids) { r->parent = prev; if (Rem* p = FindRem(prev)) r->list = p->list; }
            }
            Save();
            if (key == VK_RETURN) PostMessageW(wnd, WM_APP + 10, r->parent, 0);  // Enter: o próximo (no mesmo nível), como no Mac
        });
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
}

void NewReminder(int parent = 0) {
    if (g_sel == S_DONE) return;
    Rem r{ g_nextId++, g_sel >= 0 ? g_sel : g_lists[0].id, false, 0, L"", L"", g_sel == S_FLAG };
    if (Rem* p = parent ? FindRem(parent) : nullptr) { r.parent = parent; r.list = p->list; }
    if (g_sel == S_TODAY || g_sel == S_SCHED) { SYSTEMTIME st; GetLocalTime(&st); st.wHour = 9; st.wMinute = 0; long long m = ToMin(st); if (m < NowMin()) m = NowMin() + 60; r.due = m; }
    g_rems.push_back(r);
    Paint();  // a linha nova existe (posição) antes de abrir o campo
    // rola até o fim se precisar
    D2D1_SIZE_F sz = Size();
    if (!g_rows.empty() && g_rows.back().bottom > sz.height) { g_scroll += g_rows.back().bottom - sz.height + 10; Paint(); }
    Edit(r.id);
}

void SetDue(Rem* r, int opt) {
    SYSTEMTIME st; GetLocalTime(&st); st.wSecond = 0;
    long long now = NowMin(), today = DayStart(now);
    switch (opt) {
    case 1: r->due = now + 60; break;                            // daqui a 1 hora
    case 2: st.wHour = 18; st.wMinute = 0; r->due = ToMin(st); if (r->due < now) r->due = now + 60; break;  // hoje à noite
    case 3: r->due = today + 24 * 60 + 9 * 60; break;            // amanhã 9:00
    case 4: { int add = (6 - st.wDayOfWeek + 7) % 7; if (!add) add = 7; r->due = today + add * 24 * 60 + 9 * 60; break; }  // sábado
    case 5: { int add = (1 - st.wDayOfWeek + 7) % 7; if (!add) add = 7; r->due = today + add * 24 * 60 + 9 * 60; break; }  // segunda
    case 0: r->due = 0; break;
    }
    Save();
}

// próxima data de um lembrete que se repete (ao concluir, ele volta com a data nova, como no Mac)
void Advance(Rem* r) {
    SYSTEMTIME s = FromMin(r->due);
    long long now = NowMin();
    do {
        switch (r->repeat) {
        case 1: r->due += 24 * 60; break;
        case 2: do r->due += 24 * 60; while (FromMin(r->due).wDayOfWeek == 0 || FromMin(r->due).wDayOfWeek == 6); break;
        case 3: r->due += 7 * 24 * 60; break;
        case 4: { s = FromMin(r->due); int mo = s.wMonth; s.wMonth = (WORD)(mo % 12 + 1); if (mo == 12) s.wYear++; r->due = ToMin(s); break; }
        case 5: { s = FromMin(r->due); s.wYear++; r->due = ToMin(s); break; }
        default: return;
        }
    } while (r->due < now);
}
// concluir: o que se repete ganha a próxima data em vez de sair da lista
void Complete(Rem* r) {
    if (!r->done && r->repeat && r->due) { Advance(r); Save(); return; }
    r->done = !r->done;
    r->doneAt = r->done ? GetTickCount64() / 60000 : 0;
    Save();
}

// "Escolher Data e Hora…": dois seletores do Windows (data e hora) numa janelinha
INT_PTR g_pickResult;
LRESULT CALLBACK PickProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) { g_pickResult = LOWORD(w); DestroyWindow(h); return 0; }
    if (m == WM_CLOSE) { g_pickResult = IDCANCEL; DestroyWindow(h); return 0; }
    return DefWindowProcW(h, m, w, l);
}
bool PickDateTime(long long& io) {
    INITCOMMONCONTROLSEX ic{ sizeof(ic), ICC_DATE_CLASSES }; InitCommonControlsEx(&ic);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool reg; if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = PickProc; wc.hInstance = hi; wc.lpszClassName = L"LembretesData"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&wc); reg = true; }
    float s = Sc();
    auto S = [s](int v) { return (int)std::lround(v * s); };
    RECT wr; GetWindowRect(wnd, &wr);
    RECT want{ 0, 0, S(300), S(156) }; AdjustWindowRectExForDpi(&want, WS_CAPTION | WS_SYSMENU, FALSE, 0, GetDpiForWindow(wnd));
    int W = want.right - want.left, H = want.bottom - want.top;
    HWND d = CreateWindowExW(0, L"LembretesData", Tr(L"Data e Hora"), WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                             (wr.left + wr.right - W) / 2, (wr.top + wr.bottom - H) / 2, W, H, wnd, nullptr, hi, nullptr);
    NONCLIENTMETRICSW nm{ sizeof(nm) }; SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0, GetDpiForWindow(wnd));
    HFONT font = CreateFontIndirectW(&nm.lfMessageFont);
    HWND date = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_LONGDATEFORMAT, S(16), S(16), S(268), S(28), d, nullptr, hi, nullptr);
    HWND tim = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_TIMEFORMAT | DTS_UPDOWN, S(16), S(54), S(268), S(28), d, nullptr, hi, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(196), S(104), S(88), S(32), d, (HMENU)IDOK, hi, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", Tr(L"Cancelar"), WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(100), S(104), S(88), S(32), d, (HMENU)IDCANCEL, hi, nullptr);
    for (HWND c : { date, tim, ok, cancel }) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(tim, DTM_SETFORMATW, 0, (LPARAM)L"HH':'mm");
    SYSTEMTIME st = io ? FromMin(io) : FromMin(DayStart(NowMin()) + 24 * 60 + 9 * 60);
    SendMessageW(date, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
    SendMessageW(tim, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
    EnableWindow(wnd, FALSE);
    g_pickResult = 0;
    MSG msg;
    SYSTEMTIME a{}, b{};
    while (IsWindow(d) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && IsChild(d, msg.hwnd)) { g_pickResult = IDOK; SendMessageW(date, DTM_GETSYSTEMTIME, 0, (LPARAM)&a); SendMessageW(tim, DTM_GETSYSTEMTIME, 0, (LPARAM)&b); DestroyWindow(d); break; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { g_pickResult = IDCANCEL; DestroyWindow(d); break; }
        if (g_pickResult == 0 && IsWindow(d)) { SendMessageW(date, DTM_GETSYSTEMTIME, 0, (LPARAM)&a); SendMessageW(tim, DTM_GETSYSTEMTIME, 0, (LPARAM)&b); }
        if (!IsDialogMessageW(d, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(wnd, TRUE); SetForegroundWindow(wnd);
    DeleteObject(font);
    if (g_pickResult != IDOK) return false;
    a.wHour = b.wHour; a.wMinute = b.wMinute;
    io = ToMin(a);
    return true;
}

void RowMenu(int id, POINT scr) {
    Rem* r = FindRem(id);
    if (!r) return;
    HMENU m = CreatePopupMenu(), due = CreatePopupMenu(), mv = CreatePopupMenu();
    AppendMenuW(due, MF_STRING, 101, Tr(L"Daqui a 1 Hora"));
    AppendMenuW(due, MF_STRING, 102, Tr(L"Hoje à Noite (18:00)"));
    AppendMenuW(due, MF_STRING, 103, Tr(L"Amanhã (9:00)"));
    AppendMenuW(due, MF_STRING, 104, Tr(L"Este Fim de Semana"));
    AppendMenuW(due, MF_STRING, 105, Tr(L"Próxima Semana"));
    AppendMenuW(due, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(due, MF_STRING, 106, Tr(L"Escolher Data e Hora…"));
    AppendMenuW(due, MF_STRING | (r->due ? 0 : MF_GRAYED), 100, Tr(L"Remover Data"));
    HMENU rep = CreatePopupMenu();
    const wchar_t* rn[] = { L"Nunca", L"Diariamente", L"Dias de Semana", L"Semanalmente", L"Mensalmente", L"Anualmente" };
    for (int i = 0; i < 6; i++) AppendMenuW(rep, MF_STRING | (r->repeat == i ? MF_CHECKED : 0), 400 + i, Tr(rn[i]));
    for (auto& l : g_lists) AppendMenuW(mv, MF_STRING | (l.id == r->list ? MF_CHECKED : 0), 300 + l.id, l.name.c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)due, Tr(L"Data"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rep, Tr(L"Repetir"));
    AppendMenuW(m, MF_STRING, 201, r->flag ? Tr(L"Remover Sinalizador") : Tr(L"Sinalizar"));
    AppendMenuW(m, MF_STRING, 204, Tr(L"Editar Notas…"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mv, Tr(L"Mover para"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (!r->parent) AppendMenuW(m, MF_STRING, 205, Tr(L"Adicionar Subtarefa"));
    else AppendMenuW(m, MF_STRING, 207, Tr(L"Deixar de Ser Subtarefa"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 202, r->done ? Tr(L"Marcar como Não Concluído") : Tr(L"Marcar como Concluído"));
    AppendMenuW(m, MF_STRING, 203, Tr(L"Apagar"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c >= 100 && c <= 105) SetDue(r, c - 100);
    else if (c == 106) { long long d = r->due; if (PickDateTime(d)) { r->due = d; Save(); } }
    else if (c >= 400 && c <= 405) { r->repeat = c - 400; if (r->repeat && !r->due) SetDue(r, 3); Save(); }
    else if (c == 205) { InvalidateRect(wnd, nullptr, FALSE); NewReminder(id); return; }
    else if (c == 207) { r->parent = 0; Save(); }
    else if (c == 201) { r->flag = !r->flag; Save(); }
    else if (c == 202) { Complete(r); r->doneAt = 0; }
    else if (c == 203) { g_rems.erase(std::remove_if(g_rems.begin(), g_rems.end(), [id](const Rem& x) { return x.id == id || x.parent == id; }), g_rems.end()); Save(); }  // com as subtarefas
    else if (c == 204) {
        for (auto& rp : g_rows) if (rp.id == id) {
            g_editing = -2;
            FieldOpen(D2D1::RectF(rp.title.left, rp.title.top + 22, rp.title.right, rp.title.bottom + 22), r->notes, 12, false, [id](const std::wstring& t, int) {
                g_editing = -1; if (Rem* x = FindRem(id)) { x->notes = t; Save(); }
            });
        }
    }
    else if (c >= 300) { r->list = c - 300; Save(); }
    InvalidateRect(wnd, nullptr, FALSE);
}

void NewList() {
    List l{ g_nextId++, Tr(L"Nova Lista"), (int)g_lists.size() % 10 };
    g_lists.push_back(l);
    g_sel = l.id;
    Save();
    Paint();
    for (auto& h : g_hits) if (h.kind == 2 && h.arg == l.id) {
        int id = l.id;
        FieldOpen(D2D1::RectF(h.r.left + 40, h.r.top, h.r.right - 36, h.r.bottom), l.name, 13, false, [id](const std::wstring& t, int) {
            if (List* x = FindList(id)) { if (!t.empty()) x->name = t; Save(); }
        });
    }
}

void ListMenu(int id, POINT scr) {
    List* l = FindList(id);
    if (!l) return;
    HMENU m = CreatePopupMenu(), col = CreatePopupMenu();
    for (int i = 0; i < 10; i++) AppendMenuW(col, MF_STRING | (l->color == i ? MF_CHECKED : 0), 100 + i, Tr(COLOR_NAMES[i]));
    AppendMenuW(m, MF_STRING, 1, Tr(L"Renomear"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)col, Tr(L"Cor"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_lists.size() > 1 ? 0 : MF_GRAYED), 2, Tr(L"Apagar Lista"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c >= 100) { l->color = c - 100; Save(); }
    else if (c == 2) {
        int n = 0; for (auto& r : g_rems) if (r.list == id) n++;
        wchar_t q[200]; swprintf_s(q, Tr(L"Apagar a lista “%ls” e os %d lembretes dela?"), l->name.c_str(), n);
        if (MessageBoxW(wnd, q, Tr(L"Lembretes"), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
            g_rems.erase(std::remove_if(g_rems.begin(), g_rems.end(), [id](const Rem& x) { return x.list == id; }), g_rems.end());
            g_lists.erase(std::remove_if(g_lists.begin(), g_lists.end(), [id](const List& x) { return x.id == id; }), g_lists.end());
            g_sel = g_lists[0].id;
            Save();
        }
    } else if (c == 1) {
        for (auto& h : g_hits) if (h.kind == 2 && h.arg == id)
            FieldOpen(D2D1::RectF(h.r.left + 40, h.r.top, h.r.right - 36, h.r.bottom), l->name, 13, false, [id](const std::wstring& t, int) {
                if (List* x = FindList(id)) { if (!t.empty()) x->name = t; Save(); }
            });
    }
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
    case WM_APP + 10: NewReminder((int)w); return 0;
    case WM_TIMER:  // concluídos há pouco somem da lista; datas ("Hoje") e o arquivo mudado por fora
        if (w == 1) {
            static FILETIME last{};
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!field.h && GetFileAttributesExW(g_file.c_str(), GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &last) != 0) {
                if (last.dwLowDateTime || last.dwHighDateTime) Load();
                last = fa.ftLastWriteTime;
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        g_scroll = std::max(0.f, g_scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 60);
        float maxS = 0; if (!g_rows.empty()) maxS = std::max(0.f, g_rows.back().bottom + g_scroll - Size().height + 20);
        g_scroll = std::min(g_scroll, maxS);
        if (field.h) FieldEnd(0);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONUP: {
        if (field.h) FieldEnd(0);
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        bool right = m == WM_RBUTTONUP;
        POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
        for (auto& ht : g_hits) if (In(ht.r, x, y)) {
            if (ht.kind == 1) { g_sel = ht.arg; g_scroll = 0; }
            else if (ht.kind == 2) { if (right) { ListMenu(ht.arg, scr); return 0; } g_sel = ht.arg; g_scroll = 0; }
            else if (ht.kind == 3) { NewList(); return 0; }
            else if (ht.kind == 6) { NewReminder(); return 0; }
            else if (ht.kind == 7) g_showDone = !g_showDone;
            else if (ht.kind == 9) g_hideSide = !g_hideSide;
            else if (ht.kind == 12) {  // Limpar: apaga os concluídos da lista
                int list = g_sel;
                g_rems.erase(std::remove_if(g_rems.begin(), g_rems.end(), [list](const Rem& r) { return r.list == list && r.done; }), g_rems.end());
                Save();
            } else if (ht.kind == 10) {  // compartilhar: a lista como texto na área de transferência
                std::wstring s = SelName() + L"\r\n";
                for (auto* r : Shown()) s += std::wstring(r->done ? L"☑ " : L"☐ ") + r->title + L"\r\n";
                if (OpenClipboard(h)) {
                    EmptyClipboard();
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
                    if (g) { memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                    CloseClipboard();
                }
            } else if (ht.kind == 11) {  // busca: filtra enquanto digita
                FieldOpen(D2D1::RectF(ht.r.left + 32, ht.r.top + 8, ht.r.right - 12, ht.r.bottom - 8), g_filter, 13, false, [](const std::wstring& q, int key) {
                    g_filter = key == VK_ESCAPE ? L"" : q; g_scroll = 0; InvalidateRect(wnd, nullptr, FALSE);
                });
                return 0;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (auto& rp : g_rows) if (x > (BAR > 0 && g_hideSide ? 0 : SB) && y >= rp.top && y < rp.bottom && y >= HEAD) {
            Rem* r = FindRem(rp.id);
            if (!r) return 0;
            if (right) { RowMenu(rp.id, scr); return 0; }
            if (In(rp.circle, x, y)) {  // concluir (fica um instante na lista, como no Mac)
                Complete(r);
                SetTimer(h, 2, 1600, [](HWND hw, UINT, UINT_PTR id, DWORD) { KillTimer(hw, id); for (auto& x : g_rems) x.doneAt = 0; InvalidateRect(hw, nullptr, FALSE); });
            } else Edit(rp.id);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (!right && x > (BAR > 0 && g_hideSide ? 0 : SB) && y > HEAD) NewReminder();  // clique no vazio: lembrete novo
        return 0;
    }
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && w == 'N') { if (GetKeyState(VK_SHIFT) < 0) NewList(); else NewReminder(); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && w == 'W') { DestroyWindow(h); return 0; }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_file = DataPath(L"lembretes.txt");
    Load();
    g_sel = g_lists[0].id;
    for (auto& r : g_rems) if (!r.done && IsToday(r)) { g_sel = S_TODAY; break; }
    g_tfBig = Fmt(28, DWRITE_FONT_WEIGHT_BOLD);
    if (g_tfBig) g_tfBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_tfRow = Fmt(14);
    g_tfSub = Fmt(12);
    g_tfSide = Fmt(13);
    g_tfHead = Fmt(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfTile = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfTileNum = Fmt(22, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfIcon = Fmt(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfSmall = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfCardG = Fmt(15, DWRITE_FONT_WEIGHT_BOLD);
    g_tfCardNumG = Fmt(18, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfTitleG = Fmt(31, DWRITE_FONT_WEIGHT_BOLD);
    g_tfTitleRG = Fmt(31, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfRowG = Fmt(15);
    g_tfSubG = Fmt(13);
    g_tfListG = Fmt(15);
    g_tfListBoldG = Fmt(15, DWRITE_FONT_WEIGHT_BOLD);
    g_tfHeadG = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfInfoG = Fmt(15);
    g_tfInfoRG = Fmt(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    barWanted = 52;  // macOS 27: barra unificada
    if (semaforo::Aqua()) { SB = 358; HEAD = 143; }
    if (!CreateMain(hi, L"LembretesJanela", Tr(L"Lembretes"), Proc, semaforo::Aqua() ? 900 : 900, semaforo::Aqua() ? 708 : 620)) return 1;
    g_tfBigR = Fmt(28, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    SetTimer(wnd, 1, 20000, nullptr);
    ShowWindow(wnd, show);
    return Run();
}
