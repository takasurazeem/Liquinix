// Launchpad, como no macOS Sequoia (medido no vídeo de referência, tela de 1080): o papel de parede
// desfocado cobre a tela (as janelas somem), campo de busca no topo, grade de 7 × 5 apps com o nome
// embaixo, bolinhas de página e o Dock por cima. Tudo na GPU, animado no ritmo do monitor.
// Os apps vêm da pasta Aplicativos do Windows (shell:AppsFolder), lida numa thread de fundo.
// Abre/fecha com a tecla Windows (gancho em main.cpp) e pelo ícone do Dock.
// Abrir e fechar são do compositor do Windows (DirectComposition): o papel de parede desfocado é uma
// camada parada (feita uma vez por papel e tamanho) e a grade é desenhada uma vez só; o aparecer, o zoom
// com mola e o sumir o DWM faz sozinho, na placa de vídeo. Ajuste: launchpad_motor=composicao (padrão)
// ou antigo (quadro a quadro).
#include "shell.h"

// Tahoe: o estilo dos ícones (Escuro, Transparente, Tingido) também no Launchpad, como no Dock
static ID2D1Bitmap* StyledBitmap(ID2D1RenderTarget* rt, UINT w, UINT h, const std::vector<BYTE>& px) {
    std::wstring st = ui::IconStyle();  // (StyleCheck, mais abaixo, solta os ícones quando o estilo ou a cor mudam)
    if (st == L"padrao") return ui::Bitmap(rt, w, h, px);
    std::vector<BYTE> c = px; ui::StyleIcon(w, h, c, st);
    return ui::Bitmap(rt, w, h, c);
}
#include "../comum/simbolos.h"
#include <dcomp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include "../comum/idioma.h"
#include "../menu/context_menu.h"

namespace {

struct LApp {
    std::wstring name, parsing;   // parsing: o que vem depois de shell:AppsFolder\ (AUMID ou caminho)
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
    bool mac = false;             // ícone do pacote do Sequoia
    ID2D1Bitmap* bmp = nullptr;
};

const UINT WM_LP_READY = WM_APP + 20, WM_LP_FRAME = WM_APP + 21;
const int COLS = 7, ROWS = 5, PER_PAGE = COLS * ROWS;
const DWORD ANIM_MS = 220;

HWND g_wnd;
ui::GpuSurface g_s;
ID2D1Bitmap1* g_backdrop;       // papel de parede desfocado, do tamanho da tela
std::mutex g_mu;
std::vector<LApp> g_all;        // protegido por g_mu enquanto a thread carrega
bool g_ready;
std::vector<LApp> g_pending;    // nova lista (programa instalado/removido): a thread da janela troca em WM_LP_READY
bool g_hasPending;              // protegido por g_mu
std::atomic<bool> g_scanning, g_scanAgain;
std::atomic<ULONGLONG> g_scannedAt;  // GetTickCount64 da última leitura completa
std::vector<int> g_view;        // índices em g_all depois do filtro
// estilo dos ícones ou cor de destaque mudou: solta os desenhos (são refeitos com o estilo novo)
void StyleCheck() {
    static std::wstring last;
    std::wstring k = ui::IconStyleKey();
    if (k == last) return;
    last = k;
    for (auto& a : g_all) if (a.bmp) { a.bmp->Release(); a.bmp = nullptr; }
}
int g_page = 0, g_press = -1;
std::wstring g_query;
double g_openedAt, g_closingAt;  // ms de anim::Now (relógio de alta precisão)
bool g_open, g_closing;
IDWriteTextFormat *g_tfLabel, *g_tfSearch, *g_tfIcon;
float g_fmtK;

// compositor
const UINT_PTR T_LP_CLOSE = 9;
const float SPRING_MS = 650;
IDCompositionTarget* g_ct;            // por baixo da grade (que é a superfície de cima)
IDCompositionVisual* g_cback;
IDCompositionEffectGroup *g_backFx, *g_fgFx;
IDCompositionScaleTransform* g_fgSc;
IDCompositionSurface* g_backSurf;
std::wstring g_backKey;
bool g_comp;                          // esta abertura usa o compositor

// páginas deslizando numa mola (antes trocavam de uma vez)
float g_pageFrom = 0;
double g_pageAt = 0;
float PageNow() {
    if (g_pageAt <= 0) return (float)g_page;
    float t = (float)((anim::Now() - g_pageAt) / 1000.0);
    if (t > 0.7f) { g_pageAt = 0; return (float)g_page; }
    return g_pageFrom + (g_page - g_pageFrom) * anim::Spring(t, 0.32f, 0.9f);
}
bool Sliding() { return g_pageAt > 0 && anim::Now() - g_pageAt < 700; }
void Settle() { g_pageFrom = (float)g_page; g_pageAt = 0; }  // troca sem animação (busca, categoria, abrir)

// modo tremendo (segurar um ícone): os ícones balançam e ganham um "×" que tira o app do Launchpad
bool g_jiggle;
double g_jiggleAt;
const UINT_PTR T_LP_HOLD = 10;
bool Hidden(const std::wstring& name) {
    std::wstring all = L"|" + shell::Setting(L"launchpad_ocultos", L"") + L"|";
    return all.find(L"|" + name + L"|") != std::wstring::npos;
}
}  // namespace

// Ferramentas do próprio Windows na lista de apps: atalhos para exes do Windows (o shell:AppsFolder os mostra
// pela pasta conhecida: {1AC14E77...} = System32, {D65231B0...} = SysWOW64, {F38BF404...} = Windows), os
// "Microsoft.Windows.*" (Painel de Controle, Executar...) e os atalhos gerados de Ferramentas do Windows. Os apps
// comuns da Microsoft (Fotos, Edge, Store, Xbox...) ficam.
bool launchpad::AppDoSistema(const std::wstring& name, const std::wstring& parsing) {
    std::wstring p = parsing;
    for (auto& c : p) c = towlower(c);
    if (p.find(L"liquinix") != std::wstring::npos) return false;  // os nossos apps nunca
    // (o "Microsoft.AutoGenerated.{...}" NÃO entra: é o ID de qualquer atalho sem ID próprio, inclusive jogos como
    // VALORANT e League of Legends; as Ferramentas do Windows que vêm assim estão na lista de nomes abaixo)
    for (auto pre : { L"{1ac14e77-02e7-4e5d-b744-2eb1ae5198b7}", L"{d65231b0-b2f1-4857-a4ce-a8e7c6ea7d27}",
                      L"{f38bf404-1d43-42f2-9305-67de0b28fc23}", L"c:\\windows\\" })
        if (p.rfind(pre, 0) == 0) return true;
    // "Microsoft.Windows.ControlPanel", "Microsoft.Windows.Shell.RunDialog"...; os pacotes da Store também começam
    // assim ("Microsoft.Windows.Photos_8wekyb3d8bbwe!App", o Fotos) e esses ficam
    if (p.rfind(L"microsoft.windows.", 0) == 0 && p.find(L'!') == std::wstring::npos) return true;
    std::wstring n = name;
    for (auto& c : n) c = towlower(c);
    for (auto w : { L"ferramentas do windows", L"windows tools", L"painel de controle", L"control panel", L"windows powershell ise",
                    L"recuperação", L"recovery drive", L"unidade de recuperação", L"windows media player legacy", L"internet explorer",
                    L"odbc", L"iscsi", L"desfragmentar", L"defragment", L"limpeza de disco", L"disk cleanup", L"windows fax",
                    L"teclado virtual", L"on-screen keyboard", L"narrador", L"narrator", L"lupa", L"magnifier", L"executar", L"run",
                    L"task scheduler", L"agendador de tarefas", L"computer management", L"gerenciamento do computador",
                    L"performance monitor", L"monitor de desempenho", L"event viewer", L"visualizador de eventos",
                    L"resource monitor", L"monitor de recursos", L"local security policy", L"política de segurança local",
                    L"hyper-v manager", L"gerenciador do hyper-v", L"task manager", L"gerenciador de tarefas",
                    L"windows memory diagnostic", L"diagnóstico de memória do windows" })
        if (n == w) return true;
    return false;
}

bool launchpad::Escondido(const std::wstring& name, const std::wstring& parsing) {
    if (Hidden(name)) return true;
    return shell::Setting(L"launchpad_sem_sistema", L"0") == L"1" && AppDoSistema(name, parsing);
}

void launchpad::Ocultar(const std::wstring& name) {
    if (Hidden(name)) return;
    std::wstring cur = shell::Setting(L"launchpad_ocultos", L"");
    shell::SetSetting(L"launchpad_ocultos", cur.empty() ? name : cur + L"|" + name);
    Log(L"launchpad: \"%ls\" oculto", name.c_str());
}

namespace {

bool Skip(const std::wstring& n) {  // atalhos que não são apps (desinstaladores, manuais, sites)
    std::wstring l = n;
    for (auto& c : l) c = towlower(c);
    for (auto w : { L"uninstall", L"desinstalar", L"remover ", L"readme", L"leia-me", L"read me", L"website", L"manual",
                    L"documentation", L"documentação", L"help", L"ajuda", L"release notes", L"license", L"licença" })
        if (l.find(w) != std::wstring::npos) return true;
    return false;
}

// Thread de fundo: lista os apps e já carrega os ícones.
void Loader() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<LApp> found;
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder)))) {
        IEnumShellItems* en = nullptr;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
            IShellItem* it = nullptr;
            while (en->Next(1, &it, nullptr) == S_OK) {
                PWSTR name = nullptr, parse = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parse))) {
                    LApp a; a.name = name; a.parsing = parse;
                    if (!Skip(a.name)) found.push_back(std::move(a));
                }
                if (name) CoTaskMemFree(name);
                if (parse) CoTaskMemFree(parse);
                it->Release();
            }
            en->Release();
        }
        folder->Release();
    }
    std::sort(found.begin(), found.end(), [](const LApp& a, const LApp& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    found.erase(std::unique(found.begin(), found.end(), [](const LApp& a, const LApp& b) { return !_wcsicmp(a.name.c_str(), b.name.c_str()); }), found.end());
    int px = 256;
    // tamanho em que os ícones aparecem (108 de 1080 da altura da tela, com folga para o toque): guardar
    // a 256 px os ~300 apps custava ~90 MB de memória à toa
    UINT keep = (UINT)std::clamp((int)std::ceil(GetSystemMetrics(SM_CYSCREEN) * 0.1 * 1.15), 96, 256);
    bool reload;
    {   // nova leitura (instalou ou removeu um programa): os ícones de quem já estava na lista são reaproveitados
        std::lock_guard<std::mutex> lk(g_mu);
        reload = g_ready;
        const std::vector<LApp>& old = g_hasPending ? g_pending : g_all;
        std::map<std::wstring, const LApp*> byParse;
        for (auto& o : old) byParse[o.parsing] = &o;
        for (auto& a : found)
            if (auto it = byParse.find(a.parsing); it != byParse.end() && !it->second->px.empty()) {
                a.w = it->second->w; a.h = it->second->h; a.px = it->second->px; a.mac = it->second->mac;
            }
    }
    for (auto& a : found) {
        if (!a.px.empty()) continue;
        std::wstring exe;
        if (a.parsing.size() > 4 && !_wcsicmp(a.parsing.c_str() + a.parsing.size() - 4, L".exe")) exe = a.parsing;
        std::wstring png = apps::PackIconByName(a.name, exe);
        if (!png.empty() && ui::LoadPng(png, a.w, a.h, a.px)) { a.mac = true; ui::Downscale(a.w, a.h, a.px, keep); continue; }
        if (ui::ShellImage(L"shell:AppsFolder\\" + a.parsing, px, a.w, a.h, a.px, false)) { ui::TrimToSquare(a.w, a.h, a.px); ui::Downscale(a.w, a.h, a.px, keep); }
    }
    size_t n = found.size();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!reload) { g_all = std::move(found); g_ready = true; }
        else { g_pending = std::move(found); g_hasPending = true; }  // a janela pode estar desenhando os bitmaps de g_all
    }
    g_scannedAt = GetTickCount64();
    Log(L"launchpad: %zu apps%ls", n, reload ? L" (lista atualizada)" : L"");
    PostMessageW(g_wnd, WM_LP_READY, 0, 0);
    CoUninitialize();
}

// Lê a lista de novo numa thread de fundo; pedidos durante uma leitura fazem mais uma no fim.
void Scan() {
    g_scanAgain = true;
    bool idle = false;
    if (!g_scanning.compare_exchange_strong(idle, true)) return;
    std::thread([] {
        while (g_scanAgain.exchange(false)) Loader();
        g_scanning = false;
    }).detach();
}

// ---- Tahoe ("Apps"): categorias, como os filtros do Apps do macOS 26. O Windows não diz a categoria
// de um app; então ela sai do nome (palavras-chave), na ordem desta lista. 0 = todos.
const wchar_t* const CATS[] = { L"Todos", L"Produtividade", L"Criatividade", L"Utilitários", L"Desenvolvimento", L"Entretenimento", L"Social", L"Jogos", L"Outros" };
const int NCATS = 9;
int g_cat = 0;

int CategoryOf(const std::wstring& name) {
    std::wstring n = L" " + name + L" ";
    for (auto& c : n) c = towlower(c);
    static const std::vector<std::vector<const wchar_t*>> words = {
        {},
        { L"word", L"excel", L"powerpoint", L"outlook", L"onenote", L"office", L"teams", L"notion", L"notas", L"notes", L"calendário", L"calendar",
          L"mail", L"acrobat", L"pdf", L"to do", L"todo", L"zoom", L"chrome", L"edge", L"firefox", L"opera", L"brave", L"vivaldi", L"navegador" },
        { L"photoshop", L"illustrator", L"premiere", L"after effects", L"lightroom", L"blender", L"gimp", L"krita", L"paint", L"ableton", L" live ",
          L"fl studio", L"audacity", L" obs", L"davinci", L"figma", L"canva", L"capcut", L"reaper", L"fotos", L"photos", L"câmera", L"camera",
          L"clipchamp", L"cubase", L"studio one", L"inkscape", L"affinity", L"vegas", L"sony", L"izotope", L"native instruments", L"serum", L"splice" },
        { L"terminal", L"powershell", L"prompt", L"calculadora", L"calculator", L"monitor", L"gerenciador", L"ajustes", L"settings", L"configura",
          L"painel de controle", L"control panel", L"7-zip", L"winrar", L"finder", L"bloco de notas", L"notepad", L"ferrament", L"tool", L"captura",
          L"snipping", L"relógio", L"clock", L"defender", L"segurança", L"update", L"driver", L"nvidia", L"amd ", L"intel", L"realtek", L"logitech",
          L"bluetooth", L"disco", L"backup", L"onedrive", L"dropbox", L"google drive", L"vpn", L"utilit", L"mapa de caracteres", L"lupa", L"teclado" },
        { L"visual studio", L"vs code", L" code", L"git", L"python", L"node", L"android studio", L"intellij", L"pycharm", L"docker", L"wsl",
          L"ubuntu", L"claude", L"cursor", L"postman", L"sql", L"unity", L"unreal", L"godot", L"java", L"developer", L"sdk" },
        { L"spotify", L"netflix", L"prime video", L"youtube", L"vlc", L"música", L"music", L" tv", L"filmes", L"movies", L"media player",
          L"twitch", L"deezer", L"disney", L"plex", L"kindle", L"podcast" },
        { L"discord", L"whatsapp", L"telegram", L"instagram", L"facebook", L"messenger", L"twitter", L" x ", L"signal", L"skype", L"slack", L"reddit" },
        { L"steam", L"epic games", L"xbox", L"game", L"jogo", L"minecraft", L"roblox", L"battle.net", L"riot", L"valorant", L"league of",
          L"ea app", L"ubisoft", L"gog", L"solitaire", L"paciência", L"fortnite" },
    };
    for (int k = 1; k < (int)words.size(); k++)
        for (auto* w : words[k]) if (n.find(w) != std::wstring::npos) return k;
    return NCATS - 1;  // Outros
}

void Filter() {
    g_view.clear();
    std::wstring q = g_query;
    for (auto& c : q) c = towlower(c);
    bool cats = liquid::On() && g_cat > 0;
    for (int i = 0; i < (int)g_all.size(); i++) {
        if (cats && CategoryOf(g_all[i].name) != g_cat) continue;
        if (launchpad::Escondido(g_all[i].name, g_all[i].parsing)) continue;  // tirado (modo tremendo, botão direito) ou do sistema
        if (!q.empty()) {
            std::wstring n = g_all[i].name;
            for (auto& c : n) c = towlower(c);
            if (n.find(q) == std::wstring::npos) continue;
        }
        g_view.push_back(i);
    }
    int pages = std::max(1, (int)(g_view.size() + PER_PAGE - 1) / PER_PAGE);
    g_page = std::clamp(g_page, 0, pages - 1);
    Settle();
}

int Pages() { return std::max(1, (int)(g_view.size() + PER_PAGE - 1) / PER_PAGE); }

// Geometria em DIPs. No vídeo (1080 de altura): colunas a cada 216, linhas a cada 166, primeira
// linha centrada em 140, ícones de ~86 de desenho (108 com a margem do ícone do Mac), nome 67 abaixo
// do centro do ícone, busca de 300 × 30 em y = 29, bolinhas em y = 930. Aqui tudo escala com a altura.
struct Geo { float W, H, k, step, rowStep, icon, top, left; };
Geo Layout() {
    RECT rc; GetClientRect(g_wnd, &rc);
    float sc = ui::Scale(g_wnd);
    Geo g{};
    g.W = rc.right / sc; g.H = rc.bottom / sc;
    g.k = g.H / 1080;
    g.step = 216 * g.k;
    g.rowStep = 166 * g.k;
    g.icon = 108 * g.k;
    g.top = (liquid::On() ? 178 : 140) * g.k;  // centro da primeira linha (Tahoe: abaixo das categorias)
    g.left = g.W / 2 - g.step * (COLS - 1) / 2;  // centro da primeira coluna
    return g;
}

D2D1_POINT_2F CellCenter(const Geo& g, int k) { return { g.left + (k % COLS) * g.step, g.top + (k / COLS) * g.rowStep }; }
D2D1_RECT_F SearchRect(const Geo& g) {
    if (liquid::On()) return { g.W / 2 - 240 * g.k, 24 * g.k, g.W / 2 + 240 * g.k, 64 * g.k };  // Tahoe: cápsula mais larga
    return { g.W / 2 - 150 * g.k, 29 * g.k, g.W / 2 + 150 * g.k, 59 * g.k };
}
float DotsY(const Geo& g) { return 930 * g.k; }

int HitIcon(const Geo& g, D2D1_POINT_2F p) {
    int first = g_page * PER_PAGE;
    for (int k = 0; k < PER_PAGE && first + k < (int)g_view.size(); k++) {
        auto c = CellCenter(g, k);
        if (std::fabs(p.x - c.x) < g.step * 0.4f && p.y > c.y - g.icon * 0.5f && p.y < c.y + 80 * g.k) return first + k;
    }
    return -1;
}

void MakeFormats(float k);

// pílulas das categorias (Tahoe), centradas abaixo da busca
std::vector<D2D1_RECT_F> CatRects(const Geo& g) {
    MakeFormats(g.k);
    std::vector<D2D1_RECT_F> r;
    float gap = 8 * g.k, h = 30 * g.k, y = 80 * g.k, total = -gap;
    std::vector<float> w;
    for (int i = 0; i < NCATS; i++) { w.push_back(ui::TextWidth(g_tfSearch, Tr(CATS[i])) + 30 * g.k); total += w.back() + gap; }
    float x = g.W / 2 - total / 2;
    for (int i = 0; i < NCATS; i++) { r.push_back({ x, y, x + w[i], y + h }); x += w[i] + gap; }
    return r;
}

void MakeFormats(float k) {
    if (g_fmtK == k) return;
    for (auto** f : { &g_tfLabel, &g_tfSearch, &g_tfIcon }) if (*f) { (*f)->Release(); *f = nullptr; }
    g_tfLabel = ui::Format(14 * k, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteInlineObject* dots = nullptr;  // nomes compridos terminam em "…"
    ui::dw->CreateEllipsisTrimmingSign(g_tfLabel, &dots);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    g_tfLabel->SetTrimming(&tr, dots);
    if (dots) dots->Release();
    g_tfSearch = ui::Format(14 * k, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfIcon = ui::Format(13 * k, DWRITE_FONT_WEIGHT_NORMAL, true);
    g_fmtK = k;
}

// Papel de parede desfocado e escurecido, desenhado em `dc` (o alvo já preparado, wpx × hpx)
void DrawBackdrop(ID2D1DeviceContext* dc, const wchar_t* path, int wpx, int hpx) {
    D2D1::Matrix3x2F base; dc->GetTransform(&base);
    UINT w = 0, h = 0; std::vector<BYTE> px;
    ID2D1Bitmap* src = path[0] && ui::LoadPng(path, w, h, px) ? ui::Bitmap(dc, w, h, px) : nullptr;
    dc->Clear(D2D1::ColorF(0.08f, 0.12f, 0.28f));  // sem papel de parede: o azul do vídeo
    if (src) {
        float s = std::max(wpx / (float)w, hpx / (float)h);  // cobre a tela inteira
        ID2D1Effect* blur = nullptr;
        dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
        blur->SetInput(0, src);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 45.f / s);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation((wpx - w * s) / 2, (hpx - h * s) / 2) * base);
        dc->DrawImage(blur);
        dc->SetTransform(base);
        blur->Release();
        src->Release();
    }
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.22f), &br);
    dc->FillRectangle(D2D1::RectF(0, 0, (float)wpx, (float)hpx), br);
    br->Release();
}

// Papel de parede atual, desfocado e levemente escurecido, do tamanho da tela (feito ao abrir).
void BuildBackdrop(int wpx, int hpx) {
    auto* dc = ui::Gpu();
    if (!dc) return;
    if (g_backdrop) { g_backdrop->Release(); g_backdrop = nullptr; }
    dc->CreateBitmap(D2D1::SizeU(wpx, hpx), nullptr, 0,
                     D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &g_backdrop);
    if (!g_backdrop) return;
    wchar_t path[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    dc->SetTarget(g_backdrop);
    dc->SetDpi(96, 96);
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    DrawBackdrop(dc, path, wpx, hpx);
    dc->EndDraw();
    dc->SetTarget(nullptr);
}

// ============================================================================================
// Tahoe: o "Apps" do macOS 26 não é mais tela cheia. É o Spotlight em modo Aplicativos: um painel de
// vidro leitoso no meio da tela, com o campo no alto e seções (Sugestões e as categorias), cada uma com
// uma fileira de 5 ícones e "Mostrar Mais". Medido no vídeo: cantos ~4,5% da largura, divisórias finas
// entre as seções, título em negrito à esquerda e "Mostrar Mais" à direita.
const float AP_W = 640, AP_HEAD = 56, AP_PAD = 26, AP_ICON = 62, AP_ROW = 102, AP_SEC = 34, AP_R = 30;
const int AP_COLS = 5;
bool g_apMore[16];
float g_apScroll, g_apContentH;
struct ApHit { D2D1_RECT_F r; int kind; int v; };  // kind 0: app (índice em g_all); 1: Mostrar Mais (seção)
std::vector<ApHit> g_apHits;
IDWriteTextFormat *g_apField, *g_apSec, *g_apLabel, *g_apMoreTf;

D2D1_RECT_F ApPanel(const Geo& g) {
    float h = std::min(g.H * 0.72f, std::max(AP_HEAD + 120.f, AP_HEAD + g_apContentH));
    float x = (g.W - AP_W) / 2, y = g.H * 0.14f;
    return D2D1::RectF(x, y, x + AP_W, y + h);
}

// seções: {título, apps (índices em g_all)}
std::vector<std::pair<std::wstring, std::vector<int>>> ApSections() {
    std::vector<std::pair<std::wstring, std::vector<int>>> out;
    if (!g_query.empty()) {
        out.push_back({ Tr(L"Resultados"), g_view });
        return out;
    }
    std::vector<int> sugg;  // Sugestões: os apps fixos no Dock, na ordem dele
    for (auto& a : apps::list) {
        if (!a.pinned || a.finder) continue;
        for (int i = 0; i < (int)g_all.size(); i++)
            if (!_wcsicmp(g_all[i].name.c_str(), a.name.c_str())) { sugg.push_back(i); break; }
    }
    if (!sugg.empty()) out.push_back({ Tr(L"Sugestões"), sugg });
    for (int k = 1; k < NCATS; k++) {
        std::vector<int> v;
        for (int i : g_view) if (CategoryOf(g_all[i].name) == k) v.push_back(i);
        if (!v.empty()) out.push_back({ Tr(CATS[k]), v });
    }
    return out;
}

void PaintApps(ID2D1DeviceContext* dc, const Geo& g, float ease) {
    if (!g_apField) {
        g_apField = ui::Format(22, DWRITE_FONT_WEIGHT_NORMAL);
        g_apSec = ui::Format(13, DWRITE_FONT_WEIGHT_BOLD);
        g_apLabel = ui::Format(12, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        g_apMoreTf = ui::Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
        for (auto* tf : { g_apField, g_apSec, g_apMoreTf }) if (tf) tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (g_apLabel) {
            IDWriteInlineObject* dots = nullptr; ui::dw->CreateEllipsisTrimmingSign(g_apLabel, &dots);
            DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            g_apLabel->SetTrimming(&tr, dots); g_apLabel->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (dots) dots->Release();
        }
    }
    bool dark = ui::Dark();
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.92f);
    D2D1_COLOR_F text2 = dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.40f);
    D2D1_COLOR_F line = dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.08f);
    g_apHits.clear();
    auto P = ApPanel(g);
    ID2D1Layer* fade = nullptr;
    dc->CreateLayer(nullptr, &fade);
    // aparece como o Spotlight: esmaece e cresce 4% a partir do centro do painel
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), ease), fade);
    float z = 0.96f + 0.04f * ease;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(z, z, { (P.left + P.right) / 2, (P.top + P.bottom) / 2 }));
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(text, &br);
    // sombra curta e o corpo leitoso
    for (int i = 4; i >= 1; i--) {
        float k = i * 4.f;
        br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.05f : 0.025f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(P.left - k * 0.5f, P.top + k * 0.3f, P.right + k * 0.5f, P.bottom + k), AP_R + k * 0.5f, AP_R + k * 0.5f), br);
    }
    br->SetColor(dark ? D2D1::ColorF(0.15f, 0.15f, 0.17f, 0.90f) : D2D1::ColorF(0.95f, 0.96f, 0.99f, 0.90f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(P, AP_R, AP_R), br);
    liquid::Rim(dc, P, AP_R, dark, 1);
    // campo
    br->SetColor(text2);
    simbolo::Draw(dc, L"square.grid.2x2", D2D1::RectF(P.left + 22, P.top + AP_HEAD / 2 - 10, P.left + 42, P.top + AP_HEAD / 2 + 10), br);
    D2D1_RECT_F fr = D2D1::RectF(P.left + 54, P.top, P.right - 54, P.top + AP_HEAD);
    if (g_query.empty()) { br->SetColor(text2); dc->DrawTextW(Tr(L"Aplicativos"), (UINT32)wcslen(Tr(L"Aplicativos")), g_apField, fr, br); }
    else { br->SetColor(text); dc->DrawTextW(g_query.c_str(), (UINT32)g_query.size(), g_apField, fr, br); }
    float tw = g_query.empty() ? 0 : ui::TextWidth(g_apField, g_query);
    br->SetColor(D2D1::ColorF(0.04f, 0.45f, 0.93f, 1));
    if (((GetTickCount() / 530) & 1) == 0) dc->FillRectangle(D2D1::RectF(fr.left + tw + 1, P.top + AP_HEAD / 2 - 13, fr.left + tw + 3, P.top + AP_HEAD / 2 + 13), br);
    br->SetColor(text2);
    simbolo::Draw(dc, L"ellipsis.circle", D2D1::RectF(P.right - 42, P.top + AP_HEAD / 2 - 10, P.right - 22, P.top + AP_HEAD / 2 + 10), br);
    br->SetColor(line);
    dc->FillRectangle(D2D1::RectF(P.left + AP_PAD, P.top + AP_HEAD - 0.5f, P.right - AP_PAD, P.top + AP_HEAD + 0.5f), br);
    // seções (roláveis, recortadas ao painel)
    D2D1_RECT_F clip = D2D1::RectF(P.left, P.top + AP_HEAD + 1, P.right, P.bottom - 8);
    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = clip.top + 6 - g_apScroll, cell = (AP_W - 2 * AP_PAD) / AP_COLS;
    auto secs = ApSections();
    std::lock_guard<std::mutex> lk(g_mu);
    for (int si = 0; si < (int)secs.size(); si++) {
        auto& [title, list] = secs[si];
        if (si > 0) { br->SetColor(line); dc->FillRectangle(D2D1::RectF(P.left + AP_PAD, y - 0.5f, P.right - AP_PAD, y + 0.5f), br); }
        br->SetColor(text);
        dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_apSec, D2D1::RectF(P.left + AP_PAD, y, P.right - 140, y + AP_SEC), br);
        bool more = si < 16 && g_apMore[si];
        bool canMore = (int)list.size() > AP_COLS && g_query.empty();
        if (canMore) {
            const wchar_t* m = more ? Tr(L"Mostrar Menos") : Tr(L"Mostrar Mais");
            br->SetColor(text2);
            D2D1_RECT_F mr = D2D1::RectF(P.right - AP_PAD - 120, y, P.right - AP_PAD, y + AP_SEC);
            dc->DrawTextW(m, (UINT32)wcslen(m), g_apMoreTf, mr, br);
            g_apHits.push_back({ mr, 1, si });
        }
        y += AP_SEC;
        int n = (more || !g_query.empty()) ? (int)list.size() : std::min<int>((int)list.size(), AP_COLS);
        for (int k = 0; k < n; k++) {
            if (k > 0 && k % AP_COLS == 0) y += AP_ROW;
            LApp& a = g_all[list[k]];
            float cx = P.left + AP_PAD + cell * (k % AP_COLS + 0.5f);
            float art = a.mac ? AP_ICON : AP_ICON * 0.80f;
            D2D1_RECT_F ir = D2D1::RectF(cx - art / 2, y + 4 + (AP_ICON - art) / 2, cx + art / 2, y + 4 + (AP_ICON + art) / 2);
            StyleCheck();
            if (!a.bmp && !a.px.empty()) a.bmp = StyledBitmap(dc, a.w, a.h, a.px);
            if (a.bmp && ir.bottom > clip.top && ir.top < clip.bottom) dc->DrawBitmap(a.bmp, ir, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            if (list[k] == g_press) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.25f)); dc->FillRoundedRectangle(D2D1::RoundedRect(ir, art * 0.22f, art * 0.22f), br); }
            br->SetColor(text);
            dc->DrawTextW(a.name.c_str(), (UINT32)a.name.size(), g_apLabel, D2D1::RectF(cx - cell / 2 + 4, y + AP_ICON + 10, cx + cell / 2 - 4, y + AP_ICON + 28), br);
            g_apHits.push_back({ D2D1::RectF(cx - cell / 2 + 6, y, cx + cell / 2 - 6, y + AP_ROW - 6), 0, list[k] });
        }
        y += AP_ROW + 8;
    }
    if (secs.empty()) { br->SetColor(text2); dc->DrawTextW(Tr(L"Nenhum resultado"), (UINT32)wcslen(Tr(L"Nenhum resultado")), g_apSec, D2D1::RectF(P.left + AP_PAD, y, P.right, y + 40), br); y += 40; }
    dc->PopAxisAlignedClip();
    float contentH = y + g_apScroll - (clip.top + 6) + 14;
    if (std::fabs(contentH - g_apContentH) > 0.5f) { g_apContentH = contentH; PostMessageW(g_wnd, WM_LP_FRAME, 0, 0); }  // o painel acompanha o conteúdo
    // a área visível só aceita cliques dentro do recorte
    for (auto& h : g_apHits) { h.r.top = std::max(h.r.top, clip.top); h.r.bottom = std::min(h.r.bottom, clip.bottom); }
    br->Release();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->PopLayer();
    fade->Release();
}

void Paint() {
    if (!g_wnd || !g_open) return;
    RECT rc; GetClientRect(g_wnd, &rc);
    float sc = ui::Scale(g_wnd);
    auto* dc = ui::GpuBegin(g_s, rc.right, rc.bottom, sc);
    if (!dc) return;
    Geo g = Layout();
    if (liquid::On()) {  // Tahoe: o painel de Aplicativos (sem tela cheia)
        double now = anim::Now();
        float ease = g_closing ? 1 - anim::EaseInOut(std::min(1.f, (float)((now - g_closingAt) / ANIM_MS)))
                               : anim::Ease(std::min(1.f, (float)((now - g_openedAt) / ANIM_MS)));
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        PaintApps(dc, g, ease);
        ui::GpuEnd(g_s);
        return;
    }
    MakeFormats(g.k);
    double now = anim::Now();
    // abrir: aparece com a curva do Mac e os ícones assentam numa mola leve; fechar: some suave
    float ease, zoomK;
    if (g_closing) { float t = std::min(1.f, (float)((now - g_closingAt) / ANIM_MS)); ease = 1 - anim::EaseInOut(t); zoomK = ease; }
    else { float t = (float)((now - g_openedAt) / 1000.0); ease = anim::Ease(std::min(1.f, t * 1000 / ANIM_MS)); zoomK = anim::Spring(t, 0.42f, 0.86f); }
    if (g_comp) { ease = 1; zoomK = 1; }

    if (g_backdrop && !g_comp) dc->DrawBitmap(g_backdrop, D2D1::RectF(0, 0, g.W, g.H), ease);
    ID2D1Layer* layer = nullptr;
    dc->CreateLayer(nullptr, &layer);
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), ease), layer);
    float z = 1.10f - 0.10f * zoomK;  // os ícones chegam um pouco maiores e assentam (como no Mac)
    dc->SetTransform(D2D1::Matrix3x2F::Scale(z, z, { g.W / 2, g.H / 2 }));

    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    // busca
    auto sr = SearchRect(g);
    bool tahoe = liquid::On();
    float rad = tahoe ? (sr.bottom - sr.top) / 2 : 7 * g.k;
    if (tahoe) {  // Tahoe: busca e categorias em cápsulas de vidro
        liquid::Tile(dc, sr, rad, true, 1.2f);
        auto cr = CatRects(g);
        for (int i = 0; i < NCATS; i++) {
            float pr = (cr[i].bottom - cr[i].top) / 2;
            if (i == g_cat) { br->SetColor(D2D1::ColorF(1, 1, 1, 0.30f)); dc->FillRoundedRectangle(D2D1::RoundedRect(cr[i], pr, pr), br); liquid::Rim(dc, cr[i], pr, true, 0.9f); }
            else liquid::Tile(dc, cr[i], pr, true, 0.6f);
            br->SetColor(D2D1::ColorF(1, 1, 1, i == g_cat ? 1.f : 0.85f));
            float tw2 = ui::TextWidth(g_tfSearch, Tr(CATS[i]));
            float cx = (cr[i].left + cr[i].right) / 2;
            dc->DrawTextW(Tr(CATS[i]), (UINT32)wcslen(Tr(CATS[i])), g_tfSearch, D2D1::RectF(cx - tw2 / 2, cr[i].top, cx + tw2 / 2 + 4, cr[i].bottom), br);
        }
    } else if (liquid::Leopard() && g_query.empty()) {
        // Mac OS X (o Launchpad do Lion): sem campo de busca; digitar ainda filtra, e aí o campo aparece
    } else {
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(sr, rad, rad), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.20f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(sr, rad, rad), br, 1);
    }
    if (!(liquid::Leopard() && g_query.empty())) {
    std::wstring shown = g_query.empty() ? (tahoe ? Tr(L"Buscar apps") : Tr(L"Buscar")) : g_query;
    float tw = ui::TextWidth(g_tfSearch, shown), gw = 20 * g.k;
    float tx = tahoe ? sr.left + 16 * g.k : g_query.empty() ? (sr.left + sr.right) / 2 - (tw + gw) / 2 : sr.left + 10 * g.k;
    br->SetColor(D2D1::ColorF(1, 1, 1, g_query.empty() ? 0.50f : 0.92f));
    dc->DrawTextW(L"", 1, g_tfIcon, D2D1::RectF(tx, sr.top, tx + gw, sr.bottom), br);
    dc->DrawTextW(shown.c_str(), (UINT32)shown.size(), g_tfSearch, D2D1::RectF(tx + gw, sr.top, sr.right - 8, sr.bottom), br);
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ready) {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
            dc->DrawTextW(Tr(L"Carregando apps…"), (UINT32)wcslen(Tr(L"Carregando apps…")), g_tfLabel, D2D1::RectF(0, g.H / 2 - 20, g.W, g.H / 2 + 20), br);
        } else {
            float px = PageNow();
            D2D1_MATRIX_3X2_F base; dc->GetTransform(&base);
            int p0 = std::max(0, (int)std::floor(px)), p1 = std::min(Pages() - 1, (int)std::ceil(px));
            for (int pg = p0; pg <= p1; pg++) {
            float dx = (pg - px) * g.W;  // a página desliza inteira
            int first = pg * PER_PAGE;
            for (int k = 0; k < PER_PAGE && first + k < (int)g_view.size(); k++) {
                LApp& a = g_all[g_view[first + k]];
                auto c = CellCenter(g, k);
                c.x += dx;
                if (c.x < -g.step || c.x > g.W + g.step) continue;
                float art = a.mac ? g.icon : g.icon * 0.80f;  // ícones do Windows: o tamanho do desenho dos do Mac
                D2D1_RECT_F dr{ c.x - art / 2, c.y - art / 2, c.x + art / 2, c.y + art / 2 };
                StyleCheck();
                if (!a.bmp && !a.px.empty()) a.bmp = StyledBitmap(dc, a.w, a.h, a.px);
                if (g_jiggle) {  // balança uns 2 graus, cada um no seu ritmo, como no Mac
                    float tt = (float)((anim::Now() - g_jiggleAt) / 1000.0);
                    float ang = 2.0f * std::sin(tt * 6.2832f * 3.4f + k * 1.7f);
                    dc->SetTransform(D2D1::Matrix3x2F::Rotation(ang, c) * base);
                }
                if (a.bmp) dc->DrawBitmap(a.bmp, dr, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                if (g_jiggle) {  // o "×" no canto de cima à esquerda
                    float r = 12 * g.k;
                    D2D1_POINT_2F xc{ dr.left + art * 0.12f, dr.top + art * 0.12f };
                    br->SetColor(D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.95f));
                    dc->FillEllipse(D2D1::Ellipse(xc, r, r), br);
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
                    dc->DrawEllipse(D2D1::Ellipse(xc, r, r), br, 1.2f * g.k);
                    float q = r * 0.42f;
                    dc->DrawLine({ xc.x - q, xc.y - q }, { xc.x + q, xc.y + q }, br, 2 * g.k);
                    dc->DrawLine({ xc.x - q, xc.y + q }, { xc.x + q, xc.y - q }, br, 2 * g.k);
                }
                if (first + k == g_press) {
                    br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(dr, art * 0.2f, art * 0.2f), br);
                }
                D2D1_RECT_F lr{ c.x - g.step / 2 + 6, c.y + 57 * g.k, c.x + g.step / 2 - 6, c.y + 77 * g.k };
                static IDWriteTextFormat* tfLeo = [] {  // Mac OS X: o nome em Lucida negrito com a sombra escura do Lion
                    auto* f = ui::FormatFace(L"Lucida Sans Unicode", 12, DWRITE_FONT_WEIGHT_BOLD);
                    if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                             DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; IDWriteInlineObject* d = nullptr; ui::dw->CreateEllipsisTrimmingSign(f, &d); f->SetTrimming(&t, d); if (d) d->Release(); }
                    return f;
                }();
                if (liquid::Leopard() && tfLeo) {
                    br->SetColor(D2D1::ColorF(0, 0, 0, 0.75f));
                    dc->DrawTextW(a.name.c_str(), (UINT32)a.name.size(), tfLeo, D2D1::RectF(lr.left, lr.top + 1.5f, lr.right, lr.bottom + 1.5f), br);
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.98f));
                    dc->DrawTextW(a.name.c_str(), (UINT32)a.name.size(), tfLeo, lr, br);
                    if (g_jiggle) dc->SetTransform(base);
                    continue;
                }
                br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));  // sombra leve do texto
                dc->DrawTextW(a.name.c_str(), (UINT32)a.name.size(), g_tfLabel, D2D1::RectF(lr.left, lr.top + 1, lr.right, lr.bottom + 1), br);
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.96f));
                dc->DrawTextW(a.name.c_str(), (UINT32)a.name.size(), g_tfLabel, lr, br);
                if (g_jiggle) dc->SetTransform(base);
            }
            }
            if (g_view.empty()) {
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
                dc->DrawTextW(Tr(L"Nenhum resultado"), (UINT32)wcslen(Tr(L"Nenhum resultado")), g_tfLabel, D2D1::RectF(0, g.H / 2 - 20, g.W, g.H / 2 + 20), br);
            }
            int n = Pages();
            if (n > 1) {
                float y = DotsY(g), x0 = g.W / 2 - (n - 1) * 12.f * g.k;
                for (int i = 0; i < n; i++) {
                    float on = std::max(0.f, 1 - std::fabs(i - px));  // a bolinha acende acompanhando o deslize
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f + 0.55f * on));
                    dc->FillEllipse(D2D1::Ellipse({ x0 + i * 24 * g.k, y }, 4 * g.k, 4 * g.k), br);
                }
            }
        }
    }
    br->Release();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->PopLayer();
    layer->Release();
    ui::GpuEnd(g_s);
}

// ---- compositor
bool CompInit() {
    if (g_ct) return true;
    IDCompositionDevice* cd = ui::Comp();
    if (!cd || FAILED(cd->CreateTargetForHwnd(g_wnd, FALSE, &g_ct))) { Log(L"launchpad: sem compositor"); return false; }
    cd->CreateVisual(&g_cback);
    cd->CreateEffectGroup(&g_backFx); cd->CreateEffectGroup(&g_fgFx);
    cd->CreateScaleTransform(&g_fgSc);
    if (!g_cback || !g_backFx || !g_fgFx || !g_fgSc) return false;
    g_cback->SetEffect(g_backFx);
    g_ct->SetRoot(g_cback);
    return true;
}

// o papel de parede desfocado numa camada parada: refeito só se o papel (ou a tela) mudar
void CompBackdrop(int wpx, int hpx) {
    wchar_t path[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    std::wstring key = std::wstring(path) + L"|" + std::to_wstring(wpx) + L"x" + std::to_wstring(hpx);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (path[0] && GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) key += L"|" + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime);
    if (g_backSurf && key == g_backKey) return;
    if (g_backSurf) { g_backSurf->Release(); g_backSurf = nullptr; }
    double t0 = anim::Now();
    g_backSurf = ui::CompSurface(wpx, hpx, [&](ID2D1DeviceContext* dc) { DrawBackdrop(dc, path, wpx, hpx); });
    g_backKey = key;
    g_cback->SetContent(g_backSurf);
    Log(L"launchpad: fundo desfocado refeito em %.1f ms", anim::Now() - t0);
}

// abrir: aparece com a curva do Mac (220 ms) e a grade assenta numa mola leve (de 110% para 100%);
// fechar: some suave (220 ms) e a grade cresce um pouco
void CompAnimate(bool closing) {
    const int K = 24;
    float D = (closing ? ANIM_MS : SPRING_MS) / 1000.f;
    std::vector<float> op(K), z(K);
    for (int k = 0; k < K; k++) {
        float t = D * k / (K - 1);
        float ease, zoomK;
        if (closing) { ease = 1 - anim::EaseInOut(std::min(1.f, t * 1000 / ANIM_MS)); zoomK = ease; }
        else { ease = anim::Ease(std::min(1.f, t * 1000 / ANIM_MS)); zoomK = anim::Spring(t, 0.42f, 0.86f); }
        op[k] = ease; z[k] = 1.10f - 0.10f * zoomK;
    }
    if (!closing) { op[K - 1] = 1; z[K - 1] = 1; }
    RECT rc; GetClientRect(g_wnd, &rc);
    g_fgSc->SetCenterX(rc.right / 2.f); g_fgSc->SetCenterY(rc.bottom / 2.f);
    if (auto* a = ui::CompCurve(op, D)) { g_backFx->SetOpacity(a); a->Release(); }
    if (auto* a = ui::CompCurve(op, D)) { g_fgFx->SetOpacity(a); a->Release(); }
    if (auto* a = ui::CompCurve(z, D)) { g_fgSc->SetScaleX(a); a->Release(); }
    if (auto* a = ui::CompCurve(z, D)) { g_fgSc->SetScaleY(a); a->Release(); }
    ui::Comp()->Commit();
}

void Close() {
    if (!g_open || g_closing) return;
    g_closing = true;
    g_closingAt = anim::Now();
    if (g_comp) {
        ui::FrameClock(g_wnd, WM_LP_FRAME, false);
        CompAnimate(true);
        SetTimer(g_wnd, T_LP_CLOSE, ANIM_MS + 10, nullptr);
        return;
    }
    ui::FrameClock(g_wnd, WM_LP_FRAME, true);
}

void Launch(int i) {
    if (i < 0 || i >= (int)g_all.size()) return;
    std::wstring target = L"shell:AppsFolder\\" + g_all[i].parsing;
    if (shell::OpenCalculator(target)) { Close(); return; }  // a Calculadora do Windows: abre a nossa
    if (apps::LaunchNoOcclusion(target)) { Close(); return; }  // Chrome, Discord...: miniatura viva no Mission Control
    if (apps::ActivatePackaged(target)) { Close(); return; }  // Segurança do Windows, ChatGPT...: sem o Explorador, só pelo ativador
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr))) {
        SHELLEXECUTEINFOW se{ sizeof(se) };
        se.fMask = SEE_MASK_IDLIST;
        se.lpIDList = pidl;
        se.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&se);
        CoTaskMemFree(pidl);
    }
    Log(L"launchpad: abrindo %ls", g_all[i].name.c_str());
    Close();
}

D2D1_POINT_2F Pt(LPARAM l) { float sc = ui::Scale(g_wnd); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }

void GoPage(int p) {
    p = std::clamp(p, 0, Pages() - 1);
    if (p == g_page) return;
    g_pageFrom = PageNow();
    g_pageAt = anim::Now();
    g_page = p;
    ui::FrameClock(g_wnd, WM_LP_FRAME, true);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INITMENUPOPUP || m == WM_DRAWITEM || m == WM_MEASUREITEM || m == WM_MENUCHAR) {  // submenus das extensões
        LRESULT res = 0;
        if (ctxmenu::ForwardOwnerMsg(m, w, l, &res)) return res;
    }
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_LP_READY: {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_hasPending) {  // troca aqui, na thread que cria e desenha os bitmaps
            for (auto& a : g_all) if (a.bmp) a.bmp->Release();
            g_all = std::move(g_pending);
            g_pending.clear();
            g_hasPending = false;
            g_press = -1;
        }
        Filter();
    }
        Paint();
        return 0;
    case WM_TIMER:
        if (w == T_LP_HOLD) {  // segurou o ícone: modo tremendo
            KillTimer(h, T_LP_HOLD);
            if (g_press >= 0 && !g_jiggle && !liquid::On()) {
                g_jiggle = true; g_jiggleAt = anim::Now(); g_press = -1;
                ui::FrameClock(h, WM_LP_FRAME, true);
                Paint();
            }
            return 0;
        }
        if (w == T_LP_CLOSE) {
            KillTimer(h, T_LP_CLOSE);
            if (g_closing) { g_closing = g_open = false; ShowWindow(h, SW_HIDE); }
        }
        return 0;
    case WM_LP_FRAME: {
        double now = anim::Now();
        if (g_closing && now - g_closingAt >= ANIM_MS) {
            ui::FrameClock(h, WM_LP_FRAME, false);
            g_closing = g_open = false;
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        if (!g_closing && now - g_openedAt >= 650 && !Sliding() && !g_jiggle) ui::FrameClock(h, WM_LP_FRAME, false);  // a mola assenta
        Paint();
        return 0;
    }
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) { if (g_open && !g_closing) Log(L"launchpad: perdeu o foco, fechando"); Close(); } return 0;
    case WM_LBUTTONDOWN: {
        Geo g = Layout();
        if (liquid::On()) {
            auto pt = Pt(l);
            g_press = -1;
            for (auto& hh : g_apHits) if (hh.kind == 0 && pt.x >= hh.r.left && pt.x < hh.r.right && pt.y >= hh.r.top && pt.y < hh.r.bottom) g_press = hh.v;
            SetCapture(h); Paint(); return 0;
        }
        { std::lock_guard<std::mutex> lk(g_mu); g_press = g_jiggle ? -1 : HitIcon(g, Pt(l)); }
        if (g_press >= 0) SetTimer(h, T_LP_HOLD, 650, nullptr);
        SetCapture(h); Paint(); return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        Geo g = Layout();
        if (liquid::On()) {
            auto pt = Pt(l);
            int p = g_press; g_press = -1;
            auto P = ApPanel(g);
            bool inside = pt.x >= P.left && pt.x < P.right && pt.y >= P.top && pt.y < P.bottom;
            if (!inside) { Close(); return 0; }  // clicar fora fecha, como no Mac
            for (auto& hh : g_apHits) {
                if (pt.x < hh.r.left || pt.x >= hh.r.right || pt.y < hh.r.top || pt.y >= hh.r.bottom) continue;
                if (hh.kind == 1 && hh.v < 16) { g_apMore[hh.v] = !g_apMore[hh.v]; break; }
                if (hh.kind == 0 && hh.v == p) { Launch(hh.v); return 0; }
            }
            Paint();
            return 0;
        }
        KillTimer(h, T_LP_HOLD);
        if (g_jiggle) {  // tremendo: o "×" tira o app; clique fora de um ícone sai do modo
            auto pt = Pt(l);
            int removed = -1, onIcon = -1;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                int first = g_page * PER_PAGE;
                for (int k = 0; k < PER_PAGE && first + k < (int)g_view.size(); k++) {
                    LApp& a = g_all[g_view[first + k]];
                    auto c = CellCenter(g, k);
                    float art = a.mac ? g.icon : g.icon * 0.80f;
                    float xx = c.x - art / 2 + art * 0.12f, yy = c.y - art / 2 + art * 0.12f;
                    if (std::hypot(pt.x - xx, pt.y - yy) < 16 * g.k) removed = g_view[first + k];
                }
                onIcon = HitIcon(g, pt);
                if (removed >= 0) {
                    std::wstring cur = shell::Setting(L"launchpad_ocultos", L"");
                    shell::SetSetting(L"launchpad_ocultos", cur.empty() ? g_all[removed].name : cur + L"|" + g_all[removed].name);
                    Log(L"launchpad: \"%ls\" tirado (modo tremendo)", g_all[removed].name.c_str());
                    Filter();
                }
            }
            if (removed < 0 && onIcon < 0) g_jiggle = false;
            g_press = -1;
            Paint();
            return 0;
        }
        int hit;
        { std::lock_guard<std::mutex> lk(g_mu); hit = HitIcon(g, Pt(l)); }
        int p = g_press; g_press = -1;
        auto pt = Pt(l);
        auto sr = SearchRect(g);
        int cat = -1;
        if (liquid::On()) {
            auto cr = CatRects(g);
            for (int i = 0; i < NCATS; i++) if (pt.x >= cr[i].left && pt.x < cr[i].right && pt.y >= cr[i].top && pt.y < cr[i].bottom) cat = i;
        }
        if (cat >= 0) { g_cat = cat; g_page = 0; std::lock_guard<std::mutex> lk(g_mu); Filter(); }
        else if (hit >= 0 && hit == p) { int i; { std::lock_guard<std::mutex> lk(g_mu); i = g_view[hit]; } Launch(i); }
        else if (!(pt.x >= sr.left && pt.x < sr.right && pt.y >= sr.top && pt.y < sr.bottom)) {
            int n = Pages();  // bolinhas trocam de página; qualquer outro lugar vazio fecha
            float y = DotsY(g), x0 = g.W / 2 - (n - 1) * 12.f * g.k;
            bool dot = false;
            for (int i = 0; i < n && n > 1; i++)
                if (std::fabs(pt.x - (x0 + i * 24 * g.k)) < 10 * g.k && std::fabs(pt.y - y) < 10 * g.k) { GoPage(i); dot = true; }
            if (!dot && hit < 0) Close();
        }
        Paint();
        return 0;
    }
    case WM_RBUTTONUP: {  // botão direito num app: o menu do Windows (Executar como administrador, Abrir local do arquivo...) no visual do Mac
        if (g_jiggle) return 0;
        auto pt = Pt(l);
        int idx = -1;
        if (liquid::On()) { for (auto& hh : g_apHits) if (hh.kind == 0 && pt.x >= hh.r.left && pt.x < hh.r.right && pt.y >= hh.r.top && pt.y < hh.r.bottom) idx = hh.v; }
        else { Geo g = Layout(); std::lock_guard<std::mutex> lk(g_mu); int hit = HitIcon(g, pt); if (hit >= 0) idx = g_view[hit]; }
        POINT sp{ (short)LOWORD(l), (short)HIWORD(l) };
        ClientToScreen(h, &sp);
        if (idx < 0 || idx >= (int)g_all.size()) {  // fundo: trazer de volta o que foi tirado e as ferramentas do Windows
            auto mi = [](const std::wstring& t, int id) { ctxmenu::Item i; i.label = t; i.id = id; return i; };
            std::vector<ctxmenu::Item> items;
            ctxmenu::Item sys = mi(Tr(L"Ocultar Apps do Sistema"), 1);
            sys.checked = shell::Setting(L"launchpad_sem_sistema", L"0") == L"1";
            items.push_back(sys);
            std::vector<std::wstring> hid;
            for (std::wstring s = shell::Setting(L"launchpad_ocultos", L""), part; !s.empty();) {
                size_t b = s.find(L'|'); part = s.substr(0, b); s = b == std::wstring::npos ? L"" : s.substr(b + 1);
                if (!part.empty()) hid.push_back(part);
            }
            if (!hid.empty()) {
                ctxmenu::Item sub = mi(Tr(L"Trazer de Volta"), 0);
                for (int k = 0; k < (int)hid.size() && k < 40; k++) sub.sub.push_back(mi(hid[k], 100 + k));
                ctxmenu::Item sep; sep.separator = true; sub.sub.push_back(sep);
                sub.sub.push_back(mi(Tr(L"Trazer Todos"), 99));
                items.push_back(sub);
            }
            int id = ctxmenu::ShowItems(h, sp, items);
            if (id == 1) shell::SetSetting(L"launchpad_sem_sistema", sys.checked ? L"0" : L"1");
            else if (id == 99) shell::SetSetting(L"launchpad_ocultos", L"");
            else if (id >= 100 && id - 100 < (int)hid.size()) {
                hid.erase(hid.begin() + (id - 100));
                std::wstring s; for (auto& x : hid) { if (!s.empty()) s += L'|'; s += x; }
                shell::SetSetting(L"launchpad_ocultos", s);
            }
            if (id) { std::lock_guard<std::mutex> lk(g_mu); Filter(); }
            Paint();
            return 0;
        }
        std::wstring path = L"shell:AppsFolder\\" + g_all[idx].parsing, name = g_all[idx].name;
        ctxmenu::ExtraNext({ { Tr(L"Ocultar do Launchpad"), 2, false, false, false, L"", {}, 0xED1A } });
        auto res = ctxmenu::Show(h, sp, L"shell:AppsFolder", { path }, GetKeyState(VK_SHIFT) < 0);
        ctxmenu::ExtraNext({});
        Log(L"launchpad: menu de %ls%ls", name.c_str(), res.chosen ? L" (comando escolhido)" : L"");
        if (res.extra == 2) { launchpad::Ocultar(name); { std::lock_guard<std::mutex> lk(g_mu); Filter(); } Paint(); return 0; }
        if (res.chosen) Close();
        return 0;
    }
    case WM_CONTEXTMENU: return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        if (liquid::On()) {
            Geo g = Layout();
            auto P = ApPanel(g);
            float view = P.bottom - 8 - (P.top + AP_HEAD + 1);
            g_apScroll = std::clamp(g_apScroll - d / 120.f * 60.f, 0.f, std::max(0.f, g_apContentH - view));
            Paint();
            return 0;
        }
        GoPage(g_page + (d < 0 ? 1 : -1));
        Paint();
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && g_jiggle) { g_jiggle = false; Paint(); return 0; }
        if (w == VK_ESCAPE) { if (!g_query.empty()) { g_query.clear(); std::lock_guard<std::mutex> lk(g_mu); Filter(); } else Close(); }
        else if (w == VK_RIGHT || w == VK_NEXT) GoPage(g_page + 1);
        else if (w == VK_LEFT || w == VK_PRIOR) GoPage(g_page - 1);
        else if (w == VK_RETURN) {  // Enter na busca: abre o primeiro resultado
            int i = -1;
            { std::lock_guard<std::mutex> lk(g_mu); if (!g_view.empty() && !g_query.empty()) i = g_view[std::min(g_page * PER_PAGE, (int)g_view.size() - 1)]; }
            if (i >= 0) Launch(i);
        }
        Paint();
        return 0;
    case WM_CHAR:
        if (w == 8) { if (!g_query.empty()) g_query.pop_back(); }
        else if (w >= 32) g_query += (wchar_t)w;
        else return 0;
        { std::lock_guard<std::mutex> lk(g_mu); g_page = 0; Filter(); }
        g_apScroll = 0;
        Paint();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool launchpad::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellLaunchpad";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Launchpad", WS_POPUP,
                            0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    apps::ours.push_back(g_wnd);
    g_s.hwnd = g_wnd;
    Scan();
    return true;
}

void launchpad::Rescan() { Scan(); }

bool launchpad::IsOpen() { return g_open && !g_closing; }

// Antes da primeira abertura: o fundo desfocado (o papel de parede borrado, a parte cara do Launchpad).
// No Tahoe não há Launchpad (é o Spotlight em modo Aplicativos).
void launchpad::Warm() {
    if (liquid::On() || g_open || !g_wnd) return;
    if (shell::Setting(L"launchpad_motor", L"composicao") != L"composicao" || !CompInit()) return;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    CompBackdrop(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
}

void launchpad::Toggle() {
    // Tahoe (macOS 26): o Launchpad virou "Apps", o Spotlight no modo Aplicativos (grade de 5 com categorias)
    if (liquid::On() && !g_open) { spotlight::OpenMode(1); return; }
    Log(L"launchpad: Toggle (aberto=%d fechando=%d)", g_open ? 1 : 0, g_closing ? 1 : 0);
    if (g_open && !g_closing) { Close(); return; }
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r = mi.rcMonitor;
    KillTimer(g_wnd, T_LP_CLOSE);  // abrindo de novo no meio do fechar
    g_comp = !liquid::On() && shell::Setting(L"launchpad_motor", L"composicao") == L"composicao" && CompInit();
    g_apScroll = 0;
    for (auto& m : g_apMore) m = false;
    if (liquid::On()) { if (g_backdrop) { g_backdrop->Release(); g_backdrop = nullptr; } }  // Tahoe: sem papel de parede por trás
    else if (g_comp) {

        if (g_backdrop) { g_backdrop->Release(); g_backdrop = nullptr; }
        CompBackdrop(r.right - r.left, r.bottom - r.top);
    } else BuildBackdrop(r.right - r.left, r.bottom - r.top);
    g_query.clear();
    g_page = 0;
    Settle();
    g_jiggle = false;
    g_cat = 0;
    if (g_ready && GetTickCount64() - g_scannedAt > 5 * 60 * 1000) Scan();  // por garantia: o vigia (instalados.cpp) já avisa na hora

    { std::lock_guard<std::mutex> lk(g_mu); if (g_ready) Filter(); }
    g_open = true; g_closing = false;
    g_openedAt = anim::Now();
    if (g_comp) {
        // a grade pronta (parada) e as curvas entregues antes de a janela aparecer: o primeiro quadro já é o certo
        SetWindowPos(g_wnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
        Paint();
        if (g_s.visual) { g_s.visual->SetEffect(g_fgFx); g_s.visual->SetTransform(g_fgSc); }
        CompAnimate(false);
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(g_wnd);
        SetFocus(g_wnd);
        dock::Raise();
        return;
    }
    if (g_s.visual) { g_s.visual->SetEffect(nullptr); g_s.visual->SetTransform(nullptr); ui::Comp()->Commit(); }
    if (g_backFx) { g_backFx->SetOpacity(0.f); ui::Comp()->Commit(); }  // a camada do compositor fica de fora
    SetWindowPos(g_wnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
    dock::Raise();  // o Dock continua por cima, como no Mac
    Paint();
    ui::FrameClock(g_wnd, WM_LP_FRAME, true);
}

int launchpad::Category(const std::wstring& appName) { return CategoryOf(appName); }
int launchpad::CategoryCount() { return NCATS; }
const wchar_t* launchpad::CategoryName(int k) { return k >= 0 && k < NCATS ? Tr(CATS[k]) : L""; }
