// "Sobre Este Mac" (menu da Apple): a janela do Sequoia, mostrando um Mac Studio M4 Max.
// Medidas tiradas da referência (foto da janela no Sequoia 15.3, 1,725 px por pt): 280 × 474 pt,
// só o botão de fechar ativo, desenho do aparelho, nome em 24 pt negrito, ano, ficha em duas colunas
// (rótulos alinhados à direita), botão "Mais Informações…" e o rodapé de direitos.
// Vidro desfocado atrás (backdrop.cpp), como as outras janelas do Shell.
#include "shell.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include "../comum/idioma.h"
#include "../comum/semaforo.h"
#include "../comum/leopard.h"   // tema Mac OS X Leopard: o metal da barra
#include "../comum/simbolos.h"  // a maçã do Leopard (apple.logo em prata)

namespace {

const float W = 280;
// macOS 27 Golden Gate (About-This-Mac.png, 2x): 280 × 487, nome a 245,5 sem a linha do ano, dados a partir de 285,5
// a cada 16, "Mais Informações…" de 24 em 376,5 e o rodapé em 422 / 437 / 451
#define GG (liquid::Golden())
// Mac OS X Leopard (About This Mac do 10.5): 280 × 384, janela cinza 232 com a barra de 22, maçã prateada, "Mac OS X",
// a versão, "Atualização de Software…", Processador / Memória / Disco de Inicialização, "Mais Informações…" e o rodapé
#define LEO (liquid::Leopard())
#define H (LEO ? 384.f : GG ? 487.f : 474.f)
const float LIGHT_PITCH = 23, LIGHT_R = 7;  // padrão do projeto (o do Monitor de Atividade)
// Tahoe (guia macOS26-Theme-Reference, seção 9.1): janela só com título = cantos de 16 e o 1º botão em x 9, y 9
// (centro 16, 16); Sequoia: cantos de 10, centro em 19, 14
#define RADIUS (liquid::On() ? 16.f : LEO ? 6.f : 10.f)
#define LIGHT_X0 (liquid::On() ? 16.f : LEO ? 14.5f : 19.f)
#define LIGHT_Y (liquid::On() ? 16.f : LEO ? 11.f : 14.f)
const float IMG_TOP = 90, IMG_W = 145.5f, IMG_H = 70.7f;
const float YEAR_CY = 262;
#define TITLE_CY (GG ? 245.5f : 240.6f)
#define ROW0_CY (GG ? 285.5f : 295.7f)
#define ROW_PITCH (GG ? 16.f : 15.2f)
const float COL_SPLIT = 119, COL_GAP = 10;
#define BTN_TOP (GG ? 376.5f : 370.4f)
#define BTN_H (GG ? 24.f : 20.f)
#define REG_CY (GG ? 422.f : 411.f)
#define TM_CY (GG ? 437.f : 425.f)
#define RIGHTS_CY (GG ? 451.f : 439.4f)

HWND g_wnd, g_back;
bool g_glass, g_active;
ui::GpuSurface g_s;
int g_hover = -1, g_press = -1;  // 0 = fechar, 1 = Mais Informações, 2 = certificação
D2D1_RECT_F g_btn{}, g_reg{};
std::wstring g_serial;
ID2D1Bitmap* g_photo;            // icones\sequoia\macstudio.png: o aparelho recortado da janela do Sequoia
UINT g_pw, g_ph; std::vector<BYTE> g_px; bool g_triedPhoto;

float Sc() { return ui::Scale(g_wnd); }

// dados reais da máquina para o Sobre do Leopard (o 10.5 mostrava o processador, a memória e o disco)
std::wstring Processador() {
    wchar_t v[128] = {}; DWORD cb = sizeof(v);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, v, &cb);
    std::wstring s = v;
    while (!s.empty() && s.back() == L' ') s.pop_back();
    while (!s.empty() && s[0] == L' ') s.erase(0, 1);
    for (const wchar_t* tira : { L"(R)", L"(TM)", L" CPU", L" Processor" }) for (size_t p; (p = s.find(tira)) != std::wstring::npos;) s.erase(p, wcslen(tira));
    if (size_t p = s.find(L" @ "); p != std::wstring::npos) s = s.substr(p + 3) + L" " + s.substr(0, p);  // "3,6 GHz Intel Core i7" como no Mac
    return s.empty() ? L"Intel Core 2 Duo" : s;
}
std::wstring Memoria() {
    MEMORYSTATUSEX m{ sizeof(m) };
    GlobalMemoryStatusEx(&m);
    wchar_t b[32]; swprintf_s(b, L"%llu GB", (m.ullTotalPhys + (1ull << 29)) >> 30);
    return b;
}

// botão em pílula do Aqua (perolado; com o mouse apertado, azul)
void AquaPill(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, bool down) {
    float rad = (r.bottom - r.top) / 2;
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1), rad, rad), br);
    D2D1_GRADIENT_STOP st[4] = { { 0, down ? D2D1::ColorF(0.66f, 0.80f, 0.98f, 1) : D2D1::ColorF(1, 1, 1, 1) },
                                 { 0.5f, down ? D2D1::ColorF(0.38f, 0.60f, 0.93f, 1) : D2D1::ColorF(0.95f, 0.95f, 0.95f, 1) },
                                 { 0.5f, down ? D2D1::ColorF(0.24f, 0.47f, 0.87f, 1) : D2D1::ColorF(0.88f, 0.88f, 0.88f, 1) },
                                 { 1, down ? D2D1::ColorF(0.58f, 0.82f, 1.0f, 1) : D2D1::ColorF(0.98f, 0.98f, 0.98f, 1) } };
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st, 4, &gs)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    if (lb) lb->Release();
    if (gs) gs->Release();
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.40f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f), br, 1);
}

// número de série estável para esta máquina (10 caracteres, como os do Mac)
std::wstring Serial() {
    wchar_t guid[64] = {}; DWORD n = sizeof(guid);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, guid, &n);
    unsigned long long hsh = 1469598103934665603ull;
    for (wchar_t* p = guid; *p; p++) { hsh ^= (unsigned)*p; hsh *= 1099511628211ull; }
    const wchar_t* abc = L"0123456789ACDEFGHJKLMNPQRTUVWXYZ";
    std::wstring s;
    for (int i = 0; i < 10; i++) { s += abc[hsh % 32]; hsh /= 32; if (!hsh) hsh = 0x9E3779B97F4A7C15ull; }
    return s;
}

void Text(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, float size, DWRITE_FONT_WEIGHT wgt,
          DWRITE_TEXT_ALIGNMENT al, D2D1_RECT_F r, D2D1_COLOR_F c, bool underline = false, bool display = false) {
    IDWriteTextFormat* tf = nullptr;
    if (display) {  // nome grande: SF Pro Display (mais justa que a Text nos tamanhos grandes)
        if (SUCCEEDED(ui::dw->CreateTextFormat(L"SF Pro Display", nullptr, wgt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf)))
            tf->SetTextAlignment(al);
    } else tf = ui::Format(size, wgt, false, al);
    if (!tf) return;
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteTextLayout* lay = nullptr;
    if (SUCCEEDED(ui::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, r.right - r.left, r.bottom - r.top, &lay))) {
        if (underline) lay->SetUnderline(TRUE, { 0, (UINT32)s.size() });
        br->SetColor(c);
        dc->DrawTextLayout({ r.left, r.top }, lay, br);
        lay->Release();
    }
    tf->Release();
}

// Mac Studio de frente: caixa de alumínio com cantos arredondados, base escura recuada,
// duas USB-C e a entrada de cartão SD à esquerda, luz de ligado à direita
void DrawMacStudio(ID2D1DeviceContext* dc, float x, float y) {
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    auto grad = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        ID2D1GradientStopCollection* sc = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        dc->CreateGradientStopCollection(stops.begin(), (UINT32)stops.size(), &sc);
        if (sc) dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), sc, &lb);
        if (sc) sc->Release();
        return lb;
    };
    const float bodyH = 63.5f, baseInset = 11;
    // sombra suave embaixo
    ID2D1SolidColorBrush* sb = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.10f), &sb);
    for (int i = 0; i < 4; i++) {
        float e = 2.f + i * 2.f;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + baseInset - e, y + IMG_H - 3, x + IMG_W - baseInset + e, y + IMG_H + 1.5f + i * 0.6f), 4, 4), sb);
    }
    // base (o pé com a grade de ventilação), mais escura e recuada
    if (auto* b = grad({ 0, y + bodyH }, { 0, y + IMG_H }, { { 0, D2D1::ColorF(0.62f, 0.62f, 0.64f) }, { 0.4f, D2D1::ColorF(0.74f, 0.74f, 0.76f) }, { 1, D2D1::ColorF(0.56f, 0.56f, 0.58f) } })) {
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* k = nullptr;
        if (f && SUCCEEDED(f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&k))) {
            k->BeginFigure({ x + baseInset, y + bodyH - 1 }, D2D1_FIGURE_BEGIN_FILLED);
            k->AddLine({ x + IMG_W - baseInset, y + bodyH - 1 });
            k->AddLine({ x + IMG_W - baseInset - 1.5f, y + IMG_H });
            k->AddLine({ x + baseInset + 1.5f, y + IMG_H });
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
            k->Close(); k->Release();
            dc->FillGeometry(g, b);
            g->Release();
        }
        b->Release();
        ID2D1SolidColorBrush* v = nullptr;  // furinhos da ventilação
        dc->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.45f), &v);
        for (float vx = x + baseInset + 3; vx < x + IMG_W - baseInset - 3; vx += 1.6f)
            dc->FillEllipse(D2D1::Ellipse({ vx, y + bodyH + 3.6f }, 0.45f, 0.45f), v);
        v->Release();
    }
    // corpo: frente de alumínio, mais clara em cima e escurecendo nas laterais
    D2D1_ROUNDED_RECT body = D2D1::RoundedRect(D2D1::RectF(x, y, x + IMG_W, y + bodyH), 4.5f, 4.5f);
    if (auto* b = grad({ 0, y }, { 0, y + bodyH }, { { 0, D2D1::ColorF(0.93f, 0.93f, 0.94f) }, { 0.08f, D2D1::ColorF(0.88f, 0.88f, 0.89f) }, { 0.85f, D2D1::ColorF(0.84f, 0.84f, 0.85f) }, { 1, D2D1::ColorF(0.74f, 0.74f, 0.76f) } })) {
        dc->FillRoundedRectangle(body, b);
        b->Release();
    }
    if (auto* b = grad({ x, 0 }, { x + IMG_W, 0 }, { { 0, D2D1::ColorF(0, 0, 0, 0.16f) }, { 0.06f, D2D1::ColorF(0, 0, 0, 0) }, { 0.94f, D2D1::ColorF(0, 0, 0, 0) }, { 1, D2D1::ColorF(0, 0, 0, 0.18f) } })) {
        dc->FillRoundedRectangle(body, b);
        b->Release();
    }
    sb->SetColor(D2D1::ColorF(1, 1, 1, 0.75f));  // brilho fino na aresta de cima
    dc->DrawLine({ x + 6, y + 0.6f }, { x + IMG_W - 6, y + 0.6f }, sb, 0.8f);
    sb->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));  // aresta de baixo do corpo
    dc->DrawLine({ x + 5, y + bodyH - 0.4f }, { x + IMG_W - 5, y + bodyH - 0.4f }, sb, 0.8f);
    // portas
    float py = y + 52.5f;
    sb->SetColor(D2D1::ColorF(0.16f, 0.16f, 0.18f));
    for (float px : { 23.2f, 33.f })
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + px - 0.85f, py - 2.8f, x + px + 0.85f, py + 2.8f), 0.85f, 0.85f), sb);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 45.2f, py - 0.85f, x + 63.8f, py + 0.85f), 0.85f, 0.85f), sb);
    sb->SetColor(D2D1::ColorF(0.98f, 0.98f, 1.0f));  // luz de ligado
    dc->FillEllipse(D2D1::Ellipse({ x + 120.6f, py }, 0.9f, 0.9f), sb);
    sb->Release();
    if (f) f->Release();
}

const UINT WM_SB_FRAME = WM_APP + 72;
double g_openAt;  // a mola comum dos painéis ao aparecer

void PaintLeopard(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br) {
    // fundo cinza 232 do 10.5 com a barra de 22 em metal (cantos de cima redondos, os de baixo quase retos)
    br->SetColor(D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), RADIUS, RADIUS), br);
    leopard::Metal(dc, D2D1::RectF(0, 0, W, 22), g_active, false, true, RADIUS);
    Text(dc, br, Tr(L"Sobre Este Mac"), 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(70, 1, W - 70, 22), D2D1::ColorF(1, 1, 1, 0.5f));
    Text(dc, br, Tr(L"Sobre Este Mac"), 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, D2D1::RectF(70, 0, W - 70, 21), D2D1::ColorF(0, 0, 0, g_active ? 0.80f : 0.45f));
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), RADIUS - 0.5f, RADIUS - 0.5f), br, 1);
    // só o fechar aceso (gel); minimizar e ampliar apagados
    for (int k = 0; k < 3; k++) {
        D2D1_POINT_2F c{ LIGHT_X0 + k * 21.f, LIGHT_Y };
        br->SetColor(k == 0 && g_active ? semaforo::Close() : D2D1::ColorF(0.75f, 0.75f, 0.75f, 1));
        dc->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
        semaforo::Gloss(dc, c, 7, br);
        if (k == 0 && g_hover == 0) {
            br->SetColor(D2D1::ColorF(0.30f, 0, 0, 0.75f));
            dc->DrawLine({ c.x - 2.6f, c.y - 2.6f }, { c.x + 2.6f, c.y + 2.6f }, br, 1.2f);
            dc->DrawLine({ c.x - 2.6f, c.y + 2.6f }, { c.x + 2.6f, c.y - 2.6f }, br, 1.2f);
        }
    }
    // a maçã prateada (degradê do branco para o cinza, com o contorno escuro fino)
    {
        D2D1_RECT_F ar = D2D1::RectF(W / 2 - 34, 40, W / 2 + 34, 118);
        D2D1_GRADIENT_STOP st[3] = { { 0, D2D1::ColorF(0.97f, 0.97f, 0.98f, 1) }, { 0.55f, D2D1::ColorF(0.80f, 0.81f, 0.83f, 1) }, { 1, D2D1::ColorF(0.58f, 0.59f, 0.62f, 1) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(dc->CreateGradientStopCollection(st, 3, &gs)) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, ar.top }, { 0, ar.bottom }), gs, &lb))) {
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.22f));  // sombra
            simbolo::Draw(dc, L"apple.logo", D2D1::RectF(ar.left, ar.top + 1.5f, ar.right, ar.bottom + 1.5f), br);
            simbolo::Draw(dc, L"apple.logo", ar, lb);
        }
        if (lb) lb->Release();
        if (gs) gs->Release();
    }
    auto C = DWRITE_TEXT_ALIGNMENT_CENTER;
    D2D1_COLOR_F ink = D2D1::ColorF(0, 0, 0, 0.88f), gray = D2D1::ColorF(0, 0, 0, 0.55f);
    Text(dc, br, L"Mac OS X", 27, DWRITE_FONT_WEIGHT_LIGHT, C, D2D1::RectF(0, 124, W, 158), D2D1::ColorF(0.25f, 0.25f, 0.26f, 1));
    Text(dc, br, Tr(L"Versão 10.5.8"), 11, DWRITE_FONT_WEIGHT_NORMAL, C, D2D1::RectF(0, 158, W, 174), gray);
    auto pill = [&](const std::wstring& s, float top, D2D1_RECT_F& out, int id) {
        IDWriteTextFormat* tb = ui::Format(11, DWRITE_FONT_WEIGHT_NORMAL);
        float tw = tb ? ui::TextWidth(tb, s) : 110;
        if (tb) tb->Release();
        float bw = std::ceil(tw + 30);
        out = D2D1::RectF((W - bw) / 2, top, (W + bw) / 2, top + 19);
        AquaPill(dc, br, out, g_press == id && g_hover == id);
        Text(dc, br, s, 11, DWRITE_FONT_WEIGHT_NORMAL, C, out, ink);
    };
    pill(Tr(L"Atualização de Software…"), 184, g_reg, 2);
    const std::wstring rows[3][2] = { { Tr(L"Processador"), Processador() }, { Tr(L"Memória"), Memoria() }, { Tr(L"Disco de Inicialização"), L"Macintosh HD" } };
    for (int i = 0; i < 3; i++) {
        float cy = 232 + i * 22.f;
        Text(dc, br, rows[i][0], 11, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, D2D1::RectF(0, cy - 8, 112, cy + 8), ink);
        Text(dc, br, rows[i][1], 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D2D1::RectF(120, cy - 8, W - 10, cy + 8), ink);
    }
    pill(Tr(L"Mais Informações…"), 300, g_btn, 1);
    Text(dc, br, Tr(L"™ e © 1983-2009 Apple Inc."), 10, DWRITE_FONT_WEIGHT_NORMAL, C, D2D1::RectF(0, 344, W, 358), gray);
    Text(dc, br, Tr(L"Todos os direitos reservados."), 10, DWRITE_FONT_WEIGHT_NORMAL, C, D2D1::RectF(0, 358, W, 372), gray);
}

void Paint() {
    if (!g_wnd || !IsWindowVisible(g_wnd)) return;
    float sc = Sc();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    bool dark = ui::Dark();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    if (LEO) {  // Leopard: janela opaca (sem o vidro), o desenho do 10.5
        PaintLeopard(dc, br);
        br->Release();
        ui::GpuEnd(g_s);
        if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), RADIUS * sc } },
                                         D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f, 1), 0.f);
        anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);
        ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade);
        if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
        return;
    }
    if (!g_glass) {
        br->SetColor(dark ? D2D1::ColorF(0.145f, 0.145f, 0.15f) : D2D1::ColorF(0.93f, 0.93f, 0.94f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), RADIUS, RADIUS), br);
    }
    // borda: linha clara por dentro (escuro) / escura (claro)
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.17f) : D2D1::ColorF(0, 0, 0, 0.14f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), RADIUS - 0.5f, RADIUS - 0.5f), br, 1);

    // bolinhas: só fechar fica ativa (minimizar e ampliar ficam desligados nesta janela)
    for (int k = 0; k < 3; k++) {
        D2D1_POINT_2F c{ LIGHT_X0 + k * LIGHT_PITCH, LIGHT_Y };
        bool on = k == 0 && (g_active || g_hover == 0);
        br->SetColor(on ? semaforo::Close() : dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.14f));
        dc->FillEllipse(D2D1::Ellipse(c, LIGHT_R, LIGHT_R), br);
        if (k == 0 && g_hover == 0) {
            br->SetColor(D2D1::ColorF(0.30f, 0, 0, 0.75f));
            float a = 2.6f;
            dc->DrawLine({ c.x - a, c.y - a }, { c.x + a, c.y + a }, br, 1.2f);
            dc->DrawLine({ c.x - a, c.y + a }, { c.x + a, c.y - a }, br, 1.2f);
        }
    }

    if (!g_triedPhoto) {
        g_triedPhoto = true;
        std::wstring png = ui::PackIcon(L"macstudio");
        if (png.empty() || !ui::LoadPng(png, g_pw, g_ph, g_px)) g_px.clear();
    }
    if (!g_photo && !g_px.empty()) g_photo = ui::Bitmap(dc, g_pw, g_ph, g_px);
    if (g_photo) {  // a imagem do próprio Sequoia, na largura medida (145,5 pt)
        float h = IMG_W * g_ph / g_pw;
        dc->DrawBitmap(g_photo, D2D1::RectF((W - IMG_W) / 2, IMG_TOP, (W + IMG_W) / 2, IMG_TOP + h), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else DrawMacStudio(dc, (W - IMG_W) / 2, IMG_TOP);  // sem o arquivo: o desenho vetorial

    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.86f);
    D2D1_COLOR_F text2 = dark ? D2D1::ColorF(1, 1, 1, 0.52f) : D2D1::ColorF(0, 0, 0, 0.50f);
    auto C = DWRITE_TEXT_ALIGNMENT_CENTER;
    Text(dc, br, L"Mac Studio", 21.5f, DWRITE_FONT_WEIGHT_BOLD, C, D2D1::RectF(0, TITLE_CY - 16, W, TITLE_CY + 16), text, false, true);
    if (!GG) Text(dc, br, L"2025", 11, DWRITE_FONT_WEIGHT_NORMAL, C, D2D1::RectF(0, YEAR_CY - 8, W, YEAR_CY + 8), text2);  // macOS 27: sem o ano

    const wchar_t* rows[4][2] = {
        { L"Chip", L"Apple M4 Max" },
        { Tr(L"Memória"), Tr(L"128 GB") },
        { Tr(L"Número de série"), g_serial.c_str() },
        { L"macOS", GG ? L"Golden Gate 27.2.1" : liquid::On() ? L"Tahoe 26.7.2" : L"Sequoia 15.3" },  // Golden Gate: a última (27.2.1, nov. 2026)  // Tahoe: a última lançada (26.7.2, out. 2026)
    };
    for (int i = 0; i < 4; i++) {
        float cy = ROW0_CY + i * ROW_PITCH;
        Text(dc, br, rows[i][0], 11, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING, D2D1::RectF(0, cy - 8, COL_SPLIT, cy + 8), text);
        Text(dc, br, rows[i][1], 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, D2D1::RectF(COL_SPLIT + COL_GAP, cy - 8, W - 8, cy + 8),
             dark ? D2D1::ColorF(1, 1, 1, 0.80f) : D2D1::ColorF(0, 0, 0, 0.75f));
    }

    // botão "Mais Informações…": cinza translúcido, largura do texto
    std::wstring more = Tr(L"Mais Informações…");
    IDWriteTextFormat* tb = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL);
    float tw = tb ? ui::TextWidth(tb, more) : 110;
    if (tb) tb->Release();
    float bw = std::ceil(tw + 24);
    g_btn = D2D1::RectF((W - bw) / 2, BTN_TOP, (W + bw) / 2, BTN_TOP + BTN_H);
    bool down = g_press == 1 && g_hover == 1;
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, down ? 0.36f : 0.24f) : D2D1::ColorF(0, 0, 0, down ? 0.16f : 0.08f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(g_btn, 6, 6), br);
    if (!dark) {
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g_btn.left + 0.5f, g_btn.top + 0.5f, g_btn.right - 0.5f, g_btn.bottom - 0.5f), 5.5f, 5.5f), br, 1);
    }
    Text(dc, br, more, 13, DWRITE_FONT_WEIGHT_NORMAL, C, g_btn, text);

    // rodapé
    std::wstring reg = Tr(L"Certificação Regulatória");
    IDWriteTextFormat* tr = ui::Format(11, DWRITE_FONT_WEIGHT_NORMAL);
    float rw = tr ? ui::TextWidth(tr, reg) : 120;
    if (tr) tr->Release();
    g_reg = D2D1::RectF((W - rw) / 2 - 2, REG_CY - 8, (W + rw) / 2 + 2, REG_CY + 8);
    Text(dc, br, reg, 11, DWRITE_FONT_WEIGHT_NORMAL, C, D2D1::RectF(0, REG_CY - 8, W, REG_CY + 8),
         g_hover == 2 ? text : text2, true);
    Text(dc, br, Tr(L"™ e © 1983-2025 Apple Inc."), 11, DWRITE_FONT_WEIGHT_NORMAL, C, D2D1::RectF(0, TM_CY - 8, W, TM_CY + 8), text2);
    Text(dc, br, Tr(L"Todos os direitos reservados."), 11, DWRITE_FONT_WEIGHT_NORMAL, C, D2D1::RectF(0, RIGHTS_CY - 8, W, RIGHTS_CY + 8), text2);

    br->Release();
    ui::GpuEnd(g_s);
    // vidro fosco de janela do Mac: bem desfocado mesmo com a "Transparência do vidro" no mínimo (antes dava
    // para ler o que estava atrás); a cor do papel de parede passa, o desenho não
    if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), RADIUS * sc } },
                                     dark ? D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.72f) : D2D1::ColorF(0.93f, 0.93f, 0.94f, 0.55f), 30.f);
    anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);  // aparece com a mola comum, em volta do centro
    ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade);
    if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
}

void Close() {
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
    g_hover = g_press = -1;
}

int Hit(D2D1_POINT_2F p) {
    float dx = p.x - LIGHT_X0, dy = p.y - LIGHT_Y;
    if (dx * dx + dy * dy <= (LIGHT_R + 2) * (LIGHT_R + 2)) return 0;
    auto in = [&](D2D1_RECT_F r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    if (in(g_btn)) return 1;
    if (in(g_reg)) return 2;
    return -1;
}

void Click(int what) {
    if (what == 0) Close();
    else if (what == 1) shell::OpenSettings(L"geral");
    else if (what == 2) shell::OpenSettings(LEO ? L"atualizacao" : L"geral");  // Leopard: "Atualização de Software…"
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_SB_FRAME:
        Paint();
        if (anim::PanelIn(anim::Now() - g_openAt).done) ui::FrameClock(h, WM_SB_FRAME, false);
        return 0;
    case WM_ACTIVATE:
        g_active = LOWORD(w) != WA_INACTIVE;
        Paint();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || (w == 'W' && GetKeyState(VK_CONTROL) < 0)) Close();
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: {
        float sc = Sc();
        D2D1_POINT_2F p{ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc };
        int hit = Hit(p);
        if (m == WM_LBUTTONDOWN) {
            if (hit < 0) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); return 0; }  // arrasta por qualquer lugar
            g_press = hit; SetCapture(h);
        } else if (m == WM_LBUTTONUP) {
            int was = g_press; g_press = -1; ReleaseCapture();
            if (was >= 0 && was == hit) { Click(hit); if (hit == 0) return 0; }
        } else {
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        }
        if (hit != g_hover || m != WM_MOUSEMOVE) { g_hover = hit; Paint(); }
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover >= 0 && g_press < 0) { g_hover = -1; Paint(); } return 0;
    case WM_MOVE:
        if (IsWindowVisible(g_back)) { RECT r; GetWindowRect(h, &r); SetWindowPos(g_back, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
        return 0;
    case WM_CLOSE: Close(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool sobre::Create(HINSTANCE hi) {
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellSobreFundo";
    RegisterClassW(&bc);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellSobre"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // o fundo de vidro é o dono da janela de cima: ela fica sempre logo acima dele (sem "sempre na frente")
    g_back = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_back) return false;
    g_glass = backdrop::Attach(g_back);
    if (!g_glass) {
        DestroyWindow(g_back);
        g_back = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        DWORD corner = 2; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
        ui::Acrylic(g_back, ui::Dark() ? 0xE0201E1Eu : 0xE0F6F6F6u);
    } else { DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner)); }
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Sobre Este Mac", WS_POPUP, 0, 0, 10, 10, g_back, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    return true;
}

void sobre::Show() {
    if (g_serial.empty()) g_serial = Serial();
    if (!IsWindowVisible(g_wnd)) {
        float sc = Sc();
        int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
        MONITORINFO mi{ sizeof(mi) };
        POINT c; GetCursorPos(&c);
        GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
        int x = (mi.rcWork.left + mi.rcWork.right - wpx) / 2;
        int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - hpx) / 4;
        SetWindowPos(g_back, HWND_TOP, x, y, wpx, hpx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(g_wnd, HWND_TOP, x, y, wpx, hpx, SWP_NOACTIVATE);
        g_openAt = anim::Now();
        Paint();
        ShowWindow(g_wnd, SW_SHOW);
        ui::FrameClock(g_wnd, WM_SB_FRAME, true);
    }
    g_hover = g_press = -1;
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    SetWindowPos(g_back, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
    if (att) AttachThreadInput(me, ft, FALSE);
    g_active = true;
    Paint();
}
