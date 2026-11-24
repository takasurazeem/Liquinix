// Barra de título do Mac nas janelas dos outros programas, POR FORA (modo nativo: o Shell dentro do explorer.exe).
// Nada é carregado nos outros programas (sem gancho, sem DLL neles): para cada janela com a barra de título
// padrão do Windows, o Shell põe por cima dela uma faixa nossa (janela em camadas, logo acima da janela na
// ordem Z) do tamanho exato da barra, com a cor da própria barra, os três botões do Mac à esquerda e o título
// centralizado. A faixa cobre o título, o ícone e os botões do Windows.
//   - cor: lida da tela por um furo de 3 px na faixa (a barra do Windows aparece por ele), então acompanha o
//     tema do programa (claro, escuro, cor de destaque) e a janela ativa/inativa;
//   - botões: fechar/minimizar/ampliar mandam o comando de sistema para a janela (WM_SYSCOMMAND);
//   - arrastar e redimensionar pela borda de cima são feitos AQUI (a faixa segura o mouse e move a janela e a
//     si mesma na mesma operação, DeferWindowPos: as duas andam juntas). Repassar o clique para a barra da
//     janela (WM_NCLBUTTONDOWN) não funciona entre programas: o Windows só entrega o mouse ao programa que
//     recebeu o clique, e a janela não saía do lugar (testado na VM em 11/11/2026).
//   - duplo clique amplia/volta; botão direito abre o menu da janela.
// Janelas que desenham a própria barra: modo "barra própria".
//  - Chromium (Edge, Chrome, Electron; classe Chrome_WidgetWin_1): como o Chrome do Mac. A faixa cobre a linha de
//    cima da janela (a das abas) inteira e a redesenha DESLOCADA 76 pt para a direita, ao vivo (miniatura do DWM da
//    própria janela, sem os 138 pt dos botões do Windows), com os três botões do Mac no espaço que sobra à
//    esquerda: as abas começam depois dos botões e nada é coberto. Os cliques nessa linha são repassados à janela
//    76 pt à esquerda (mensagens de mouse); a parte vazia arrasta a janela (feito aqui).
//    Limites: arrastar uma aba para reordenar/destacar não acompanha (o programa lê o ponteiro de verdade), e os
//    menus que o programa abre nessa linha aparecem 76 pt à esquerda.
//  - Outros (Bloco de Notas novo, WinUI, apps da Loja, Terminal): não aceitam cliques repassados, então ficam com
//    dois remendos: os botões do Mac à esquerda (cobrem o canto) e um por cima dos botões do Windows.
// `barras_proprias=0` desliga este modo.
// Fora do modo nativo quem faz a barra é a WacOSWindow.dll (src\janelas). Desligar: janelas_mac=0 no
// ajustes.txt ou C:\Users\Public\MacTitulo-desligado.txt. Roda numa thread própria (um programa travado
// não segura o Dock nem a barra de menus).
#include "shell.h"
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <uiautomation.h>
#include "../comum/semaforo.h"

namespace {

const wchar_t* CLS = L"MacBarraTitulo";
const float LX0 = 20, LPITCH = 23, LR = 7;  // botões: 14 pt, 23 pt entre centros (o padrão do projeto)
const float PATCH_L = 88, PATCH_R = 3 * 46, BAR_PROPRIA = 32;  // barra própria (pt): botões do Mac, botões do Windows, altura padrão
const float FURO_DIR = 150;                 // furo de leitura da cor: a 150 pt da direita (antes dos botões do Windows)

struct Info {
    HWND alvo = nullptr, faixa = nullptr;
    RECT r{};                       // a faixa na tela (px)
    SIZE pintado{};                 // tamanho do último desenho
    COLORREF cor = CLR_INVALID;     // cor da barra em uso
    COLORREF lida = CLR_INVALID;    // última leitura (só vale lida duas vezes igual)
    DWORD corAte = 0;               // modo deslocado: a cor está mudando (o programa escurecendo a janela): relê a cada quadro até aqui
    COLORREF bordaE = CLR_INVALID, bordaD = CLR_INVALID;  // cor do fio de 1 px da borda da janela (lida logo abaixo da faixa)
    bool ativa = false, hover = false, zoom = false, sujo = true;
    bool propria = false;           // o programa desenha a barra dele
    int acima = 0;                  // barra própria: 1 = modo deslocado (Chromium), 0 = remendos
    float direita = PATCH_R;        // modo deslocado: largura (pt) dos botões de janela do programa, que ficam de fora da cópia
    float corte = 8;                // modo deslocado: quanto da esquerda da linha fica de fora da cópia (pt)
    int raio = 0;                   // a DLL recortou os cantos desta janela (propriedade "MacJanelaCanto"): raio em px
    HWND cantos = nullptr;          // os dois cantos de baixo suavizados (janela em camadas, não recebe cliques)
    RECT vis{};                     // a parte visível da janela (tela)
    COLORREF baixoE = CLR_INVALID, baixoD = CLR_INVALID;  // cor da janela junto aos cantos de baixo
    HTHUMBNAIL mini3 = nullptr;     // Chrome: a curva de baixo da primeira aba (fica à esquerda do começo da aba)
    HTHUMBNAIL mini = nullptr, mini2 = nullptr;  // mini2: o botão "buscar abas" do Chrome, levado para a ponta direita      // modo deslocado: a linha de cima da janela, ao vivo
    RECT miniSrc{}, miniDst{};
    bool chrome = false;            // chrome.exe: o começo da linha das abas é conferido na própria janela (LerLinhaChrome)
    DWORD linhaEm = 0;              // quando conferir de novo (GetTickCount)
    int press = -1;
    LONG estilo = 0;
    std::wstring titulo;
};

std::map<HWND, Info> g_j;           // janela do programa -> faixa
std::atomic<DWORD> g_tid{ 0 };
std::atomic<bool> g_rodando{ false };  // a thread é solta (sem std::thread global: travaria a saída do explorer)
HWINEVENTHOOK g_ev[2];
ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
IDWriteTextFormat* g_tf;

float Sc(HWND h) { UINT d = GetDpiForWindow(h); return (d ? d : 96) / 96.f; }

bool TemaEscuro() {
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}
bool Escura(COLORREF c) { return GetRValue(c) * 0.299 + GetGValue(c) * 0.587 + GetBValue(c) * 0.114 < 140; }

// A janela tem a barra de título padrão do Windows? Devolve o retângulo da faixa (tela, px).
// Altura da linha de cima do programa (a das abas / barra dele), em px: até onde os botões do Windows respondem
int AlturaPropria(HWND h, const RECT& f, float sc) {
    static std::map<HWND, std::pair<LONG, int>> cache;  // por janela e largura
    auto it = cache.find(h);
    // por janela e estado (ampliada ou não), NÃO pela largura: medir de novo a cada largura fazia ~100 perguntas à
    // janela a cada passo do redimensionar, e a faixa ficava bem atrás dela (sobrando para fora ou mostrando os
    // botões do Windows; vídeo do usuário de 12/11/2026)
    LONG chave = IsZoomed(h) ? 2 : 1;
    if (it != cache.end() && it->second.first == chave) return it->second.second;
    int alt = (int)std::lround(BAR_PROPRIA * sc), x = f.right - (int)(20 * sc);
    int ultimo = 0;
    for (int y = (int)(4 * sc); y <= (int)(64 * sc); y += 2) {
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM(x, f.top + y), SMTO_ABORTIFHUNG, 60, &r)) break;
        if (r == HTCLOSE || r == HTMAXBUTTON || r == HTMINBUTTON) ultimo = y; else if (ultimo) break;
    }
    if (ultimo >= (int)(20 * sc)) alt = ultimo + 2;
    // A linha das abas costuma ser mais alta que os botões do Windows (Chrome: 41 pt contra ~30). Cobrindo só a altura
    // dos botões, a parte de baixo das abas aparecia no lugar antigo, desalinhada da parte de cima (um degrau embaixo
    // de cada aba). A altura certa é até onde o espaço vazio da linha (à esquerda dos botões) ainda arrasta a janela.
    int xv = f.right - (int)((PATCH_R + 24) * sc), fim = 0;
    for (int y = (int)(6 * sc); y <= (int)(72 * sc); y += 1) {
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM(xv, f.top + y), SMTO_ABORTIFHUNG, 60, &r)) break;
        if (r == HTCAPTION) fim = y; else if (fim) break;
    }
    if (fim > alt && fim <= (int)(64 * sc)) alt = fim;  // sem o último px: ele já é a borda de cima da barra de endereço
    cache[h] = { chave, alt };
    return alt;
}

// Barra própria: só classes conhecidas de programas que põem os botões do Windows no canto de cima à direita
bool Propria(HWND h, const wchar_t* cls, LONG st, RECT& bar, bool& zoom, int& acima) {
    static const wchar_t* CLASSES[] = { L"Chrome_WidgetWin_1", L"WinUIDesktopWin32WindowClass", L"Notepad", L"ApplicationFrameWindow",
                                        L"CASCADIA_HOSTING_WINDOW_CLASS", L"MozillaWindowClass" };
    bool known = false;
    for (auto c : CLASSES) if (!wcscmp(cls, c)) known = true;
    if (!known || shell::Setting(L"barras_proprias", L"1") == L"0") return false;
    // Só para ganhar a faixa: quem já tem continua com ela. O Bloco de Notas tira o "ampliar" da janela enquanto mostra
    // um diálogo dele (ex.: "arquivo não encontrado"): a faixa sumia e apareciam os botões do Windows até o diálogo fechar.
    if (!g_j.count(h) && (!(st & WS_THICKFRAME) || !(st & WS_MAXIMIZEBOX) || !(st & WS_MINIMIZEBOX))) return false;
    if (GetWindowTextLengthW(h) == 0 && !g_j.count(h)) return false;  // sem título só barra janelas novas (aba nova deixa o título vazio por um instante)
    float sc = Sc(h);
    RECT f{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &f, sizeof(f)))) GetWindowRect(h, &f);
    zoom = IsZoomed(h) != FALSE;
    if (!zoom && GetPropW(h, L"MacJanelaCanto")) {  // recortada pela DLL: a parte visível é a janela menos a borda invisível
        RECT wr{}; GetWindowRect(h, &wr);
        int bi = std::max(0, (int)(INT_PTR)GetPropW(h, L"MacJanelaCantoB") - 1);
        f = { wr.left + bi, wr.top, wr.right - bi, wr.bottom - bi };
    }
    if (f.right - f.left < (LONG)(360 * sc) || f.bottom - f.top < (LONG)(200 * sc)) return false;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    if (!zoom && f.left <= mi.rcMonitor.left && f.top <= mi.rcMonitor.top && f.right >= mi.rcMonitor.right && f.bottom >= mi.rcMonitor.bottom) return false;  // tela cheia
    bool chromium = !wcscmp(cls, L"Chrome_WidgetWin_1");
    // 1 = deslocado com cliques repassados por mensagem (Chromium); 2 = deslocado nos outros (as mensagens vão para a
    // janela filha que está no ponto)
    acima = shell::Setting(L"barras_deslocar", L"1") == L"0" ? 0 : chromium ? 1 : 2;
    int top = zoom ? std::max(f.top, mi.rcWork.top) : f.top;
    RECT fv{ std::max(f.left, mi.rcMonitor.left), top, std::min(f.right, mi.rcMonitor.right), f.bottom };
    int alt = acima ? AlturaPropria(h, fv, sc) : (int)std::lround(BAR_PROPRIA * sc);
    if (acima && !wcscmp(cls, L"Notepad")) alt = (int)std::lround(42 * sc);  // a linha de abas dele é mais alta que os botões
    bar = { fv.left, top, fv.right, top + alt };
    return true;
}

bool Elegivel(HWND h, RECT& bar, bool& zoom, bool& propria, int& acima) {
    propria = false; acima = 0;
    if (!IsWindow(h) || GetAncestor(h, GA_ROOT) != h || !IsWindowVisible(h) || IsIconic(h)) return false;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (st & WS_CHILD) return false;
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    // as nossas (Dock, barra de menus, painéis, mesa...): as classes do Shell são registradas com o módulo dele.
    // (apps::IsShellWindow não: é da thread principal do Shell)
    if ((HINSTANCE)GetWindowLongPtrW(h, GWLP_HINSTANCE) == shell::inst) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    for (auto c : { L"ArquivosJanela", L"ArquivosEscolher", L"AjustesJanela", L"MonitorAtividadeJanela", L"TerminalMacJanela", L"CalculadoraJanela",
                    L"NotasJanela", L"NotasRapidaJanela", L"MacJanelaBarra", L"Shell_TrayWnd", L"Windows.UI.Core.CoreWindow" })
        if (!wcscmp(cls, c)) return false;
    if (GetPropW(h, L"MacJanela")) return false;  // já tratada pela DLL (fora do nativo)
    auto tentaPropria = [&] { propria = Propria(h, cls, st, bar, zoom, acima); return propria; };
    if (!wcscmp(cls, L"ApplicationFrameWindow")) return tentaPropria();  // apps da Loja: a moldura desenha a barra
    if ((st & WS_CAPTION) != WS_CAPTION) return tentaPropria();
    TITLEBARINFO tb{ sizeof(tb) };
    if (!GetTitleBarInfo(h, &tb) || (tb.rgstate[0] & STATE_SYSTEM_INVISIBLE)) return tentaPropria();
    float sc = Sc(h);
    if (tb.rcTitleBar.bottom - tb.rcTitleBar.top < (LONG)(16 * sc)) return tentaPropria();
    POINT c{ 0, 0 }; ClientToScreen(h, &c);
    if (c.y < tb.rcTitleBar.bottom - 2) return tentaPropria();  // a área do programa cobre a barra: ele desenha a dele
    RECT f{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &f, sizeof(f)))) GetWindowRect(h, &f);
    if (f.right - f.left < (LONG)(140 * sc)) return false;
    zoom = IsZoomed(h) != FALSE;
    if (zoom) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        bar = { std::max(f.left, mi.rcMonitor.left), tb.rcTitleBar.top, std::min(f.right, mi.rcMonitor.right), tb.rcTitleBar.bottom };
    } else bar = { f.left, std::min(f.top, tb.rcTitleBar.top), f.right, tb.rcTitleBar.bottom };
    return bar.bottom - bar.top >= (LONG)(16 * sc);
}

bool Ativa(HWND h) { HWND f = GetForegroundWindow(); return f && (f == h || GetAncestor(f, GA_ROOTOWNER) == h); }

POINT Furo(const Info& in) {  // ponto da tela onde a barra do Windows aparece pelo furo
    float sc = Sc(in.alvo);
    // barra própria: logo à esquerda do remendo da direita, perto do topo (ali é a barra do programa, sem abas)
    // deslocado: a faixa cobre a linha toda; o furo fica na divisa entre os botões de ampliar e fechar do Windows
    // (fundo da barra do programa), fora da área redesenhada
    if (in.propria && in.acima) return { in.r.right - (LONG)(46 * sc), in.r.top + (LONG)std::lround(5 * sc) };
    if (in.propria) return { in.r.right - (LONG)((PATCH_R + 5) * sc), in.r.top + (LONG)std::lround(5 * sc) };
    return { in.r.right - (LONG)(FURO_DIR * sc), in.r.bottom - (LONG)std::lround(4 * sc) };
}

void Pintar(Info& in) {
    if (!in.faixa || !g_d2d) return;
    int w = std::max<int>(in.r.right - in.r.left, 1), h = std::max<int>(in.r.bottom - in.r.top, 1);
    float sc = Sc(in.alvo);
    COLORREF cor = in.cor != CLR_INVALID ? in.cor : (TemaEscuro() ? RGB(32, 32, 32) : RGB(243, 243, 243));
    bool escura = Escura(cor);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(mem); ReleaseDC(nullptr, screen); return; }
    HGDIOBJ old = SelectObject(mem, bmp);
    ID2D1DCRenderTarget* rt = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * sc, 96 * sc);
    if (SUCCEEDED(g_d2d->CreateDCRenderTarget(&props, &rt))) {
        RECT br{ 0, 0, w, h };
        rt->BindDC(mem, &br);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush* b = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(GetRValue(cor) / 255.f, GetGValue(cor) / 255.f, GetBValue(cor) / 255.f, 1), &b);
        float W = w / sc, HT = h / sc;
        float H = HT;
        // fundo: a cor da barra, com os cantos de cima arredondados como os da janela (ampliada: retos)
        float R = in.zoom ? 0.f : in.raio ? in.raio / sc : 8.f;
        ID2D1PathGeometry* g = nullptr;
        g_d2d->CreatePathGeometry(&g);
        ID2D1GeometrySink* sk = nullptr;
        g->Open(&sk);
        // modo deslocado: o remendo começa 1 px abaixo do topo, para a linha de cima da própria janela continuar inteira
        // (a cópia das abas também começa ali). Dos lados vai até a borda: 1 px para dentro deixava aparecer uma coluna
        // da barra original, de outra cor (um risco claro na lateral e o canto parecendo reto).
        // O arco tem o MESMO centro do canto da janela (antes ficava 1 px abaixo: dois contornos, canto áspero); a
        // linha de cima fica de fora por recorte. Com o recorte da DLL não há linha da janela para mostrar: e = 0.
        float e = in.propria && in.acima && !in.zoom && !in.raio ? 1.f / sc : 0.f;
        sk->BeginFigure({ 0, HT }, D2D1_FIGURE_BEGIN_FILLED);
        sk->AddLine({ 0, R });
        if (R > 0) sk->AddArc(D2D1::ArcSegment({ R, 0 }, { R, R }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine({ W - R, 0 });
        if (R > 0) sk->AddArc(D2D1::ArcSegment({ W, R }, { R, R }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine({ W, HT });
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        sk->Close(); sk->Release();
        rt->PushAxisAlignedClip(D2D1::RectF(0, e, W, HT), D2D1_ANTIALIAS_MODE_ALIASED);
        rt->FillGeometry(g, b);
        rt->PopAxisAlignedClip();
        g->Release();
        if (!in.zoom) {  // o fio de 1 px da borda da janela continua pela faixa (sem ele a faixa parecia 1 px mais larga que a janela)
            float hp = 0.5f / sc;
            auto fio = [&](COLORREF c, bool esq) {
                if (c == CLR_INVALID) return;
                ID2D1PathGeometry* pg = nullptr; ID2D1GeometrySink* ps = nullptr;
                g_d2d->CreatePathGeometry(&pg);
                if (!pg || FAILED(pg->Open(&ps))) { if (pg) pg->Release(); return; }
                if (esq) {
                    ps->BeginFigure({ hp, HT }, D2D1_FIGURE_BEGIN_HOLLOW);
                    ps->AddLine({ hp, R });
                    if (R > 0) ps->AddArc(D2D1::ArcSegment({ R, hp }, { R - hp, R - hp }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                } else {
                    ps->BeginFigure({ W - hp, HT }, D2D1_FIGURE_BEGIN_HOLLOW);
                    ps->AddLine({ W - hp, R });
                    if (R > 0) ps->AddArc(D2D1::ArcSegment({ W - R, hp }, { R - hp, R - hp }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                }
                ps->EndFigure(D2D1_FIGURE_END_OPEN);
                ps->Close(); ps->Release();
                ID2D1SolidColorBrush* fb = nullptr;
                rt->CreateSolidColorBrush(D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, 1), &fb);
                if (fb) { rt->PushAxisAlignedClip(D2D1::RectF(0, e, W, HT), D2D1_ANTIALIAS_MODE_ALIASED); rt->DrawGeometry(pg, fb, 1.f / sc); rt->PopAxisAlignedClip(); fb->Release(); }
                pg->Release();
            };
            fio(in.bordaE, true);
            fio(in.bordaD, false);
        }
        if (in.propria && !in.acima && W > PATCH_L + PATCH_R) {  // remendos: o meio fica transparente (e passa os cliques)
            rt->PushAxisAlignedClip(D2D1::RectF(PATCH_L, 0, W - PATCH_R, H), D2D1_ANTIALIAS_MODE_ALIASED);
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            rt->PopAxisAlignedClip();
        }
        // os três botões
        static const D2D1_COLOR_F on_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.341f), D2D1::ColorF(0.996f, 0.737f, 0.18f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* on = semaforo::Pick(on_c, semaforo::FILL);
        bool can[3] = { true, (in.estilo & WS_MINIMIZEBOX) != 0, (in.estilo & WS_MAXIMIZEBOX) != 0 };
        for (int k = 0; k < 3; k++) {
            D2D1_POINT_2F c{ LX0 + k * LPITCH, H / 2 };
            bool lit = (in.ativa || in.hover) && can[k];
            D2D1_COLOR_F col = lit ? on[k] : (escura ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.18f));
            if (lit && in.press == k) { col.r *= 0.78f; col.g *= 0.78f; col.b *= 0.78f; }
            b->SetColor(col);
            rt->FillEllipse(D2D1::Ellipse(c, LR, LR), b);
            semaforo::Gloss(rt, c, LR, b);
            b->SetColor(D2D1::ColorF(0, 0, 0, 0.12f));
            rt->DrawEllipse(D2D1::Ellipse(c, LR - 0.25f, LR - 0.25f), b, 0.5f);
            if (!in.hover || !can[k]) continue;
            // Os símbolos como no macOS: cada um num tom escuro da cor do próprio botão (não preto), o "x" e o traço com
            // pontas redondas, e no verde os dois triângulos da tela cheia (◤◢); o "+" só aparece no Mac com Option.
            static const D2D1_COLOR_F tinta[3] = { D2D1::ColorF(0.30f, 0.0f, 0.01f, 0.82f), D2D1::ColorF(0.60f, 0.34f, 0.0f, 0.90f), D2D1::ColorF(0.0f, 0.40f, 0.0f, 0.88f) };
            b->SetColor(tinta[k]);
            static ID2D1StrokeStyle* redondo;
            if (!redondo) g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &redondo);
            if (k == 0) { float a = 2.9f; rt->DrawLine({ c.x - a, c.y - a }, { c.x + a, c.y + a }, b, 1.35f, redondo); rt->DrawLine({ c.x - a, c.y + a }, { c.x + a, c.y - a }, b, 1.35f, redondo); }
            else if (k == 1) rt->DrawLine({ c.x - 3.9f, c.y }, { c.x + 3.9f, c.y }, b, 1.6f, redondo);
            else {
                float q = 3.1f, cat = 4.9f;
                ID2D1PathGeometry* tg = nullptr; ID2D1GeometrySink* ts = nullptr;
                g_d2d->CreatePathGeometry(&tg);
                if (tg && SUCCEEDED(tg->Open(&ts))) {
                    ts->BeginFigure({ c.x - q, c.y - q }, D2D1_FIGURE_BEGIN_FILLED);
                    ts->AddLine({ c.x - q + cat, c.y - q }); ts->AddLine({ c.x - q, c.y - q + cat });
                    ts->EndFigure(D2D1_FIGURE_END_CLOSED);
                    ts->BeginFigure({ c.x + q, c.y + q }, D2D1_FIGURE_BEGIN_FILLED);
                    ts->AddLine({ c.x + q - cat, c.y + q }); ts->AddLine({ c.x + q, c.y + q - cat });
                    ts->EndFigure(D2D1_FIGURE_END_CLOSED);
                    ts->Close(); ts->Release();
                    rt->FillGeometry(tg, b);
                }
                if (tg) tg->Release();
            }
        }
        // título centralizado
        if (g_tf && !in.titulo.empty() && W > 180 && !in.propria) {
            b->SetColor(escura ? D2D1::ColorF(1, 1, 1, in.ativa ? 0.86f : 0.42f) : D2D1::ColorF(0, 0, 0, in.ativa ? 0.85f : 0.40f));
            rt->DrawTextW(in.titulo.c_str(), (UINT32)in.titulo.size(), g_tf, D2D1::RectF(80, 0, W - 80, H), b);
        }
        b->Release();
        rt->EndDraw();
        rt->Release();
    }
    if (!(in.propria && in.acima)) {  // o furo (3 px, transparente): por ele se lê a cor da barra do Windows que está embaixo
        POINT f = Furo(in);
        int fx = f.x - in.r.left, fy = f.y - in.r.top;
        auto* px = (UINT32*)bits;
        for (int y = fy - 1; y <= fy + 1; y++)
            for (int x = fx - 1; x <= fx + 1; x++)
                if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = 0;
    }
    POINT dst{ in.r.left, in.r.top }, src{ 0, 0 };
    SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(in.faixa, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    in.pintado = sz;
    in.sujo = false;
}

// Modo deslocado: a linha de cima da janela, redesenhada na faixa PATCH_L à direita (sem os botões do Windows)
// Chrome: o que há no começo da linha das abas DESTA janela. O botão "buscar abas" (a setinha) nem sempre está na ponta
// esquerda: no Chrome do PC do usuário (157, perfil antigo) ele não fica na linha das abas, e a faixa cortava 36 pt
// contando com ele: comia o começo do primeiro grupo/aba ("WORK" virava "ORK"). A própria janela diz (UI Automation,
// classes TabSearchButton / TabStrip), andando só pelos elementos que tocam a linha de cima (nunca o conteúdo da página).
// Devolve false se não deu para saber (fica como estava).
IUIAutomation* g_uia;
bool LerLinhaChrome(const Info& in, bool& seta, int& ini) {
    if (!g_uia) {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia))) || !g_uia) return false;
        IUIAutomation2* u2 = nullptr;
        if (SUCCEEDED(g_uia->QueryInterface(IID_PPV_ARGS(&u2))) && u2) { u2->put_ConnectionTimeout(400); u2->put_TransactionTimeout(400); u2->Release(); }
    }
    IUIAutomationCacheRequest* cr = nullptr;
    IUIAutomationTreeWalker* tw = nullptr;
    IUIAutomationElement* raiz = nullptr;
    g_uia->CreateCacheRequest(&cr);
    g_uia->get_RawViewWalker(&tw);
    bool ok = false, achouLinha = false;
    seta = false; ini = INT_MAX;
    int setaDir = 0;
    if (cr && tw) {
        cr->AddProperty(UIA_ClassNamePropertyId);
        cr->AddProperty(UIA_BoundingRectanglePropertyId);
        if (SUCCEEDED(g_uia->ElementFromHandleBuildCache(in.alvo, cr, &raiz)) && raiz) {
            std::vector<IUIAutomationElement*> fila{ raiz };
            raiz = nullptr;
            float sc = Sc(in.alvo);
            int vistos = 0;
            for (size_t k = 0; k < fila.size() && vistos < 500; k++) {
                IUIAutomationElement* f = nullptr;
                tw->GetFirstChildElementBuildCache(fila[k], cr, &f);
                while (f && vistos < 500) {
                    vistos++;
                    BSTR cls = nullptr; RECT r{};
                    f->get_CachedClassName(&cls);
                    f->get_CachedBoundingRectangle(&r);
                    std::wstring c = cls ? cls : L"";
                    if (cls) SysFreeString(cls);
                    bool naLinha = r.bottom > r.top && r.top < in.r.bottom - 2 && r.bottom > in.r.top + 2 && r.right > r.left;
                    bool desce = false;
                    if (naLinha && c != L"MacBarraTitulo") {
                        if (c == L"TabSearchButton" || c == L"TabSearchContainer") {
                            if ((r.left + r.right) / 2 < in.r.left + (int)(80 * sc)) { seta = true; setaDir = std::max(setaDir, (int)r.right); }
                        } else if (c == L"TabStrip") { achouLinha = true; ini = std::min(ini, (int)r.left); }
                        else if (c != L"Tab" && c != L"TabGroupHeader" && c.find(L"WebView") == std::wstring::npos && c.find(L"Chrome_Render") == std::wstring::npos) desce = true;
                    }
                    IUIAutomationElement* prox = nullptr;
                    tw->GetNextSiblingElementBuildCache(f, cr, &prox);
                    if (desce) fila.push_back(f); else f->Release();
                    f = prox;
                }
                if (f) f->Release();
            }
            for (auto e : fila) e->Release();
            ok = achouLinha;
            if (ok) ini = std::max(0, ini - (int)in.r.left);
        }
    }
    if (cr) cr->Release();
    if (tw) tw->Release();
    return ok;
}
// O retângulo (tela) da aba ou do grupo de abas que está no ponto real t. Mesma caminhada, entrando na linha das abas.
bool RectDaAba(const Info& in, POINT t, RECT& aba) {
    if (!g_uia) { bool s; int i; LerLinhaChrome(in, s, i); if (!g_uia) return false; }
    IUIAutomationCacheRequest* cr = nullptr; IUIAutomationTreeWalker* tw = nullptr; IUIAutomationElement* raiz = nullptr;
    g_uia->CreateCacheRequest(&cr); g_uia->get_RawViewWalker(&tw);
    bool ok = false;
    if (cr && tw) {
        cr->AddProperty(UIA_ClassNamePropertyId); cr->AddProperty(UIA_BoundingRectanglePropertyId);
        if (SUCCEEDED(g_uia->ElementFromHandleBuildCache(in.alvo, cr, &raiz)) && raiz) {
            std::vector<IUIAutomationElement*> fila{ raiz };
            int vistos = 0;
            for (size_t k = 0; k < fila.size() && vistos < 900 && !ok; k++) {
                IUIAutomationElement* f = nullptr;
                tw->GetFirstChildElementBuildCache(fila[k], cr, &f);
                while (f && vistos < 900) {
                    vistos++;
                    BSTR cls = nullptr; RECT r{};
                    f->get_CachedClassName(&cls); f->get_CachedBoundingRectangle(&r);
                    std::wstring c = cls ? cls : L"";
                    if (cls) SysFreeString(cls);
                    bool naLinha = r.bottom > r.top && r.top < in.r.bottom - 2 && r.bottom > in.r.top + 2 && r.right > r.left;
                    bool desce = false;
                    if (naLinha && c != L"MacBarraTitulo") {
                        if (c == L"Tab" || c == L"TabGroupHeader") { if (PtInRect(&r, t)) { aba = r; ok = true; } }
                        else if (c.find(L"WebView") == std::wstring::npos && c.find(L"Chrome_Render") == std::wstring::npos && c.find(L"DragContext") == std::wstring::npos) desce = true;
                    }
                    IUIAutomationElement* prox = nullptr;
                    tw->GetNextSiblingElementBuildCache(f, cr, &prox);
                    if (desce && !ok) fila.push_back(f); else f->Release();
                    f = prox;
                    if (ok) break;
                }
                if (f) f->Release();
            }
            for (auto e : fila) e->Release();
        }
    }
    if (cr) cr->Release();
    if (tw) tw->Release();
    return ok;
}
void ConferirLinhaChrome(Info& in) {
    if (!in.chrome || !in.propria || !in.acima || !IsWindowVisible(in.faixa) || IsHungAppWindow(in.alvo)) return;
    DWORD now = GetTickCount();
    if (in.linhaEm && (int)(in.linhaEm - now) > 0) return;
    in.linhaEm = (now + 8000) | 1;
    bool seta = false; int ini = 0;
    if (!LerLinhaChrome(in, seta, ini)) return;
    // Com a setinha à esquerda, a linha das abas começa ~29 pt para dentro (a primeira aba, 7 pt depois: as abas se
    // sobrepõem); sem ela, começa na borda (0 ampliada, ~8 normal). O nome da classe do botão muda entre versões:
    // vale o espaço que ele ocupa.
    float corte = ini / Sc(in.alvo);
    seta = seta || corte > 20;
    corte = seta ? std::max(corte + 7, 21.f) : std::min(corte, 8.f);
    if (std::fabs(corte - in.corte) > 0.6f) {
        Log(L"barras: Chrome %p: linha das abas começa em %d px, setinha na esquerda=%d (corte %.1f -> %.1f pt)", in.alvo, ini, seta, in.corte, corte);
        in.corte = corte; in.sujo = true; in.miniSrc = in.miniDst = RECT{};
    }
}

void Miniatura(Info& in) {
    bool quer = in.propria && in.acima && IsWindowVisible(in.faixa);
    if (!quer) { if (in.mini) { DwmUnregisterThumbnail(in.mini); in.mini = nullptr; in.miniSrc = in.miniDst = RECT{}; } if (in.mini2) { DwmUnregisterThumbnail(in.mini2); in.mini2 = nullptr; } if (in.mini3) { DwmUnregisterThumbnail(in.mini3); in.mini3 = nullptr; } return; }
    if (!in.mini && FAILED(DwmRegisterThumbnail(in.faixa, in.alvo, &in.mini))) { in.mini = nullptr; return; }
    float sc = Sc(in.alvo);
    RECT wr{}; GetWindowRect(in.alvo, &wr);
    int w = in.r.right - in.r.left, h = in.r.bottom - in.r.top;
    int pl = (int)std::lround(PATCH_L * sc), pr = (int)std::lround(in.direita * sc);
    // a cópia começa 8 pt para dentro: sem a borda clara e o canto arredondado da janela (apareciam como um risco
    // vertical logo depois das bolinhas)
    int c = (int)std::lround(in.corte * sc);
    RECT src{ in.r.left - wr.left + c, in.r.top - wr.top, in.r.right - wr.left - pr, in.r.bottom - wr.top };
    src.top += 1;
    RECT dst{ pl, 1, pl + (w - pr - c), h };
    if (in.corte > 20) {
        if (!in.mini3 && FAILED(DwmRegisterThumbnail(in.faixa, in.alvo, &in.mini3))) in.mini3 = nullptr;
        if (in.mini3) {
            int f8 = (int)std::lround(8 * sc), f7 = (int)std::lround(7 * sc);
            DWM_THUMBNAIL_PROPERTIES t3{};
            t3.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            f8 = (int)std::lround(6 * sc); f7 = (int)std::lround(6 * sc);  // 6 x 6 pt: a curva sem o canto do botão vizinho
            t3.rcSource = { in.r.left - wr.left + c - f8, in.r.bottom - wr.top - f7, in.r.left - wr.left + c, in.r.bottom - wr.top };
            t3.rcDestination = { pl - f8, h - f7, pl, h };
            t3.fVisible = TRUE; t3.opacity = 255; t3.fSourceClientAreaOnly = FALSE;
            DwmUpdateThumbnailProperties(in.mini3, &t3);
        }
    } else {
        if (in.mini3) { DwmUnregisterThumbnail(in.mini3); in.mini3 = nullptr; }
        if (in.mini2) { DwmUnregisterThumbnail(in.mini2); in.mini2 = nullptr; }
    }
    if (in.corte > 20) {  // Chrome: a setinha "buscar abas" na ponta direita da linha, como no Chrome do Mac
        if (!in.mini2 && FAILED(DwmRegisterThumbnail(in.faixa, in.alvo, &in.mini2))) in.mini2 = nullptr;
        if (in.mini2) {
            int b8 = (int)std::lround(8 * sc), m = (int)std::lround(7 * sc);
            DWM_THUMBNAIL_PROPERTIES t2{};
            t2.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            // só as linhas do botão (5 pt abaixo do topo até 8 pt acima do pé): o pé trazia junto a curva da primeira aba
            // (um dentinho branco na ponta direita) e o topo passaria do canto arredondado
            int ct = (int)std::lround(5 * sc), cb = (int)std::lround(8 * sc);
            t2.rcSource = { in.r.left - wr.left + b8, in.r.top - wr.top + ct, in.r.left - wr.left + c, in.r.bottom - wr.top - cb };
            t2.rcDestination = { w - m - (c - b8), ct, w - m, h - cb };
            t2.fVisible = TRUE; t2.opacity = 255; t2.fSourceClientAreaOnly = FALSE;
            DwmUpdateThumbnailProperties(in.mini2, &t2);
        }
    }
    if (EqualRect(&src, &in.miniSrc) && EqualRect(&dst, &in.miniDst)) return;
    DWM_THUMBNAIL_PROPERTIES tp{};
    tp.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    tp.rcSource = src; tp.rcDestination = dst; tp.fVisible = TRUE; tp.opacity = 255; tp.fSourceClientAreaOnly = FALSE;
    if (SUCCEEDED(DwmUpdateThumbnailProperties(in.mini, &tp))) { in.miniSrc = src; in.miniDst = dst; }
}

// Modo deslocado: o ponto da faixa (px) corresponde a que ponto da tela na janela de verdade? false = fora da área redesenhada
bool PontoReal(const Info& in, POINT p, POINT& tela) {
    if (!in.propria || !in.acima) return false;
    float sc = Sc(in.alvo);
    int pl = (int)std::lround(PATCH_L * sc), pr = (int)std::lround(in.direita * sc), w = in.r.right - in.r.left;
    int c = (int)std::lround(in.corte * sc);
    if (in.corte > 20) {  // a setinha do Chrome, na ponta direita
        int b8 = (int)std::lround(8 * sc), m = (int)std::lround(7 * sc), x0 = w - m - (c - b8);
        if (p.x >= x0 && p.x < w - m) { tela = { in.r.left + b8 + (p.x - x0), in.r.top + p.y }; return true; }
    }
    if (p.x < pl || p.x >= pl + (w - pr - c)) return false;
    tela = { in.r.left + p.x - pl + c, in.r.top + p.y };
    return true;
}
// O que a janela diz que há naquele ponto (HTCAPTION = barra vazia, HTCLIENT = aba/botão dela...)
LRESULT HitReal(const Info& in, POINT tela) {
    DWORD_PTR r = HTCAPTION;
    SendMessageTimeoutW(in.alvo, WM_NCHITTEST, 0, MAKELPARAM(tela.x, tela.y), SMTO_ABORTIFHUNG, 60, &r);
    return (LRESULT)r;
}
// Repassa uma mensagem de mouse para a janela, no ponto de verdade
void Repassar(const Info& in, UINT m, WPARAM w, POINT tela) {
    HWND dest = in.alvo;
    if (in.acima == 2)  // não Chromium: a janela filha mais funda que está no ponto
        for (int i = 0; i < 8; i++) {
            POINT c = tela; ScreenToClient(dest, &c);
            HWND ch = ChildWindowFromPointEx(dest, c, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
            if (!ch || ch == dest) break;
            dest = ch;
        }
    POINT c = tela; ScreenToClient(dest, &c);
    PostMessageW(dest, m, w, MAKELPARAM(c.x, c.y));
}

// Programas que ignoram mensagens de mouse repassadas (WinUI, Terminal, Bloco de Notas): o clique de verdade.
// Só quando o botão SOLTA (clique completo): a faixa deixa o mouse passar, o ponteiro salta para o ponto real
// (76 pt à esquerda), um clique inteiro (apertar + soltar) é injetado lá, e 40 ms depois o ponteiro volta e a faixa
// volta a receber. Nunca fica botão "preso": o aperto injetado vem sempre com o soltar junto.
struct Salto { HWND faixa = nullptr; int dx = 0; } g_salto;
void Saltar(Info& in, POINT tela, bool direito) {
    if (g_salto.faixa) return;
    POINT c; GetCursorPos(&c);
    g_salto = { in.faixa, c.x - tela.x };
    SetWindowLongW(in.faixa, GWL_EXSTYLE, GetWindowLongW(in.faixa, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    SetForegroundWindow(in.alvo);
    // Ir ao ponto real, apertar, soltar e VOLTAR vão num pacote só de entrada: o ponteiro passa uns microssegundos
    // lá e nunca chega a ser desenhado fora do lugar. Antes ele era levado com SetCursorPos e só voltava 40 ms depois
    // (2 a 3 quadros): o usuário via o mouse pular para a esquerda a cada clique na linha das abas, e a volta
    // "relativa" somava qualquer movimento da mão nesse meio tempo (vídeo do usuário de 12/11/2026 14:40).
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    auto nx = [&](int x) { return (LONG)((x - vx + 0.5) * 65536.0 / vw); };
    auto ny = [&](int y) { return (LONG)((y - vy + 0.5) * 65536.0 / vh); };
    INPUT i[4]{};
    for (auto& e : i) e.type = INPUT_MOUSE;
    i[0].mi.dx = nx(tela.x); i[0].mi.dy = ny(tela.y);
    i[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    i[1].mi.dwFlags = direito ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    i[2].mi.dwFlags = direito ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    i[3].mi.dx = nx(c.x); i[3].mi.dy = ny(c.y);
    i[3].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(4, i, sizeof(INPUT));
    SetTimer(in.faixa, 7, 40, nullptr);
}
void FimDoSalto(HWND faixa) {
    KillTimer(faixa, 7);
    if (g_salto.faixa != faixa) return;
    SetWindowLongW(faixa, GWL_EXSTYLE, GetWindowLongW(faixa, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
    g_salto = Salto{};
}
// O clique completo no ponto real. Chromium (Chrome, Edge, Electron): por mensagens, SEM tocar no ponteiro do usuário.
// O clique inteiro (mover, apertar, soltar) entra de uma vez na fila da janela quando o botão solta; o "mouse saiu" que
// o Chrome recebe (o ponteiro de verdade está sobre a faixa) só chega depois do soltar, e o clique vale. O que falhava
// com mouse real era repassar o apertar na hora e o soltar depois: no intervalo o Chrome cancelava. Os outros programas
// (WinUI, Terminal, Bloco de Notas) ignoram mensagens repassadas: neles o ponteiro vai e volta num pacote só (Saltar).
// barras_clique=real no ajustes.txt força o Saltar em todos.
void CliqueReal(Info& in, POINT tela, bool direito) {
    if (in.acima != 1 || shell::Setting(L"barras_clique", L"mensagem") == L"real") { Saltar(in, tela, direito); return; }
    SetForegroundWindow(in.alvo);
    Repassar(in, WM_MOUSEMOVE, 0, tela);
    Repassar(in, direito ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, direito ? MK_RBUTTON : MK_LBUTTON, tela);
    Repassar(in, direito ? WM_RBUTTONUP : WM_LBUTTONUP, 0, tela);
}

// O ponto da faixa (px) está em cima da setinha "buscar abas" do Chrome (levada para a ponta direita)?
bool NaSeta(const Info& in, POINT p) {
    if (!in.propria || !in.acima || in.corte <= 20) return false;
    float sc = Sc(in.alvo);
    int c = (int)std::lround(in.corte * sc), b8 = (int)std::lround(8 * sc), m = (int)std::lround(7 * sc), w = in.r.right - in.r.left;
    return p.x >= w - m - (c - b8) && p.x < w - m;
}
// O menu que o Chrome abre embaixo da setinha nasce onde o botão de verdade está (à esquerda). Por 1,5 s depois do
// clique, a janela dele (do mesmo processo, logo abaixo da linha das abas, encostada na esquerda) é levada para
// baixo da setinha, alinhada pela direita, como no Chrome do Mac.
struct MenuSeta { HWND alvo = nullptr; int tentativas = 0; } g_menuSeta;
void LevarMenuDaSeta(HWND faixa) {
    auto it = g_j.find(g_menuSeta.alvo);
    if (it == g_j.end() || ++g_menuSeta.tentativas > 50) { KillTimer(faixa, 8); g_menuSeta = MenuSeta{}; return; }
    Info& in = it->second;
    float sc = Sc(in.alvo);
    DWORD pid = 0; GetWindowThreadProcessId(in.alvo, &pid);
    struct F { DWORD pid; HWND alvo, achou; RECT faixa; float sc; } f{ pid, in.alvo, nullptr, in.r, sc };
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        auto* f = (F*)l;
        if (w == f->alvo || !IsWindowVisible(w)) return TRUE;
        DWORD p = 0; GetWindowThreadProcessId(w, &p);
        if (p != f->pid) return TRUE;
        if (!(GetWindowLongW(w, GWL_STYLE) & WS_POPUP)) return TRUE;
        RECT r; GetWindowRect(w, &r);
        int ww = r.right - r.left, hh = r.bottom - r.top;
        if (ww < (int)(180 * f->sc) || ww > (int)(520 * f->sc) || hh < (int)(60 * f->sc)) return TRUE;
        if (r.top < f->faixa.top || r.top > f->faixa.bottom + (int)(24 * f->sc)) return TRUE;  // logo abaixo da linha das abas
        f->achou = w;
        return FALSE;
    }, (LPARAM)&f);
    if (!f.achou) return;
    RECT r; GetWindowRect(f.achou, &r);
    int x = in.r.right - (int)std::lround(7 * sc) - (r.right - r.left);
    if (std::abs(r.left - x) > 2) SetWindowPos(f.achou, nullptr, x, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// Cantos de baixo suavizados (só com o recorte da DLL): o SetWindowRgn corta em pixel inteiro (serrilhado); um arco de
// 3 px, suavizado, na cor da janela junto a cada canto, cobre o serrilhado. Janela em camadas que não recebe cliques.
void Cantos(Info& in) {
    bool quer = in.raio > 0 && !in.zoom && IsWindowVisible(in.faixa) && in.vis.right - in.vis.left > 4 * in.raio;
    if (!quer) { if (in.cantos && IsWindowVisible(in.cantos)) ShowWindow(in.cantos, SW_HIDE); return; }
    if (!in.cantos) {
        static bool reg;
        if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = shell::inst; wc.lpszClassName = L"MacBarraCantos"; RegisterClassW(&wc); reg = true; }
        in.cantos = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"MacBarraCantos", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, shell::inst, nullptr);
        if (!in.cantos) return;
    }
    float sc = Sc(in.alvo);
    int r = in.raio, w = in.vis.right - in.vis.left, h = r + 2;
    {   // cor da janela logo acima de cada canto
        POINT pe{ in.vis.left + 2, in.vis.bottom - r - 3 }, pd{ in.vis.right - 3, in.vis.bottom - r - 3 };
        HDC dc = GetDC(nullptr);
        HWND ae = WindowFromPoint(pe), ad = WindowFromPoint(pd);
        if (ae && GetAncestor(ae, GA_ROOT) == in.alvo) { COLORREF c = GetPixel(dc, pe.x, pe.y); if (c != CLR_INVALID) in.baixoE = c; }
        if (ad && GetAncestor(ad, GA_ROOT) == in.alvo) { COLORREF c = GetPixel(dc, pd.x, pd.y); if (c != CLR_INVALID) in.baixoD = c; }
        ReleaseDC(nullptr, dc);
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(mem); ReleaseDC(nullptr, screen); return; }
    HGDIOBJ old = SelectObject(mem, bmp);
    ID2D1DCRenderTarget* rt = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (g_d2d && SUCCEEDED(g_d2d->CreateDCRenderTarget(&props, &rt))) {
        RECT br{ 0, 0, w, h };
        rt->BindDC(mem, &br);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        auto arco = [&](COLORREF c, bool esq) {
            if (c == CLR_INVALID) return;
            ID2D1PathGeometry* pg = nullptr; ID2D1GeometrySink* ps = nullptr;
            g_d2d->CreatePathGeometry(&pg);
            if (!pg || FAILED(pg->Open(&ps))) { if (pg) pg->Release(); return; }
            float R = (float)r, t = 1.5f, y0 = 2;  // o canto começa 2 px abaixo do topo desta janela; traço de 3 px por dentro do arco
            if (esq) {
                ps->BeginFigure({ t, y0 }, D2D1_FIGURE_BEGIN_HOLLOW);
                ps->AddArc(D2D1::ArcSegment({ R, y0 + R - t }, { R - t, R - t }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            } else {
                ps->BeginFigure({ w - t, y0 }, D2D1_FIGURE_BEGIN_HOLLOW);
                ps->AddArc(D2D1::ArcSegment({ w - R, y0 + R - t }, { R - t, R - t }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            }
            ps->EndFigure(D2D1_FIGURE_END_OPEN);
            ps->Close(); ps->Release();
            ID2D1SolidColorBrush* fb = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, 1), &fb);
            if (fb) { rt->DrawGeometry(pg, fb, 3.f); fb->Release(); }
            pg->Release();
        };
        arco(in.baixoE, true);
        arco(in.baixoD, false);
        rt->EndDraw();
        rt->Release();
    }
    POINT dst{ in.vis.left, in.vis.bottom - h }, src{ 0, 0 };
    SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(in.cantos, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    SetWindowPos(in.cantos, in.faixa, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);  // logo abaixo da faixa, acima da janela
}

// A faixa logo acima da janela na ordem Z (e na mesma faixa "sempre no topo" que ela)
void Ordem(Info& in) {
    if (GetWindow(in.alvo, GW_HWNDPREV) == in.faixa) return;
    const UINT F = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (GetWindow(in.faixa, GW_OWNER) != in.alvo) {
        // Janela de administrador: o Windows não deixa o explorer ser dono dela, nem usar a janela (ou as que são
        // dela, como a do teclado/IME, que fica colada acima) como referência de ordem: tudo falha com "acesso
        // negado" e a faixa ficava embaixo da janela (barra do Windows à mostra) ou solta por cima das outras.
        // A faixa vai para logo abaixo da primeira janela de OUTRO programa que estiver acima dela.
        HWND w = GetWindow(in.alvo, GW_HWNDPREV);
        while (w && w != in.faixa && GetAncestor(w, GA_ROOTOWNER) == in.alvo) w = GetWindow(w, GW_HWNDPREV);
        if (w == in.faixa) return;
        for (; w; w = GetWindow(w, GW_HWNDPREV)) if (SetWindowPos(in.faixa, w, 0, 0, 0, 0, F)) return;
        SetWindowPos(in.faixa, (GetWindowLongW(in.alvo, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, F);
        return;
    }
    // A faixa fica na mesma camada da janela. Antes ela ia para logo abaixo da janela de cima, que podia ser "sempre no
    // topo" (Dock, barra de menus): a faixa virava "sempre no topo" e, no passo de baixo, a janela do app também
    // (Chrome, Claude, Finder ficavam por cima de tudo e os outros apps não vinham mais para a frente; Win+D repetido)
    const bool alvoTop = (GetWindowLongW(in.alvo, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (((GetWindowLongW(in.faixa, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != alvoTop)
        SetWindowPos(in.faixa, alvoTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, F);
    HWND prev = GetWindow(in.alvo, GW_HWNDPREV);
    if (prev && !alvoTop && (GetWindowLongW(prev, GWL_EXSTYLE) & WS_EX_TOPMOST)) prev = nullptr;  // a 1ª da camada comum
    SetWindowPos(in.faixa, prev ? prev : HWND_TOP, 0, 0, 0, 0, F);
    if (GetWindow(in.alvo, GW_HWNDPREV) == in.faixa) return;
    // não ficou colada: põe a faixa embaixo da janela e a janela embaixo da faixa (sem esperar o programa)
    if (((GetWindowLongW(in.faixa, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != alvoTop) return;  // camadas diferentes: não arrasta o app
    SetWindowPos(in.faixa, in.alvo, 0, 0, 0, 0, F);
    SetWindowPos(in.alvo, in.faixa, 0, 0, 0, 0, F | SWP_ASYNCWINDOWPOS);
}

// Lê a cor da barra do Windows pelo furo (só quando nada está na frente daquele ponto)
void LerCor(Info& in) {
    if (!IsWindowVisible(in.faixa)) return;
    POINT f = Furo(in);
    bool desloc = in.propria && in.acima;
    if (desloc) {
        // Modo deslocado: a cor vem da própria cópia ao vivo, nos últimos px dela (a barra do programa logo à
        // esquerda dos botões do Windows). Pelo furo lia-se a área dos botões, que o programa NÃO escurece quando
        // abre um diálogo por cima da janela: as pontas da faixa ficavam claras com o resto escuro (vídeo do
        // usuário de 12/11/2026).
        float sc = Sc(in.alvo);
        int w = in.r.right - in.r.left;
        int fim = (int)std::lround(PATCH_L * sc) + (w - (int)std::lround(in.direita * sc) - (int)std::lround(in.corte * sc));
        f = { in.r.left + fim - 3, in.r.top + (int)std::lround(5 * sc) };
    }
    HWND at = WindowFromPoint(f);
    if (!at || (desloc ? at != in.faixa : GetAncestor(at, GA_ROOT) != in.alvo)) return;
    HDC dc = GetDC(nullptr);
    COLORREF c = GetPixel(dc, f.x, f.y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) return;
    if (desloc) { if (c != in.cor) { in.cor = c; in.sujo = true; in.corAte = GetTickCount() + 700; } }
    else if (c == in.lida && c != in.cor) { in.cor = c; in.sujo = true; }
    in.lida = c;
    // o fio da borda: a coluna de 1 px de cada lado, 3 px abaixo da faixa (ali a janela está à mostra)
    if (!in.zoom) {
        int y = in.r.bottom + 3;
        POINT pe{ in.r.left, y }, pd{ in.r.right - 1, y };
        HDC d2 = GetDC(nullptr);
        HWND ae = WindowFromPoint(pe), ad = WindowFromPoint(pd);
        if (ae && GetAncestor(ae, GA_ROOT) == in.alvo) { COLORREF b = GetPixel(d2, pe.x, pe.y); if (b != CLR_INVALID && b != in.bordaE) { in.bordaE = b; in.sujo = true; } }
        if (ad && GetAncestor(ad, GA_ROOT) == in.alvo) { COLORREF b = GetPixel(d2, pd.x, pd.y); if (b != CLR_INVALID && b != in.bordaD) { in.bordaD = b; in.sujo = true; } }
        ReleaseDC(nullptr, d2);
    }
}

void Atualizar(HWND alvo) {
    auto it = g_j.find(alvo);
    RECT bar{}; bool zoom = false;
    bool propria = false; int acima = 0;
    bool ok = Elegivel(alvo, bar, zoom, propria, acima);
    if (!ok) {
        if (it != g_j.end()) {
            if (!IsWindow(alvo)) { if (it->second.mini) DwmUnregisterThumbnail(it->second.mini); if (it->second.cantos) DestroyWindow(it->second.cantos); DestroyWindow(it->second.faixa); g_j.erase(it); }
            else if (IsWindowVisible(it->second.faixa)) { ShowWindow(it->second.faixa, SW_HIDE); Miniatura(it->second); Cantos(it->second); }
        }
        return;
    }
    if (it == g_j.end()) {
        Info in;
        in.alvo = alvo;
        in.faixa = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLS, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, shell::inst, nullptr);
        if (!in.faixa) return;
        SetWindowLongPtrW(in.faixa, GWLP_USERDATA, (LONG_PTR)alvo);
        // A faixa passa a ser "da" janela (dona = a janela do programa): o Windows mantém a janela de um dono SEMPRE
        // acima dele. Antes a ordem era corrigida depois de cada evento: a cada clique na janela ela vinha para cima
        // da faixa por alguns milésimos e a barra original aparecia (vídeo do usuário de 12/11/2026).
        SetWindowLongPtrW(in.faixa, GWLP_HWNDPARENT, (LONG_PTR)alvo);
        {   // Chrome: como no Chrome do Mac, a primeira aba começa logo depois das bolinhas. O botão "buscar abas" (a
            // setinha que o Chrome do Windows põe à esquerda; no Mac fica à direita) sai da cópia. Ctrl+Shift+A ainda abre.
            DWORD pid = 0; GetWindowThreadProcessId(alvo, &pid);
            wchar_t exe[MAX_PATH] = {}; DWORD n = MAX_PATH;
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hp) { QueryFullProcessImageNameW(hp, 0, exe, &n); CloseHandle(hp); }
            const wchar_t* b = wcsrchr(exe, L'\\');
            // Discord: os botões de janela dele são menores (3 x 36 px + o separador): com os 138 pt do Chrome a cópia cortava
            // o ícone de ajuda e sobrava um vazio na ponta direita (relato do usuário de 12/11/2026)
            if (b && (!_wcsicmp(b + 1, L"Discord.exe") || !_wcsicmp(b + 1, L"DiscordPTB.exe") || !_wcsicmp(b + 1, L"DiscordCanary.exe"))) in.direita = 114;
            if (b && !_wcsicmp(b + 1, L"chrome.exe") && shell::Setting(L"barras_chrome_seta", L"direita") != L"esquerda") { in.corte = 36; in.chrome = true; in.linhaEm = (GetTickCount() + 700) | 1; }  // o botão ocupa de 8 a 36 pt; a aba (com a curva dela) começa em 36
        }
        it = g_j.emplace(alvo, std::move(in)).first;
    }
    Info& in = it->second;
    wchar_t t[256] = {}; InternalGetWindowText(alvo, t, 256);
    LONG st = GetWindowLongW(alvo, GWL_STYLE);
    bool ativa = Ativa(alvo);
    if (in.titulo != t) { in.titulo = t; in.sujo = true; }
    if (in.estilo != st) { in.estilo = st; in.sujo = true; }
    if (in.ativa != ativa) { in.ativa = ativa; in.sujo = true; }
    if (in.zoom != zoom) { in.zoom = zoom; in.sujo = true; if (in.chrome) in.linhaEm = (GetTickCount() + 400) | 1; }
    if (in.propria != propria) { in.propria = propria; in.cor = in.lida = CLR_INVALID; in.sujo = true; }
    if (in.acima != acima) { in.acima = acima; in.sujo = true; }
    bool moveu = !EqualRect(&bar, &in.r);
    bool tamanho = bar.right - bar.left != in.pintado.cx || bar.bottom - bar.top != in.pintado.cy;
    in.r = bar;
    {   // recorte da DLL: raio e a parte visível da janela (área do programa), para os cantos de baixo
        int raio = in.zoom ? 0 : (int)(INT_PTR)GetPropW(alvo, L"MacJanelaCanto");
        if (raio != in.raio) { in.raio = raio; in.sujo = true; }
        RECT wr{}; GetWindowRect(alvo, &wr);
        int bi = std::max(0, (int)(INT_PTR)GetPropW(alvo, L"MacJanelaCantoB") - 1);  // borda invisível (a DLL mediu)
        in.vis = { wr.left + bi, wr.top, wr.right - bi, wr.bottom - bi };
    }
    if (in.sujo || tamanho) Pintar(in);
    else if (moveu) SetWindowPos(in.faixa, nullptr, bar.left, bar.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!IsWindowVisible(in.faixa)) ShowWindow(in.faixa, SW_SHOWNOACTIVATE);
    Miniatura(in);
    Ordem(in);
    Cantos(in);
}

void Todas() {
    std::vector<HWND> v;
    for (auto& [h, in] : g_j) v.push_back(h);
    for (HWND h : v) Atualizar(h);
    for (auto& [h, in] : g_j) { LerCor(in); if (in.sujo && IsWindowVisible(in.faixa)) Pintar(in); }
}

void Varrer() {  // janelas que já estavam abertas
    EnumWindows([](HWND h, LPARAM) -> BOOL { Atualizar(h); return TRUE; }, 0);
}

HWND g_seguir;          // janela sendo movida/redimensionada agora (a faixa segue quadro a quadro)
DWORD g_seguirDesde;

void CALLBACK Evento(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    if (ev == EVENT_OBJECT_DESTROY) {
        auto it = g_j.find(h);
        if (it != g_j.end()) { if (it->second.mini) DwmUnregisterThumbnail(it->second.mini); if (it->second.cantos) DestroyWindow(it->second.cantos); DestroyWindow(it->second.faixa); g_j.erase(it); }
        return;
    }
    if (ev == EVENT_SYSTEM_FOREGROUND) { Atualizar(h); Todas(); return; }
    if (ev == EVENT_OBJECT_LOCATIONCHANGE || ev == EVENT_OBJECT_NAMECHANGE || ev == EVENT_OBJECT_REORDER) {
        if (g_j.count(h)) Atualizar(h);  // só as que já têm faixa (o resto entra ao aparecer ou no relógio)
        return;
    }
    if (ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_HIDE || ev == EVENT_OBJECT_CLOAKED || ev == EVENT_OBJECT_UNCLOAKED ||
        ev == EVENT_SYSTEM_MINIMIZESTART || ev == EVENT_SYSTEM_MINIMIZEEND || ev == EVENT_SYSTEM_MOVESIZESTART || ev == EVENT_SYSTEM_MOVESIZEEND) {
        // movendo/redimensionando pela própria janela: a faixa acompanha a cada quadro da tela (laço da thread)
        if (ev == EVENT_SYSTEM_MOVESIZESTART && g_j.count(h)) { g_seguir = h; g_seguirDesde = GetTickCount(); }
        if (ev == EVENT_SYSTEM_MOVESIZEEND && g_seguir == h) g_seguir = nullptr;
        Atualizar(h);
    }
}

// arrasto em curso (um por vez: o mouse é um só)
struct Arrasto {
    HWND alvo = nullptr;
    int modo = 0;        // 0 nada, 1 botão apertado (ainda não andou), 2 movendo, 3 redimensionando
    int borda = HTCAPTION;
    POINT ini{};         // cursor no começo (tela)
    RECT jan{};          // retângulo da janela no começo
    POINT off{};         // faixa em relação à janela
} g_arr;

void MoverJuntas(Info& in, int x, int y) {  // janela e faixa na mesma operação
    int fx = x + g_arr.off.x, fy = y + g_arr.off.y;
    if (IsHungAppWindow(in.alvo)) {  // programa travado: não espera por ele
        SetWindowPos(in.alvo, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        SetWindowPos(in.faixa, nullptr, fx, fy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        HDWP d = BeginDeferWindowPos(2);
        if (d) d = DeferWindowPos(d, in.alvo, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (d) d = DeferWindowPos(d, in.faixa, nullptr, fx, fy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (!d || !EndDeferWindowPos(d)) {
            SetWindowPos(in.alvo, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            SetWindowPos(in.faixa, nullptr, fx, fy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    int w = in.r.right - in.r.left, h = in.r.bottom - in.r.top;
    in.r = { fx, fy, fx + w, fy + h };
}

void Arrastar(Info& in) {
    POINT c; GetCursorPos(&c);
    int dx = c.x - g_arr.ini.x, dy = c.y - g_arr.ini.y;
    if (g_arr.modo == 1) {
        if (std::abs(dx) + std::abs(dy) < 4) return;  // clique parado não move
        if (g_arr.borda != HTCAPTION) g_arr.modo = 3;
        else {
            if (IsZoomed(in.alvo)) {  // ampliada: volta ao tamanho normal, com o cursor no mesmo ponto da barra
                float ratio = (g_arr.ini.x - in.r.left) / (float)std::max<LONG>(in.r.right - in.r.left, 1);
                int offY = g_arr.ini.y - in.r.top;
                ShowWindowAsync(in.alvo, SW_RESTORE);
                for (int i = 0; i < 40 && IsZoomed(in.alvo); i++) Sleep(10);
                RECT wr; GetWindowRect(in.alvo, &wr);
                int w = wr.right - wr.left;
                SetWindowPos(in.alvo, nullptr, c.x - (int)(w * ratio), c.y - offY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                Atualizar(in.alvo);
                GetWindowRect(in.alvo, &g_arr.jan);
                g_arr.off = { in.r.left - g_arr.jan.left, in.r.top - g_arr.jan.top };
                g_arr.ini = c; dx = dy = 0;
            }
            g_arr.modo = 2;
        }
    }
    if (g_arr.modo == 2) {
        int x = g_arr.jan.left + dx, y = g_arr.jan.top + dy;
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
        y = std::max<int>(y, mi.rcWork.top - g_arr.off.y);  // a barra não entra embaixo da barra de menus
        MoverJuntas(in, x, y);
    } else if (g_arr.modo == 3) {
        RECT n = g_arr.jan;
        float sc = Sc(in.alvo);
        int minW = (int)(220 * sc), minH = (int)(120 * sc);
        int b = g_arr.borda;
        if (b == HTTOP || b == HTTOPLEFT || b == HTTOPRIGHT) n.top = std::min<LONG>(g_arr.jan.top + dy, g_arr.jan.bottom - minH);
        if (b == HTLEFT || b == HTTOPLEFT) n.left = std::min<LONG>(g_arr.jan.left + dx, g_arr.jan.right - minW);
        if (b == HTRIGHT || b == HTTOPRIGHT) n.right = std::max<LONG>(g_arr.jan.right + dx, g_arr.jan.left + minW);
        SetWindowPos(in.alvo, nullptr, n.left, n.top, n.right - n.left, n.bottom - n.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | (IsHungAppWindow(in.alvo) ? SWP_ASYNCWINDOWPOS : 0));
        Atualizar(in.alvo);  // a faixa acompanha o tamanho que a janela aceitou
    }
}

int BotaoEm(const Info& in, POINT p) {  // p em px da faixa
    float sc = Sc(in.alvo);
    float x = p.x / sc, y = p.y / sc, H = (in.r.bottom - in.r.top) / sc;
    for (int k = 0; k < 3; k++) {
        float dx = x - (LX0 + k * LPITCH), dy = y - H / 2;
        if (dx * dx + dy * dy <= (LR + 2) * (LR + 2)) return k;
    }
    return -1;
}
bool SobreBotoes(const Info& in, POINT p) {
    float sc = Sc(in.alvo);
    return p.x / sc >= LX0 - LR - 6 && p.x / sc <= LX0 + 2 * LPITCH + LR + 6;
}
// borda de cima (redimensionar): o que a barra do Windows responderia ali
int Borda(const Info& in, POINT p) {
    if (in.zoom || !(in.estilo & WS_THICKFRAME)) return HTCAPTION;
    float sc = Sc(in.alvo);
    int w = in.r.right - in.r.left, e = (int)(5 * sc), canto = (int)(12 * sc);
    if (p.y < e) return p.x < canto ? HTTOPLEFT : p.x >= w - canto ? HTTOPRIGHT : HTTOP;
    if (p.x < e) return HTLEFT;
    if (p.x >= w - e) return HTRIGHT;
    return HTCAPTION;
}

// Arrastar uma aba (Chromium): o botão desce sobre uma aba na cópia; se o mouse andar com ele apertado, o aperto é
// mandado para a aba de verdade e o Chrome pega o mouse para si: dali em diante ele recebe o mouse DE VERDADE (onde o
// ponteiro está, que é PATCH_L à direita da aba real). Para o Chrome não ver um pulo desse tamanho, o aperto é mandado
// já nesse ponto (aba real + PATCH_L = onde o ponteiro está), que quase sempre ainda é a mesma aba; se não for (pegou
// perto da ponta direita), vai o mais à direita que couber na aba, antes do "x", e a aba dá um pulinho do que faltou.
// Sem andar, é um clique (CliqueReal ao soltar). Antes o aperto era guardado até soltar e não dava para mover as abas
// (relato do usuário de 12/11/2026). Se o programa não pegar o mouse, os movimentos são repassados daqui até soltar.
struct ArrastoAba { HWND alvo = nullptr; POINT ini{}, real{}; bool andou = false; } g_aba;
POINT PontoAba() { POINT c; GetCursorPos(&c); return { c.x - (g_aba.ini.x - g_aba.real.x), c.y }; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    HWND alvo = (HWND)GetWindowLongPtrW(h, GWLP_USERDATA);
    auto it = alvo ? g_j.find(alvo) : g_j.end();
    if (it == g_j.end()) return DefWindowProcW(h, m, w, l);
    Info& in = it->second;
    POINT p{ (short)LOWORD(l), (short)HIWORD(l) };
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: {
        POINT c; GetCursorPos(&c); ScreenToClient(h, &c);
        int b = SobreBotoes(in, c) ? HTCAPTION : Borda(in, c);
        SetCursor(LoadCursorW(nullptr, b == HTTOP ? IDC_SIZENS : b == HTTOPLEFT ? IDC_SIZENWSE : b == HTTOPRIGHT ? IDC_SIZENESW : (b == HTLEFT || b == HTRIGHT) ? IDC_SIZEWE : IDC_ARROW));
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        if (g_arr.modo && g_arr.alvo == alvo && GetCapture() == h) { Arrastar(in); return 0; }
        if (g_aba.alvo == alvo && GetCapture() == h) {
            POINT c; GetCursorPos(&c);
            if (!g_aba.andou) {
                if (std::abs(c.x - g_aba.ini.x) + std::abs(c.y - g_aba.ini.y) < 5) return 0;
                g_aba.andou = true;
                {   // o ponto do aperto: onde o ponteiro está, se ainda for a mesma aba
                    float sc = Sc(alvo);
                    POINT q{ g_aba.ini.x, g_aba.real.y };
                    RECT aba{};
                    if (RectDaAba(in, g_aba.real, aba)) {
                        LONG lim = aba.right - (LONG)(34 * sc);
                        if (q.x > lim) q.x = std::max(lim, g_aba.real.x);
                    } else if (HitReal(in, q) != HTCLIENT) q = g_aba.real;
                    g_aba.real = q;
                }
                SetForegroundWindow(alvo);
                Repassar(in, WM_MOUSEMOVE, 0, g_aba.real);
                Repassar(in, WM_LBUTTONDOWN, MK_LBUTTON, g_aba.real);
            }
            Repassar(in, WM_MOUSEMOVE, MK_LBUTTON, PontoAba());
            return 0;
        }
        { POINT t; if (!(w & (MK_LBUTTON | MK_RBUTTON)) && !g_salto.faixa && PontoReal(in, p, t)) Repassar(in, WM_MOUSEMOVE, w & (MK_SHIFT | MK_CONTROL), t); }
        bool hv = SobreBotoes(in, p);
        if (hv != in.hover) { in.hover = hv; Pintar(in); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_aba.alvo == alvo) {  // perdeu o mouse no meio (o programa pegou para si, Alt+Tab...): solta a aba onde está
            // o próprio programa pegou o mouse (o esperado no Chrome): ele segue sozinho com o mouse de verdade
            if (g_aba.andou && (HWND)l != alvo && !IsChild(alvo, (HWND)l)) Repassar(in, WM_LBUTTONUP, 0, PontoAba());
            g_aba = ArrastoAba{};
        }
        if (g_arr.modo) { g_arr = Arrasto{}; Atualizar(alvo); }
        return 0;
    case WM_MOUSELEAVE:
        if (in.propria && in.acima && !g_aba.alvo) PostMessageW(alvo, WM_MOUSELEAVE, 0, 0);
        if (in.hover || in.press >= 0) { in.hover = false; if (GetCapture() != h) in.press = -1; Pintar(in); }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int k = BotaoEm(in, p);
        if (k >= 0) { in.press = k; SetCapture(h); Pintar(in); return 0; }
        if (SobreBotoes(in, p)) return 0;
        {   // modo deslocado: aba ou botão do programa embaixo do ponteiro? o clique é dele
            POINT t;
            if (PontoReal(in, p, t) && HitReal(in, t) == HTCLIENT) {
                // O clique vai quando o botão soltar, como clique DE VERDADE no ponto real (Saltar), também no Chromium: com
                // mensagens repassadas, qualquer movimento do mouse entre apertar e soltar fazia o Chrome cancelar o clique
                // (ele recebe "o mouse saiu" porque o ponteiro de verdade não está em cima dele): com mouse real o "+"
                // e o "x" das abas não respondiam (relato do usuário de 12/11/2026).
                if (in.acima == 1 && m == WM_LBUTTONDOWN) { g_aba = ArrastoAba{}; g_aba.alvo = alvo; GetCursorPos(&g_aba.ini); g_aba.real = t; SetCapture(h); }
                return 0;
                SetForegroundWindow(alvo);
                Repassar(in, m, MK_LBUTTON, t);
                return 0;
            }
        }
        SetForegroundWindow(alvo);
        if (m == WM_LBUTTONDBLCLK) {  // duplo clique na barra: ampliar / voltar
            if (in.estilo & WS_MAXIMIZEBOX) PostMessageW(alvo, WM_SYSCOMMAND, IsZoomed(alvo) ? SC_RESTORE : SC_MAXIMIZE, 0);
            return 0;
        }
        // arrastar / redimensionar: feito aqui (ver o começo do arquivo)
        g_arr = Arrasto{};
        g_arr.alvo = alvo; g_arr.modo = 1; g_arr.borda = Borda(in, p);
        GetCursorPos(&g_arr.ini);
        GetWindowRect(alvo, &g_arr.jan);
        g_arr.off = { in.r.left - g_arr.jan.left, in.r.top - g_arr.jan.top };
        SetCapture(h);
        return 0;
    }
    case WM_TIMER: if (w == 7) FimDoSalto(h); else if (w == 8) LevarMenuDaSeta(h); return 0;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_RBUTTONDOWN: {
        POINT t;
        if (m == WM_RBUTTONDOWN) return 0;  // o botão direito também vai ao soltar, como clique de verdade
        if (PontoReal(in, p, t) && HitReal(in, t) == HTCLIENT) Repassar(in, m, m == WM_MBUTTONDOWN ? MK_MBUTTON : m == WM_RBUTTONDOWN ? MK_RBUTTON : 0, t);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_aba.alvo == alvo) {
            bool andou = g_aba.andou; POINT fim = PontoAba();
            g_aba = ArrastoAba{};
            if (GetCapture() == h) ReleaseCapture();
            if (andou) { Repassar(in, WM_LBUTTONUP, 0, fim); return 0; }
        }
        { POINT t; if (!g_arr.modo && in.press < 0 && PontoReal(in, p, t) && HitReal(in, t) == HTCLIENT) { CliqueReal(in, t, false); if (NaSeta(in, p)) { g_menuSeta = { alvo, 0 }; SetTimer(h, 8, 30, nullptr); } return 0; } }
        // clique sem arrastar num programa que não diz onde estão os botões dele: o clique de verdade vai para o ponto real.
        // No Chromium (acima == 1) não: ele diz certo o que é aba/botão e o que é barra vazia, e ali o clique não é de ninguém
        // (mandá-lo só fazia o mouse pular à toa a cada clique no vazio da linha das abas)
        if (g_arr.modo == 1 && in.propria && in.acima == 2) {
            POINT t;
            g_arr = Arrasto{};
            if (GetCapture() == h) ReleaseCapture();
            if (PontoReal(in, p, t)) Saltar(in, t, false);
            return 0;
        }
        if (g_arr.modo) {
            if (g_arr.modo == 2 && (in.estilo & WS_MAXIMIZEBOX)) {  // soltou na borda da tela: encaixa (cima = ampliar, lados = metade)
                POINT c; GetCursorPos(&c);
                MONITORINFO mi{ sizeof(mi) };
                GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
                RECT wk = mi.rcWork;
                if (c.y <= mi.rcMonitor.top + 1) PostMessageW(alvo, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                else if (c.x <= mi.rcMonitor.left + 1 || c.x >= mi.rcMonitor.right - 2) {
                    bool esq = c.x <= mi.rcMonitor.left + 1;
                    int w2 = (wk.right - wk.left) / 2;
                    // a moldura invisível de redimensionar (uns 7 px de cada lado) fica para fora: a parte visível encosta na borda
                    RECT wr{}, fr{};
                    GetWindowRect(alvo, &wr);
                    if (FAILED(DwmGetWindowAttribute(alvo, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) fr = wr;
                    int bl = fr.left - wr.left, br = wr.right - fr.right, bb = wr.bottom - fr.bottom, bt = fr.top - wr.top;
                    SetWindowPos(alvo, nullptr, (esq ? wk.left : wk.left + w2) - bl, wk.top - bt, w2 + bl + br, wk.bottom - wk.top + bt + bb, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                }
            }
            g_arr = Arrasto{};
            if (GetCapture() == h) ReleaseCapture();
            Atualizar(alvo);
            return 0;
        }
        int was = in.press;
        if (GetCapture() == h) ReleaseCapture();
        in.press = -1;
        if (was >= 0) {
            bool can[3] = { true, (in.estilo & WS_MINIMIZEBOX) != 0, (in.estilo & WS_MAXIMIZEBOX) != 0 };
            if (BotaoEm(in, p) == was && can[was] && !IsHungAppWindow(alvo)) {
                WPARAM cmd = was == 0 ? SC_CLOSE : was == 1 ? SC_MINIMIZE : (IsZoomed(alvo) ? SC_RESTORE : SC_MAXIMIZE);
                if (was == 2) SetForegroundWindow(alvo);
                if (was == 0) {  // fechar animado: a foto da janela (com a nossa barra) encolhe e some; quem anima é a thread do Shell
                    static UINT jFechar = RegisterWindowMessageW(L"ShellJanelaFechar");
                    HWND msg = FindWindowExW(nullptr, nullptr, L"ShellMensagens", nullptr);
                    DWORD pid = 0; if (msg) GetWindowThreadProcessId(msg, &pid);
                    COLORREF cor = in.cor != CLR_INVALID ? in.cor : (TemaEscuro() ? RGB(32, 32, 32) : RGB(243, 243, 243));
                    DWORD_PTR r = 0;
                    if (msg && pid == GetCurrentProcessId() &&
                        SendMessageTimeoutW(msg, jFechar, (WPARAM)alvo, (LPARAM)(in.propria ? 0 : (0x1000000 | (1 << 25) | (cor & 0xFFFFFF))), SMTO_ABORTIFHUNG, 400, &r) && r)
                        ShowWindow(in.faixa, SW_HIDE);  // a faixa some com a janela (se o programa não fechar, ela volta no próximo relógio)
                }
                PostMessageW(alvo, WM_SYSCOMMAND, cmd, 0);
            }
            if (g_j.count(alvo)) Pintar(in);
        }
        return 0;
    }
    case WM_RBUTTONUP: {  // menu da janela (Restaurar, Mover, Fechar...)
        { POINT t; if (PontoReal(in, p, t) && HitReal(in, t) == HTCLIENT) { CliqueReal(in, t, true); return 0; } }
        POINT s = p; ClientToScreen(h, &s);
        SetForegroundWindow(alvo);
        PostMessageW(alvo, 0x0313 /*WM_POPUPSYSTEMMENU*/, 0, MAKELPARAM(s.x, s.y));
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// Janela de administrador: o Windows não deixa o explorer movê-la, redimensioná-la nem lhe mandar cliques. A faixa
// então só "pega" o mouse sobre as bolinhas e sobre a ponta direita (onde ficam, escondidos, os botões do Windows);
// no resto ela deixa o mouse passar e quem responde é a barra de verdade embaixo: arrastar, duplo clique, menu e
// redimensionar pela borda de cima funcionam como sempre. Conferido a cada quadro (a faixa "transparente" não recebe mouse).
void Protegidas() {
    POINT c; GetCursorPos(&c);
    for (auto& [h, in] : g_j) {
        if (in.propria || !IsWindowVisible(in.faixa) || GetWindow(in.faixa, GW_OWNER) == in.alvo) continue;
        if (g_salto.faixa == in.faixa) continue;
        float sc = Sc(in.alvo);
        POINT p{ c.x - in.r.left, c.y - in.r.top };
        bool dentro = PtInRect(&in.r, c) != FALSE;
        bool pega = in.press >= 0 || (dentro && (SobreBotoes(in, p) || p.x >= (in.r.right - in.r.left) - (int)(PATCH_R * sc)));
        LONG ex = GetWindowLongW(in.faixa, GWL_EXSTYLE);
        bool passa = (ex & WS_EX_TRANSPARENT) != 0;
        if (passa == pega) SetWindowLongW(in.faixa, GWL_EXSTYLE, pega ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT));
        if (!pega && in.hover) { in.hover = false; Pintar(in); }
    }
}

void Fontes() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    if (!g_dw) return;
    const wchar_t* fam = L"Segoe UI";
    IDWriteFontCollection* fc = nullptr;
    if (SUCCEEDED(g_dw->GetSystemFontCollection(&fc)) && fc) {
        for (auto n : { L"SF Pro Text", L"SF Pro Display", L"SF Pro", L"Inter", L"Segoe UI Variable Text", L"Segoe UI" }) {
            UINT32 idx = 0; BOOL ok = FALSE;
            if (SUCCEEDED(fc->FindFamilyName(n, &idx, &ok)) && ok) { fam = n; break; }
        }
        fc->Release();
    }
    g_dw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"pt-br", &g_tf);
    if (g_tf) {
        g_tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* ell = nullptr;
        g_dw->CreateEllipsisTrimmingSign(g_tf, &ell);
        g_tf->SetTrimming(&tr, ell);
        if (ell) ell->Release();
    }
    Log(L"barras: fonte do título %ls", fam);
}

void Thread() {
    g_tid = GetCurrentThreadId();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Fontes();
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Proc; wc.hInstance = shell::inst; wc.lpszClassName = CLS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    g_ev[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, Evento, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_ev[1] = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_UNCLOAKED, nullptr, Evento, 0, 0, WINEVENT_OUTOFCONTEXT);
    Varrer();
    UINT_PTR rapido = SetTimer(nullptr, 0, 200, nullptr), lento = SetTimer(nullptr, 0, 1500, nullptr), fino = SetTimer(nullptr, 0, 16, nullptr);
    Log(L"barras: barra de título do Mac por fora ligada (%zu janelas)", g_j.size());
    MSG msg;
    for (;;) {
        if (g_seguir) {  // um quadro: mensagens pendentes, a faixa no lugar novo, espera o próximo quadro da tela
            bool sair = false;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { sair = true; break; }
                if (msg.message == WM_TIMER && !msg.hwnd) continue;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (sair) break;
            if (g_seguir) {
                if (!IsWindow(g_seguir) || (GetTickCount() - g_seguirDesde > 400 && GetAsyncKeyState(VK_LBUTTON) >= 0 && GetAsyncKeyState(VK_RBUTTON) >= 0)) g_seguir = nullptr;
                else { Atualizar(g_seguir); DwmFlush(); }
            }
            continue;
        }
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        if (msg.message == WM_TIMER && !msg.hwnd) {
            if (msg.wParam == fino) {  // cor mudando (janela escurecendo/clareando): acompanha a cada quadro
                DWORD now = GetTickCount();
                Protegidas();
                HWND fg = GetForegroundWindow();  // a janela da frente é lida sempre (o escurecer começa sem aviso)
                for (auto& [h, in] : g_j) if ((in.propria && in.acima && h == fg) || (in.corAte && (int)(in.corAte - now) > 0)) { LerCor(in); if (in.sujo && IsWindowVisible(in.faixa)) Pintar(in); }
                continue;
            }
            if (msg.wParam == rapido) {               // posição, ordem, cor, ativa/inativa
                Todas();
                for (auto& [h, in] : g_j) if (in.chrome) { DWORD antes = in.linhaEm; ConferirLinhaChrome(in); if (in.linhaEm != antes) { if (in.sujo) { Miniatura(in); Pintar(in); } break; } }  // uma janela por vez
            }
            else if (msg.wParam == lento) Varrer();   // janelas que ganharam barra sem avisar
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(nullptr, rapido); KillTimer(nullptr, lento); KillTimer(nullptr, fino);
    for (auto& e : g_ev) if (e) { UnhookWinEvent(e); e = nullptr; }
    for (auto& [h, in] : g_j) { if (in.cantos) DestroyWindow(in.cantos); DestroyWindow(in.faixa); }
    g_j.clear();
    CoUninitialize();
    g_rodando = false;
}

}  // namespace

void barras::Start() {
    if (!shell::nativo || g_rodando) return;
    if (shell::Setting(L"janelas_mac", L"1") == L"0" || GetFileAttributesW(L"C:\\Users\\Public\\MacTitulo-desligado.txt") != INVALID_FILE_ATTRIBUTES) {
        Log(L"barras: desligada (janelas_mac=0 ou MacTitulo-desligado.txt)");
        return;
    }
    g_rodando = true;
    std::thread(Thread).detach();
}

void barras::Stop() {
    DWORD t = g_tid.exchange(0);
    if (t) PostThreadMessageW(t, WM_QUIT, 0, 0);
    for (int i = 0; i < 100 && g_rodando; i++) Sleep(20);  // as faixas somem antes de o Shell sair
}
