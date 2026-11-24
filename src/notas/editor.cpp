// Notas: o editor de texto rico. Cada parágrafo vira um IDWriteTextLayout (estilo, negrito/itálico/
// sublinhado/tachado, realces, variáveis e resultados das Notas Matemáticas); cursor amarelo, seleção,
// listas (marcadores, traços, números, verificação com bolinhas), seções recolhíveis nos títulos,
// desfazer/refazer e área de transferência com formatação (formato próprio + texto puro).
#include "notas.h"
#include <cwctype>
#include <algorithm>
#include <cmath>
#include "../comum/simbolos.h"
#include "../comum/semaforo.h"  // tema Leopard: o papel amarelo pautado das Notas do 10.5

using editor::Pos;

namespace {

Note* N;
Pos caret, anchor;
bool focused, blinkOn = true;
float scrollY, desiredX = -1;
D2D1_RECT_F area{};
std::wstring family = L"Segoe UI", monoFamily = L"Consolas";
bool dark;
std::wstring query;
uint8_t typing; bool typingSet;
bool wantReveal;
int hoverChevron = -1;
int clickMode;   // 1 caractere, 2 palavra, 3 parágrafo (arrastar estende nessa unidade)
Pos clickA, clickB;

struct PL { IDWriteTextLayout* tl = nullptr; float x = 0, y = 0, h = 0, w = 0, base = 0; bool hidden = false; int number = 0, resultAt = -1; };
std::vector<PL> pl;
bool dirtyLayout = true;
float layoutW = -1;
float contentH;
std::vector<MathLine> math;
ID2D1DeviceContext* lastDc;
ID2D1SolidColorBrush *brAccent, *brHi[HI_COUNT + 1];
IDWriteTextFormat* fmt[5];

const float MARGIN = 24, INDENT = 26, TOP_PAD = 34;

// medidas de cada estilo (tamanho, altura da linha, espaço depois)
struct StyleM { float size, line, after; DWRITE_FONT_WEIGHT w; };
const StyleM SM[5] = {
    { 24, 30, 6, DWRITE_FONT_WEIGHT_BOLD },        // Título
    { 18, 23, 4, DWRITE_FONT_WEIGHT_BOLD },        // Cabeçalho
    { 14, 18, 3, DWRITE_FONT_WEIGHT_BOLD },        // Subcabeçalho
    { 13, 17.5f, 0, DWRITE_FONT_WEIGHT_NORMAL },   // Corpo
    { 12, 16.5f, 0, DWRITE_FONT_WEIGHT_NORMAL },   // Monoespaçado
};

D2D1_COLOR_F TextColor() { return dark ? D2D1::ColorF(1, 1, 1, 0.88f) : D2D1::ColorF(0.11f, 0.11f, 0.12f); }
D2D1_COLOR_F Text2() { return dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.45f); }

// realces do Sequoia: roxo, rosa, laranja, menta, azul (texto na cor, fundo claro da mesma cor)
D2D1_COLOR_F HiText(int h) {
    static const D2D1_COLOR_F L[HI_COUNT] = { D2D1::ColorF(0.55f, 0.27f, 0.85f), D2D1::ColorF(0.86f, 0.18f, 0.45f), D2D1::ColorF(0.85f, 0.42f, 0.0f), D2D1::ColorF(0.0f, 0.55f, 0.48f), D2D1::ColorF(0.0f, 0.45f, 0.90f) };
    static const D2D1_COLOR_F D[HI_COUNT] = { D2D1::ColorF(0.80f, 0.62f, 1.0f), D2D1::ColorF(1.0f, 0.50f, 0.68f), D2D1::ColorF(1.0f, 0.68f, 0.35f), D2D1::ColorF(0.40f, 0.90f, 0.80f), D2D1::ColorF(0.45f, 0.72f, 1.0f) };
    return dark ? D[h - 1] : L[h - 1];
}
D2D1_COLOR_F HiBack(int h) {
    static const D2D1_COLOR_F c[HI_COUNT] = { D2D1::ColorF(0.69f, 0.42f, 0.98f), D2D1::ColorF(1.0f, 0.30f, 0.55f), D2D1::ColorF(1.0f, 0.58f, 0.15f), D2D1::ColorF(0.20f, 0.82f, 0.72f), D2D1::ColorF(0.25f, 0.60f, 1.0f) };
    D2D1_COLOR_F k = c[h - 1]; k.a = dark ? 0.30f : 0.22f; return k;
}

bool Less(Pos a, Pos b) { return a.p < b.p || (a.p == b.p && a.o < b.o); }
bool Same(Pos a, Pos b) { return a.p == b.p && a.o == b.o; }
Pos SelStart() { return Less(caret, anchor) ? caret : anchor; }
Pos SelEnd() { return Less(caret, anchor) ? anchor : caret; }
bool HasSel() { return !Same(caret, anchor); }
Para& P(int i) { return N->paras[i]; }
int Count() { return N ? (int)N->paras.size() : 0; }
int Level(const Para& p) { return p.style <= S_SUBHEADING ? p.style : 99; }

void ReleaseLayouts() { for (auto& l : pl) if (l.tl) l.tl->Release(); pl.clear(); dirtyLayout = true; }
void ReleaseBrushes() {
    if (brAccent) brAccent->Release(); brAccent = nullptr;
    for (auto& b : brHi) { if (b) b->Release(); b = nullptr; }
}

void Clamp() {
    if (!N) return;
    auto fix = [](Pos& q) { q.p = std::clamp(q.p, 0, Count() - 1); q.o = std::clamp(q.o, 0, (int)P(q.p).text.size()); };
    fix(caret); fix(anchor);
}

// ---------------------------------------------------------------------------
// layout
void ComputeHidden() {
    int hideLevel = -1;  // escondendo até achar um título de nível <= hideLevel
    for (int i = 0; i < Count(); i++) {
        int lv = Level(P(i));
        if (hideLevel >= 0) {
            if (lv <= hideLevel) hideLevel = -1;
            else { pl[i].hidden = true; continue; }
        }
        pl[i].hidden = false;
        if (P(i).collapsed && lv < 99) hideLevel = lv;
    }
}

void BuildLayouts(ID2D1DeviceContext* dc, float width) {
    if (dc != lastDc) { ReleaseBrushes(); ReleaseLayouts(); lastDc = dc; }
    if (!brAccent) {
        dc->CreateSolidColorBrush(AccentText(), &brAccent);
        for (int h = 1; h <= HI_COUNT; h++) dc->CreateSolidColorBrush(HiText(h), &brHi[h]);
    }
    if (!dirtyLayout && width == layoutW && (int)pl.size() == Count()) return;
    for (auto& l : pl) if (l.tl) l.tl->Release();
    pl.assign(Count(), PL{});
    layoutW = width;
    dirtyLayout = false;
    math = EvalMath(N->paras);
    int counters[10] = {};
    float y = TOP_PAD;
    for (int i = 0; i < Count(); i++) {
        Para& p = P(i);
        PL& l = pl[i];
        // numeração das listas numeradas
        if (p.list == L_NONE) std::fill(std::begin(counters), std::end(counters), 0);
        else {
            for (int k = p.indent + 1; k < 10; k++) counters[k] = 0;
            if (p.list == L_NUMBER) l.number = ++counters[std::min<int>(p.indent, 9)];
            else counters[std::min<int>(p.indent, 9)] = 0;
        }
        float base = MARGIN + p.indent * INDENT;
        float lead = p.list == L_CHECK ? 28.f : p.list != L_NONE ? 24.f : 0.f;
        l.x = base + lead; l.base = base;
        l.w = std::max(40.f, width - l.x - MARGIN);
        std::wstring s = p.text;
        int resultAt = -1;
        if (i < (int)math.size() && !math[i].result.empty()) {
            if (s.size() >= 2 && s.back() == L'=' && s[s.size() - 2] == L' ') s += L' ';  // "x =" -> "x = 5"
            resultAt = (int)s.size(); s += math[i].result;
        }
        l.resultAt = resultAt;
        const StyleM& m = SM[p.style];
        gfx::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt[p.style], l.w, 10000, &l.tl);
        if (!l.tl) continue;
        l.tl->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, m.line, m.line * 0.8f);
        UINT32 n = (UINT32)p.text.size();
        for (UINT32 k = 0; k < n;) {  // corridas de atributos
            UINT32 j = k; uint8_t a = p.at[k];
            while (j < n && p.at[j] == a) j++;
            DWRITE_TEXT_RANGE r{ k, j - k };
            if (a & A_BOLD) l.tl->SetFontWeight(p.style == S_BODY || p.style == S_MONO ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_HEAVY, r);
            if (a & A_ITALIC) l.tl->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
            if (a & A_UNDER) l.tl->SetUnderline(TRUE, r);
            if (a & A_STRIKE) l.tl->SetStrikethrough(TRUE, r);
            if (HiOf(a)) l.tl->SetDrawingEffect(brHi[HiOf(a)], r);
            k = j;
        }
        if (i < (int)math.size())
            for (auto& v : math[i].vars) l.tl->SetDrawingEffect(brAccent, { (UINT32)v.first, (UINT32)v.second });
        if (resultAt >= 0) l.tl->SetDrawingEffect(brAccent, { (UINT32)resultAt, (UINT32)math[i].result.size() });
        DWRITE_TEXT_METRICS tm{}; l.tl->GetMetrics(&tm);
        l.h = std::max(tm.height, m.line);
        l.y = y;
        y += l.h + m.after;
    }
    ComputeHidden();
    // recolhidos não ocupam espaço
    y = TOP_PAD;
    for (int i = 0; i < Count(); i++) {
        pl[i].y = y;
        if (!pl[i].hidden) y += pl[i].h + SM[P(i).style].after;
    }
    contentH = y + 40;
}

void Changed() {
    if (!N) return;
    N->modified = store::Now();
    N->dirty = true;
    dirtyLayout = true;
    wantReveal = true;
    blinkOn = true;
    NoteEdited(N);
}

// cursor em coordenadas do conteúdo (antes da rolagem)
D2D1_RECT_F CaretRectAt(Pos q) {
    if (q.p >= (int)pl.size() || !pl[q.p].tl) return { 0, 0, 0, 0 };
    float x, y; DWRITE_HIT_TEST_METRICS m;
    pl[q.p].tl->HitTestTextPosition((UINT32)q.o, FALSE, &x, &y, &m);
    return { pl[q.p].x + x, pl[q.p].y + m.top, pl[q.p].x + x + 2, pl[q.p].y + m.top + m.height };
}

Pos HitContent(float x, float y) {  // ponto no conteúdo -> posição
    Pos r{ 0, 0 };
    if (!N || pl.empty()) return r;
    int best = -1;
    for (int i = 0; i < Count(); i++) {
        if (pl[i].hidden) continue;
        best = i;
        if (y < pl[i].y + pl[i].h + SM[P(i).style].after) break;
    }
    if (best < 0) return r;
    r.p = best;
    if (!pl[best].tl) return r;
    BOOL trailing, inside; DWRITE_HIT_TEST_METRICS m;
    pl[best].tl->HitTestPoint(x - pl[best].x, y - pl[best].y, &trailing, &inside, &m);
    r.o = std::min((int)(m.textPosition + (trailing ? m.length : 0)), (int)P(best).text.size());
    return r;
}

void Reveal() {
    if (!N || pl.empty()) return;
    D2D1_RECT_F c = CaretRectAt(caret);
    float h = area.bottom - area.top;
    if (c.top - 8 < scrollY) scrollY = c.top - 8;
    if (c.bottom + 24 > scrollY + h) scrollY = c.bottom + 24 - h;
    scrollY = std::clamp(scrollY, 0.f, std::max(0.f, contentH - h));
}

// ---------------------------------------------------------------------------
// edição
void Checkpoint(int kind) {
    if (!N) return;
    ULONGLONG now = GetTickCount64();
    if (kind == 3 || kind != N->lastEditKind || now - N->lastEditAt > 1500) {
        N->undo.push_back({ N->paras, caret.p, caret.o, anchor.p, anchor.o });
        if (N->undo.size() > 200) N->undo.erase(N->undo.begin());
        N->redo.clear();
    }
    N->lastEditKind = kind; N->lastEditAt = now;
}

uint8_t AttrAtCaret() {
    if (typingSet) return typing;
    Para& p = P(caret.p);
    if (caret.o > 0 && caret.o <= (int)p.at.size()) return p.at[caret.o - 1];
    if (!p.at.empty()) return p.at[0];
    return 0;
}

void DeleteRange(Pos a, Pos b) {
    if (!Less(a, b)) return;
    Para& pa = P(a.p);
    if (a.p == b.p) {
        pa.text.erase(a.o, b.o - a.o);
        pa.at.erase(pa.at.begin() + a.o, pa.at.begin() + b.o);
    } else {
        Para& pb = P(b.p);
        pa.text = pa.text.substr(0, a.o) + pb.text.substr(b.o);
        std::vector<uint8_t> at(pa.at.begin(), pa.at.begin() + a.o);
        at.insert(at.end(), pb.at.begin() + b.o, pb.at.end());
        pa.at = at;
        if (a.o == 0 && pa.text.size() == pb.text.size() - b.o && b.o == 0) { pa.style = pb.style; pa.list = pb.list; pa.indent = pb.indent; pa.checked = pb.checked; }
        N->paras.erase(N->paras.begin() + a.p + 1, N->paras.begin() + b.p + 1);
    }
    caret = anchor = a;
}

void DeleteSel() { if (HasSel()) { Pos a = SelStart(), b = SelEnd(); DeleteRange(a, b); } }

void InsertPlain(const std::wstring& s, uint8_t at) {
    Para& p = P(caret.p);
    p.text.insert(caret.o, s);
    p.at.insert(p.at.begin() + caret.o, s.size(), at);
    caret.o += (int)s.size();
    anchor = caret;
}

void SplitPara() {
    Para& p = P(caret.p);
    Para np;
    np.text = p.text.substr(caret.o);
    np.at.assign(p.at.begin() + caret.o, p.at.end());
    p.text.resize(caret.o);
    p.at.resize(caret.o);
    np.list = p.list; np.indent = p.indent;
    np.style = p.style <= S_SUBHEADING ? (uint8_t)S_BODY : p.style;
    if (p.collapsed) p.collapsed = false;
    N->paras.insert(N->paras.begin() + caret.p + 1, np);
    caret = anchor = { caret.p + 1, 0 };
}

void InsertParas(std::vector<Para> v) {
    if (v.empty()) return;
    DeleteSel();
    if (v.size() == 1) {
        Para& p = P(caret.p);
        p.text.insert(caret.o, v[0].text);
        p.at.insert(p.at.begin() + caret.o, v[0].at.begin(), v[0].at.end());
        caret.o += (int)v[0].text.size(); anchor = caret;
        return;
    }
    Para& p = P(caret.p);
    std::wstring tail = p.text.substr(caret.o);
    std::vector<uint8_t> tailAt(p.at.begin() + caret.o, p.at.end());
    p.text.resize(caret.o); p.at.resize(caret.o);
    p.text += v[0].text; p.at.insert(p.at.end(), v[0].at.begin(), v[0].at.end());
    if (caret.o == 0) { p.style = v[0].style; p.list = v[0].list; p.indent = v[0].indent; p.checked = v[0].checked; }
    int at = caret.p + 1;
    for (size_t k = 1; k < v.size(); k++) N->paras.insert(N->paras.begin() + at++, v[k]);
    Para& last = P(at - 1);
    caret = anchor = { at - 1, (int)last.text.size() };
    last.text += tail; last.at.insert(last.at.end(), tailAt.begin(), tailAt.end());
}

bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }
int PrevWord(const std::wstring& t, int i) { while (i > 0 && !IsWordChar(t[i - 1])) i--; while (i > 0 && IsWordChar(t[i - 1])) i--; return i; }
int NextWord(const std::wstring& t, int i) { int n = (int)t.size(); while (i < n && !IsWordChar(t[i])) i++; while (i < n && IsWordChar(t[i])) i++; return i; }

void UncollapseAround(int i) {  // o cursor entrou numa seção recolhida: abre os títulos que a escondem
    if (pl.size() != (size_t)Count() || !pl[i].hidden) return;
    for (int k = i - 1; k >= 0; k--) if (P(k).collapsed && Level(P(k)) < Level(P(i))) { P(k).collapsed = false; }
    dirtyLayout = true;
}

int PrevVisible(int i) { for (int k = i - 1; k >= 0; k--) if (k >= (int)pl.size() || !pl[k].hidden) return k; return -1; }
int NextVisible(int i) { for (int k = i + 1; k < Count(); k++) if (k >= (int)pl.size() || !pl[k].hidden) return k; return -1; }

// parágrafos atingidos pela seleção (ou o do cursor)
void SelParas(int& a, int& b) {
    Pos s = SelStart(), e = SelEnd();
    a = s.p; b = e.p;
    if (HasSel() && e.o == 0 && e.p > s.p) b--;  // seleção que termina no começo do parágrafo não o inclui
}

std::vector<Para> SelectionParas() {
    std::vector<Para> v;
    Pos s = SelStart(), e = SelEnd();
    for (int i = s.p; i <= e.p; i++) {
        Para p = P(i);
        int a = i == s.p ? s.o : 0, b = i == e.p ? e.o : (int)p.text.size();
        p.text = p.text.substr(a, b - a);
        p.at = std::vector<uint8_t>(P(i).at.begin() + a, P(i).at.begin() + b);
        p.collapsed = false;
        v.push_back(p);
    }
    return v;
}

UINT ClipFormat() { static UINT f = RegisterClipboardFormatW(L"NotasParagrafos"); return f; }

}  // namespace

// ---------------------------------------------------------------------------
namespace editor {

void SetFormats(const std::wstring& fam, const std::wstring& mono) {
    family = fam; monoFamily = mono;
    for (int s = 0; s < 5; s++) {
        if (fmt[s]) fmt[s]->Release();
        gfx::dw->CreateTextFormat(s == S_MONO ? monoFamily.c_str() : family.c_str(), nullptr, SM[s].w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, SM[s].size, L"pt-br", &fmt[s]);
        if (fmt[s]) fmt[s]->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    ReleaseLayouts();
}
void SetDark(bool d) { dark = d; ReleaseBrushes(); ReleaseLayouts(); }
void SetSearch(const std::wstring& q) { query = q; for (auto& c : query) c = towlower(c); }

Note* Current() { return N; }

void SetNote(Note* n) {
    if (n == N) return;
    N = n;
    caret = anchor = { 0, 0 };
    scrollY = 0; desiredX = -1; typingSet = false;
    ReleaseLayouts();
    if (N && N->paras.empty()) N->paras.push_back(Para{});
}

void PlaceCaretAtEnd() {
    if (!N) return;
    caret.p = Count() - 1; caret.o = (int)P(caret.p).text.size(); anchor = caret;
    wantReveal = true;
}

void Focus(bool on) { focused = on; blinkOn = true; }
bool Focused() { return focused; }
void BlinkReset() { blinkOn = true; }
void ToggleBlink() { blinkOn = !blinkOn; }
void Invalidate() { dirtyLayout = true; }
bool HasSelection() { return N && HasSel(); }

void Draw(ID2D1DeviceContext* dc, D2D1_RECT_F a) {
    area = a;
    if (!N) return;
    Clamp();
    BuildLayouts(dc, a.right - a.left);
    if (wantReveal) { Reveal(); wantReveal = false; }
    float h = a.bottom - a.top;
    scrollY = std::clamp(scrollY, 0.f, std::max(0.f, contentH - h));
    float ox = a.left, oy = a.top - scrollY;
    dc->PushAxisAlignedClip(a, D2D1_ANTIALIAS_MODE_ALIASED);
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(TextColor(), &br);
    if (semaforo::Leopard()) {
        // Leopard: o bloco amarelo das Notas do Mail — amarelo-claro, as pautas finas a cada 24 (rolam com o texto) e a
        // margem dupla avermelhada à esquerda
        br->SetColor(D2D1::ColorF(1, 0.976f, 0.73f));
        dc->FillRectangle(a, br);
        br->SetColor(D2D1::ColorF(0.55f, 0.62f, 0.80f, 0.35f));
        float y0 = a.top + 40 - std::fmod(scrollY, 24.f);
        for (float y = y0; y < a.bottom; y += 24) dc->DrawLine({ a.left, y }, { a.right, y }, br, 1);
        br->SetColor(D2D1::ColorF(0.85f, 0.35f, 0.30f, 0.45f));
        dc->DrawLine({ a.left + 10, a.top }, { a.left + 10, a.bottom }, br, 1);
        dc->DrawLine({ a.left + 13, a.top }, { a.left + 13, a.bottom }, br, 1);
        br->SetColor(TextColor());
    }

    // data no alto, centralizada e cinza
    {
        std::wstring d = LongDate(N->modified);
        IDWriteTextFormat* tf = nullptr;
        gfx::dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"pt-br", &tf);
        if (tf) {
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            br->SetColor(Text2());
            dc->DrawTextW(d.c_str(), (UINT32)d.size(), tf, D2D1::RectF(ox, oy + 6, a.right, oy + 24), br);
            tf->Release();
        }
    }
    Pos ss = SelStart(), se = SelEnd();
    bool sel = HasSel();
    D2D1_COLOR_F accent = Accent();
    for (int i = 0; i < Count(); i++) {
        PL& l = pl[i];
        if (l.hidden || !l.tl) continue;
        float py = oy + l.y;
        if (py > a.bottom || py + l.h < a.top) continue;
        Para& p = P(i);
        float px = ox + l.x;
        auto rangeRects = [&](int from, int len, D2D1_COLOR_F c, float pad) {
            if (len <= 0) return;
            UINT32 cnt = 0;
            l.tl->HitTestTextRange(from, len, 0, 0, nullptr, 0, &cnt);
            std::vector<DWRITE_HIT_TEST_METRICS> ms(cnt);
            l.tl->HitTestTextRange(from, len, 0, 0, ms.data(), cnt, &cnt);
            br->SetColor(c);
            for (auto& m : ms) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(px + m.left - pad, py + m.top + 1, px + m.left + m.width + pad, py + m.top + m.height), 3, 3), br);
        };
        // realces
        for (int k = 0; k < (int)p.at.size();) {
            int j = k; int hcol = HiOf(p.at[k]);
            while (j < (int)p.at.size() && HiOf(p.at[j]) == hcol) j++;
            if (hcol) rangeRects(k, j - k, HiBack(hcol), 1);
            k = j;
        }
        // resultados das contas: fundo amarelado discreto
        if (i < (int)math.size() && !math[i].result.empty())
            rangeRects(l.resultAt, (int)math[i].result.size(), D2D1::ColorF(accent.r, accent.g, accent.b, dark ? 0.16f : 0.13f), 1);
        // busca
        if (!query.empty() && query.size() <= p.text.size()) {
            std::wstring low = p.text; for (auto& c : low) c = towlower(c);
            for (size_t f = low.find(query); f != std::wstring::npos; f = low.find(query, f + query.size()))
                rangeRects((int)f, (int)query.size(), D2D1::ColorF(1.0f, 0.85f, 0.1f, dark ? 0.55f : 0.6f), 0.5f);
        }
        // seleção
        if (sel && i >= ss.p && i <= se.p) {
            int from = i == ss.p ? ss.o : 0, to = i == se.p ? se.o : (int)p.text.size();
            D2D1_COLOR_F sc = focused ? D2D1::ColorF(accent.r, accent.g, accent.b, dark ? 0.40f : 0.38f) : (dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.10f));
            rangeRects(from, to - from, sc, 0);
            if (i < se.p) {  // a quebra de linha também está selecionada
                D2D1_RECT_F c = CaretRectAt({ i, (int)p.text.size() });
                br->SetColor(sc);
                dc->FillRectangle(D2D1::RectF(ox + c.left, oy + c.top + 1, ox + c.left + 6, oy + c.bottom), br);
            }
        }
        // marcador da lista
        float x0, y0; DWRITE_HIT_TEST_METRICS m0;
        l.tl->HitTestTextPosition(0, FALSE, &x0, &y0, &m0);
        float cy = py + m0.top + m0.height / 2;
        float mx = ox + l.base;
        if (p.list == L_CHECK) {
            D2D1_POINT_2F c{ mx + 9, cy };
            if (p.checked) {
                br->SetColor(accent);
                dc->FillEllipse(D2D1::Ellipse(c, 8.5f, 8.5f), br);
                br->SetColor(D2D1::ColorF(1, 1, 1));
                simbolo::Draw(dc, L"checkmark", D2D1::RectF(c.x - 4.6f, c.y - 4.2f, c.x + 4.6f, c.y + 4.2f), br);
            } else {
                br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.38f) : D2D1::ColorF(0, 0, 0, 0.30f));
                dc->DrawEllipse(D2D1::Ellipse(c, 8, 8), br, 1.1f);
            }
        } else if (p.list != L_NONE) {
            std::wstring mk = p.list == L_BULLET ? L"•" : p.list == L_DASH ? L"–" : std::to_wstring(l.number) + L".";
            IDWriteTextLayout* ml = nullptr;
            gfx::dw->CreateTextLayout(mk.c_str(), (UINT32)mk.size(), fmt[p.style], 40, 100, &ml);
            if (ml) {
                ml->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, SM[p.style].line, SM[p.style].line * 0.8f);
                DWRITE_TEXT_METRICS tm{}; ml->GetMetrics(&tm);
                float x = p.list == L_NUMBER ? mx + 19 - tm.width : mx + 4;
                br->SetColor(TextColor());
                dc->DrawTextLayout(D2D1::Point2F(x, py), ml, br);
                ml->Release();
            }
        }
        // seta das seções recolhíveis (títulos): aparece com o mouse em cima ou quando recolhida
        if (Level(p) < 99 && (p.collapsed || hoverChevron == i)) {
            br->SetColor(Text2());
            float cx = ox + 11, s = 4.2f;
            ID2D1PathGeometry* g = nullptr; ID2D1Factory* f = nullptr; dc->GetFactory(&f);
            if (f && SUCCEEDED(f->CreatePathGeometry(&g))) {
                ID2D1GeometrySink* k = nullptr; g->Open(&k);
                if (p.collapsed) { k->BeginFigure({ cx - s / 2, cy - s }, D2D1_FIGURE_BEGIN_HOLLOW); k->AddLine({ cx + s / 2, cy }); k->AddLine({ cx - s / 2, cy + s }); }
                else { k->BeginFigure({ cx - s, cy - s / 2 }, D2D1_FIGURE_BEGIN_HOLLOW); k->AddLine({ cx, cy + s / 2 }); k->AddLine({ cx + s, cy - s / 2 }); }
                k->EndFigure(D2D1_FIGURE_END_OPEN); k->Close(); k->Release();
                ID2D1StrokeStyle* rs = nullptr;
                f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &rs);
                dc->DrawGeometry(g, br, 1.6f, rs);
                if (rs) rs->Release();
                g->Release();
            }
            if (f) f->Release();
        }
        br->SetColor(TextColor());
        dc->DrawTextLayout(D2D1::Point2F(px, py), l.tl, br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    // cursor amarelo
    if (focused && blinkOn && !sel && caret.p < (int)pl.size() && !pl[caret.p].hidden) {
        D2D1_RECT_F c = CaretRectAt(caret);
        br->SetColor(accent);
        dc->FillRectangle(D2D1::RectF(ox + c.left - 0.5f, oy + c.top, ox + c.left + 1.5f, oy + c.bottom), br);
    }
    br->Release();
    dc->PopAxisAlignedClip();
}

void CaretScreenRect(D2D1_RECT_F& r) {
    D2D1_RECT_F c = CaretRectAt(caret);
    r = { area.left + c.left, area.top - scrollY + c.top, area.left + c.right, area.top - scrollY + c.bottom };
}

void OnWheel(float dy) { scrollY = std::max(0.f, scrollY - dy); }

static int ChevronAt(D2D1_POINT_2F p) {
    if (!N || pl.size() != (size_t)Count() || p.x < area.left || p.x > area.left + MARGIN) return -1;
    float y = p.y - area.top + scrollY;
    for (int i = 0; i < Count(); i++)
        if (!pl[i].hidden && Level(P(i)) < 99 && y >= pl[i].y && y < pl[i].y + SM[P(i).style].line + 2) return i;
    return -1;
}
bool OverChevronArea(D2D1_POINT_2F p) { return ChevronAt(p) >= 0; }

void OnMouseMove(D2D1_POINT_2F p, bool dragging) {
    if (!N) return;
    if (!dragging) {
        // a seta aparece com o mouse em qualquer lugar da linha do título
        int hc = -1;
        if (pl.size() == (size_t)Count() && p.x >= area.left && p.x < area.right) {
            float y = p.y - area.top + scrollY;
            for (int i = 0; i < Count(); i++)
                if (!pl[i].hidden && Level(P(i)) < 99 && y >= pl[i].y && y < pl[i].y + pl[i].h) { hc = i; break; }
        }
        if (hc != hoverChevron) { hoverChevron = hc; RequestPaint(); }
        return;
    }
    Pos q = HitContent(p.x - area.left, p.y - area.top + scrollY);
    if (clickMode == 2) {
        const std::wstring& t = P(q.p).text;
        if (Less(q, clickA)) { caret = { q.p, PrevWord(t, std::min(q.o + 1, (int)t.size())) }; anchor = clickB; }
        else { caret = { q.p, NextWord(t, q.o) }; anchor = clickA; }
    } else if (clickMode == 3) {
        if (q.p < clickA.p) { caret = { q.p, 0 }; anchor = clickB; }
        else { caret = { q.p, (int)P(q.p).text.size() }; anchor = clickA; }
    } else caret = q;
    if (p.y < area.top) scrollY = std::max(0.f, scrollY - 12);
    if (p.y > area.bottom) scrollY += 12;
    desiredX = -1;
    RequestPaint();
}

void OnMouseDown(D2D1_POINT_2F p, bool shift, int clicks) {
    if (!N) return;
    focused = true; blinkOn = true;
    int ch = ChevronAt(p);
    if (ch >= 0 && clicks == 1) {
        Checkpoint(3);
        P(ch).collapsed = !P(ch).collapsed;
        if (P(ch).collapsed && caret.p > ch) { caret = anchor = { ch, (int)P(ch).text.size() }; }
        N->dirty = true; dirtyLayout = true; NoteEdited(N);
        return;
    }
    float cx = p.x - area.left, cy = p.y - area.top + scrollY;
    // bolinha da lista de verificação
    for (int i = 0; i < Count() && pl.size() == (size_t)Count(); i++) {
        if (pl[i].hidden || P(i).list != L_CHECK || !pl[i].tl) continue;
        float x0, y0; DWRITE_HIT_TEST_METRICS m0;
        pl[i].tl->HitTestTextPosition(0, FALSE, &x0, &y0, &m0);
        float ccx = pl[i].base + 9, ccy = pl[i].y + m0.top + m0.height / 2;
        if (std::fabs(cx - ccx) <= 11 && std::fabs(cy - ccy) <= 11) {
            Checkpoint(3);
            P(i).checked = !P(i).checked;
            Changed();
            wantReveal = false;
            return;
        }
    }
    Pos q = HitContent(cx, cy);
    typingSet = false; desiredX = -1;
    const std::wstring& t = P(q.p).text;
    if (clicks == 2) {
        int a = q.o, b = q.o;
        if (a < (int)t.size() && IsWordChar(t[a])) { while (a > 0 && IsWordChar(t[a - 1])) a--; while (b < (int)t.size() && IsWordChar(t[b])) b++; }
        else if (b < (int)t.size()) b++;
        anchor = { q.p, a }; caret = { q.p, b };
        clickA = anchor; clickB = caret; clickMode = 2;
    } else if (clicks >= 3) {
        anchor = { q.p, 0 }; caret = { q.p, (int)t.size() };
        clickA = anchor; clickB = caret; clickMode = 3;
    } else {
        caret = q;
        if (!shift) anchor = q;
        clickMode = 1;
    }
    RequestPaint();
}

void SelectAll() { if (!N) return; anchor = { 0, 0 }; caret = { Count() - 1, (int)P(Count() - 1).text.size() }; }

void Copy(bool cut) {
    if (!N || !HasSel()) return;
    auto v = SelectionParas();
    std::wstring plain;
    for (size_t i = 0; i < v.size(); i++) { if (i) plain += L"\r\n"; plain += v[i].text; }
    std::wstring rich = store::Serialize(v);
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        auto put = [](UINT fmtId, const std::wstring& s) {
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
            if (!g) return;
            memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            if (!SetClipboardData(fmtId, g)) GlobalFree(g);
        };
        put(CF_UNICODETEXT, plain);
        put(ClipFormat(), rich);
        CloseClipboard();
    }
    if (cut) { Checkpoint(3); DeleteSel(); Changed(); }
}

void Paste() {
    if (!N || !OpenClipboard(nullptr)) return;
    std::vector<Para> v;
    if (HANDLE h = GetClipboardData(ClipFormat())) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) { v = store::Deserialize(p); GlobalUnlock(h); }
    } else if (HANDLE h2 = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h2)) {
            std::wstring s = p; GlobalUnlock(h2);
            uint8_t at = AttrAtCaret() & ~0x70;  // texto colado sem realce
            size_t a = 0;
            for (;;) {
                size_t e = s.find(L'\n', a);
                std::wstring line = s.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                Para q; q.text = line; q.at.assign(line.size(), at);
                q.style = S_BODY;
                v.push_back(q);
                if (e == std::wstring::npos) break;
                a = e + 1;
            }
            if (v.size() == 1) v[0].style = P(caret.p).style;
        }
    }
    CloseClipboard();
    if (v.empty()) return;
    Checkpoint(3);
    InsertParas(v);
    typingSet = false;
    Changed();
}

void Undo() {
    if (!N || N->undo.empty()) return;
    N->redo.push_back({ N->paras, caret.p, caret.o, anchor.p, anchor.o });
    auto s = N->undo.back(); N->undo.pop_back();
    N->paras = s.paras; caret = { s.cp, s.co }; anchor = { s.ap, s.ao };
    N->lastEditKind = 0;
    Clamp(); Changed();
}
void Redo() {
    if (!N || N->redo.empty()) return;
    N->undo.push_back({ N->paras, caret.p, caret.o, anchor.p, anchor.o });
    auto s = N->redo.back(); N->redo.pop_back();
    N->paras = s.paras; caret = { s.cp, s.co }; anchor = { s.ap, s.ao };
    N->lastEditKind = 0;
    Clamp(); Changed();
}

int CurrentStyle() { return N ? P(caret.p).style : S_BODY; }
int CurrentList() { return N ? P(caret.p).list : L_NONE; }
uint8_t CurrentAttrs() {
    if (!N) return 0;
    if (HasSel()) { Pos s = SelStart(); return s.o < (int)P(s.p).at.size() ? P(s.p).at[s.o] : 0; }
    return AttrAtCaret();
}

void ApplyStyle(int style) {
    if (!N) return;
    Checkpoint(3);
    int a, b; SelParas(a, b);
    for (int i = a; i <= b; i++) { P(i).style = (uint8_t)style; if (style > S_SUBHEADING) P(i).collapsed = false; }
    Changed();
}

void ToggleAttr(uint8_t flag) {
    if (!N) return;
    if (!HasSel()) { typing = AttrAtCaret() ^ flag; typingSet = true; return; }
    Checkpoint(3);
    Pos s = SelStart(), e = SelEnd();
    bool all = true;
    for (int i = s.p; i <= e.p; i++) {
        int from = i == s.p ? s.o : 0, to = i == e.p ? e.o : (int)P(i).text.size();
        for (int k = from; k < to; k++) if (!(P(i).at[k] & flag)) all = false;
    }
    for (int i = s.p; i <= e.p; i++) {
        int from = i == s.p ? s.o : 0, to = i == e.p ? e.o : (int)P(i).text.size();
        for (int k = from; k < to; k++) P(i).at[k] = all ? (P(i).at[k] & ~flag) : (P(i).at[k] | flag);
    }
    Changed();
}

void SetHighlight(int h) {
    if (!N) return;
    if (!HasSel()) { typing = WithHi(AttrAtCaret(), h); typingSet = true; return; }
    Checkpoint(3);
    Pos s = SelStart(), e = SelEnd();
    for (int i = s.p; i <= e.p; i++) {
        int from = i == s.p ? s.o : 0, to = i == e.p ? e.o : (int)P(i).text.size();
        for (int k = from; k < to; k++) P(i).at[k] = WithHi(P(i).at[k], h);
    }
    Changed();
}

void ToggleList(int kind) {
    if (!N) return;
    Checkpoint(3);
    int a, b; SelParas(a, b);
    bool all = true;
    for (int i = a; i <= b; i++) if (P(i).list != kind) all = false;
    for (int i = a; i <= b; i++) {
        P(i).list = all ? (uint8_t)L_NONE : (uint8_t)kind;
        if (!all && P(i).style != S_MONO) P(i).style = P(i).style <= S_SUBHEADING && P(i).text.empty() ? (uint8_t)S_BODY : P(i).style;
        P(i).checked = false;
    }
    Changed();
}

void Indent(int d) {
    if (!N) return;
    Checkpoint(3);
    int a, b; SelParas(a, b);
    for (int i = a; i <= b; i++) P(i).indent = (uint8_t)std::clamp(P(i).indent + d, 0, 8);
    Changed();
}

void ToggleChecked() {
    if (!N) return;
    Checkpoint(3);
    int a, b; SelParas(a, b);
    for (int i = a; i <= b; i++) if (P(i).list == L_CHECK) P(i).checked = !P(i).checked;
    Changed();
}

void OnChar(wchar_t c) {
    if (!N || !focused) return;
    if (c == L'\r' || c == L'\n') {
        Checkpoint(3);
        DeleteSel();
        Para& p = P(caret.p);
        if (p.list != L_NONE && p.text.empty()) {  // Enter na linha vazia da lista: sai da lista (ou volta um nível)
            if (p.indent > 0) p.indent--; else p.list = L_NONE;
        } else if (caret.o == 0 && !p.text.empty()) {  // no começo: abre uma linha em cima com a mesma formatação
            Para np = p; np.text.clear(); np.at.clear(); np.checked = false; np.collapsed = false;
            N->paras.insert(N->paras.begin() + caret.p, np);
            caret = anchor = { caret.p + 1, 0 };
        } else SplitPara();
        typingSet = false;
        Changed();
        return;
    }
    if (c == L'\t') return;  // tratado em OnKey
    if (c < 32 || c == 127) return;
    Checkpoint(1);
    uint8_t at = AttrAtCaret();
    DeleteSel();
    Para& p = P(caret.p);
    // atalhos de digitação: "* " / "- " / "1. " viram listas, como no Mac
    if (c == L' ' && p.list == L_NONE && caret.o == (int)p.text.size() && p.style != S_MONO) {
        int kind = -1;
        if (p.text == L"*" || p.text == L"•") kind = L_BULLET;
        else if (p.text == L"-" || p.text == L"–") kind = L_DASH;
        else if (p.text == L"1." || p.text == L"1)") kind = L_NUMBER;
        else if (p.text == L"[]" || p.text == L"[ ]") kind = L_CHECK;
        if (kind >= 0) {
            Checkpoint(3);
            p.text.clear(); p.at.clear(); p.list = (uint8_t)kind; caret.o = 0; anchor = caret;
            if (p.style <= S_SUBHEADING) p.style = S_BODY;
            Changed();
            return;
        }
    }
    InsertPlain(std::wstring(1, c), at);
    typing = at;  // continua com o mesmo atributo
    Changed();
}

bool OnKey(UINT vk) {
    if (!N || !focused) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    blinkOn = true;
    auto move = [&](Pos q, bool keepX = false) {
        caret = q;
        if (!shift) anchor = q;
        if (!keepX) desiredX = -1;
        typingSet = false;
        if (q.p < (int)pl.size() && pl[q.p].hidden) UncollapseAround(q.p);
        wantReveal = true;
    };
    // atalhos de formatação (Ctrl no lugar do Cmd)
    if (ctrl && shift) {
        switch (vk) {
        case 'T': ApplyStyle(S_TITLE); return true;
        case 'H': ApplyStyle(S_HEADING); return true;
        case 'J': ApplyStyle(S_SUBHEADING); return true;
        case 'B': ApplyStyle(S_BODY); return true;
        case 'M': ApplyStyle(S_MONO); return true;
        case '7': ToggleList(L_BULLET); return true;
        case '8': ToggleList(L_DASH); return true;
        case '9': ToggleList(L_NUMBER); return true;
        case 'L': ToggleList(L_CHECK); return true;
        case 'U': ToggleChecked(); return true;
        case 'X': ToggleAttr(A_STRIKE); return true;
        case 'Z': Redo(); return true;
        }
    }
    if (ctrl && !shift) {
        switch (vk) {
        case 'B': ToggleAttr(A_BOLD); return true;
        case 'I': ToggleAttr(A_ITALIC); return true;
        case 'U': ToggleAttr(A_UNDER); return true;
        case 'A': SelectAll(); return true;
        case 'C': Copy(false); return true;
        case 'X': Copy(true); return true;
        case 'V': Paste(); return true;
        case 'Z': Undo(); return true;
        case 'Y': Redo(); return true;
        case VK_OEM_6: Indent(1); return true;   // Ctrl+]
        case VK_OEM_4: Indent(-1); return true;  // Ctrl+[
        }
    }
    Para& p = P(caret.p);
    switch (vk) {
    case VK_LEFT:
        if (!shift && HasSel()) { move(SelStart()); return true; }
        if (caret.o > 0) move({ caret.p, ctrl ? PrevWord(p.text, caret.o) : caret.o - 1 });
        else { int k = PrevVisible(caret.p); if (k >= 0) move({ k, (int)P(k).text.size() }); }
        return true;
    case VK_RIGHT:
        if (!shift && HasSel()) { move(SelEnd()); return true; }
        if (caret.o < (int)p.text.size()) move({ caret.p, ctrl ? NextWord(p.text, caret.o) : caret.o + 1 });
        else { int k = NextVisible(caret.p); if (k >= 0) move({ k, 0 }); }
        return true;
    case VK_UP: case VK_DOWN: {
        if (pl.size() != (size_t)Count()) return true;
        if (ctrl) { move(vk == VK_UP ? Pos{ 0, 0 } : Pos{ Count() - 1, (int)P(Count() - 1).text.size() }); return true; }
        D2D1_RECT_F c = CaretRectAt(caret);
        if (desiredX < 0) desiredX = c.left;
        float y = vk == VK_UP ? c.top - 2 : c.bottom + 2;
        if (vk == VK_UP && caret.p == 0 && c.top <= pl[0].y + 1) { move({ 0, 0 }, true); return true; }
        if (vk == VK_DOWN && y >= contentH - 40) { move({ Count() - 1, (int)P(Count() - 1).text.size() }, true); return true; }
        // pula o espaço entre parágrafos
        Pos q = HitContent(desiredX, y);
        if (Same(q, caret)) q = HitContent(desiredX, vk == VK_UP ? y - 6 : y + 6);
        move(q, true);
        return true;
    }
    case VK_HOME: case VK_END: {
        if (ctrl) { move(vk == VK_HOME ? Pos{ 0, 0 } : Pos{ Count() - 1, (int)P(Count() - 1).text.size() }); return true; }
        if (pl.size() != (size_t)Count() || !pl[caret.p].tl) return true;
        D2D1_RECT_F c = CaretRectAt(caret);
        BOOL tr, in; DWRITE_HIT_TEST_METRICS m;
        pl[caret.p].tl->HitTestPoint(vk == VK_HOME ? -1000.f : 100000.f, (c.top + c.bottom) / 2 - pl[caret.p].y, &tr, &in, &m);
        int o = (int)m.textPosition + (tr ? (int)m.length : 0);
        o = std::min(o, (int)p.text.size());
        if (vk == VK_END && o > 0 && o < (int)p.text.size() && p.text[o - 1] == L' ') o--;  // fim da linha quebrada
        move({ caret.p, o });
        return true;
    }
    case VK_PRIOR: case VK_NEXT: {
        float h = area.bottom - area.top;
        D2D1_RECT_F c = CaretRectAt(caret);
        scrollY += vk == VK_PRIOR ? -h : h;
        move(HitContent(c.left, (c.top + c.bottom) / 2 + (vk == VK_PRIOR ? -h : h)));
        return true;
    }
    case VK_BACK:
        if (HasSel()) { Checkpoint(2); DeleteSel(); Changed(); return true; }
        if (caret.o == 0) {
            Checkpoint(3);
            if (p.list != L_NONE) p.list = L_NONE;
            else if (p.indent > 0) p.indent--;
            else if (caret.p > 0) {
                int k = PrevVisible(caret.p);
                if (k < 0) k = caret.p - 1;
                if (P(k).text.empty() && !p.text.empty()) {  // linha vazia em cima: some ela, este fica como está
                    N->paras.erase(N->paras.begin() + k);
                    caret = anchor = { k, 0 };
                } else {
                    for (int z = k; z < caret.p; z++) P(z).collapsed = false;
                    Pos a{ k, (int)P(k).text.size() };
                    DeleteRange(a, { caret.p, 0 });
                }
            } else if (p.style != S_BODY && p.text.empty() && Count() > 1) p.style = S_BODY;
            Changed();
            return true;
        }
        Checkpoint(2);
        DeleteRange({ caret.p, ctrl ? PrevWord(p.text, caret.o) : caret.o - 1 }, caret);
        Changed();
        return true;
    case VK_DELETE:
        if (HasSel()) { Checkpoint(2); DeleteSel(); Changed(); return true; }
        Checkpoint(2);
        if (caret.o < (int)p.text.size()) DeleteRange(caret, { caret.p, ctrl ? NextWord(p.text, caret.o) : caret.o + 1 });
        else if (caret.p + 1 < Count()) { P(caret.p).collapsed = false; DeleteRange(caret, { caret.p + 1, 0 }); }
        Changed();
        return true;
    case VK_TAB: {
        int a, b; SelParas(a, b);
        if (p.list != L_NONE || a != b) { Indent(shift ? -1 : 1); return true; }
        if (!shift) { Checkpoint(1); uint8_t at = AttrAtCaret(); DeleteSel(); InsertPlain(L"\t", at); Changed(); }
        return true;
    }
    case VK_ESCAPE:
        if (HasSel()) { anchor = caret; return true; }
        return false;
    }
    return false;
}

}  // namespace editor
