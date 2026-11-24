// Confirmações de excluir do Explorer ("Delete File", "Delete Multiple Items"...) no alerta do Mac.
// A DLL não entra no Explorer (é a área de trabalho: arriscado), então o Shell faz por fora: ao ver a
// janela de confirmação, lê a pergunta e o nome do arquivo pela UI Automation, tira a janela original
// da tela e mostra o alerta do Mac (ícone da Lixeira, título em negrito, "Você não pode desfazer esta
// ação.", Cancelar | Apagar). A resposta é dada na janela original, pelos botões dela (Invoke).
// Se a janela original some sozinha, o alerta some junto.
// Também a confirmação de substituir de qualquer programa ("Confirm Save As": "x.wav already exists. Do
// you want to replace it?"), com o ícone do programa, como a do Mac: "“x.wav” já existe. Deseja
// substituí-lo?" | Cancelar | Substituir.
// E as caixas de mensagem comuns de qualquer programa (MessageBox com um ou dois botões: OK, OK/Cancelar,
// Sim/Não, Repetir/Cancelar): reconhecidas pela forma (só o texto de id 0xFFFF, o ícone e os botões padrão),
// viram o alerta do Mac com o ícone do programa; a resposta volta como o clique no botão original (WM_COMMAND
// com o id dele, sem depender do idioma). Três botões (Sim/Não/Cancelar...) ficam empilhados, como no Mac.
// As de programas elevados ficam com o Windows.
#include "shell.h"
#include "../comum/semaforo.h"
#include <UIAutomation.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include "../comum/idioma.h"

namespace {

float AlertWidth();
#define AW AlertWidth()
const float AW0 = 260, APAD = 20, AICON = 64, ABTN = 28, AGAP = 8, RADIUS = 12;
const UINT WM_AL_CHECK = WM_APP + 70, WM_AL_FRAME = WM_APP + 71;
double g_openAt;  // a mola comum dos painéis ao aparecer

HWND g_wnd, g_back;
HWND g_pendente; RECT g_pendentePos;  // TaskDialog respondida que ainda não fechou
HWND g_tentando; int g_tentativas;    // confirmação de substituir ainda sem texto: olha de novo
bool g_glass;
ui::GpuSurface g_s;
HWINEVENTHOOK g_hook;
IUIAutomation* g_uia;

struct State {
    HWND target = nullptr;                 // a janela do Explorer
    std::function<void()> onYes;           // pergunta do próprio Shell (sem janela original)
    std::wstring title, info, yes, no;
    float titleH = 0, infoH = 0;
    int hover = -1, press = -1, focus = 1;  // botões: 0 = cancelar, 1 = confirmar (padrão)
    D2D1_RECT_F btn[3]{};                  // 0 cancelar, 1 confirmar, 2 o terceiro (caixas de três botões)
    ID2D1Bitmap* icon = nullptr;
    UINT iw = 0, ih = 0;
    std::vector<BYTE> px;
    std::wstring iconOf;                   // de quem é o ícone guardado ("" = Lixeira)
    int idYes = 0, idNo = 0, idThird = 0;  // caixa de mensagem comum: ids dos botões originais
    std::wstring third;                    // rótulo do terceiro botão
    bool single = false;                   // um botão só (OK)
    bool taskDlg = false;                  // a original é um TaskDialog (sem botões de verdade): TDM_CLICK_BUTTON
} g;

// o ícone do alerta: a Lixeira (apagar) ou o do programa que pergunta (substituir)
void SetIcon(const std::wstring& exe) {
    if (!g.px.empty() && g.iconOf == exe) return;
    if (g.icon) { g.icon->Release(); g.icon = nullptr; }
    g.px.clear(); g.iw = g.ih = 0;
    g.iconOf = exe;
    if (exe.rfind(L"pack:", 0) == 0) {  // ícone do pacote (ex.: o computador no alerta de desligar)
        std::wstring png = ui::PackIcon(exe.substr(5));
        if (png.empty() || !ui::LoadPng(png, g.iw, g.ih, g.px)) ui::ShellImage(L"shell:MyComputerFolder", 128, g.iw, g.ih, g.px, true);
    } else if (exe.empty()) {
        std::wstring png = ui::PackIcon(L"trashfull@2x");
        if (png.empty() || !ui::LoadPng(png, g.iw, g.ih, g.px)) ui::ShellImage(L"shell:RecycleBinFolder", 128, g.iw, g.ih, g.px, true);
    } else if (liquid::Leopard()) {
        // Leopard: o ícone do app no tema (pacote/aquificado); sem ele, o "Alert Note" do 10.5 (o balão com a exclamação)
        // em vez do ícone pequeno do .exe ampliado num quadro
        std::wstring nm = exe.substr(exe.find_last_of(L'\\') + 1);
        if (nm.size() > 4) nm.resize(nm.size() - 4);
        std::wstring png = apps::PackIconByName(nm, exe);
        if (png.empty() || !ui::LoadPng(png, g.iw, g.ih, g.px)) {
            png = ui::PackIcon(L"Alert Note");
            if (png.empty() || !ui::LoadPng(png, g.iw, g.ih, g.px))
                if (ui::ShellImage(exe, 128, g.iw, g.ih, g.px, true)) ui::TrimToSquare(g.iw, g.ih, g.px);
        }
    } else if (ui::ShellImage(exe, 128, g.iw, g.ih, g.px, true)) ui::TrimToSquare(g.iw, g.ih, g.px);
}

std::wstring ExeOf(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return L"";
    wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
    QueryFullProcessImageNameW(hp, 0, path, &n);
    CloseHandle(hp);
    return path;
}

float Sc() { return ui::Scale(g_wnd); }

float TextH(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w, float width) {
    if (s.empty()) return 0;
    IDWriteTextFormat* tf = ui::Format(size, w, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (tf) tf->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    IDWriteTextLayout* lay = nullptr; float h = 0;
    if (tf && SUCCEEDED(ui::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, width, 4000, &lay))) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); h = m.height; lay->Release(); }
    if (tf) tf->Release();
    return h;
}

// Alerta do macOS 26 (Tahoe), medido na referência do usuário (alerta de 260 pt de largura): ícone de 52 pt no canto
// de cima à ESQUERDA (22 pt da borda, 24 do topo); título em negrito 13 e texto 13, os dois alinhados à esquerda com
// 22 pt de margem; botões em cápsula de 28 pt, um por linha, de borda a borda (16 pt de margem, 8 pt entre eles),
// TODOS cinza (sem o azul do botão padrão), o de confirmar em cima; cantos de 26 pt.
// Medidas do guia macOS26-Theme-Reference (seções 14 e 31, _NSAlertPanel medido no macOS 26): 260 de largura, layout
// vertical centralizado (como no iOS): ícone de 32 × 32 no meio em cima, título em negrito 13 e texto 11 centralizados
// com 20 de margem lateral, botões de 28 empilhados em largura total com 6 entre eles e 16 de margem; cantos de 26.
const float T_PAD = 20, T_TOP = 20, T_ICON = 32, T_BM = 16, T_BTN = 28, T_GAP = 6;
bool TahoeAlert() { return liquid::On(); }
int Buttons() { return g.single ? 1 : g.idThird ? 3 : 2; }
// macOS 27 Golden Gate (Finder-Empty-Trash.png, 2x): 260 de largura, cantos 26; ícone de 64 no canto de cima à esquerda
// (20, 17); título em negrito 13 e texto 13 alinhados à esquerda a 23 da borda, o título a ~100 do alto; botões em
// cápsula de 27,5 LADO A LADO (110 cada, 8 entre eles, 16 das bordas), o padrão azul (0,122,255) à direita
const float G_ICON = 64, G_IX = 20, G_IY = 17, G_PAD = 23, G_BTN = 27.5f, G_BM = 16, G_GAP = 8;
bool GoldenAlert() { return liquid::Golden(); }
// macOS 27, três botões (o "já existe" do Finder, captura Finder-Replace, 2x): janela opaca de 404 × 120 com os botões da
// janela, o ícone de 32 a 20 / 36, o texto normal de 13 a 62 da borda e, embaixo à direita, três botões cinza de
// 85,5 × 19,5 lado a lado (8,5 entre eles, 12,5 da borda e 13 de baixo): o terceiro, Cancelar e o de confirmar
bool GoldenWide() { return GoldenAlert() && g.idThird; }
// Mac OS X Leopard (alerta do 10.5): janela cinza 232 de 420 de largura, cantos quase retos; ícone de 64 em (24, 15);
// a mensagem em negrito 13 e o texto 11 a 104 da borda; botões em pílula Aqua de 20 embaixo à direita (o padrão em gel
// azul, à direita; Cancelar à esquerda dele; o terceiro na ponta esquerda do texto)
bool LeoAlert() { return liquid::Leopard(); }
const float L_W = 420, L_TX = 104, L_TOP = 15;
float AlertWidth() { return LeoAlert() ? L_W : GoldenWide() ? 404.f : AW0; }
float AlertR() { return LeoAlert() ? 6.f : GoldenWide() ? 16.f : TahoeAlert() ? 26.f : RADIUS; }
float PanelH() {
    if (LeoAlert()) return std::max(L_TOP + 64.f, L_TOP + 2 + g.titleH + (g.infoH > 0 ? 8 + g.infoH : 0)) + 18 + 20 + 18;
    if (GoldenWide()) return std::max(120.f, 34 + g.titleH + (g.infoH > 0 ? 2 + g.infoH : 0) + 14 + 19.5f + 13);
    if (GoldenAlert()) { int n = Buttons(); int rows = n == 3 ? 3 : 1; return G_IY + G_ICON + 19 + g.titleH + (g.infoH > 0 ? 10 + g.infoH : 0) + 17 + rows * G_BTN + (rows - 1) * G_GAP + G_BM; }
    if (TahoeAlert()) { int n = Buttons(); return T_TOP + T_ICON + 12 + g.titleH + (g.infoH > 0 ? 6 + g.infoH : 0) + 18 + n * T_BTN + (n - 1) * T_GAP + T_BM; }
 return APAD + AICON + 12 + g.titleH + (g.infoH > 0 ? 6 + g.infoH : 0) + 16 + ABTN + (g.idThird ? 2 * (ABTN + AGAP) : 0) + APAD; }

// Sombra do alerta (como a do Mac: larga, suave, caindo para baixo), numa janela em camadas logo abaixo do fundo.
// Segue o canto arredondado do cartão (distância até o retângulo arredondado), não um retângulo reto.
HWND g_shadow;
void ShadowShow(int x, int y, int w, int h, float sc, float radius) {
    if (!g_shadow) {
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellAlertaSombra";
        RegisterClassW(&wc);
        g_shadow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, shell::inst, nullptr);
        if (g_shadow) apps::ours.push_back(g_shadow);
    }
    if (!g_shadow) return;
    const float sigma = 22 * sc, dy = 12 * sc, amount = ui::Dark() ? 0.55f : 0.30f, r = radius * sc;
    int m = (int)(70 * sc), W = w + 2 * m, H = h + 2 * m + (int)dy;
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, dib);
    auto dist = [&](float px, float py, float cx, float cy) {  // distância até o retângulo arredondado (negativa dentro)
        float qx = std::fabs(px - cx) - (w / 2.f - r), qy = std::fabs(py - cy) - (h / 2.f - r);
        float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
        return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
    };
    auto* px = (UINT32*)bits;
    float cx = m + w / 2.f, cy = m + h / 2.f;
    for (int yy = 0; yy < H; yy++)
        for (int xx = 0; xx < W; xx++) {
            float in = dist(xx + 0.5f, yy + 0.5f, cx, cy);
            if (in < -1.5f) { px[yy * W + xx] = 0; continue; }  // embaixo do cartão: nada
            float d = dist(xx + 0.5f, yy + 0.5f, cx, cy + dy);
            float a = d <= 0 ? amount : amount * std::exp(-(d * d) / (2 * sigma * sigma));
            px[yy * W + xx] = (UINT32)(BYTE)std::lround(std::min(a, 1.f) * 255) << 24;  // preto pré-multiplicado
        }
    POINT pos{ x - m, y - m }, zero{ 0, 0 };
    SIZE size{ W, H };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_shadow, nullptr, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA);
    SetWindowPos(g_shadow, g_back, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SelectObject(mem, old); DeleteObject(dib); DeleteDC(mem);
}

void Paint() {
    float sc = Sc();
    int wpx = (int)std::lround(AW * sc), hpx = (int)std::lround(PanelH() * sc);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    bool dark = ui::Dark();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    float H = PanelH();
    const float R = AlertR();
    if (LeoAlert()) {  // Leopard: desenho próprio do alerta do 10.5
        if (!g.icon && !g.px.empty()) g.icon = ui::Bitmap(dc, g.iw, g.ih, g.px);
        br->SetColor(D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, AW, H), R, R), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.40f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, AW - 0.5f, H - 0.5f), R - 0.5f, R - 0.5f), br, 1);
        if (g.icon) dc->DrawBitmap(g.icon, D2D1::RectF(24, L_TOP, 24 + 64, L_TOP + 64), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        IDWriteTextFormat* tb = ui::FormatFace(L"Lucida Sans Unicode", 13, DWRITE_FONT_WEIGHT_BOLD);
        IDWriteTextFormat* ti = ui::FormatFace(L"Lucida Sans Unicode", 11, DWRITE_FONT_WEIGHT_NORMAL);
        for (auto* f : { tb, ti }) if (f) { f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR); }
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.9f));
        float y = L_TOP + 2;
        if (tb) dc->DrawTextW(g.title.c_str(), (UINT32)g.title.size(), tb, D2D1::RectF(L_TX, y, AW - 20, y + g.titleH + 4), br);
        y += g.titleH + 8;
        if (ti && g.infoH > 0) dc->DrawTextW(g.info.c_str(), (UINT32)g.info.size(), ti, D2D1::RectF(L_TX, y, AW - 20, y + g.infoH + 4), br);
        IDWriteTextFormat* tbtn = ui::FormatFace(L"Lucida Sans Unicode", 13, DWRITE_FONT_WEIGHT_NORMAL);
        if (tbtn) tbtn->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        auto pill = [&](int i, D2D1_RECT_F r, bool blue) {
            g.btn[i] = r;
            bool down = g.press == i && g.hover == i;
            float rad = (r.bottom - r.top) / 2;
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1), rad, rad), br);
            bool b = blue || down;
            D2D1_GRADIENT_STOP st[4] = { { 0, b ? D2D1::ColorF(0.66f, 0.80f, 0.98f, 1) : D2D1::ColorF(1, 1, 1, 1) },
                                         { 0.5f, b ? D2D1::ColorF(0.38f, 0.60f, 0.93f, 1) : D2D1::ColorF(0.95f, 0.95f, 0.95f, 1) },
                                         { 0.5f, b ? D2D1::ColorF(0.24f, 0.47f, 0.87f, 1) : D2D1::ColorF(0.88f, 0.88f, 0.88f, 1) },
                                         { 1, b ? D2D1::ColorF(0.58f, 0.82f, 1.0f, 1) : D2D1::ColorF(0.98f, 0.98f, 0.98f, 1) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(dc->CreateGradientStopCollection(st, 4, &gs)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            br->SetColor(b ? D2D1::ColorF(0.12f, 0.27f, 0.58f, 0.9f) : D2D1::ColorF(0, 0, 0, 0.42f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f), br, 1);
            const std::wstring& lab = i == 0 ? g.no : i == 1 ? g.yes : g.third;
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.9f));
            if (tbtn) dc->DrawTextW(lab.c_str(), (UINT32)lab.size(), tbtn, r, br);
        };
        auto bw = [&](const std::wstring& t) { return std::max(82.f, (tbtn ? ui::TextWidth(tbtn, t) : 60) + 32); };
        for (int i = 0; i < 3; i++) g.btn[i] = D2D1::RectF(0, 0, 0, 0);
        float by = H - 18 - 20, x = AW - 20;
        float w1 = bw(g.yes);
        pill(1, D2D1::RectF(x - w1, by, x, by + 20), true);  // o padrão (gel azul) na direita
        x -= w1 + 12;
        if (!g.single) { float w0 = bw(g.no); pill(0, D2D1::RectF(x - w0, by, x, by + 20), false); }
        if (g.idThird) { float w2 = bw(g.third); pill(2, D2D1::RectF(L_TX, by, L_TX + w2, by + 20), false); }
        for (auto* f : { tb, ti, tbtn }) if (f) f->Release();
        br->Release();
        ui::GpuEnd(g_s);
        if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), R * sc } },
                                         D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f, 1), 0.f);  // opaca
        anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);
        ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade);
        if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
        return;
    }
    if (!backdrop::Liquid() || !backdrop::Real()) {  // com o Liquid Glass (vidro real) o próprio vidro desenha o contorno (duas bordas viravam um aro)
        if (liquid::On()) liquid::Rim(dc, D2D1::RectF(0, 0, AW, H), R, dark);
        else {
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.12f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, AW - 0.5f, H - 0.5f), R - 0.5f, R - 0.5f), br, 1);
        }
    }
    if (!g.icon && !g.px.empty()) g.icon = ui::Bitmap(dc, g.iw, g.ih, g.px);
    if (GoldenWide()) {
        D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.88f);
        // botões da janela: só o do meio aceso (como na captura), os outros apagados
        for (int k = 0; k < 3; k++) {
            D2D1_POINT_2F c{ 16.f + k * 23, 16 };
            br->SetColor(k == 1 ? D2D1::ColorF(0.996f, 0.737f, 0.18f) : (dark ? D2D1::ColorF(1, 1, 1, 0.20f) : D2D1::ColorF(0.86f, 0.86f, 0.86f)));
            dc->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
            semaforo::Gloss(dc, c, 7, br);
        }
        if (g.icon) dc->DrawBitmap(g.icon, D2D1::RectF(20, 36, 52, 68), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        IDWriteTextFormat* ti = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (ti) ti->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        br->SetColor(text);
        float y = 34;
        if (ti) dc->DrawTextW(g.title.c_str(), (UINT32)g.title.size(), ti, D2D1::RectF(62, y, AW - 14, y + g.titleH), br);
        y += g.titleH + 2;
        if (ti && g.infoH > 0) dc->DrawTextW(g.info.c_str(), (UINT32)g.info.size(), ti, D2D1::RectF(62, y, AW - 14, y + g.infoH), br);
        IDWriteTextFormat* tbtn = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (tbtn) tbtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const std::wstring* labels[3] = { &g.no, &g.yes, &g.third };
        const int order[3] = { 2, 0, 1 };  // da esquerda: o terceiro (Manter Ambos), Parar/Cancelar, Substituir
        float bw = 85.5f, gap = 8.5f, by = H - 13 - 19.5f, x = AW - 12.5f - 3 * bw - 2 * gap;
        for (int k = 0; k < 3; k++, x += bw + gap) {
            int i = order[k];
            D2D1_RECT_F r = D2D1::RectF(x, by, x + bw, by + 19.5f);
            g.btn[i] = r;
            bool down = g.press == i && g.hover == i;
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, down ? 0.24f : g.hover == i ? 0.18f : 0.13f) : D2D1::ColorF(0, 0, 0, down ? 0.16f : g.hover == i ? 0.12f : 0.085f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 5.5f, 5.5f), br);
            if (g.focus == i) {
                br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.55f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1.5f, r.top - 1.5f, r.right + 1.5f, r.bottom + 1.5f), 7, 7), br, 3);
            }
            br->SetColor(text);
            if (tbtn) dc->DrawTextW(labels[i]->c_str(), (UINT32)labels[i]->size(), tbtn, r, br);
        }
        for (auto* f : { ti, tbtn }) if (f) f->Release();
        br->Release();
        ui::GpuEnd(g_s);
        if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), 16 * sc } },
                                         dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.98f) : D2D1::ColorF(1, 1, 1, 0.98f), 30.f);  // opaca
        anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);
        ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade);
        if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
        return;
    }
    if (GoldenAlert()) {
        D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.88f);
        if (g.icon) dc->DrawBitmap(g.icon, D2D1::RectF(G_IX, G_IY, G_IX + G_ICON, G_IY + G_ICON), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        float y = G_IY + G_ICON + 19;
        IDWriteTextFormat* tb = ui::Format(13, DWRITE_FONT_WEIGHT_BOLD, false, DWRITE_TEXT_ALIGNMENT_LEADING);
        IDWriteTextFormat* ti = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_LEADING);
        for (auto* f : { tb, ti }) if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        br->SetColor(text);
        if (tb) dc->DrawTextW(g.title.c_str(), (UINT32)g.title.size(), tb, D2D1::RectF(G_PAD, y, AW - G_PAD, y + g.titleH), br);
        y += g.titleH + 10;
        if (ti && g.infoH > 0) dc->DrawTextW(g.info.c_str(), (UINT32)g.info.size(), ti, D2D1::RectF(G_PAD, y, AW - G_PAD, y + g.infoH), br);
        IDWriteTextFormat* tbtn = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (tbtn) tbtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const std::wstring* labels[3] = { &g.no, &g.yes, &g.third };
        for (int i = 0; i < 3; i++) g.btn[i] = D2D1::RectF(0, 0, 0, 0);
        float bottom = H - G_BM;
        if (g.idThird) {  // três: empilhados (confirmar, o terceiro, cancelar), de borda a borda
            const int order[3] = { 1, 2, 0 };
            for (int k = 0; k < 3; k++) { float y0 = bottom - (3 - k) * G_BTN - (2 - k) * G_GAP; g.btn[order[k]] = D2D1::RectF(G_BM, y0, AW - G_BM, y0 + G_BTN); }
        } else if (g.single) g.btn[1] = D2D1::RectF(G_BM, bottom - G_BTN, AW - G_BM, bottom);
        else {
            float bw = (AW - 2 * G_BM - G_GAP) / 2;
            g.btn[0] = D2D1::RectF(G_BM, bottom - G_BTN, G_BM + bw, bottom);
            g.btn[1] = D2D1::RectF(G_BM + bw + G_GAP, bottom - G_BTN, AW - G_BM, bottom);
        }
        for (int i = 0; i < 3; i++) {
            D2D1_RECT_F r = g.btn[i];
            if (r.right <= r.left) continue;
            bool down = g.press == i && g.hover == i;
            float a = dark ? (down ? 0.22f : g.hover == i ? 0.17f : 0.12f) : (down ? 0.14f : g.hover == i ? 0.10f : 0.07f);
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, a) : D2D1::ColorF(0, 0, 0, a));
            if (i == 1) br->SetColor(down ? D2D1::ColorF(0.0f, 0.40f, 0.86f, 1) : D2D1::ColorF(0.0f, 122 / 255.f, 1.0f, g.hover == i ? 0.92f : 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, G_BTN / 2, G_BTN / 2), br);
            if (g.focus == i && i != 1) {
                br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.55f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1.5f, r.top - 1.5f, r.right + 1.5f, r.bottom + 1.5f), G_BTN / 2 + 1.5f, G_BTN / 2 + 1.5f), br, 3);
            }
            br->SetColor(i == 1 ? D2D1::ColorF(1, 1, 1, 1) : text);
            if (tbtn) dc->DrawTextW(labels[i]->c_str(), (UINT32)labels[i]->size(), tbtn, r, br);
        }
        for (auto* f : { tb, ti, tbtn }) if (f) f->Release();
        br->Release();
        ui::GpuEnd(g_s);
        if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), R * sc } },
                                         dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.82f) : D2D1::ColorF(0.96f, 0.95f, 0.94f, 0.80f), 30.f);
        anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);
        ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade);
        if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
        return;
    }
    if (TahoeAlert()) {
        D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.88f);
        if (g.icon) dc->DrawBitmap(g.icon, D2D1::RectF(AW / 2 - T_ICON / 2, T_TOP, AW / 2 + T_ICON / 2, T_TOP + T_ICON), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        float y = T_TOP + T_ICON + 12;
        IDWriteTextFormat* tb = ui::Format(13, DWRITE_FONT_WEIGHT_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        IDWriteTextFormat* ti = ui::Format(11, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        for (auto* f : { tb, ti }) if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        br->SetColor(text);
        if (tb) dc->DrawTextW(g.title.c_str(), (UINT32)g.title.size(), tb, D2D1::RectF(T_PAD, y, AW - T_PAD, y + g.titleH), br);
        y += g.titleH + 6;
        if (ti && g.infoH > 0) dc->DrawTextW(g.info.c_str(), (UINT32)g.info.size(), ti, D2D1::RectF(T_PAD, y, AW - T_PAD, y + g.infoH), br);
        IDWriteTextFormat* tbtn = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (tbtn) tbtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        int n = Buttons();
        float y0 = H - T_BM - n * T_BTN - (n - 1) * T_GAP;
        const int order[3] = { 1, 2, 0 };  // confirmar, o terceiro, cancelar
        const std::wstring* labels[3] = { &g.no, &g.yes, &g.third };
        for (int i = 0; i < 3; i++) g.btn[i] = D2D1::RectF(0, 0, 0, 0);
        int row = 0;
        for (int k = 0; k < 3; k++) {
            int i = order[k];
            if ((i == 0 && g.single) || (i == 2 && !g.idThird)) continue;
            D2D1_RECT_F r = D2D1::RectF(T_BM, y0 + row * (T_BTN + T_GAP), AW - T_BM, y0 + row * (T_BTN + T_GAP) + T_BTN);
            row++;
            g.btn[i] = r;
            bool down = g.press == i && g.hover == i;
            float a = dark ? (down ? 0.22f : g.hover == i ? 0.17f : 0.13f) : (down ? 0.16f : g.hover == i ? 0.11f : 0.08f);
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, a) : D2D1::ColorF(0, 0, 0, a));
            // o botão padrão (confirmar) leva a tinta de destaque do macOS 26: Bezel Tint #0086FF (guia, seção 31)
            if (i == 1) br->SetColor(down ? D2D1::ColorF(0.0f, 0.44f, 0.86f, 1) : D2D1::ColorF(0.0f, 0.525f, 1.0f, g.hover == i ? 0.92f : 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, T_BTN / 2, T_BTN / 2), br);
            if (g.focus == i && i != 1) {  // anel de foco (Tab muda o foco)
                br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.55f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1.5f, r.top - 1.5f, r.right + 1.5f, r.bottom + 1.5f), T_BTN / 2 + 1.5f, T_BTN / 2 + 1.5f), br, 3);
            }
            br->SetColor(i == 1 ? D2D1::ColorF(1, 1, 1, 1) : text);
            if (tbtn) dc->DrawTextW(labels[i]->c_str(), (UINT32)labels[i]->size(), tbtn, r, br);
        }
        for (auto* f : { tb, ti, tbtn }) if (f) f->Release();
        br->Release();
        ui::GpuEnd(g_s);
        if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), R * sc } },
                                         dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.82f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.86f), 30.f);
        anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);
        ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade);
        if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
        return;
    }
    float cx = AW / 2;
    if (g.icon) dc->DrawBitmap(g.icon, D2D1::RectF(cx - AICON / 2, APAD, cx + AICON / 2, APAD + AICON), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.86f);
    float y = APAD + AICON + 12;
    IDWriteTextFormat* tb = ui::Format(13, DWRITE_FONT_WEIGHT_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat* ti = ui::Format(11, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (auto* f : { tb, ti }) if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);  // o título quebra em linhas
    br->SetColor(text);
    if (tb) dc->DrawTextW(g.title.c_str(), (UINT32)g.title.size(), tb, D2D1::RectF(APAD, y, AW - APAD, y + g.titleH), br);
    y += g.titleH + 6;
    if (ti && g.infoH > 0) dc->DrawTextW(g.info.c_str(), (UINT32)g.info.size(), ti, D2D1::RectF(APAD, y, AW - APAD, y + g.infoH), br);
    y = H - APAD - ABTN;
    float bw = (AW - 2 * APAD - AGAP) / 2;
    IDWriteTextFormat* tbtn = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (tbtn) tbtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const float BR = liquid::On() ? ABTN / 2 : 7.f;  // Tahoe: botões em cápsula
    const std::wstring* labels[3] = { &g.no, &g.yes, &g.third };
    for (int i = 0; i < (g.idThird ? 3 : 2); i++) {
        D2D1_RECT_F r = D2D1::RectF(APAD + i * (bw + AGAP), y, APAD + i * (bw + AGAP) + bw, y + ABTN);
        if (g.idThird) {  // três botões: empilhados (confirmar, o terceiro, cancelar), como no Mac
            int row = i == 1 ? 0 : i == 2 ? 1 : 2;
            float y0 = H - APAD - 3 * ABTN - 2 * AGAP + row * (ABTN + AGAP);
            r = D2D1::RectF(APAD, y0, AW - APAD, y0 + ABTN);
        }
        if (g.single) {  // um botão só: ocupa a largura toda
            if (i == 0) { g.btn[0] = D2D1::RectF(0, 0, 0, 0); continue; }
            r = D2D1::RectF(APAD, y, AW - APAD, y + ABTN);
        }
        g.btn[i] = r;
        bool def = i == 1, down = g.press == i && g.hover == i;
        D2D1_COLOR_F fill = def ? D2D1::ColorF(dark ? 0.04f : 0.0f, dark ? 0.52f : 0.48f, 1.0f) : (dark ? D2D1::ColorF(1, 1, 1, 0.20f) : D2D1::ColorF(1, 1, 1, 1));
        if (down) { fill.r *= 0.85f; fill.g *= 0.85f; fill.b *= 0.85f; }
        if (!dark && !def) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.12f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 0.5f, r.right, r.bottom + 0.8f), BR, BR), br); }
        br->SetColor(fill);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, BR, BR), br);
        if (g.focus == i && !def) {  // anel de foco do Mac (Tab muda o foco)
            br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.55f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1.5f, r.top - 1.5f, r.right + 1.5f, r.bottom + 1.5f), BR + 1.5f, BR + 1.5f), br, 3);
        }
        br->SetColor(def ? D2D1::ColorF(1, 1, 1) : text);
        if (tbtn) dc->DrawTextW(labels[i]->c_str(), (UINT32)labels[i]->size(), tbtn, r, br);
    }
    for (auto* f : { tb, ti, tbtn }) if (f) f->Release();
    br->Release();
    ui::GpuEnd(g_s);
    // vidro fosco (alerta do Mac): desfoque mínimo garantido, como nos menus e no Spotlight
    if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), R * sc } },
                                     dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.80f) : D2D1::ColorF(0.94f, 0.94f, 0.95f, 0.70f), 30.f);
    anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);  // aparece com a mola comum, em volta do centro
    ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade);
    if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
}

void Close() {
    if (g_shadow) ShowWindow(g_shadow, SW_HIDE);
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
    KillTimer(g_wnd, 1);
    g.target = nullptr;
    g.onYes = nullptr;
    g.idYes = g.idNo = g.idThird = 0;
    g.taskDlg = false;
    g.third.clear();
    g.btn[2] = D2D1::RectF(0, 0, 0, 0);
    g.single = false;
}

bool Aberto() { return g.target || g.onYes; }  // alerta na tela (de outro programa ou do próprio Shell)

// aperta o botão da janela original ("Yes"/"No") pela UI Automation
void Answer(bool yes, bool third = false) {
    HWND t = g.target;
    auto onYes = g.onYes;
    int idYes = g.idYes, idNo = g.idNo, idThird = g.idThird;
    bool taskDlg = g.taskDlg;
    RECT back{}; GetWindowRect(g_wnd, &back);
    Close();
    if (onYes) { if (yes) onYes(); return; }
    if (idYes) {  // caixa de mensagem comum: o clique no botão original
        int id = (third && idThird) ? idThird : (yes || !idNo) ? idYes : idNo;
        if (t && IsWindow(t)) {
            if (taskDlg) {  // TaskDialog: o "clique" documentado; se ela não fechar, volta para a tela (ninguém fica preso)
                PostMessageW(t, WM_USER + 102 /*TDM_CLICK_BUTTON*/, id, 0);
                g_pendente = t; g_pendentePos = back;
                SetTimer(g_wnd, 3, 800, nullptr);
            } else PostMessageW(t, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(t, id));
        }
        return;
    }
    if (!t || !IsWindow(t) || !g_uia) return;
    IUIAutomationElement* root = nullptr;
    if (FAILED(g_uia->ElementFromHandle(t, &root)) || !root) return;
    VARIANT v; v.vt = VT_I4; v.lVal = UIA_ButtonControlTypeId;
    IUIAutomationCondition* cond = nullptr;
    g_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
    IUIAutomationElementArray* arr = nullptr;
    if (cond && SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
        int n = 0; arr->get_Length(&n);
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* e = nullptr; arr->GetElement(i, &e);
            if (!e) continue;
            BSTR name = nullptr; e->get_CurrentName(&name);
            std::wstring s = name ? name : L"";
            if (name) SysFreeString(name);
            s.erase(std::remove(s.begin(), s.end(), L'&'), s.end());
            for (auto& c : s) c = towlower(c);
            bool isYes = s == L"yes" || s == L"sim", isNo = s == L"no" || s == L"não" || s == L"nao";
            if ((yes && isYes) || (!yes && isNo)) {
                IUIAutomationInvokePattern* inv = nullptr;
                if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv) { inv->Invoke(); inv->Release(); }
                e->Release();
                break;
            }
            e->Release();
        }
        arr->Release();
    }
    if (cond) cond->Release();
    root->Release();
}

void Show(int cx, int cy);
void TakeMessage(HWND dlg);

bool HasDirectUI(HWND dlg) {
    bool f = false;
    EnumChildWindows(dlg, [](HWND c, LPARAM l) -> BOOL {
        wchar_t cls[32] = {}; GetClassNameW(c, cls, 32);
        if (!wcscmp(cls, L"DirectUIHWND")) { *(bool*)l = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f;
}

// textos da janela original (pergunta, nome do arquivo) e o alerta do Mac no lugar
void Take(HWND dlg) {
    if (g.target) return;
    // Os textos direto dos Static da janela (caixas de mensagem como a "Confirm Save As" do Ableton). A UI
    // Automation voltava vazia nelas e a caixa ficava a do Windows (16/11/2026); ela segue para as do Explorer.
    std::vector<std::wstring> texts;
    EnumChildWindows(dlg, [](HWND c, LPARAM l) -> BOOL {
        wchar_t cls[16] = {}; GetClassNameW(c, cls, 16);
        if (_wcsicmp(cls, L"Static")) return TRUE;
        wchar_t b[1024] = {};
        SendMessageTimeoutW(c, WM_GETTEXT, 1024, (LPARAM)b, SMTO_ABORTIFHUNG, 200, nullptr);
        // "x.wav already exists. / Do you want to replace it?" vem num texto só: uma linha de cada vez
        std::wstring s = b, line;
        for (wchar_t ch : s + L"\n") {
            if (ch == L'\n' || ch == L'\r') { if (!line.empty()) ((std::vector<std::wstring>*)l)->push_back(line); line.clear(); }
            else line += ch;
        }
        return TRUE;
    }, (LPARAM)&texts);
    IUIAutomationElement* root = nullptr;
    if (g_uia && FAILED(g_uia->ElementFromHandle(dlg, &root))) root = nullptr;
    VARIANT v; v.vt = VT_I4; v.lVal = UIA_TextControlTypeId;
    IUIAutomationCondition* cond = nullptr;
    if (root) g_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
    IUIAutomationElementArray* arr = nullptr;
    if (root && cond && SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
        int n = 0; arr->get_Length(&n);
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* e = nullptr; arr->GetElement(i, &e);
            if (!e) continue;
            BSTR name = nullptr; e->get_CurrentName(&name);
            if (name && *name) texts.push_back(name);
            if (name) SysFreeString(name);
            e->Release();
        }
        arr->Release();
    }
    if (cond) cond->Release();
    if (root) root->Release();
    std::wstring q, file;
    // substituir: "x.wav already exists." / "x.wav já existe." + "Do you want to replace it?"
    wchar_t cap[128] = {}; GetWindowTextW(dlg, cap, 128);
    bool saveAs = !_wcsicmp(cap, L"Confirm Save As") || !_wcsicmp(cap, L"Confirmar Salvar como");
    bool achou = false;
    std::wstring name;
    for (auto& t : texts) {
        std::wstring l = t;
        for (auto& c : l) c = towlower(c);
        size_t at = l.find(L" already exists");
        if (at == std::wstring::npos) at = l.find(L" já existe");
        if (at == std::wstring::npos || at == 0) continue;
        name = t.substr(0, at); achou = true;
        break;
    }
    // A do Abrir/Salvar novo (a do Ableton) é um TaskDialog: nem o texto nem a UI Automation leem por fora
    // ("0 textos", 16/11/2026). Olha de novo algumas vezes; sem texto, o nome vem do que foi escolhido no Finder.
    if (!achou && saveAs) {
        if (texts.empty() && (g_tentando != dlg || g_tentativas < 3)) {
            if (g_tentando != dlg) { g_tentando = dlg; g_tentativas = 0; }
            g_tentativas++;
            SetTimer(g_wnd, 2, 150, nullptr);
            return;
        }
        std::wstring last = dialogs::LastSaved();
        if (!last.empty()) name = PathFindFileNameW(last.c_str());
        achou = true;
    }
    if (achou) {
        g.title = name.empty() ? Tr(L"Já existe um item com este nome. Deseja substituí-lo?") : L"“" + name + Tr(L"” já existe. Deseja substituí-lo?");
        g.info = Tr(L"Já existe um arquivo com o mesmo nome. Substituí-lo irá sobrescrever o conteúdo atual.");
        g.yes = Tr(L"Substituir");
        g.no = Tr(L"Cancelar");
        g.target = dlg;
        g.onYes = nullptr;
        // caixa de mensagem com Sim/Não de verdade: responde pelo id do botão, sem UI Automation;
        // TaskDialog (sem botões de verdade): TDM_CLICK_BUTTON com Sim/Não
        if (GetDlgItem(dlg, IDYES) && GetDlgItem(dlg, IDNO)) { g.idYes = IDYES; g.idNo = IDNO; }
        else if (HasDirectUI(dlg)) { g.idYes = IDYES; g.idNo = IDNO; g.taskDlg = true; }
        SetIcon(ExeOf(dlg));
        RECT wr; GetWindowRect(dlg, &wr);
        // ASYNC: a janela é de outro programa; sem isto o Shell esperava a thread dela (e travava se ela estivesse ocupada)
        SetWindowPos(dlg, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        Show((wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2);
        Log(L"alertas: confirmação de substituir no alerta do Mac (%ls%ls)", name.c_str(), g.taskDlg ? L", TaskDialog" : L"");
        return;
    }
    for (size_t i = 0; i < texts.size(); i++) {
        std::wstring l = texts[i];
        for (auto& c : l) c = towlower(c);
        if (q.empty() && (l.find(L"are you sure") != std::wstring::npos || l.find(L"tem certeza") != std::wstring::npos)) {
            q = texts[i];
            if (i + 1 < texts.size() && texts[i + 1].find(L':') == std::wstring::npos) file = texts[i + 1];
        }
    }
    if (q.empty()) {  // não é a confirmação que conhecemos: fica a do Windows
        Log(L"alertas: confirmação não reconhecida (%zu textos%ls)", texts.size(), texts.empty() ? L"" : (L": " + texts[0].substr(0, 60)).c_str());
        return;
    }
    std::wstring lq = q;
    for (auto& c : lq) c = towlower(c);
    bool permanent = lq.find(L"permanent") != std::wstring::npos;
    bool toBin = lq.find(L"recycle bin") != std::wstring::npos || lq.find(L"lixeira") != std::wstring::npos;
    int count = 0;
    for (size_t i = 0; i < lq.size(); i++) if (iswdigit(lq[i])) { count = _wtoi(lq.c_str() + i); break; }
    std::wstring what = !file.empty() && count == 0 ? L"“" + file + L"”" : count > 1 ? Tr(L"estes ") + std::to_wstring(count) + Tr(L" itens") : Tr(L"este item");
    if (permanent) {
        g.title = Tr(L"Tem certeza de que deseja apagar ") + what + Tr(L" permanentemente?");
        g.info = Tr(L"Você não pode desfazer esta ação.");
        g.yes = Tr(L"Apagar");
    } else if (toBin) {
        g.title = Tr(L"Tem certeza de que deseja mover ") + what + Tr(L" para a Lixeira?");
        g.info = L"";
        g.yes = Tr(L"Mover para a Lixeira");
    } else {
        g.title = q;  // pergunta que não reconhecemos: o texto do Windows, no alerta do Mac
        g.info = L"";
        g.yes = Tr(L"Sim");
    }
    g.no = Tr(L"Cancelar");
    g.target = dlg;
    g.onYes = nullptr;
    SetIcon(L"");
    // a janela original sai da tela (continua viva, esperando a resposta); sem esperar a thread dela
    RECT wr; GetWindowRect(dlg, &wr);
    SetWindowPos(dlg, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    Show((wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2);
    Log(L"alertas: confirmação do Explorer no alerta do Mac (%ls)", g.title.c_str());
}

// Caixa de mensagem comum (MessageBox) de qualquer programa → alerta do Mac
std::wstring Limpo(HWND c) {
    wchar_t b[1024] = {};
    SendMessageTimeoutW(c, WM_GETTEXT, 1024, (LPARAM)b, SMTO_ABORTIFHUNG, 200, nullptr);
    std::wstring s = b;
    s.erase(std::remove(s.begin(), s.end(), L'&'), s.end());
    return s;
}
std::wstring Rotulo(const std::wstring& s) {  // botões do Windows em inglês → os do projeto
    static const std::pair<const wchar_t*, const wchar_t*> T[] = {
        { L"ok", L"OK" }, { L"yes", L"Sim" }, { L"no", L"Não" }, { L"cancel", L"Cancelar" }, { L"retry", L"Tentar Novamente" },
        { L"try again", L"Tentar Novamente" }, { L"continue", L"Continuar" }, { L"close", L"Fechar" }, { L"ignore", L"Ignorar" }, { L"abort", L"Anular" } };
    std::wstring l = s;
    for (auto& c : l) c = towlower(c);
    for (auto& [en, pt] : T) if (l == en) return Tr(pt);
    return s;
}
void TakeMessage(HWND dlg) {
    if (g.target || !IsWindowVisible(dlg)) return;
    struct F { HWND text = nullptr; HWND btn[4]{}; int ids[4]{}; int n = 0; bool outro = false; } f;
    EnumChildWindows(dlg, [](HWND c, LPARAM l) -> BOOL {
        auto* f = (F*)l;
        wchar_t cls[32] = {}; GetClassNameW(c, cls, 32);
        int id = GetDlgCtrlID(c);
        if (!_wcsicmp(cls, L"Static")) { if (id == 0xFFFF) f->text = c; else if (id != 20) f->outro = true; }  // 20 = o ícone
        else if (!_wcsicmp(cls, L"Button") && ((id >= 1 && id <= 7) || id == 10 || id == 11)) { if (f->n < 4) { f->btn[f->n] = c; f->ids[f->n] = id; } f->n++; }
        else f->outro = true;
        return TRUE;
    }, (LPARAM)&f);
    if (!f.text || f.outro || f.n < 1 || f.n > 3) return;  // não é uma caixa de mensagem simples: fica a do Windows
    std::wstring texto;
    { wchar_t b[2048] = {}; SendMessageTimeoutW(f.text, WM_GETTEXT, 2048, (LPARAM)b, SMTO_ABORTIFHUNG, 200, nullptr); texto = b; }
    while (!texto.empty() && iswspace(texto.back())) texto.pop_back();
    if (texto.empty()) return;  // programa elevado (não deixa ler) ou caixa vazia
    wchar_t cap[256] = {}; GetWindowTextW(dlg, cap, 256);
    // título em negrito = o primeiro parágrafo; o resto vira o texto menor (mensagem longa sem parágrafos: o nome da janela)
    size_t br = texto.find(L"\n\n");
    size_t skip = 2;
    if (br == std::wstring::npos) { br = texto.find(L"\r\n\r\n"); skip = 4; }
    if (br == std::wstring::npos && LeoAlert()) {  // Leopard: a 1ª linha é o texto em negrito, o resto o informativo (10.5)
        br = texto.find(L'\n'); skip = 1;
        if (br != std::wstring::npos && br > 0 && texto[br - 1] == L'\r') { br--; skip = 2; }
    }
    if (br != std::wstring::npos) { g.title = texto.substr(0, br); g.info = texto.substr(br + skip); }
    else if (texto.size() > 140 && cap[0]) { g.title = cap; g.info = texto; }
    else { g.title = texto; g.info = L""; }
    auto afirma = [](int id) { return id == IDOK || id == IDYES || id == IDRETRY || id == 10 || id == 11; };
    int y = -1, n = -1, t = -1;  // qual botão confirma, qual cancela, qual é o terceiro
    for (int i = 0; i < f.n && y < 0; i++) if (afirma(f.ids[i])) y = i;
    if (y < 0) y = 0;
    for (int i = 0; i < f.n; i++) if (i != y && f.ids[i] == IDCANCEL) n = i;
    for (int i = 0; i < f.n && n < 0; i++) if (i != y && (f.ids[i] == IDABORT || f.ids[i] == IDNO)) n = i;
    for (int i = 0; i < f.n && n < 0; i++) if (i != y) n = i;
    for (int i = 0; i < f.n; i++) if (i != y && i != n) t = i;
    g.idYes = f.ids[y];
    g.yes = Rotulo(Limpo(f.btn[y]));
    g.single = f.n == 1;
    g.idNo = n >= 0 ? f.ids[n] : 0;
    g.no = n >= 0 ? Rotulo(Limpo(f.btn[n])) : L"";
    g.idThird = t >= 0 ? f.ids[t] : 0;
    g.third = t >= 0 ? Rotulo(Limpo(f.btn[t])) : L"";
    g.target = dlg;
    g.onYes = nullptr;
    SetIcon(ExeOf(dlg));
    RECT wr; GetWindowRect(dlg, &wr);
    // sem esperar a thread do programa: o Ableton ocupado (escaneando plugins) com a caixa do Kontakt aberta
    // deixava o Shell parado até o vigia derrubá-lo (12/11/2026)
    if (!SetWindowPos(dlg, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) {  // não deixou mexer (elevado)
        g.target = nullptr; g.idYes = g.idNo = 0; g.single = false;
        return;
    }
    Show((wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2);
    Log(L"alertas: caixa de mensagem de %ls no alerta do Mac (%d botão(ões)): %ls", cap, f.n, g.title.substr(0, 80).c_str());
}

void Show(int cx, int cy) {
    float inner = LeoAlert() ? L_W - L_TX - 20 : GoldenWide() ? AW - 62 - 14 : GoldenAlert() ? AW - 2 * G_PAD : TahoeAlert() ? AW - 2 * T_PAD : AW - 2 * APAD;
    g.titleH = TextH(g.title, 13, GoldenWide() ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_BOLD, inner);
    g.infoH = TextH(g.info, GoldenAlert() ? 13.f : 11.f, DWRITE_FONT_WEIGHT_NORMAL, inner);
    g.hover = g.press = -1;
    g.focus = 1;
    if (g.px.empty()) SetIcon(g.iconOf);
    float sc = Sc();
    int wpx = (int)std::lround(AW * sc), hpx = (int)std::lround(PanelH() * sc);
    int x = cx - wpx / 2, y = cy - hpx / 2;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    SetWindowPos(g_back, g_wnd, x, y, wpx, hpx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShadowShow(x, y, wpx, hpx, sc, AlertR());
    g_openAt = anim::Now();
    Paint();
    ShowWindow(g_wnd, SW_SHOW);
    ui::FrameClock(g_wnd, WM_AL_FRAME, true);
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
    if (att) AttachThreadInput(me, ft, FALSE);
    if (g.target) SetTimer(g_wnd, 1, 300, nullptr);  // a original sumiu? o alerta some junto
}

}  // namespace

// pergunta do próprio Shell (Esvaziar Lixeira do Dock): o mesmo alerta, no meio da tela
void alertas::Confirm(const std::wstring& title, const std::wstring& info, const std::wstring& yes, std::function<void()> onYes, const std::wstring& icon) {
    if (g.target) return;  // já tem um alerta aberto
    g.title = title; g.info = info; g.yes = yes; g.no = Tr(L"Cancelar");
    g.onYes = std::move(onYes);
    SetIcon(icon);
    MONITORINFO mi{ sizeof(mi) };
    POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    Show((mi.rcWork.left + mi.rcWork.right) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 3);
    g.target = nullptr;
}

namespace {

bool IsDeleteDialog(HWND h) {
    wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
    if (wcscmp(cls, L"#32770")) return false;
    wchar_t t[128] = {}; GetWindowTextW(h, t, 128);
    for (auto s : { L"Delete File", L"Delete Folder", L"Delete Multiple Items", L"Delete Shortcut",
                    L"Excluir Arquivo", L"Excluir Pasta", L"Excluir Vários Itens", L"Excluir Atalho",
                    L"Confirm Save As", L"Confirmar Salvar como", L"Confirmar Salvar Como" })
        if (!_wcsicmp(t, s)) return true;
    return false;
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h || GetAncestor(h, GA_ROOT) != h) return;
    if (IsDeleteDialog(h)) { PostMessageW(g_wnd, WM_AL_CHECK, 0, (LPARAM)h); return; }
    wchar_t cls[16] = {}; GetClassNameW(h, cls, 16);
    if (!wcscmp(cls, L"#32770") && shell::Setting(L"alertas_mac", L"1") != L"0") PostMessageW(g_wnd, WM_AL_CHECK, 1, (LPARAM)h);  // talvez uma caixa de mensagem  // fora do evento: a UI Automation não pode rodar dentro dele
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_AL_CHECK: if (IsWindow((HWND)l)) { if (w == 1) TakeMessage((HWND)l); else Take((HWND)l); } return 0;
    case WM_AL_FRAME:
        if (Aberto()) Paint();  // pergunta do próprio Shell não tem target: antes parava no 1º quadro (quase transparente)
        if (!Aberto() || anim::PanelIn(anim::Now() - g_openAt).done) ui::FrameClock(h, WM_AL_FRAME, false);
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); if (Aberto()) Paint(); return 0; }
    case WM_TIMER:
        if (w == 2) { KillTimer(h, 2); if (IsWindow(g_tentando)) Take(g_tentando); return 0; }
        if (w == 3) {
            KillTimer(h, 3);
            if (IsWindow(g_pendente)) {  // não fechou com a resposta: devolve à tela para o usuário responder nela
                Log(L"alertas: a confirmação original não respondeu; voltou para a tela");
                SetWindowPos(g_pendente, HWND_TOP, g_pendentePos.left, g_pendentePos.top, 0, 0, SWP_NOSIZE | SWP_ASYNCWINDOWPOS);
            }
            g_pendente = nullptr;
            return 0;
        }
        if (g.target && !IsWindow(g.target)) Close();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Answer(false);
        else if (w == VK_RETURN) Answer(true);
        else if (w == VK_SPACE) { if (g.focus == 2) Answer(false, true); else Answer(g.focus == 1); }
        else if ((w == VK_TAB || w == VK_LEFT || w == VK_RIGHT || w == VK_DOWN || w == VK_UP) && !g.single) {
            if (g.idThird) g.focus = g.focus == 1 ? 2 : g.focus == 2 ? 0 : 1;
            else g.focus = 1 - g.focus;
            Paint();
        }
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: {
        float sc = Sc();
        D2D1_POINT_2F p{ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc };
        int hit = -1;
        for (int i = 0; i < 3; i++) if (p.x >= g.btn[i].left && p.x < g.btn[i].right && p.y >= g.btn[i].top && p.y < g.btn[i].bottom) hit = i;
        if (m == WM_LBUTTONDOWN) {
            if (hit < 0) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); return 0; }  // arrasta pelo fundo
            g.press = hit; SetCapture(h);
        } else if (m == WM_LBUTTONUP) {
            int was = g.press; g.press = -1; ReleaseCapture();
            if (was >= 0 && was == hit) { if (hit == 2) Answer(false, true); else Answer(hit == 1); return 0; }
        }
        if (hit != g.hover || m != WM_MOUSEMOVE) { g.hover = hit; Paint(); }
        return 0;
    }
    case WM_MOVE:
        if (IsWindowVisible(g_back)) {
            RECT r; GetWindowRect(h, &r); SetWindowPos(g_back, h, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            if (g_shadow && IsWindowVisible(g_shadow)) { int m = (int)(70 * Sc()); SetWindowPos(g_shadow, g_back, r.left - m, r.top - m, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE); }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool alertas::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellAlerta"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellAlertaFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Alerta", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) return false;
    g_glass = backdrop::Attach(g_back);
    if (!g_glass) {
        DestroyWindow(g_back);
        g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        DWORD corner = 2; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
        ui::Acrylic(g_back, ui::Dark() ? 0xE0201E1Eu : 0xE0F6F6F6u);
    } else { DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner)); }
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia));
    // programa ocupado: a UI Automation esperava até 20 s (o padrão) na thread do Shell; o vigia derruba em 10 s
    if (IUIAutomation2* u2 = nullptr; g_uia && SUCCEEDED(g_uia->QueryInterface(IID_PPV_ARGS(&u2)))) {
        u2->put_ConnectionTimeout(500);
        u2->put_TransactionTimeout(1000);
        u2->Release();
    }
    g_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    return true;
}

void alertas::Stop() {
    if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
    if (g.target && IsWindow(g.target)) {  // devolve a janela original à tela
        RECT r; GetWindowRect(g_wnd, &r);
        SetWindowPos(g.target, HWND_TOP, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_ASYNCWINDOWPOS);
    }
    if (g_uia) { g_uia->Release(); g_uia = nullptr; }
}
