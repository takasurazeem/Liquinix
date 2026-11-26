// Pilhas do Dock, como no Mac: clicar na pasta Downloads mostra os arquivos dela em Leque (os ícones sobem em
// arco a partir do ícone, com o nome numa etiqueta à esquerda: o padrão do Mac, "Automático"), em Grade
// (painel de vidro com as miniaturas) ou em Lista (menu). O botão direito escolhe a ordem e o jeito de ver
// (pilha_ordem / pilha_ver no ajustes.txt). Clicar num arquivo abre o arquivo; Esc ou clique fora fecha.
#include "shell.h"
#include "../comum/leopard.h"  // Leopard: as pastas azuis e os documentos do 10.5 nas Pilhas
#include "../menu/context_menu.h"
#include <dwmapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

namespace {

const UINT WM_PL_ICON = WM_APP + 81, WM_PL_FRAME = WM_APP + 82;
const int COLS = 6;
int MAXN = 18;      // Downloads: 18; a pilha Aplicativos do Leopard mostra mais (g_apps)
bool g_apps;        // Leopard: a pasta Aplicativos da Dock como Pilha em Grade (os atalhos do Menu Iniciar)
const float CELL_W = 78, CELL_H = 86, PAD_SEQ = 12, HEAD = 30, FOOT = 30, ICON = 52, R_SEQ = 12, ARROW = 9;
// a pilha em grade é um popover: no Tahoe raio 20 e 13 de margem (guia macOS26-Theme-Reference, seção 31)
#define R (liquid::On() ? 20.f : R_SEQ)
#define PAD (liquid::On() ? 13.f : PAD_SEQ)

HWND g_wnd, g_back;
HWND g_prevFg;  // a janela que estava na frente: volta para ela ao fechar (o painel não tira você dela)
ui::GpuSurface g_s;
bool g_glass, g_open;
double g_openAt;
POINT g_anchor;  // tela (px): centro de cima do ícone de Downloads
int g_hover = -2;  // -1 = botão do rodapé
struct File { std::wstring path, name; std::vector<BYTE> px; UINT w = 0, h = 0; ID2D1Bitmap* bmp = nullptr; bool asked = false; };
std::vector<File> g_files;
std::mutex g_mx;
int g_gen;
IDWriteTextFormat *g_tfTitle, *g_tfName, *g_tfBtn;

float Sc() { return ui::Scale(g_wnd); }
int Rows() { return std::max(1, (int)((g_files.size() + COLS - 1) / COLS)); }
float PW() { return PAD * 2 + COLS * CELL_W; }
float PH() { return HEAD + Rows() * CELL_H + FOOT + ARROW; }  // com a setinha embaixo (fora do vidro)
D2D1_RECT_F CellRect(int i) { float x = PAD + (i % COLS) * CELL_W, y = HEAD + (i / COLS) * CELL_H; return { x, y, x + CELL_W, y + CELL_H }; }
D2D1_RECT_F BtnRect() { return { PW() / 2 - 70, HEAD + Rows() * CELL_H + 2, PW() / 2 + 70, HEAD + Rows() * CELL_H + 26 }; }
float g_arrowX;  // DIP, dentro do painel: onde a setinha aponta (o ícone no Dock)
int g_side;      // Dock à esquerda (1) ou à direita (2): a Grade abre ao lado, sem setinha, crescendo do lado do Dock
// o ponto (px no painel) de onde o painel cresce e para onde volta: o ícone no Dock
float GrowX(int wpx, float sc) { return g_side == 1 ? 0.f : g_side == 2 ? (float)wpx : g_arrowX * sc; }
float GrowY(int hpx) { return g_side ? hpx / 2.f : (float)hpx; }
// ---- Leque
bool g_fan;                 // aberta em Leque (sem vidro: cada item com a etiqueta dele)
const int FAN_MAX = 10;
const float FAN_W = 460, FAN_ITEM = 56, FAN_ICON = 46, FAN_BOTTOM = 18;
float FanH() { return FAN_BOTTOM + (g_files.size() + 1) * FAN_ITEM + 24; }
// posição final do item i (0 = o de baixo, mais perto do Dock; g_files.size() = "Abrir no Finder"), em DIP
D2D1_POINT_2F FanPos(int i) {
    float x = FAN_W - 70 + 0.9f * i * i;  // o arco: sobe e vai para a direita, como no Mac
    float y = FanH() - FAN_BOTTOM - (i + 0.5f) * FAN_ITEM;
    return { x, y };
}
bool g_closing;
double g_closeAt;

void Free() {
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& f : g_files) if (f.bmp) f.bmp->Release();
    g_files.clear();
}

void Hide(bool restore = false) {
    if (!g_open || g_closing) return;
    if (restore && g_prevFg && IsWindow(g_prevFg)) SetForegroundWindow(g_prevFg);
    g_closing = true;  // some na mola de volta para o ícone; o relógio de quadros esconde no fim
    g_closeAt = anim::Now();
    ui::FrameClock(g_wnd, WM_PL_FRAME, true);
}
void HideNow() {
    g_open = g_closing = false;
    ui::FrameClock(g_wnd, WM_PL_FRAME, false);
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
}

bool g_inGlass;

// a mola de abrir (crescendo a partir do ícone, embaixo no meio): só escala e opacidade, sem redesenhar
void PaintFan();
void Motion() {
    if (g_fan) { PaintFan(); return; }
    float sc = Sc();
    int wpx = (int)std::lround(PW() * sc), hpx = (int)std::lround(PH() * sc);
    anim::Panel mo = g_closing ? anim::PanelOut(anim::Now() - g_closeAt) : anim::PanelIn(anim::Now() - g_openAt);
    float cx = GrowX(wpx, sc), cy = GrowY(hpx);  // cresce e volta para o ícone
    if (g_inGlass) backdrop::Fade(g_back, mo.fade, mo.scale, cx, cy);  // o desenho está dentro do vidro: vão juntos
    else { ui::GpuMotion(g_s, mo.scale, cx, cy, mo.fade); if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, cx, cy); }
}

// Mac OS X Leopard: a Pilha em Grade do 10.5 — painel preto translúcido (sem desfoque) de cantos redondos com o fio
// claro em volta, o nome da pasta em branco no alto, ícones com o nome em branco e o botão escuro "Abrir no Finder ›"
void PaintLeopard() {
    float sc = Sc();
    int wpx = (int)std::lround(PW() * sc), hpx = (int)std::lround(PH() * sc);
    if (g_back && IsWindowVisible(g_back)) ShowWindow(g_back, SW_HIDE);
    g_inGlass = false;
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
    if (!br) { ui::GpuEnd(g_s); return; }
    const float BODY = PH() - ARROW, RR = 12;
    ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
    ID2D1PathGeometry* shape = nullptr;  // o corpo e a setinha numa peça só
    if (fac && SUCCEEDED(fac->CreatePathGeometry(&shape))) {
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(shape->Open(&sk))) {
            float ax = std::clamp(g_arrowX, RR + ARROW, PW() - RR - ARROW);
            sk->BeginFigure({ RR, 0 }, D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine({ PW() - RR, 0 }); sk->AddArc(D2D1::ArcSegment({ PW(), RR }, { RR, RR }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine({ PW(), BODY - RR }); sk->AddArc(D2D1::ArcSegment({ PW() - RR, BODY }, { RR, RR }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine({ ax + ARROW, BODY }); sk->AddLine({ ax, BODY + ARROW - 1 }); sk->AddLine({ ax - ARROW, BODY });
            sk->AddLine({ RR, BODY }); sk->AddArc(D2D1::ArcSegment({ 0, BODY - RR }, { RR, RR }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine({ 0, RR }); sk->AddArc(D2D1::ArcSegment({ RR, 0 }, { RR, RR }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
            br->SetColor(D2D1::ColorF(0.11f, 0.11f, 0.12f, 0.92f));  // a grade do 10.5 quase não deixava ver o fundo
            dc->FillGeometry(shape, br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
            dc->DrawGeometry(shape, br, 1.2f);
        }
        shape->Release();
    }
    if (fac) fac->Release();
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
    {
        std::wstring title = g_apps ? std::wstring(Tr(L"Aplicativos")) : std::wstring(Tr(L"Downloads"));
        dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_tfTitle, D2D1::RectF(0, 7, PW(), HEAD), br);
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (g_files.empty()) {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
            dc->DrawTextW(Tr(L"Nenhum item"), (UINT32)wcslen(Tr(L"Nenhum item")), g_tfName, D2D1::RectF(0, HEAD + 30, PW(), HEAD + 60), br);
        }
        for (int i = 0; i < (int)g_files.size(); i++) {
            auto& f = g_files[i];
            D2D1_RECT_F c = CellRect(i);
            float cx = (c.left + c.right) / 2;
            D2D1_RECT_F ir{ cx - ICON / 2, c.top + 8, cx + ICON / 2, c.top + 8 + ICON };
            if (i == g_hover) {  // o item embaixo do ponteiro: quadro claro translúcido
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.16f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ir.left - 6, ir.top - 6, ir.right + 6, ir.bottom + 6), 6, 6), br);
            }
            if (!f.bmp && !f.px.empty()) { f.bmp = ui::Bitmap(dc, f.w, f.h, f.px); f.px.clear(); f.px.shrink_to_fit(); }
            if (f.bmp) {
                D2D1_SIZE_F sz = f.bmp->GetSize();
                float k = std::min(ICON / sz.width, ICON / sz.height);
                float w = sz.width * k, hh = sz.height * k, iy = (ir.top + ir.bottom) / 2;
                dc->DrawBitmap(f.bmp, D2D1::RectF(cx - w / 2, iy - hh / 2, cx + w / 2, iy + hh / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            } else ui::DrawFolder(dc, ir, 0);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
            dc->DrawTextW(f.name.c_str(), (UINT32)f.name.size(), g_tfName, D2D1::RectF(c.left + 4, ir.bottom + 4, c.right - 4, c.bottom - 2), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    D2D1_RECT_F b = BtnRect();  // "Abrir no Finder ›" numa pílula escura com fio claro
    std::wstring btn = std::wstring(Tr(L"Abrir no Finder")) + L"  ›";
    float tw = ui::TextWidth(g_tfBtn, btn) + 24, bc = (b.left + b.right) / 2;
    D2D1_RECT_F pill = D2D1::RectF(bc - tw / 2, (b.top + b.bottom) / 2 - 10, bc + tw / 2, (b.top + b.bottom) / 2 + 10);
    br->SetColor(g_hover == -1 ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(1, 1, 1, 0.10f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(pill, 10, 10), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(pill, 10, 10), br, 1);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
    dc->DrawTextW(btn.c_str(), (UINT32)btn.size(), g_tfBtn, pill, br);
    br->Release();
    ui::GpuEnd(g_s);
    Motion();
}

void Paint() {
    if (!g_open) return;
    if (g_fan) { PaintFan(); return; }
    if (liquid::Leopard()) { PaintLeopard(); return; }
    float sc = Sc();
    int wpx = (int)std::lround(PW() * sc), hpx = (int)std::lround(PH() * sc);
    bool dark = ui::Dark();
    // o vidro primeiro, já no tamanho certo (antes aparecia uma caixa vazia fora do lugar no começo)
    int bodyPx = (int)std::lround((PH() - ARROW) * sc);
    if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)bodyPx), R * sc } },
                                     dark ? D2D1::ColorF(0.13f, 0.13f, 0.14f, 0.55f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.50f), 24.f);
    ID2D1DeviceContext* dc = g_glass ? backdrop::ContentBegin(g_back, wpx, hpx, sc) : nullptr;
    g_inGlass = dc != nullptr;
    if (!dc) dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
    if (!br) { if (g_inGlass) backdrop::ContentEnd(g_back); else ui::GpuEnd(g_s); return; }
    const float BODY = PH() - ARROW;
    if (!g_glass) { br->SetColor(dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.92f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, 0.92f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, PW(), BODY), R, R), br); }
    if (liquid::On()) liquid::Rim(dc, D2D1::RectF(0, 0, PW(), BODY), R, dark);
    else {  // contorno fino, como os painéis do Sequoia
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.12f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, PW() - 0.5f, BODY - 0.5f), R, R), br, 1);
    }
    if (!g_side) {   // setinha apontando para o ícone no Dock (na cor do vidro); com o Dock do lado não há
        ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
        ID2D1PathGeometry* tri = nullptr; if (fac) fac->CreatePathGeometry(&tri);
        if (tri) {
            ID2D1GeometrySink* sk = nullptr; tri->Open(&sk);
            float ax = std::clamp(g_arrowX, R + ARROW, PW() - R - ARROW);
            sk->BeginFigure({ ax - ARROW, BODY - 0.5f }, D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine({ ax, BODY + ARROW - 1 }); sk->AddLine({ ax + ARROW, BODY - 0.5f });
            sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
            br->SetColor(dark ? D2D1::ColorF(0.20f, 0.20f, 0.21f, 0.92f) : D2D1::ColorF(0.90f, 0.90f, 0.91f, 0.95f));
            dc->FillGeometry(tri, br);
            tri->Release();
        }
        if (fac) fac->Release();
    }
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.85f);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.5f));  // título pequeno, cinza, no meio
    dc->DrawTextW(L"Downloads", 9, g_tfTitle, D2D1::RectF(0, 7, PW(), HEAD), br);
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (g_files.empty()) {
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.5f) : D2D1::ColorF(0, 0, 0, 0.45f));
            dc->DrawTextW(Tr(L"Nenhum item"), (UINT32)wcslen(Tr(L"Nenhum item")), g_tfName, D2D1::RectF(0, HEAD + 30, PW(), HEAD + 60), br);
        }
        for (int i = 0; i < (int)g_files.size(); i++) {
            auto& f = g_files[i];
            D2D1_RECT_F c = CellRect(i);
            if (i == g_hover) {
                br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.08f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + 4, c.top + 2, c.right - 4, c.bottom - 2), 10, 10), br);
            }
            float cx = (c.left + c.right) / 2;
            D2D1_RECT_F ir{ cx - ICON / 2, c.top + 8, cx + ICON / 2, c.top + 8 + ICON };
            if (!f.bmp && !f.px.empty()) { f.bmp = ui::Bitmap(dc, f.w, f.h, f.px); f.px.clear(); f.px.shrink_to_fit(); }
            if (f.bmp) {  // miniatura no quadrado, sem esticar
                D2D1_SIZE_F sz = f.bmp->GetSize();
                float k = std::min(ICON / sz.width, ICON / sz.height);
                float w = sz.width * k, hh = sz.height * k, iy = (ir.top + ir.bottom) / 2;
                dc->DrawBitmap(f.bmp, D2D1::RectF(cx - w / 2, iy - hh / 2, cx + w / 2, iy + hh / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            } else ui::DrawFolder(dc, ir, 0);
            br->SetColor(text);
            dc->DrawTextW(f.name.c_str(), (UINT32)f.name.size(), g_tfName, D2D1::RectF(c.left + 4, ir.bottom + 4, c.right - 4, c.bottom - 2), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    D2D1_RECT_F b = BtnRect();  // rodapé: "Abrir no Finder", um link discreto como no Mac (azul com o mouse em cima)
    br->SetColor(g_hover == -1 ? D2D1::ColorF(0.25f, 0.56f, 1.f, 1.f) : (dark ? D2D1::ColorF(1, 1, 1, 0.62f) : D2D1::ColorF(0, 0, 0, 0.55f)));
    std::wstring btn = std::wstring(Tr(L"Abrir no Finder")) + L"  \u203A";
    dc->DrawTextW(btn.c_str(), (UINT32)btn.size(), g_tfBtn, b, br);
    br->Release();
    if (g_inGlass) backdrop::ContentEnd(g_back); else ui::GpuEnd(g_s);
    Motion();
}

// Leque: cada item sobe do ícone no Dock com a sua mola, um pouco depois do de baixo (cascata); fechando,
// voltam todos juntos e somem
float FanProgress(int i) {
    if (g_closing) { float u = std::min(1.f, (float)((anim::Now() - g_closeAt) / 160.0)); return 1 - u * u; }
    float t = (float)((anim::Now() - g_openAt) / 1000.0) - i * 0.022f;
    return t <= 0 ? 0.f : anim::Spring(t, 0.34f, 0.82f);
}
bool FanDone() {
    if (g_closing) return anim::Now() - g_closeAt > 170;
    return anim::Now() - g_openAt > 380 + g_files.size() * 22;
}
D2D1_RECT_F FanLabel(ID2D1RenderTarget*, const std::wstring& t, D2D1_POINT_2F c) {
    float tw = std::min(260.f, ui::TextWidth(g_tfBtn, t) + 22);
    return { c.x - FAN_ICON / 2 - 10 - tw, c.y - 11, c.x - FAN_ICON / 2 - 10, c.y + 11 };
}

void PaintFan() {
    float sc = Sc();
    int wpx = (int)std::lround(FAN_W * sc), hpx = (int)std::lround(FanH() * sc);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
    if (!br) { ui::GpuEnd(g_s); return; }
    bool dark = ui::Dark();
    D2D1_POINT_2F from{ g_arrowX, FanH() + 10 };  // o ícone no Dock, logo abaixo
    std::lock_guard<std::mutex> l(g_mx);
    int n = (int)g_files.size();
    for (int i = n; i >= 0; i--) {  // de cima para baixo: o de baixo fica por cima (como no Mac)
        float pr = FanProgress(i);
        if (pr <= 0.001f) continue;
        D2D1_POINT_2F to = FanPos(i);
        D2D1_POINT_2F c{ from.x + (to.x - from.x) * pr, from.y + (to.y - from.y) * pr };
        float k = 0.35f + 0.65f * std::min(1.f, pr), a = std::clamp(pr * 1.4f, 0.f, 1.f);
        D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(k, k, c);
        dc->SetTransform(m);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
        D2D1_RECT_F ir{ c.x - FAN_ICON / 2, c.y - FAN_ICON / 2, c.x + FAN_ICON / 2, c.y + FAN_ICON / 2 };
        std::wstring name;
        bool hot = i == g_hover || (i == n && g_hover == -1);
        if (i == n) {  // topo: "Abrir no Finder" (círculo com a seta)
            name = Tr(L"Abrir no Finder");
            br->SetColor(dark ? D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.95f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.98f));
            dc->FillEllipse(D2D1::Ellipse(c, 17, 17), br);
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(0, 0, 0, 0.75f));
            dc->DrawLine({ c.x - 5, c.y + 5 }, { c.x + 5, c.y - 5 }, br, 2);
            dc->DrawLine({ c.x - 2, c.y - 5 }, { c.x + 5, c.y - 5 }, br, 2);
            dc->DrawLine({ c.x + 5, c.y - 5 }, { c.x + 5, c.y + 2 }, br, 2);
        } else {
            auto& f = g_files[i];
            name = f.name;
            if (!f.bmp && !f.px.empty()) { f.bmp = ui::Bitmap(dc, f.w, f.h, f.px); f.px.clear(); f.px.shrink_to_fit(); }
            if (f.bmp) {
                D2D1_SIZE_F sz = f.bmp->GetSize();
                float kk = std::min(FAN_ICON / sz.width, FAN_ICON / sz.height);
                float w = sz.width * kk, hh = sz.height * kk;
                dc->DrawBitmap(f.bmp, D2D1::RectF(c.x - w / 2, c.y - hh / 2, c.x + w / 2, c.y + hh / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            } else ui::DrawFolder(dc, ir, 0);
        }
        // etiqueta com o nome, à esquerda (escura e arredondada, como no Mac; azul com o mouse em cima)
        D2D1_RECT_F lr = FanLabel(dc, name, c);
        br->SetColor(hot ? D2D1::ColorF(0.04f, 0.48f, 1.f, 0.95f) : D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.82f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(lr, 11, 11), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        dc->DrawTextW(name.c_str(), (UINT32)name.size(), g_tfBtn, D2D1::RectF(lr.left + 10, lr.top, lr.right - 8, lr.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->PopLayer();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    br->Release();
    ui::GpuEnd(g_s);
}

int HitAt(LPARAM l) {
    float sc = Sc();
    float x = (short)LOWORD(l) / sc, y = (short)HIWORD(l) / sc;
    if (g_fan) {  // ícone ou etiqueta de cada item; o de cima é o "Abrir no Finder"
        std::lock_guard<std::mutex> lk(g_mx);
        int n = (int)g_files.size();
        for (int i = 0; i <= n; i++) {
            D2D1_POINT_2F c = FanPos(i);
            D2D1_RECT_F lr = FanLabel(nullptr, i == n ? std::wstring(Tr(L"Abrir no Finder")) : g_files[i].name, c);
            if (y >= c.y - FAN_ITEM / 2 && y < c.y + FAN_ITEM / 2 && x >= lr.left && x < c.x + FAN_ICON / 2) return i == n ? -1 : i;
        }
        return -2;
    }
    D2D1_RECT_F b = BtnRect();
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) return -1;
    std::lock_guard<std::mutex> lk(g_mx);
    for (int i = 0; i < (int)g_files.size(); i++) { D2D1_RECT_F c = CellRect(i); if (x >= c.left && x < c.right && y >= c.top && y < c.bottom) return i; }
    return -2;
}

void OpenDownloadsInFinder() {
    std::wstring finder = shell::dir + L"\\" EXE_FINDER;
    ShellExecuteW(nullptr, L"open", finder.c_str(), g_apps ? L"shell:Programs" : L"shell:Downloads", nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_PL_ICON: if (w == (WPARAM)g_gen) Paint(); return 0;
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR: {
        LRESULT r = 0;
        if (ctxmenu::ForwardOwnerMsg(m, w, l, &r)) return r;
        break;
    }
    case WM_PL_FRAME:
        Motion();  // Grade: só a mola (o desenho já está pronto); Leque: cada item na sua mola
        if (g_fan) { if (FanDone()) { if (g_closing) HideNow(); else ui::FrameClock(h, WM_PL_FRAME, false); } return 0; }
        if (g_closing) { if (anim::PanelOut(anim::Now() - g_closeAt).done) HideNow(); }
        else if (anim::PanelIn(anim::Now() - g_openAt).done) ui::FrameClock(h, WM_PL_FRAME, false);
        return 0;
    case WM_MOUSEMOVE: {
        int hv = HitAt(l);
        if (hv != g_hover) { g_hover = hv; Paint(); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover != -2) { g_hover = -2; Paint(); } return 0;
    case WM_LBUTTONUP: {
        int hv = HitAt(l);
        if (hv == -1) { Hide(); OpenDownloadsInFinder(); return 0; }
        if (hv == -2) { Hide(true); return 0; }  // clique no vazio do painel: fecha e volta para a janela de antes
        if (hv >= 0) {
            std::wstring p;
            { std::lock_guard<std::mutex> lk(g_mx); if (hv < (int)g_files.size()) p = g_files[hv].path; }
            Hide();
            if (!p.empty() && !AbrirNoAppMac(p)) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Hide(true);
        if (w == VK_SPACE && g_hover >= 0) {  // Quick Look do arquivo sob o mouse, como no Mac
            std::wstring p;
            { std::lock_guard<std::mutex> l(g_mx); if (g_hover < (int)g_files.size()) p = g_files[g_hover].path; }
            Hide();
            quicklook::Toggle(p);
        }
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) Hide(); return 0;
    case WM_CLOSE: Hide(true); return 0;  // nunca destruir: a janela é reaproveitada (destruída, os cliques no Dock paravam)
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool Ready() {
    if (g_wnd && IsWindow(g_wnd)) return true;
    if (g_wnd) { if (g_back && IsWindow(g_back)) DestroyWindow(g_back); g_wnd = g_back = nullptr; g_open = g_closing = false; }  // alguém destruiu: refaz
    HINSTANCE hi = shell::inst;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellPilha"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellPilhaFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Downloads", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) { Log(L"pilha: janela não criada (%lu)", GetLastError()); return false; }
    g_glass = backdrop::Attach(g_back);
    if (!g_glass) {
        DestroyWindow(g_back);
        g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    }
    DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
    COLORREF none = 0xFFFFFFFE; DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    apps::ours.push_back(g_wnd); apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    g_tfTitle = ui::Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfName = ui::Format(10, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfBtn = ui::Format(11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (g_tfBtn) g_tfBtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_tfName) { g_tfName->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; g_tfName->SetTrimming(&t, nullptr); }
    return true;
}

std::wstring DownloadsDir() {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) d = p;
    CoTaskMemFree(p);
    return d;
}

// Leopard, pilha Aplicativos: os atalhos dos apps do Menu Iniciar (do usuário e de todos), sem desinstaladores e
// manuais, em ordem alfabética
std::vector<std::wstring> ListApps(int max) {
    std::vector<std::pair<std::wstring, std::wstring>> v;  // nome, caminho
    for (auto* id : { &FOLDERID_Programs, &FOLDERID_CommonPrograms }) {
        PWSTR p = nullptr;
        if (FAILED(SHGetKnownFolderPath(*id, 0, nullptr, &p))) continue;
        std::wstring root = p; CoTaskMemFree(p);
        std::vector<std::wstring> dirs{ root };
        for (size_t k = 0; k < dirs.size() && k < 200; k++) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dirs[k] + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.cFileName[0] == L'.') continue;
                std::wstring full = dirs[k] + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (k == 0) dirs.push_back(full); continue; }  // um nível de pastas
                const wchar_t* x = wcsrchr(fd.cFileName, L'.');
                if (!x || _wcsicmp(x, L".lnk")) continue;
                std::wstring name(fd.cFileName, x - fd.cFileName), low = name;
                for (auto& c : low) c = towlower(c);
                bool skip = false;
                for (const wchar_t* w : { L"uninstall", L"desinstal", L"readme", L"leia-me", L"help", L"ajuda", L"manual", L"website", L"license" })
                    if (low.find(w) != std::wstring::npos) skip = true;
                if (skip) continue;
                bool dup = false;
                for (auto& e : v) if (!_wcsicmp(e.first.c_str(), name.c_str())) dup = true;
                if (!dup) v.push_back({ name, full });
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::vector<std::wstring> out;
    for (size_t i = 0; i < v.size() && (int)i < max; i++) out.push_back(v[i].second);
    return out;
}

// os arquivos de Downloads na ordem do botão direito (Data de Adição: o mais novo primeiro, como no Mac)
std::vector<std::wstring> List(int max) {
    if (g_apps) return ListApps(max);
    std::wstring d = DownloadsDir(), order = shell::Setting(L"pilha_ordem", L"adicao");
    struct E { std::wstring path, name, ext; ULONGLONG made, mod; };
    std::vector<E> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = d.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((d + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            E e; e.name = fd.cFileName; e.path = d + L"\\" + fd.cFileName;
            const wchar_t* x = wcsrchr(fd.cFileName, L'.'); e.ext = x && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? x : L"";
            for (auto& c : e.ext) c = towlower(c);
            e.made = ((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime;
            e.mod = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            v.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto ci = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
    if (order == L"nome") std::sort(v.begin(), v.end(), [&](const E& a, const E& b) { return ci(a.name, b.name); });
    else if (order == L"modificacao") std::sort(v.begin(), v.end(), [](const E& a, const E& b) { return a.mod > b.mod; });
    else if (order == L"tipo") std::sort(v.begin(), v.end(), [&](const E& a, const E& b) { return a.ext != b.ext ? a.ext < b.ext : ci(a.name, b.name); });
    else std::sort(v.begin(), v.end(), [](const E& a, const E& b) { return a.made > b.made; });  // adição / criação
    std::vector<std::wstring> out;
    for (size_t i = 0; i < v.size() && (int)i < max; i++) out.push_back(v[i].path);
    return out;
}

// Onde abrir um menu que sai do ícone no Dock (anchor: o lado de dentro do ícone): embaixo, para cima dele; com o
// Dock à esquerda ou à direita, ao lado, centrado na altura do ícone (o menu vira para a esquerda sozinho se não couber)
POINT MenuPt(POINT anchor, int hpx) {
    if (dock::Posicao() == 0) return { anchor.x - (LONG)std::lround(60 * Sc()), anchor.y - hpx };
    return { anchor.x, anchor.y - hpx / 2 };
}

// Lista: um menu com os nomes, como antes (e como o "Lista" do Mac)
void ShowList(POINT anchor) {
    auto files = List(14);
    std::vector<ctxmenu::Item> m;
    for (int k = 0; k < (int)files.size(); k++) { ctxmenu::Item i; i.label = files[k].substr(files[k].find_last_of(L'\\') + 1); i.id = 5000 + k; m.push_back(i); }
    if (m.empty()) { ctxmenu::Item i; i.label = Tr(L"Nenhum item"); i.disabled = true; m.push_back(i); }
    ctxmenu::Item sep; sep.separator = true; m.push_back(sep);
    ctxmenu::Item op; op.label = Tr(L"Abrir no Finder"); op.id = 30; m.push_back(op);
    ctxmenu::PlainNext();
    SetForegroundWindow(g_wnd);
    POINT pt = MenuPt(anchor, (int)std::lround((m.size() * 22 + 14) * Sc()));
    int id = ctxmenu::ShowItems(g_wnd, pt, m);
    if (id == 30) OpenDownloadsInFinder();
    else if (id >= 5000 && id - 5000 < (int)files.size() && !AbrirNoAppMac(files[id - 5000])) ShellExecuteW(nullptr, L"open", files[id - 5000].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

// Botão direito em Downloads (como no Mac): ordenar, como ver o conteúdo e abrir a pasta
void pilhas::Menu(POINT anchor) {
    if (!Ready()) return;
    auto MI = [](const std::wstring& l, int id, bool on = false) { ctxmenu::Item i; i.label = l; i.id = id; i.checked = on; return i; };
    std::wstring ord = shell::Setting(L"pilha_ordem", L"adicao"), ver = shell::Setting(L"pilha_ver", L"auto");
    ctxmenu::Item sortBy = MI(Tr(L"Ordenar por"), 0);
    const wchar_t* ordK[] = { L"nome", L"adicao", L"modificacao", L"criacao", L"tipo" };
    const wchar_t* ordN[] = { L"Nome", L"Data de Adição", L"Data de Modificação", L"Data de Criação", L"Tipo" };
    for (int i = 0; i < 5; i++) sortBy.sub.push_back(MI(Tr(ordN[i]), 100 + i, ord == ordK[i]));
    ctxmenu::Item view = MI(Tr(L"Ver conteúdo como"), 0);
    const wchar_t* verK[] = { L"leque", L"grade", L"lista", L"auto" };
    const wchar_t* verN[] = { L"Leque", L"Grade", L"Lista", L"Automático" };
    for (int i = 0; i < 4; i++) view.sub.push_back(MI(Tr(verN[i]), 200 + i, ver == verK[i]));
    // Exibir como: Pasta (o \u00edcone da pasta) ou Pilha (os arquivos mais novos empilhados, o padr\u00e3o do Mac)
    std::wstring ex = shell::Setting(L"pilha_exibir", L"pilha");
    ctxmenu::Item show = MI(Tr(L"Exibir como"), 0);
    show.sub = { MI(Tr(L"Pasta"), 400, ex == L"pasta"), MI(Tr(L"Pilha"), 401, ex != L"pasta") };
    ctxmenu::Item sep; sep.separator = true;
    std::vector<ctxmenu::Item> m = { sortBy, show, view, sep, MI(Tr(L"Abrir \u201cDownloads\u201d"), 300) };
    ctxmenu::PlainNext();
    SetForegroundWindow(g_wnd);
    POINT pt = MenuPt(anchor, (int)std::lround(120 * Sc()));
    int id = ctxmenu::ShowItems(g_wnd, pt, m);
    if (id >= 100 && id < 105) shell::SetSetting(L"pilha_ordem", ordK[id - 100]);
    else if (id >= 200 && id < 204) shell::SetSetting(L"pilha_ver", verK[id - 200]);
    else if (id == 300) OpenDownloadsInFinder();
    else if (id == 400 || id == 401) shell::SetSetting(L"pilha_exibir", id == 400 ? L"pasta" : L"pilha");
}

std::vector<std::wstring> pilhas::Recentes(int max) { bool a = g_apps; g_apps = false; auto v = List(max); g_apps = a; return v; }
void ShowStack(POINT anchor, bool apps);
void pilhas::Show(POINT anchor) { ShowStack(anchor, false); }
void pilhas::ShowApps(POINT anchor) { ShowStack(anchor, true); }

bool pilhas::IsOpen() { return g_open && !g_closing; }

void ShowStack(POINT anchor, bool apps) {
    Log(L"pilha: abrir %s (aberta=%d fechando=%d janela=%d)", apps ? L"Aplicativos" : L"Downloads", (int)g_open, (int)g_closing, g_wnd && IsWindow(g_wnd) ? 1 : 0);
    if (g_open && !g_closing) { bool same = g_apps == apps; Hide(true); if (same) return; HideNow(); }
    if (g_closing) HideNow();
    if (!Ready()) return;
    g_apps = apps;
    MAXN = apps ? 42 : 18;
    std::wstring ver = apps ? std::wstring(L"grade") : shell::Setting(L"pilha_ver", L"auto");  // Aplicativos: sempre a Grade
    if (ver == L"lista") { ShowList(anchor); return; }
    g_fan = ver != L"grade";  // Automático: o Leque, como o Mac faz com Downloads
    if (dock::Posicao() != 0) g_fan = false;  // Dock do lado: o Leque só existe embaixo (no Mac também vira Grade)
    std::vector<std::wstring> files = List(g_fan ? FAN_MAX : MAXN);
    {   // quem estava na frente (não o Shell): volta para ela ao fechar sem abrir nada
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        g_prevFg = fg && pid != GetCurrentProcessId() ? fg : nullptr;
    }
    if (!Ready()) return;
    Free();
    int gen = ++g_gen;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (size_t i = 0; i < files.size() && (int)i < MAXN; i++) {
            File f; f.path = files[i];
            f.name = files[i].substr(files[i].find_last_of(L'\\') + 1);
            if (g_apps && f.name.size() > 4 && !_wcsicmp(f.name.c_str() + f.name.size() - 4, L".lnk")) f.name.resize(f.name.size() - 4);  // Aplicativos: sem o ".lnk"
            g_files.push_back(f);
        }
    }
    // miniaturas numa thread (as que já vierem aparecem enquanto o painel abre)
    float sc = Sc();
    int px = (int)std::lround(ICON * sc);
    std::vector<std::wstring> paths = files;
    const bool appsIcons = g_apps;
    std::thread([paths, px, gen, appsIcons] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (size_t i = 0; i < paths.size() && (int)i < MAXN; i++) {
            UINT w = 0, h = 0; std::vector<BYTE> bgra;
            bool got = false;
            if (appsIcons) {  // Aplicativos: o ícone do tema (Leopard/aquificado) quando o pacote conhece o app
                std::wstring nm = paths[i].substr(paths[i].find_last_of(L'\\') + 1);
                if (nm.size() > 4) nm.resize(nm.size() - 4);
                std::wstring png = apps::PackIconByName(nm, L"");
                got = !png.empty() && ui::LoadPng(png, w, h, bgra);
            }
            if (!got && !appsIcons && liquid::Leopard()) {  // Leopard: pasta azul / documento do Aqua (fotos e vídeos: miniatura)
                DWORD at = GetFileAttributesW(paths[i].c_str());
                std::wstring nm = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) ? std::wstring(L"folder default") : leopard::DocIcon(paths[i]);
                std::wstring png = nm.empty() ? L"" : ui::PackIcon(nm);
                got = !png.empty() && ui::LoadPng(png, w, h, bgra);
            }
            if (!got && !ui::FileImage(paths[i], px, w, h, bgra)) continue;
            {
                std::lock_guard<std::mutex> l(g_mx);
                if (gen != g_gen || i >= g_files.size()) break;
                g_files[i].px = std::move(bgra); g_files[i].w = w; g_files[i].h = h;
            }
            PostMessageW(g_wnd, WM_PL_ICON, gen, 0);
        }
        CoUninitialize();
    }).detach();
    g_anchor = anchor;
    int wpx = (int)std::lround((g_fan ? FAN_W : PW()) * sc), hpx = (int)std::lround((g_fan ? FanH() : PH()) * sc);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x, y;
    g_side = 0;
    if (g_fan) {  // o arco sai do ícone: o ícone fica embaixo, perto da borda direita da janela
        x = anchor.x - (int)std::lround((FAN_W - 70) * sc);
        y = anchor.y - hpx;
        g_arrowX = FAN_W - 70;
    } else if (dock::Posicao() != 0) {
        // Dock à esquerda ou à direita: a Grade abre ao lado do ícone, na altura dele (sem a setinha de baixo)
        int pos = dock::Posicao();
        x = pos == 1 ? anchor.x + (int)std::lround(4 * sc) : anchor.x - wpx - (int)std::lround(4 * sc);
        y = std::clamp((int)anchor.y - hpx / 2, (int)mi.rcWork.top + 8, std::max((int)mi.rcWork.top + 8, (int)mi.rcWork.bottom - hpx - 8));
        g_arrowX = 0; g_side = pos;
    } else {
        x = std::clamp((int)anchor.x - wpx / 2, (int)mi.rcWork.left + 8, (int)mi.rcWork.right - wpx - 8);
        y = anchor.y - hpx - (int)std::lround(2 * sc);
        g_arrowX = (anchor.x - x) / sc;
    }
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    SetWindowPos(g_back, g_wnd, x, y, wpx, hpx, SWP_NOACTIVATE);
    g_open = true; g_hover = -2;
    g_openAt = anim::Now();
    if (g_glass) backdrop::Fade(g_back, 0, 0.96f, GrowX(wpx, sc), GrowY(hpx));  // a última abertura deixou em 100%: piscava
    ui::GpuMotion(g_s, g_fan ? 1.f : 0.96f, GrowX(wpx, sc), GrowY(hpx), g_fan ? 1.f : 0.f);  // o Leque se anima item a item
    Paint();  // vidro e conteúdo prontos (e invisíveis: a mola começa em 0) antes de mostrar
    if (!g_fan) ShowWindow(g_back, SW_SHOWNOACTIVATE);
    else ShowWindow(g_back, SW_HIDE);
    ShowWindow(g_wnd, SW_SHOW);
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    if (att) AttachThreadInput(me, ft, FALSE);
    ui::FrameClock(g_wnd, WM_PL_FRAME, true);
}
