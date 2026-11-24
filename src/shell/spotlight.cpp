// Spotlight, como o do Mac: campo de busca de vidro no alto da tela (Win+Espaço ou a lupa da barra de
// menus). Digitando, o painel cresce numa coluna só: o melhor resultado num cartão, depois Aplicativos,
// Ajustes do Sistema, Documentos, Pastas, Imagens (em miniaturas), Músicas e Filmes, Outros e Web,
// com o trecho digitado em negrito. Fontes: apps da pasta Aplicativos do Windows, páginas do Ajustes,
// o índice de arquivos próprio (indice.cpp, numa thread), uma calculadora e "Pesquisar na Web".
// Teclas: ↑/↓ (e ←/→ nas miniaturas) escolhem, Enter abre, Ctrl+Enter mostra no Arquivos, Esc apaga/fecha.
#include "shell.h"
#include <condition_variable>
#include <deque>
#include "../comum/simbolos.h"
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <winhttp.h>
#include "../comum/idioma.h"
#include "../menu/context_menu.h"
#include "../comum/vidro.h"
#include <set>
#pragma comment(lib, "winhttp.lib")
#import "C:\\Program Files\\Common Files\\System\\ado\\msado15.dll" no_namespace rename("EOF", "AdoEOF")

namespace {

// Medidas do Spotlight novo do Mac (pt): painel de 600, campo de 52, raio 18; uma coluna só, com o
// melhor resultado num cartão (56), cabeçalhos (24), linhas (30) e as imagens em miniaturas (68).
// W, FIELD_H, CARD_H e WIN_H mudam com o tema (Metricas(), a cada abertura): Sequoia 720 × 60; Tahoe 640 × 56
float W = 720, FIELD_H = 60, CARD_H = 66;
const float RADIUS = 22, PAD = 12, ROW_H = 36, HEAD_H = 28, TILE = 86, TILE_GAP = 10, MAX_LIST_H = 470;
// grade de apps (modo Aplicativos do Tahoe): antes 5 colunas de 112 com ícones de 64; o Apps do macOS 26 (guia,
// seção 39, medido) é um painel de 764 de largura com 7 colunas de células de 112 × 92, ícones de 77 e nome de 18
float APP_CELL_H = 92, APP_ICON = 77, APP_COLS = 7, APP_ICON_TOP = 1, APP_NAME_TOP = 76;
const float CHIPS_H = 42;
float WIN_H = FIELD_H + 1 + MAX_LIST_H;  // as janelas têm sempre este tamanho; só o recorte do vidro anima
float CIRC_GAP = 12;
// macOS 27 Golden Gate (capturas 2x do 512 Pixels: Spotlight-Search-v2, Spotlight-Actions, Apps): a barra é uma
// cápsula escura e esfumada de 520 × 85 e os resultados vêm num painel de vidro separado, 11 abaixo, com cantos de 30;
// linhas de 63,5 com ícone de 36 e a escolhida numa pílula clara (não azul). Sem a cápsula com os quatro círculos.
float GG_GAP = 0;
const float GG_BAR_W = 520;
// Tahoe, guia macOS26-Theme-Reference (seções 39 e 56, medido com Acessibilidade): barra de 640 × 56 (a cápsula e
// os 4 círculos de modo, um a cada 64), lista de 640 de largura, linhas de 57 com ícone de 38, topo a 30 % da
// altura da tela; com resultados, cantos de 26
extern int g_mode;
void Metricas() {
    bool t = liquid::On();
    W = t ? (g_mode == 1 ? 764.f : 640.f) : 720.f;  // Tahoe: o Apps (modo Aplicativos) é mais largo
    FIELD_H = t ? 56.f : 60.f;
    CARD_H = t ? 57.f : 66.f;
    CIRC_GAP = t ? 8.f : 12.f;
    GG_GAP = 0;
    if (t && liquid::Golden()) {
        W = g_mode == 1 ? 842.f : GG_BAR_W;  // Apps (captura Apps.png): painel de 842 sob a barra de 520 centrada
        FIELD_H = 85;
        CARD_H = 63.5f;
        GG_GAP = 11;
    }
    if (liquid::Leopard()) {  // Leopard: o menu do Spotlight do 10.5, preso na barra de menus à direita
        W = 380; FIELD_H = 34; CARD_H = 20;
    }
    WIN_H = FIELD_H + 1 + GG_GAP + MAX_LIST_H;
}
const float LEO_ROW = 20, LEO_COL = 112;  // Leopard: linhas de 20 e a coluna das categorias
const UINT WM_SP_FILES = WM_APP + 60, WM_SP_ICON = WM_APP + 61, WM_SP_APPS = WM_APP + 62, WM_SP_FRAME = WM_APP + 63, WM_SP_SUGG = WM_APP + 64;
const UINT_PTR T_SEARCH = 1, T_CARET = 2, T_ICONS = 3, T_REGRAB = 4;

enum Kind { K_APP, K_SETTING, K_DOC, K_FOLDER, K_IMAGE, K_MEDIA, K_OTHER, K_CALC, K_WEB };

struct Result {
    Kind kind;
    std::wstring name, target, detail;  // target: parsing do app, id da página, caminho, texto da conta
    int score = 0;
};

enum RowType { R_HEAD, R_CARD, R_ROW, R_TILE, R_APP, R_MODE };  // R_MODE: macOS 27, os modos na lista do Spotlight vazio  // R_APP: célula da grade de apps do Tahoe (ícone e nome embaixo)
struct Row {  // cabeçalho (res < 0), cartão do melhor resultado, linha ou miniatura
    std::wstring head;
    int res = -1;
    RowType type = R_ROW;
    D2D1_RECT_F r{};  // posição no conteúdo (sem a rolagem)
    int sect = -1;    // cabeçalho: o Kind da seção (para o "Mostrar Mais")
};
std::set<int> g_more, g_canMore;  // seções abertas por inteiro / seções que têm mais do que mostram (como no Mac)

struct LApp { std::wstring name, parsing, lower; int use = 0; std::wstring words; };  // words: apelidos (nossos apps)  // use: quanto é usado (UserAssist do Windows + aberturas pelo Spotlight)
struct SettingPage { const wchar_t* name; const wchar_t* words; const wchar_t* id; };

const SettingPage SETTINGS[] = {
    { L"Wi‑Fi", L"wifi rede sem fio internet", L"wifi" }, { L"Bluetooth", L"fone mouse teclado dispositivos", L"bluetooth" },
    { L"Rede", L"ethernet internet ip firewall vpn proxy", L"rede" }, { L"Energia", L"bateria economia repouso", L"energia" },
    { L"Geral", L"sobre atualização armazenamento data hora idioma início", L"geral" },
    { L"Acessibilidade", L"narrador lupa legendas", L"acessibilidade" }, { L"Aparência", L"tema escuro claro destaque cor", L"aparencia" },
    { L"Apple Intelligence e Siri", L"copilot fala voz assistente", L"inteligencia" },
    { L"Central de Controle", L"barra de menus relógio ícones", L"central" }, { L"Mesa e Dock", L"dock tamanho ampliação ocultar janelas", L"dock" },
    { L"Telas", L"monitor brilho resolução escala night shift", L"telas" }, { L"Protetor de Tela", L"proteção de tela", L"protetor" },
    { L"Spotlight", L"pesquisa busca índice", L"spotlight" }, { L"Imagem de Fundo", L"papel de parede wallpaper", L"fundo" },
    { L"Notificações", L"avisos alertas", L"notificacoes" }, { L"Som", L"volume saída entrada alto-falante microfone", L"som" },
    { L"Foco", L"não incomodar", L"foco" }, { L"Tempo de Uso", L"família limites", L"tempo" },
    { L"Tela Bloqueada", L"bloqueio senha protetor", L"bloqueio" },
    { L"Privacidade e Segurança", L"localização câmera microfone permissões segurança", L"privacidade" },
    { L"Touch ID e Senha", L"windows hello digital pin senha", L"senha" }, { L"Usuários e Grupos", L"contas usuários", L"usuarios" },
    { L"Contas de Internet", L"email e-mail contas", L"contas" }, { L"Game Center", L"jogos xbox", L"jogos" },
    { L"iCloud", L"onedrive nuvem backup", L"nuvem" }, { L"Teclado", L"repetição teclas digitação", L"teclado" },
    { L"Mouse", L"rastreamento clique rolagem", L"mouse" }, { L"Trackpad", L"touchpad gestos", L"trackpad" },
    { L"Impressoras e Scanners", L"imprimir scanner", L"impressoras" },
};

HWND g_wnd, g_back;
bool g_glass, g_open, g_visible;  // open: aceita teclado; visible: na tela (inclui a animação de fechar)
// animação (no ritmo do monitor): altura do painel, destaque e rolagem deslizam até o alvo; abrir e
// fechar esmaecem com uma leve mudança de escala, como no Mac
float g_h = FIELD_H, g_selTop = -1, g_selBot = -1, g_scrollNow = 0;
bool g_keyNav;          // macOS 27: o app escolhido só aparece realçado depois das setas (a grade abre sem realce)
double g_panelInAt = -1e9;  // macOS 27: quando o painel de baixo apareceu (entra esmaecendo, sem crescer)
double g_openAt, g_closeAt, g_lastFrame;  // ms de anim::Now (relógio de alta precisão)
ui::GpuSurface g_s;
IDWriteTextFormat *g_tfField, *g_tfRow, *g_tfHead, *g_tfTitle, *g_tfSub, *g_tfCell, *g_tfChip;
std::wstring g_query;
bool g_selAll;                   // Ctrl+A: o próximo caractere substitui tudo
bool g_caretOn = true;
DWORD g_closedAt;
HWND g_prevFg;      // o app que estava na frente ao abrir (volta para ele ao fechar com Esc/atalho, como no Mac)
int g_regrabs;      // quantas vezes já pegou o foco de volta desde que abriu

std::mutex g_mu;                 // protege g_apps (carregados numa thread)
std::vector<LApp> g_apps;
std::vector<Result> g_res;       // resultados mostrados
std::vector<Row> g_rows;
int g_sel = -1;                  // índice em g_rows
float g_scroll = 0;
std::atomic<int> g_gen{ 0 };     // geração da busca de arquivos (respostas antigas são ignoradas)

// ícones: chave (app:parsing / arquivo:caminho / pagina) → pixels; bitmaps da GPU criados ao desenhar
// small: ícone de baixa resolução (só 16/32/48 px no programa): vai num quadrado cinza, como o macOS 26 faz
// com ícones fora do padrão; failed: o app não tem ícone (quadrado com a inicial)
struct Icon { UINT w = 0, h = 0; std::vector<BYTE> px; ID2D1Bitmap* bmp = nullptr; bool loading = false, lowres = false, failed = false; };
std::map<std::wstring, Icon> g_icons;
std::mutex g_iconMu;

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

// sem acentos, para "configuracoes" achar "Configurações"
std::wstring Fold(const std::wstring& s) {
    std::wstring o = Lower(s);
    static const std::wstring from = L"áàâãäéèêëíìîïóòôõöúùûüçñ", to = L"aaaaaeeeeiiiiooooouuuucn";
    for (auto& c : o) { size_t i = from.find(c); if (i != std::wstring::npos) c = to[i]; }
    return o;
}

// 0 = não casa. Começo do nome > começo de uma palavra > iniciais > qualquer parte.
int Score(const std::wstring& name, const std::wstring& q) {
    std::wstring n = Fold(name);
    if (q.empty()) return 0;
    if (n == q) return 120;
    if (n.rfind(q, 0) == 0) return 100;
    for (size_t i = 1; i < n.size(); i++)
        if ((n[i - 1] == L' ' || n[i - 1] == L'-' || n[i - 1] == L'.') && n.compare(i, q.size(), q) == 0) return 80;
    std::wstring ini;
    bool start = true;
    for (wchar_t c : n) { if (start && iswalnum(c)) ini += c; start = c == L' ' || c == L'-'; }
    if (q.size() >= 2 && ini.rfind(q, 0) == 0) return 70;
    if (n.find(q) != std::wstring::npos) return 50;
    return 0;
}

bool Skip(const std::wstring& n) {  // atalhos que não são apps (desinstaladores, manuais, sites)
    std::wstring l = Lower(n);
    for (auto w : { L"uninstall", L"desinstalar", L"remover ", L"readme", L"leia-me", L"read me", L"website", L"manual",
                    L"documentation", L"documentação", L"release notes", L"license", L"licença" })
        if (l.find(w) != std::wstring::npos) return true;
    return false;
}

// ---- O que o usuário mais usa (como o Spotlight do Mac, que aprende)
// 1) Quantas vezes cada app foi aberto, contado pelo próprio Windows (UserAssist: nomes em ROT13, contagem
//    no byte 4 de cada valor). Serve de ponto de partida no primeiro dia.
// 2) O que foi aberto pelo Spotlight e com qual texto digitado ("chr" → Chrome): fica em
//    %APPDATA%\Shell\spotlight-uso.txt (uma linha por app: alvo, aberturas, último uso, textos digitados).
struct Learned { int opens = 0; long long last = 0; std::vector<std::wstring> queries; };
std::map<std::wstring, Learned> g_learn;   // alvo (parsing) → uso
std::mutex g_learnMu;

std::wstring LearnFile() {
    PWSTR p = nullptr; std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { f = std::wstring(p) + L"\\Shell\\spotlight-uso.txt"; CoTaskMemFree(p); }
    return f;
}
// Roda a cada vez que a lista de apps é refeita: SUBSTITUI o que está na memória. Antes somava os textos do
// arquivo aos que já estavam carregados; a cada recarga a lista dobrava e o próximo SaveLearned gravava a lista
// dobrada (spotlight-uso.txt chegou a 442 MB e o Shell a 30 GB de memória). Agora cada app guarda no máximo 8
// textos diferentes, e um arquivo inchado por esse erro é consertado aqui mesmo (lido em pedaços, campo a campo:
// as linhas dele passam de milhões de caracteres) e regravado.
constexpr size_t MAX_QUERIES = 8;
void SaveLearned();
void LoadLearned() {
    std::wstring file = LearnFile();
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"r, ccs=UTF-8") || !f) return;
    std::map<std::wstring, Learned> got;
    bool repaired = false;
    std::vector<std::wstring> fields;
    std::wstring cur;
    bool curTooLong = false;
    auto endField = [&] {
        if (curTooLong) { cur.clear(); repaired = true; }  // texto absurdo: não é algo que alguém digitou
        fields.push_back(std::move(cur));
        cur.clear(); curTooLong = false;
        if (fields.size() > 3) {  // textos digitados: sem repetir, até 8
            auto& q = fields.back();
            bool drop = q.empty() || fields.size() > 3 + MAX_QUERIES || std::find(fields.begin() + 3, fields.end() - 1, q) != fields.end() - 1;
            if (drop) { if (!q.empty()) repaired = true; fields.pop_back(); }
        }
    };
    auto endLine = [&] {
        endField();
        if (fields.size() >= 3 && !fields[0].empty()) {
            Learned& L = got[fields[0]];
            L.opens = _wtoi(fields[1].c_str()); L.last = _wtoi64(fields[2].c_str());
            L.queries.assign(fields.begin() + 3, fields.end());
        }
        fields.clear();
    };
    static wchar_t buf[64 * 1024];
    bool any = false;
    while (fgetws(buf, (int)std::size(buf), f)) {
        for (const wchar_t* p = buf; *p; p++) {
            wchar_t c = *p;
            if (c == 0xFEFF) continue;
            any = true;
            if (c == L'\n') { endLine(); any = false; }
            else if (c == L'\r') {}
            else if (c == L'\t') endField();
            else if (cur.size() < 256) cur += c;
            else curTooLong = true;
        }
    }
    if (any) endLine();
    fclose(f);
    {
        std::lock_guard<std::mutex> l(g_learnMu);
        g_learn = std::move(got);
    }
    if (repaired) {
        SaveLearned();
        Log(L"spotlight: spotlight-uso.txt consertado (textos repetidos de versões antigas)");
    }
}
void SaveLearned() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, LearnFile().c_str(), L"w, ccs=UTF-8") || !f) return;
    std::lock_guard<std::mutex> l(g_learnMu);
    for (auto& [t, L] : g_learn) {
        fwprintf(f, L"%ls\t%d\t%lld", t.c_str(), L.opens, L.last);
        for (size_t i = 0; i < L.queries.size() && i < MAX_QUERIES; i++) fwprintf(f, L"\t%ls", L.queries[i].c_str());
        fputwc(L'\n', f);
    }
    fclose(f);
}
// abriu `target` depois de digitar `q` (já em minúsculas sem acento)
void Learn(const std::wstring& target, const std::wstring& q) {
    {
        std::lock_guard<std::mutex> l(g_learnMu);
        Learned& L = g_learn[target];
        L.opens++;
        L.last = (long long)time(nullptr);
        if (!q.empty()) {
            L.queries.erase(std::remove(L.queries.begin(), L.queries.end(), q), L.queries.end());
            L.queries.insert(L.queries.begin(), q);
            if (L.queries.size() > 8) L.queries.resize(8);
        }
        // o mesmo texto não fica "ensinado" para dois apps: o último escolhido ganha
        for (auto& [t, o] : g_learn) if (t != target) o.queries.erase(std::remove(o.queries.begin(), o.queries.end(), q), o.queries.end());
    }
    SaveLearned();
}
// bônus de quem casa com o texto: uso geral (até +30) e texto já usado para abrir este app (+60)
int LearnedBonus(const LApp& a, const std::wstring& q) {
    int bonus = std::min(30, (int)std::lround(7 * std::log2(1.0 + a.use)));
    std::lock_guard<std::mutex> l(g_learnMu);
    auto it = g_learn.find(a.parsing);
    if (it != g_learn.end()) {
        for (auto& lq : it->second.queries)
            if (lq.rfind(q, 0) == 0 || (q.size() >= lq.size() && q.rfind(lq, 0) == 0)) { bonus += 60; break; }
        long long age = (long long)time(nullptr) - it->second.last;
        if (age < 3 * 86400) bonus += 8;  // usado nos últimos dias
    }
    return bonus;
}

// apps do Dock: fixado (+20), aberto agora (+12), usado nas últimas 24 h (+6). É o sinal mais forte de
// "o que eu uso" (o UserAssist do Windows só conta o que abre pelo Iniciar e pela barra de tarefas)
int DockBonus(const LApp& a) {
    std::wstring p = Lower(a.parsing), file;
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) file = p.substr(slash + 1);
    for (auto& d : apps::list) {
        std::wstring id = Lower(d.id), exe = Lower(d.exe), launch = Lower(d.launch);
        bool same = id == L"aumid:" + p || id == p || launch == L"shell:appsfolder\\" + p ||
                    (!file.empty() && file.size() > 4 && (exe.size() >= file.size() && exe.compare(exe.size() - file.size(), file.size(), file) == 0)) ||
                    !_wcsicmp(d.name.c_str(), a.name.c_str());
        if (!same) continue;
        int b = 0;
        if (d.pinned) b += 20;
        if (!d.wins.empty()) b += 12;
        if (d.lastActive && GetTickCount() - d.lastActive < 24u * 3600 * 1000) b += 6;
        return b;
    }
    return 0;
}

// UserAssist: contagem de aberturas de cada programa/atalho/app da loja, mantida pelo Windows
std::map<std::wstring, int> UserAssistCounts() {
    std::map<std::wstring, int> out;
    const wchar_t* guids[] = { L"{CEBFF5CD-ACE2-4F4F-9178-9926F41749EA}", L"{F4E57C4B-2036-45F0-A9AB-443BCFE33D9F}" };
    for (auto g : guids) {
        HKEY k = nullptr;
        std::wstring path = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist\\") + g + L"\\Count";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; i++) {
            wchar_t name[1024]; DWORD nl = 1024, type = 0; BYTE data[128]; DWORD dl = sizeof(data);
            LONG r = RegEnumValueW(k, i, name, &nl, nullptr, &type, data, &dl);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || dl < 8) continue;
            std::wstring n(name, nl);
            for (auto& c : n) {  // ROT13
                if (c >= L'a' && c <= L'z') c = L'a' + (c - L'a' + 13) % 26;
                else if (c >= L'A' && c <= L'Z') c = L'A' + (c - L'A' + 13) % 26;
            }
            int count = *(int*)(data + 4);
            if (count <= 0 || count > 100000) continue;
            std::wstring key = Lower(n);
            out[key] += count;
            size_t slash = key.find_last_of(L"\\/");  // também pelo nome do arquivo (chrome.exe, firefox.exe…)
            if (slash != std::wstring::npos) out[key.substr(slash + 1)] += count;
        }
        RegCloseKey(k);
    }
    return out;
}

std::atomic<bool> g_appsScanning, g_appsAgain;
std::atomic<ULONGLONG> g_appsAt;  // GetTickCount64 da última leitura dos apps

void LoadApps() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<LApp> found;
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder)))) {
        IEnumShellItems* en = nullptr;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
            IShellItem* it = nullptr;
            while (en->Next(1, &it, nullptr) == S_OK) {
                PWSTR name = nullptr, parse = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parse)) && !Skip(name))
                    found.push_back({ name, parse, Fold(name) });
                if (name) CoTaskMemFree(name);
                if (parse) CoTaskMemFree(parse);
                it->Release();
            }
            en->Release();
        }
        folder->Release();
    }
    std::sort(found.begin(), found.end(), [](const LApp& a, const LApp& b) { return a.lower < b.lower; });
    found.erase(std::unique(found.begin(), found.end(), [](const LApp& a, const LApp& b) { return a.lower == b.lower; }), found.end());
    // Os nossos apps entram sempre, pelo .exe (antes só se tivessem atalho no Menu Iniciar: no PC novo o
    // Ajustes não aparecia ao digitar "ajustes"/"settings"). Com apelidos em português e inglês.
    struct Own { const wchar_t* exe; const wchar_t* name; const wchar_t* words; };
    static const Own own[] = {
        { EXE_SETTINGS, L"Ajustes do Sistema", L"ajustes settings system settings configuracoes preferencias opcoes painel de controle" },
        { EXE_FINDER, L"Finder", L"arquivos explorador explorer pastas files gerenciador de arquivos" },
        { EXE_MONITOR, L"Monitor de Atividade", L"activity monitor gerenciador de tarefas task manager processos" },
        { EXE_TERMINAL, L"Terminal", L"prompt cmd powershell console linha de comando" },
        { EXE_NOTES, L"Notas", L"notes anotacoes bloco" },
        { EXE_CALCULATOR, L"Calculadora", L"calculator calc" },
        { EXE_SCREENSHOT, L"Captura de Tela", L"screenshot print captura recorte snipping" },
        { EXE_PREVIEW, L"Pré-Visualização", L"preview visualizador fotos imagens pdf leitor" },
        { EXE_REMINDERS, L"Lembretes", L"reminders tarefas todo afazeres lista" },
        { EXE_CALENDAR, L"Calendário", L"calendar agenda eventos compromissos" },
        { EXE_TEXTEDIT, L"TextEdit", L"editor de texto bloco de notas notepad wordpad rtf documento" },
        { EXE_CLOCK, L"Relógio", L"clock alarme alarm cronometro stopwatch timer temporizador relogio mundial" },
        { EXE_WEATHER, L"Clima", L"weather tempo previsao temperatura chuva" },
        { EXE_CONTACTS, L"Contatos", L"contacts agenda telefones pessoas vcard" },
        { EXE_PHOTOS, L"Fotos", L"photos imagens galeria fotografias" },
        { EXE_QUICKTIME, L"QuickTime Player", L"quicktime video filme player assistir mp4 mov" },
        { EXE_MUSIC, L"Música", L"music musicas player mp3 album artista tocar" },
        { EXE_TIMEMACHINE, L"Time Machine", L"backup historico de arquivos copia de seguranca restaurar" },
    };
    for (auto& o : own) {
        std::wstring exe = shell::dir + L"\\" + o.exe;
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::wstring le = Lower(exe);
        // o mesmo .exe pelo atalho do Menu Iniciar (o "Liquinix" do instalador) não aparece duas vezes
        found.erase(std::remove_if(found.begin(), found.end(), [&](const LApp& a) { return Lower(a.parsing) == le; }), found.end());
        std::wstring nm = Tr(o.name);  // nome do app no idioma escolhido; o outro fica nos apelidos
        LApp a{ nm, exe, Fold(nm) };
        a.words = Fold(std::wstring(o.words) + L" " + o.name);
        found.push_back(a);
    }
    {   // o próprio Shell pelo atalho do Menu Iniciar não é um app para abrir pelo Spotlight
        std::wstring self = Lower(shell::dir + L"\\" EXE_SHELL);
        found.erase(std::remove_if(found.begin(), found.end(), [&](const LApp& a) { return Lower(a.parsing) == self || a.name == L"Liquinix"; }), found.end());
    }
    LoadLearned();
    auto ua = UserAssistCounts();
    for (auto& a : found) {
        std::wstring k = Lower(a.parsing);
        int use = 0;
        if (auto it = ua.find(k); it != ua.end()) use = it->second;                // AUMID ou caminho igual
        size_t slash = k.find_last_of(L"\\/");
        if (!use && slash != std::wstring::npos) if (auto it = ua.find(k.substr(slash + 1)); it != ua.end()) use = it->second;  // mesmo .exe
        if (!use) if (auto it = ua.find(Lower(a.name) + L".lnk"); it != ua.end()) use = it->second;  // pelo atalho do menu Iniciar
        {
            std::lock_guard<std::mutex> l(g_learnMu);
            if (auto it = g_learn.find(a.parsing); it != g_learn.end()) use += it->second.opens * 3;  // o que abre pelo Spotlight conta mais
        }
        a.use = use;
    }
    { std::lock_guard<std::mutex> l(g_mu); g_apps = std::move(found); }
    g_appsAt = GetTickCount64();
    CoUninitialize();
    if (g_wnd) PostMessageW(g_wnd, WM_SP_APPS, 0, 0);
}

// Lê os apps de novo numa thread de fundo; pedidos durante uma leitura fazem mais uma no fim.
void ScanApps() {
    g_appsAgain = true;
    bool idle = false;
    if (!g_appsScanning.compare_exchange_strong(idle, true)) return;
    std::thread([] {
        while (g_appsAgain.exchange(false)) LoadApps();
        g_appsScanning = false;
    }).detach();
}

// ---- calculadora: + - × ÷ ^ %, parênteses, vírgula ou ponto decimal
struct Calc {
    const wchar_t* p; bool ok = true;
    void Ws() { while (*p == L' ') p++; }
    double Num() {
        Ws();
        if (*p == L'(') { p++; double v = Expr(); Ws(); if (*p == L')') p++; else ok = false; return v; }
        if (*p == L'-') { p++; return -Num(); }
        if (*p == L'+') { p++; return Num(); }
        std::wstring s;
        while (iswdigit(*p) || *p == L'.' || *p == L',') { s += *p == L',' ? L'.' : *p; p++; }
        if (s.empty()) { ok = false; return 0; }
        double v = _wtof(s.c_str());
        Ws();
        if (*p == L'%') { p++; v /= 100; }
        return v;
    }
    double Pow() { double b = Num(); Ws(); if (*p == L'^') { p++; return std::pow(b, Pow()); } return b; }
    double Term() {
        double v = Pow();
        for (;;) {
            Ws();
            if (*p == L'*' || *p == L'x' || *p == L'×') { p++; v *= Pow(); }
            else if (*p == L'/' || *p == L'÷') { p++; double d = Pow(); if (d == 0) ok = false; else v /= d; }
            else return v;
        }
    }
    double Expr() {
        double v = Term();
        for (;;) {
            Ws();
            if (*p == L'+') { p++; v += Term(); }
            else if (*p == L'-') { p++; v -= Term(); }
            else return v;
        }
    }
};

bool Calculate(const std::wstring& q, std::wstring& out) {
    bool op = false, digit = false;
    for (wchar_t c : q) {
        if (iswdigit(c)) digit = true;
        else if (wcschr(L"+-*/x×÷^%()", c)) op = true;
        else if (c != L' ' && c != L'.' && c != L',') return false;
    }
    if (!digit || !op) return false;
    Calc c{ q.c_str() };
    double v = c.Expr();
    c.Ws();
    if (!c.ok || *c.p || !std::isfinite(v)) return false;
    wchar_t b[64];
    swprintf_s(b, L"%.10g", v);
    out = b;
    for (auto& ch : out) if (ch == L'.') ch = L',';  // vírgula decimal, como no Brasil
    return true;
}

// ---- arquivos: índice do Windows Search (thread)
Kind KindOf(const std::wstring& path, bool dir) {
    if (dir) return K_FOLDER;
    std::wstring e = Lower(path.substr(path.find_last_of(L'.') + 1));
    for (auto x : { L"pdf", L"doc", L"docx", L"txt", L"rtf", L"odt", L"xls", L"xlsx", L"csv", L"ppt", L"pptx", L"md", L"pages", L"key", L"numbers" })
        if (e == x) return K_DOC;
    for (auto x : { L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"heic", L"tif", L"tiff", L"svg", L"psd" }) if (e == x) return K_IMAGE;
    for (auto x : { L"mp3", L"wav", L"flac", L"aac", L"m4a", L"ogg", L"mp4", L"mov", L"mkv", L"avi", L"webm", L"als", L"flp", L"cpr" })
        if (e == x) return K_MEDIA;
    return K_OTHER;
}

std::wstring LikeEscape(const std::wstring& q) {
    std::wstring o;
    for (wchar_t c : q) {
        if (c == L'\'') o += L"''";
        else if (c == L'%' || c == L'_' || c == L'[') { o += L'['; o += c; o += L']'; }
        else o += c;
    }
    return o;
}

void SearchFiles(int gen, std::wstring q) {
    auto* out = new std::vector<Result>;
    if (indice::Ready()) {  // o índice próprio (indice.cpp): tudo na memória, milissegundos
        std::vector<indice::Hit> hits;
        indice::Search(Fold(q), 60, [gen] { return gen != g_gen; }, hits);
        for (auto& h : hits) {
            if (h.name.size() > 4 && !_wcsicmp(h.name.c_str() + h.name.size() - 4, L".lnk")) continue;  // atalhos: os apps já aparecem
            out->push_back({ KindOf(h.path, h.dir), h.name, h.path, h.path.substr(0, h.path.find_last_of(L'\\')), h.score });
        }
        if (gen != g_gen || !PostMessageW(g_wnd, WM_SP_FILES, gen, (LPARAM)out)) delete out;
        return;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // índice ainda sendo feito: o do Windows Search
    try {
        _ConnectionPtr con;
        if (SUCCEEDED(con.CreateInstance(__uuidof(Connection)))) {
            con->Open(L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';", L"", L"", adConnectUnspecified);
            // só pelo nome (como a lista do Spotlight); os mais usados recentemente primeiro
            std::wstring sql = L"SELECT TOP 60 System.ItemUrl, System.ItemNameDisplay FROM SystemIndex WHERE System.FileName LIKE '%" + LikeEscape(q) +
                               L"%' AND System.FileExtension <> '.lnk' ORDER BY System.DateModified DESC";
            _RecordsetPtr rs;
            rs.CreateInstance(__uuidof(Recordset));
            rs->Open(sql.c_str(), _variant_t((IDispatch*)con, true), adOpenForwardOnly, adLockReadOnly, adCmdText);
            std::wstring fq = Fold(q);
            while (!rs->AdoEOF && gen == g_gen) {
                _variant_t v = rs->Fields->GetItem(0L)->Value;
                if (v.vt == VT_BSTR) {
                    std::wstring url = v.bstrVal;
                    if (url.rfind(L"file:", 0) == 0) {
                        std::wstring p = url.substr(5);
                        for (auto& c : p) if (c == L'/') c = L'\\';
                        std::wstring low = Lower(p);
                        if (low.find(L"\\appdata\\") == std::wstring::npos && low.find(L"\\$recycle.bin") == std::wstring::npos) {
                            DWORD at = GetFileAttributesW(p.c_str());
                            if (at != INVALID_FILE_ATTRIBUTES) {
                                Result r;
                                r.name = p.substr(p.find_last_of(L'\\') + 1);
                                r.kind = KindOf(p, (at & FILE_ATTRIBUTE_DIRECTORY) != 0);
                                r.target = p;
                                r.detail = p.substr(0, p.find_last_of(L'\\'));
                                r.score = Score(r.name, fq);
                                out->push_back(r);
                            }
                        }
                    }
                }
                rs->MoveNext();
            }
            rs->Close();
            con->Close();
        }
    } catch (_com_error&) {}
    CoUninitialize();
    std::stable_sort(out->begin(), out->end(), [](const Result& a, const Result& b) { return a.score > b.score; });
    if (gen != g_gen || !PostMessageW(g_wnd, WM_SP_FILES, gen, (LPARAM)out)) delete out;
}

// ---- ícones (thread por pedido; poucos de cada vez)
std::wstring IconKey(const Result& r) {
    switch (r.kind) {
    case K_APP: return L"app:" + ui::IconStyleKey() + L":" + r.target;  // estilo/cor de destaque na chave
    case K_SETTING: return L"ajustes";
    case K_CALC: return L"calc";
    case K_WEB: return L"web";
    default: return L"file:" + r.target;
    }
}

// Moldura do Windows em volta de um ícone pequeno ampliado: a borda (2 px) quase toda opaca e, logo para
// dentro dela, uma faixa quase toda transparente. Ícone de verdade cheio (quadrado) tem o miolo opaco;
// redondo tem a borda quase toda transparente.
bool Framed(UINT s, const std::vector<BYTE>& px) {
    if (s < 40) return false;
    auto a = [&](UINT x, UINT y) { return px[((size_t)y * s + x) * 4 + 3]; };
    int edge = 0, in = 0, n = 0;
    UINT k = s / 8;  // faixa de dentro, a 1/8 da borda
    for (UINT i = 2; i + 2 < s; i++) {
        n++;
        edge += (a(i, 0) > 24 || a(i, 1) > 24) + (a(i, s - 1) > 24 || a(i, s - 2) > 24) + (a(0, i) > 24 || a(1, i) > 24) + (a(s - 1, i) > 24 || a(s - 2, i) > 24);
        if (i > k && i + k < s) in += (a(i, k) < 24) + (a(i, s - 1 - k) < 24) + (a(k, i) < 24) + (a(s - 1 - k, i) < 24);
    }
    return edge > n * 4 * 0.85 && in > (n - 2 * (int)k) * 4 * 0.6;
}

// Ícones: uma fila e duas threads fixas (antes, uma thread nova por ícone: digitando, dezenas pediam ao
// Windows ao mesmo tempo e o desenho do Spotlight brigava com elas pelo processador, quadros de 40-70 ms).
// Os mais recentes primeiro: ao digitar, os resultados de antes já não importam.
struct IconJob { Result r; std::wstring key; int px; };
std::deque<IconJob> g_iconQ;
std::condition_variable g_iconCv;
int g_iconWorkers;

void LoadIcon(const Result& copy, const std::wstring& key, int px);

void IconWorker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // o desenho vem antes
    for (;;) {
        IconJob j;
        {
            std::unique_lock<std::mutex> l(g_iconMu);
            g_iconCv.wait(l, [] { return !g_iconQ.empty(); });
            j = std::move(g_iconQ.back());
            g_iconQ.pop_back();
        }
        LoadIcon(j.r, j.key, j.px);
    }
}

void RequestIcon(const Result& r, int px) {
    std::wstring key = IconKey(r);
    if (key == L"calc" || key == L"web") return;
    {
        std::lock_guard<std::mutex> l(g_iconMu);
        auto& ic = g_icons[key];
        if (ic.loading || !ic.px.empty() || ic.bmp) return;
        ic.loading = true;
        g_iconQ.push_back({ r, key, px });
        while (g_iconQ.size() > 64) {  // pedidos velhos demais saem da fila (voltam a ser pedidos se aparecerem)
            g_icons[g_iconQ.front().key].loading = false;
            g_iconQ.pop_front();
        }
        while (g_iconWorkers < 2) { g_iconWorkers++; std::thread(IconWorker).detach(); }
    }
    g_iconCv.notify_one();
}

void LoadIcon(const Result& copy, const std::wstring& key, int px) {
    {
        Icon got;
        bool ok = false;
        if (copy.kind == K_APP) {
            std::wstring exe;
            if (copy.target.size() > 4 && !_wcsicmp(copy.target.c_str() + copy.target.size() - 4, L".exe")) exe = copy.target;
            std::wstring png = apps::PackIconByName(copy.name, exe);  // ícone do Mac quando houver
            ok = !png.empty() && ui::LoadPng(png, got.w, got.h, got.px);
            if (!ok) {
                // só o ícone (sem a moldura de miniatura do Windows, que deixava o ícone minúsculo dentro de uma
                // caixa), no tamanho grande; o que sobra em volta é cortado
                ok = ui::ShellImage(L"shell:AppsFolder\\" + copy.target, 256, got.w, got.h, got.px, true);
                if (!ok) ok = ui::ShellImage(L"shell:AppsFolder\\" + copy.target, px, got.w, got.h, got.px, false);
                if (!ok && !exe.empty()) ok = ui::ShellImage(exe, 256, got.w, got.h, got.px, true);  // nossos apps (pelo .exe)
                if (ok) {
                    UINT before = got.w;
                    ui::TrimToSquare(got.w, got.h, got.px);
                    // o Windows centraliza o ícone pequeno no quadro grande sem ampliar: o que restou é o tamanho real
                    got.lowres = got.w < 96 && got.w < before;
                    // programa só com ícone pequeno: no tamanho grande o Windows põe uma moldura fina em volta dele
                    // (contorno opaco com o miolo vazio). Aí vale o ícone de 48 px, sem moldura, no quadrado cinza.
                    if (!got.lowres && Framed(got.w, got.px)) {
                        Icon s48;
                        if (ui::ShellImage(L"shell:AppsFolder\\" + copy.target, 48, s48.w, s48.h, s48.px, true)) {
                            ui::TrimToSquare(s48.w, s48.h, s48.px);
                            got = std::move(s48);
                        }
                        got.lowres = true;
                    }
                }
            }
        } else if (copy.kind == K_SETTING) {
            std::wstring png = apps::PackIconByName(L"System Settings");
            ok = !png.empty() && ui::LoadPng(png, got.w, got.h, got.px);
            if (!ok) ok = ui::ShellImage(shell::dir + L"\\" EXE_SETTINGS, px, got.w, got.h, got.px, true);
        } else {
            ok = ui::ShellImage(copy.target, px, got.w, got.h, got.px, copy.kind != K_IMAGE);
        }
        {
            std::lock_guard<std::mutex> l(g_iconMu);
            auto& ic = g_icons[key];
            ic.loading = false;
            if (ok) { ic.w = got.w; ic.h = got.h; ic.px = std::move(got.px); ic.lowres = got.lowres; }
            else ic.failed = true;
        }
        if (g_wnd) PostMessageW(g_wnd, WM_SP_ICON, 0, 0);
    }
}

// 0 = normal (ou ainda carregando), 1 = pequeno, 2 = sem ícone
int IconState(const Result& r) {
    std::lock_guard<std::mutex> l(g_iconMu);
    auto it = g_icons.find(IconKey(r));
    if (it == g_icons.end()) return 0;
    return it->second.failed ? 2 : it->second.lowres ? 1 : 0;
}

// no máximo 3 ícones novos sobem para a GPU por quadro (cada um de 256 px custa ~1 ms); os outros, no próximo
int g_uploads;
double g_uploadFrame;
ID2D1Bitmap* IconFor(ID2D1RenderTarget* rt, const Result& r) {
    std::lock_guard<std::mutex> l(g_iconMu);
    auto it = g_icons.find(IconKey(r));
    if (it == g_icons.end()) return nullptr;
    auto& ic = it->second;
    if (!ic.bmp && !ic.px.empty()) {
        double now = anim::Now();
        if (now - g_uploadFrame > 8) { g_uploadFrame = now; g_uploads = 0; }
        if (g_uploads >= 3) { if (g_wnd) PostMessageW(g_wnd, WM_SP_ICON, 0, 0); return nullptr; }  // pede outro quadro
        g_uploads++;
        if (r.kind == K_APP) { std::wstring st = ui::IconStyle(); if (st != L"padrao") ui::StyleIcon(ic.w, ic.h, ic.px, st); }  // Tahoe: Escuro/Transparente/Tingido também aqui
        ic.bmp = ui::Bitmap(rt, ic.w, ic.h, ic.px); ic.px.clear(); ic.px.shrink_to_fit();
    }
    return ic.bmp;
}

// ---- resultados e linhas
std::vector<Result> g_files;     // última resposta do índice
std::wstring g_calc;

const wchar_t* Section(Kind k) {
    switch (k) {
    case K_APP: return Tr(L"Aplicativos");
    case K_SETTING: return Tr(L"Ajustes do Sistema");
    case K_DOC: return Tr(L"Documentos");
    case K_FOLDER: return Tr(L"Pastas");
    case K_IMAGE: return Tr(L"Imagens");
    case K_MEDIA: return Tr(L"Músicas e Filmes");
    case K_CALC: return Tr(L"Calculadora");
    case K_WEB: return Tr(L"Web");
    default: return Tr(L"Outros");
    }
}

const wchar_t* KindName(Kind k) {
    switch (k) {
    case K_APP: return Tr(L"Aplicativo");
    case K_SETTING: return Tr(L"Ajustes do Sistema");
    case K_DOC: return Tr(L"Documento");
    case K_FOLDER: return Tr(L"Pasta");
    case K_IMAGE: return Tr(L"Imagem");
    case K_MEDIA: return Tr(L"Mídia");
    case K_CALC: return Tr(L"Calculadora");
    case K_WEB: return Tr(L"Pesquisar na Web");
    default: return Tr(L"Arquivo");
    }
}

// ---- Sugestões do Google (como as sugestões da web do Spotlight do Mac)
// O texto digitado vai para suggestqueries.google.com; desliga com spotlight_google=0 nos ajustes do Shell.
std::vector<std::wstring> g_sugg;  // sugestões para g_suggFor
std::wstring g_suggFor;

std::wstring UrlEncode(const std::wstring& q) {
    std::wstring u;
    for (wchar_t c : q) {
        if (c < 128 && (iswalnum(c) || c == L'-' || c == L'_' || c == L'.')) u += c;
        else if (c == L' ') u += L'+';
        else {
            char b[8]; int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, b, 8, nullptr, nullptr);
            for (int i = 0; i < n; i++) { wchar_t h[4]; swprintf_s(h, L"%%%02X", (unsigned char)b[i]); u += h; }
        }
    }
    return u;
}

// ["consulta",["s1","s2",...],...]: pega as strings do segundo item
std::vector<std::wstring> ParseSuggestions(const std::string& body) {
    std::vector<std::wstring> out;
    std::wstring w;
    int n = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
    w.resize(n); MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), w.data(), n);
    size_t i = w.find(L'[');
    if (i == std::wstring::npos) return out;
    i = w.find(L'[', i + 1);  // começo da lista de sugestões
    if (i == std::wstring::npos) return out;
    i++;
    while (i < w.size() && w[i] != L']') {
        if (w[i] != L'"') { i++; continue; }
        std::wstring cur;
        for (i++; i < w.size() && w[i] != L'"'; i++) {
            if (w[i] == L'\\' && i + 1 < w.size()) {
                wchar_t e = w[++i];
                if (e == L'u' && i + 4 < w.size()) { cur += (wchar_t)wcstol(w.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; }
                else if (e == L'n') cur += L' ';
                else cur += e;
            } else cur += w[i];
        }
        i++;
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

void FetchSuggestions(int gen, std::wstring q) {
    Sleep(120);  // espera a digitação parar um instante
    if (gen != g_gen) return;
    auto* out = new std::pair<std::wstring, std::vector<std::wstring>>(q, {});
    HINTERNET ses = WinHttpOpen(L"ShellProprio/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 2000, 2000, 2000, 2000);
        HINTERNET con = WinHttpConnect(ses, L"suggestqueries.google.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        std::wstring path = L"/complete/search?client=chrome&hl=" + std::wstring(idioma::Locale()) + L"&ie=UTF-8&oe=UTF-8&q=" + UrlEncode(q);
        HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
            std::string body;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 64 * 1024) {
                std::string chunk(avail, '\0'); DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
                body.append(chunk.data(), got);
            }
            out->second = ParseSuggestions(body);
        }
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
    }
    if (gen != g_gen || !g_wnd || !PostMessageW(g_wnd, WM_SP_SUGG, gen, (LPARAM)out)) delete out;
}

int g_mode;  // Tahoe: 0 tudo, 1 aplicativos, 2 arquivos, 3 ações (círculos ao lado da cápsula)
bool ModeAllows(int k);

// ---- Tahoe: painel de cada modo (vídeos do macOS 26). Com o campo vazio, escolher um círculo (ou o "Apps"
// do Dock) abre o painel do modo: cabeçalho com o símbolo e o nome, uma fileira de filtros (categorias dos
// apps; tipos de arquivo) e o conteúdo: grade de 5 apps (ou lista, no botão ⋯), Recentes dos arquivos,
// Sugestões das ações.
int g_chip;          // filtro escolhido (0 = nenhum)
float g_pill = -1;   // Tahoe: largura atual do vidro do campo (anima entre a cápsula e o painel inteiro)
double g_listIn;     // quando o conteúdo do modo mudou (entra esmaecendo e subindo um pouco)
void Place();
void ModeChanged() {
    g_listIn = anim::Now();
    float w0 = W;
    Metricas();  // Tahoe: entrar/sair do Apps muda a largura do painel
    if (W != w0 && g_visible) { g_pill = W; Place(); }
}
bool g_appsList;     // apps em lista em vez de grade (botão ⋯)
std::vector<D2D1_RECT_F> g_chipR;
D2D1_RECT_F g_moreR{};

bool ModePanel() { return liquid::On() && g_mode >= 1 && g_mode <= 3; }
std::vector<std::wstring> Chips() {
    std::vector<std::wstring> v;
    if (!liquid::On()) return v;
    if (g_mode == 1) for (int k = 1; k < launchpad::CategoryCount(); k++) v.push_back(launchpad::CategoryName(k));
    else if (g_mode == 2 || (g_mode == 0 && liquid::Golden())) v = { Tr(L"Documentos"), Tr(L"Pastas"), Tr(L"Imagens"), Tr(L"Músicas e Filmes"), Tr(L"Outros") };
    return v;
}
bool HasChips() { return (ModePanel() && g_mode != 3) || (liquid::Golden() && g_mode == 0 && !g_query.empty()); }
// macOS 27: com fichas, a lista começa 6 abaixo delas (fichas de 26 com o centro a 33 do alto do painel)
float ListTop() { if (liquid::Leopard()) return FIELD_H + 1; return liquid::Golden() && HasChips() ? FIELD_H + GG_GAP + 52 : FIELD_H + 1 + GG_GAP + (HasChips() ? CHIPS_H : 0); }
bool ChipAllows(Kind k, const std::wstring& name) {
    bool ggAll = liquid::Golden() && g_mode == 0;  // macOS 27: as fichas também no Spotlight comum
    if (!g_chip || (!ModePanel() && !ggAll)) return true;
    if (g_mode == 1) return k != K_APP || launchpad::Category(name) == g_chip;
    if (g_mode == 2 || ggAll) {
        static const Kind kinds[] = { K_OTHER, K_DOC, K_FOLDER, K_IMAGE, K_MEDIA, K_OTHER };
        return g_chip < 6 && k == kinds[g_chip];
    }
    return true;
}

// arquivos recentes do Windows (atalhos da pasta Recent), do mais novo para o mais velho
std::vector<Result> RecentFiles(size_t max) {
    std::vector<std::pair<FILETIME, std::wstring>> lnks;
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_RECENT, nullptr, 0, dir))) return {};
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((std::wstring(dir) + L"\\*.lnk").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return {};
    do lnks.push_back({ fd.ftLastWriteTime, std::wstring(dir) + L"\\" + fd.cFileName }); while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(lnks.begin(), lnks.end(), [](auto& a, auto& b) { return CompareFileTime(&a.first, &b.first) > 0; });
    std::vector<Result> out;
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return {};
    IPersistFile* pf = nullptr;
    sl->QueryInterface(IID_PPV_ARGS(&pf));
    for (auto& [t, path] : lnks) {
        if (out.size() >= max || !pf) break;
        if (FAILED(pf->Load(path.c_str(), STGM_READ))) continue;
        wchar_t target[MAX_PATH] = {};
        if (FAILED(sl->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) || !target[0]) continue;
        DWORD a = GetFileAttributesW(target);
        if (a == INVALID_FILE_ATTRIBUTES) continue;  // apagado ou num disco que não está ligado
        std::wstring tp = target;
        size_t sl2 = tp.find_last_of(L'\\');
        bool dirk = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        Kind k = KindOf(tp, dirk);
        if (!ChipAllows(k, L"")) continue;
        out.push_back({ k, sl2 == std::wstring::npos ? tp : tp.substr(sl2 + 1), tp, sl2 == std::wstring::npos ? L"" : tp.substr(0, sl2), 0 });
    }
    if (pf) pf->Release();
    sl->Release();
    return out;
}

// Sem a lista de recentes do Windows (desligada nos ajustes de privacidade): os arquivos mudados por último
// nas pastas do usuário (só o primeiro nível de cada uma, para ser rápido)
std::vector<Result> RecentByDate(size_t max) {
    static const KNOWNFOLDERID* ids[] = { &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Desktop, &FOLDERID_Pictures, &FOLDERID_Videos, &FOLDERID_Music };
    std::vector<std::pair<FILETIME, Result>> all;
    for (auto* id : ids) {
        PWSTR p = nullptr;
        if (FAILED(SHGetKnownFolderPath(*id, 0, nullptr, &p))) continue;
        std::wstring dir = p;
        CoTaskMemFree(p);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
            bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring full = dir + L"\\" + fd.cFileName;
            Kind k = KindOf(full, d);
            if (!ChipAllows(k, L"")) continue;
            all.push_back({ fd.ftLastWriteTime, { k, fd.cFileName, full, dir, 0 } });
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return CompareFileTime(&a.first, &b.first) > 0; });
    std::vector<Result> out;
    for (auto& [t, r] : all) { if (out.size() >= max) break; out.push_back(r); }
    return out;
}

// ---- Apps (Tahoe), como o Launchpad: segurar um app faz a grade tremer; arrastar muda a ordem (ajuste
// apps_ordem, nomes separados por "|"; app novo vai para o fim); o × tira o app do painel (launchpad_ocultos,
// a busca ainda acha); botão direito no fundo: Editar Apps, Organizar por Nome, Trazer de Volta.
bool g_edit;                // a grade está tremendo
double g_editAt;
int g_press = -1;           // célula pressionada (índice em g_rows)
bool g_pressWasSel;         // já era a escolhida: soltar sem arrastar abre (o segundo clique)
bool g_dragging;
D2D1_POINT_2F g_downPt, g_dragPt;
std::map<std::wstring, D2D1_POINT_2F> g_cellAt;  // onde cada app está desenhado: desliza até o lugar novo
const UINT_PTR T_HOLD = 5;
// Arrastar um resultado (arquivo, pasta, app com .exe) para fora, como no Mac: soltar copia (ou cria atalho).
int g_fpress = -1;          // linha de arquivo pressionada
bool g_fileDrag;            // o arrasto do shell está em curso (não fecha ao perder o foco)

std::vector<std::wstring> SplitBar(const std::wstring& s) {
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(L'|', a);
        if (b == std::wstring::npos) b = s.size();
        if (b > a) v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}
std::wstring JoinBar(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& x : v) { if (!s.empty()) s += L'|'; s += x; }
    return s;
}
bool CanArrange() { return g_mode == 1 && !g_appsList && g_chip == 0 && Fold(g_query).empty(); }

void BuildMode() {
    if (g_mode == 1) {
        std::vector<Result> v;
        {
            std::lock_guard<std::mutex> l(g_mu);
            for (auto& a : g_apps)  // sem os tirados à mão e, com launchpad_sem_sistema, sem as ferramentas do Windows
                if (ChipAllows(K_APP, a.name) && !launchpad::Escondido(a.name, a.parsing)) v.push_back({ K_APP, a.name, a.parsing, L"", a.use });
        }
        std::sort(v.begin(), v.end(), [](const Result& a, const Result& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
        auto ord = SplitBar(shell::Setting(L"apps_ordem", L""));  // a ordem arrastada; quem não está nela vai para o fim
        if (!ord.empty()) {
            std::map<std::wstring, int> rank;
            for (int k = 0; k < (int)ord.size(); k++) rank.emplace(ord[k], k);
            auto rk = [&](const Result& r) { auto it = rank.find(r.name); return it == rank.end() ? 1 << 30 : it->second; };
            std::stable_sort(v.begin(), v.end(), [&](const Result& a, const Result& b) { return rk(a) < rk(b); });
        }
        // macOS 27 (Apps.png): a primeira fileira são os 7 apps mais usados, separada do resto por uma linha fina
        if (liquid::Golden() && !g_appsList && g_chip == 0 && v.size() > 14) {
            std::vector<Result> top = v;
            std::stable_sort(top.begin(), top.end(), [](const Result& a, const Result& b) { return a.score > b.score; });
            for (int k = 0; k < 7; k++) { g_res.push_back(top[k]); Row s{ L"", (int)g_res.size() - 1, R_APP }; s.sect = 7; g_rows.push_back(s); }
        }
        bool firstAfter = true;
        for (auto& r : v) {
            g_res.push_back(r);
            Row a{ L"", (int)g_res.size() - 1, g_appsList ? R_ROW : R_APP };
            if (firstAfter && !g_rows.empty() && g_rows.back().sect == 7) a.sect = 8;  // a primeira depois da linha
            firstAfter = false;
            g_rows.push_back(a);
        }
    } else if (g_mode == 2) {
        // lidos do disco no máximo a cada 5 s (trocar de filtro e voltar não relê)
        static std::vector<Result> cache;
        static DWORD at;
        static int chipAt = -1;
        if (cache.empty() || GetTickCount() - at > 5000 || chipAt != g_chip) {
            cache = RecentFiles(40);
            if (cache.empty()) cache = RecentByDate(40);
            at = GetTickCount(); chipAt = g_chip;
        }
        auto v = cache;
        if (!v.empty()) g_rows.push_back({ Tr(L"Recentes"), -1, R_HEAD });
        for (auto& r : v) { g_res.push_back(r); g_rows.push_back({ L"", (int)g_res.size() - 1, R_ROW }); }
    } else if (g_mode == 3) {
        g_rows.push_back({ Tr(L"Sugestões"), -1, R_HEAD });
        for (auto& pg : SETTINGS) { g_res.push_back({ K_SETTING, Tr(pg.name), pg.id, L"", 0 }); g_rows.push_back({ L"", (int)g_res.size() - 1, R_ROW }); }
    }
    for (int i = 0; i < (int)g_rows.size(); i++) if (g_rows[i].res >= 0) { g_sel = i; break; }
    for (int i = 0, n = 0; i < (int)g_rows.size() && n < 25; i++)  // o resto pede ao aparecer na tela
        if (g_rows[i].res >= 0) { RequestIcon(g_res[g_rows[i].res], 96); n++; }
}

void Rebuild() {
    g_res.clear();
    g_rows.clear();
    g_canMore.clear();
    g_sel = -1;
    g_scroll = 0;
    std::wstring q = Fold(g_query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    if (q.empty()) {
        if (ModePanel()) BuildMode();
        else if (liquid::Golden() && g_mode == 0) {
            // macOS 27 (vídeo do MacVince, 5:56): com o campo vazio, a lista mostra os modos com os atalhos à direita
            for (int k = 0; k < 4; k++) { Row m; m.type = R_MODE; m.sect = k; g_rows.push_back(m); }
        }
        return;
    }
    std::vector<Result> appsR, setR;
    {
        std::lock_guard<std::mutex> l(g_mu);
        for (auto& a : g_apps) {
            int s = Score(a.name, q);
            if (!a.words.empty() && s < 95) {  // apelido: "settings" acha o Ajustes do Sistema
                size_t pos = a.words.find(q);
                while (pos != std::wstring::npos && pos > 0 && a.words[pos - 1] != L' ') pos = a.words.find(q, pos + 1);
                if (pos != std::wstring::npos) s = 95;
            }
            if (!s || !ChipAllows(K_APP, a.name)) continue;
            if (s == 80) s = 90;  // começo de uma palavra do nome ("chr" em "Google Chrome") vale quase o começo do nome
            appsR.push_back({ K_APP, a.name, a.parsing, L"", s + LearnedBonus(a, q) + DockBonus(a) });
        }
    }
    for (auto& p : SETTINGS) {
        int s = Score(Tr(p.name), q);
        if (!s) s = Score(p.name, q);  // em inglês, o nome em português também acha
        if (!s && q.size() >= 3 && Fold(p.words).find(q) != std::wstring::npos) s = 40;  // palavras-chave da página
        if (s) setR.push_back({ K_SETTING, Tr(p.name), p.id, L"", s });
    }
    auto byScore = [](const Result& a, const Result& b) { return a.score > b.score || (a.score == b.score && a.name.size() < b.name.size()); };
    std::stable_sort(appsR.begin(), appsR.end(), byScore);
    std::stable_sort(setR.begin(), setR.end(), byScore);
    // melhor resultado: a conta; senão o app/ajuste/arquivo de maior nota (o app ganha no empate)
    std::vector<std::pair<const wchar_t*, std::vector<Result>>> groups;
    Result top{ K_OTHER };
    bool hasTop = false;
    if (Calculate(g_query, g_calc)) { top = { K_CALC, g_calc, g_calc, g_query }; hasTop = true; }
    else {
        // como no Mac, apps vêm antes de ajustes e arquivos com a mesma nota (e mesmo um pouco menor)
        const Result* best = nullptr;
        int bestScore = 0;
        const int bonus[] = { 25, 5, 0 };  // app ganha de pasta/arquivo com nota parecida (antes: uma pasta "Chrome" passava na frente do Google Chrome)
        int k = 0;
        for (auto* v : { &appsR, &setR, &g_files }) {
            if (!v->empty() && (*v)[0].score + bonus[k] > bestScore) { best = &(*v)[0]; bestScore = best->score + bonus[k]; }
            k++;
        }
        if (best && best->score >= 50) { top = *best; hasTop = true; }
    }
    auto take = [&](std::vector<Result>& v, Kind k, size_t max) {
        std::vector<Result> o;
        if (!ModeAllows(k)) return;
        if (g_more.count(k)) max = 200;
        for (auto& r : v) {
            if (r.kind != k || !ChipAllows(r.kind, r.name)) continue;
            if (hasTop && r.kind == top.kind && r.target == top.target) continue;
            if (o.size() >= max) { g_canMore.insert(k); break; }
            o.push_back(r);
        }
        if (!o.empty()) groups.push_back({ Section(k), o });
    };
    if (hasTop && (!ModeAllows(top.kind) || !ChipAllows(top.kind, top.name))) hasTop = false;  // Tahoe: modo escolhido num círculo
    if (hasTop) groups.push_back({ Tr(L"Melhor Resultado"), { top } });
    take(appsR, K_APP, 5);
    take(setR, K_SETTING, 3);
    for (Kind k : { K_DOC, K_FOLDER, K_IMAGE, K_MEDIA, K_OTHER }) take(g_files, k, k == K_IMAGE ? 14 : 4);  // imagens: duas fileiras
    if (g_mode == 0) {
        std::vector<Result> web{ { K_WEB, Tr(L"Pesquisar no Google por “") + g_query + L"”", g_query } };
        std::wstring fq = Fold(g_query);
        if (Fold(g_suggFor) == fq || (!g_suggFor.empty() && fq.rfind(Fold(g_suggFor), 0) == 0)) {
            for (auto& sg : g_sugg) {
                if (Fold(sg) == fq) continue;
                if (fq.size() > Fold(g_suggFor).size() && Fold(sg).rfind(fq, 0) != 0) continue;  // sugestão antiga que não serve mais
                web.push_back({ K_WEB, sg, sg });
                if (web.size() >= 5) break;
            }
        }
        groups.push_back({ Tr(L"Sugestões do Google"), web });
    }
    // macOS 27 Golden Gate: uma lista só, sem cabeçalhos, sem o cartão do melhor resultado e sem miniaturas (cada
    // resultado numa linha de 63,5 com o nome e, embaixo, o tipo e o lugar em cinza)
    const bool flat = liquid::Golden();
    for (size_t g = 0; g < groups.size(); g++) {
        auto& [head, list] = groups[g];
        bool card = hasTop && g == 0 && !flat;   // melhor resultado: cartão, sem cabeçalho
        bool tiles = !flat && !wcscmp(head, Section(K_IMAGE));
        if (!card && !flat) {
            Row hr{ head, -1, R_HEAD };
            for (int k = K_APP; k <= K_OTHER; k++) if (!wcscmp(head, Section((Kind)k))) hr.sect = k;
            g_rows.push_back(hr);
        }
        for (auto& r : list) { g_res.push_back(r); g_rows.push_back({ L"", (int)g_res.size() - 1, card ? R_CARD : tiles ? R_TILE : R_ROW }); }
    }
    for (int i = 0; i < (int)g_rows.size(); i++) if (g_rows[i].res >= 0) { g_sel = i; break; }
    for (int i = 0, n = 0; i < (int)g_rows.size() && n < 16; i++)
        if (g_rows[i].res >= 0) { RequestIcon(g_res[g_rows[i].res], g_rows[i].type == R_TILE ? 160 : 64); n++; }
}

// posições de tudo no conteúdo (a partir do topo da lista); devolve a altura total
float Layout() {
    if (liquid::Leopard()) {  // Leopard: uma lista só; o cabeçalho de cada seção vai na coluna da esquerda da 1ª linha
        float y = 4;
        bool first = true;
        for (auto& row : g_rows) {
            if (row.type == R_HEAD) { if (!first) y += 5; row.r = D2D1::RectF(0, y, W, y); first = false; continue; }
            if (row.type == R_APP || row.type == R_MODE) { row.r = D2D1::RectF(0, 0, 0, 0); continue; }
            row.r = D2D1::RectF(0, y, W, y + LEO_ROW);
            y += LEO_ROW;
            first = false;
        }
        return y + 4;
    }
    const bool gg = liquid::Golden();
    float y = gg ? (g_mode == 1 ? 17.5f : HasChips() ? 7.5f : 8.f) : 8, x = PAD;  // (a lista já começa 6 abaixo das fichas)
    bool inTiles = false;
    // macOS 27 (Apps.png): 7 colunas de 115 a 18,5 da borda, fileiras de 102, linha de 15 depois dos mais usados
    const float gpad = gg ? 18.5f : PAD, cellW = gg ? 115.f : (W - 2 * PAD) / APP_COLS, cellH = gg ? 102.f : APP_CELL_H;
    int col = 0;
    for (auto& row : g_rows) {
        if (row.type == R_APP) {  // grade de 5, como o Apps do macOS 26
            if (row.sect == 8 && col) { col = 0; y += cellH; }
            if (row.sect == 8) y += 15;
            if (col == (int)APP_COLS) { col = 0; y += cellH; }
            row.r = D2D1::RectF(gpad + col * cellW, y, gpad + (col + 1) * cellW, y + cellH);
            col++;
            continue;
        }
        if (col) { y += cellH; col = 0; }
        if (row.type == R_TILE) {
            if (!inTiles) { x = PAD + 4; inTiles = true; }
            if (x + TILE > W - PAD) { x = PAD + 4; y += TILE + TILE_GAP; }
            row.r = D2D1::RectF(x, y, x + TILE, y + TILE);
            x += TILE + TILE_GAP;
            continue;
        }
        if (inTiles) { y += TILE + TILE_GAP; inTiles = false; }
        float h = row.type == R_MODE ? 47.f : row.type == R_HEAD ? HEAD_H : row.type == R_CARD ? CARD_H : liquid::Golden() ? CARD_H : ROW_H;
        row.r = D2D1::RectF(PAD, y, W - PAD, y + h);
        y += h + (row.type == R_CARD ? 4 : 0);
    }
    if (inTiles) y += TILE + TILE_GAP;
    if (col) y += cellH;
    return y + 6;
}

float ListHeight() { return Layout(); }

float PanelH() { return g_rows.empty() && !HasChips() ? FIELD_H : ListTop() + std::min(MAX_LIST_H - (ListTop() - FIELD_H - 1), ListHeight()); }

float Sc() { return ui::Scale(g_wnd); }

struct Pal { D2D1_COLOR_F tint, text, text2, sep, sel, hi, card, place; };
Pal Colors() {
    // macOS 27: o Spotlight é sempre escuro e esfumado, com texto branco, também no modo claro (captura e vídeo)
    if (liquid::Golden())
        return { D2D1::ColorF(0.09f, 0.07f, 0.06f, 0.42f), D2D1::ColorF(1, 1, 1, 0.95f), D2D1::ColorF(1, 1, 1, 0.55f),
                 D2D1::ColorF(1, 1, 1, 0.10f), D2D1::ColorF(0.04f, 0.45f, 0.96f, 1), D2D1::ColorF(1, 1, 1, 0.14f),
                 D2D1::ColorF(1, 1, 1, 0.08f), D2D1::ColorF(1, 1, 1, 0.50f) };
    if (ui::Dark()) return { D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.62f), D2D1::ColorF(1, 1, 1, 0.92f), D2D1::ColorF(1, 1, 1, 0.50f),
                             D2D1::ColorF(1, 1, 1, 0.10f), D2D1::ColorF(0.04f, 0.40f, 0.86f, 1), D2D1::ColorF(1, 1, 1, 0.12f),
                             D2D1::ColorF(1, 1, 1, 0.07f), D2D1::ColorF(1, 1, 1, 0.32f) };
    return { D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.62f), D2D1::ColorF(0, 0, 0, 0.86f), D2D1::ColorF(0, 0, 0, 0.48f),
             D2D1::ColorF(0, 0, 0, 0.10f), D2D1::ColorF(0.04f, 0.45f, 0.93f, 1), D2D1::ColorF(0, 0, 0, 0.09f),
             D2D1::ColorF(1, 1, 1, 0.55f), D2D1::ColorF(0, 0, 0, 0.28f) };
}

void Text(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, ID2D1SolidColorBrush* br, D2D1_COLOR_F c) {
    br->SetColor(c);
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// nome com o trecho digitado em negrito ("zoom preços"), como no Mac
void NameText(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, ID2D1SolidColorBrush* br, D2D1_COLOR_F c) {
    IDWriteTextLayout* lay = nullptr;
    if (FAILED(ui::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, r.right - r.left, r.bottom - r.top, &lay))) return;
    std::wstring q = Fold(g_query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    size_t at = q.empty() ? std::wstring::npos : Fold(s).find(q);
    if (at != std::wstring::npos) lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, { (UINT32)at, (UINT32)q.size() });
    br->SetColor(c);
    rt->DrawTextLayout({ r.left, r.top }, lay, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    lay->Release();
}

void DrawIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Result& r, D2D1_RECT_F box, const Pal& p) {
    if (r.kind == K_CALC || r.kind == K_WEB) {
        br->SetColor(p.text2);
        float k = (box.right - box.left) * 0.12f;
        D2D1_RECT_F in{ box.left + k, box.top + k, box.right - k, box.bottom - k };
        if (!simbolo::Draw(dc, r.kind == K_WEB ? L"globe" : L"plus.magnifyingglass", in, br))
            dc->DrawEllipse(D2D1::Ellipse({ (in.left + in.right) / 2, (in.top + in.bottom) / 2 }, (in.right - in.left) / 2, (in.bottom - in.top) / 2), br);
        return;
    }
    int st = r.kind == K_APP ? IconState(r) : 0;
    if (st) {  // Tahoe: ícone fora do padrão (ou nenhum) num quadrado arredondado cinza
        float S = box.right - box.left, rad = S * 0.225f;
        D2D1_RECT_F q = D2D1::RectF(box.left + S * 0.04f, box.top + S * 0.04f, box.right - S * 0.04f, box.bottom - S * 0.04f);
        ID2D1GradientStopCollection* sc = nullptr;
        D2D1_GRADIENT_STOP gs[2] = { { 0, D2D1::ColorF(0.93f, 0.93f, 0.94f) }, { 1, D2D1::ColorF(0.80f, 0.80f, 0.82f) } };
        if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, &sc))) {
            ID2D1LinearGradientBrush* lg = nullptr;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, q.top }, { 0, q.bottom }), sc, &lg))) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(q, rad, rad), lg);
                lg->Release();
            }
            sc->Release();
        }
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(q, rad, rad), br, std::max(0.5f, S / 96));
        float cx = (q.left + q.right) / 2, cy = (q.top + q.bottom) / 2;
        if (ID2D1Bitmap* b = st == 1 ? IconFor(dc, r) : nullptr) {
            D2D1_SIZE_F bs = b->GetSize();
            float k = S * 0.58f / std::max(bs.width, bs.height);
            float dw = bs.width * k, dh = bs.height * k;
            dc->DrawBitmap(b, D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {  // sem ícone: a inicial
            std::wstring ini = r.name.empty() ? L"?" : std::wstring(1, (wchar_t)towupper(r.name[0]));
            br->SetColor(D2D1::ColorF(0.35f, 0.35f, 0.38f));
            static IDWriteTextFormat* big = nullptr;
            if (!big) { big = ui::Format(30, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER); if (big) big->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
            if (big) {
                float k = S / 64;
                D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
                dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, { cx, cy }) * old);
                dc->DrawTextW(ini.c_str(), (UINT32)ini.size(), big, D2D1::RectF(cx - 32, cy - 32, cx + 32, cy + 32), br);
                dc->SetTransform(old);
            }
        }
        return;
    }
    if (ID2D1Bitmap* b = IconFor(dc, r)) {
        if (r.kind == K_IMAGE) {  // miniatura: preenche o quadrado, cortando o que sobra
            D2D1_SIZE_F s = b->GetSize();
            float bw = box.right - box.left, bh = box.bottom - box.top, k = std::max(bw / s.width, bh / s.height);
            float dw = s.width * k, dh = s.height * k, cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
            dc->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);  // o que sobra não vaza do quadrado
            dc->DrawBitmap(b, D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            dc->PopAxisAlignedClip();
        } else dc->DrawBitmap(b, box, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }
    if (r.kind == K_FOLDER) ui::DrawFolder(dc, box, 0);
}

// o que vai à direita da linha: a pasta do arquivo, "Ajustes do Sistema", ↗ na web
std::wstring Accessory(const Result& r) {
    switch (r.kind) {
    case K_APP: return L"";
    case K_SETTING: return Tr(L"Ajustes do Sistema");
    case K_WEB: return L"\x2197";
    case K_CALC: return Tr(L"Calculadora");
    default: {
        size_t s = r.detail.find_last_of(L'\\');
        return s == std::wstring::npos ? r.detail : r.detail.substr(s + 1);
    }
    }
}

void Transition(float& fade, float& scale);

// ---- Tahoe (vídeo do macOS 26): só com o campo vazio, o Spotlight é uma cápsula (largura ~6,7 vezes a
// altura) seguida de 4 círculos de vidro do tamanho da altura: Aplicativos, Arquivos, Ações e Área de
// Transferência. Escolher um círculo põe o campo nesse modo (só aquele tipo de resultado).
// (g_mode: declarado antes de Rebuild; a área de transferência abre a do Windows)
bool Compact() { return liquid::On() && !liquid::Golden() && g_query.empty() && g_mode == 0 && g_h <= FIELD_H + 1.5f; }
// Tahoe (vídeo do macOS 26, quadro a quadro): o Spotlight abre como uma cápsula comprida, sem círculos. Com o
// ponteiro em cima, a ponta direita recua rápido (passa um pouco e volta, numa mola) e o que sobra vira uma gota
// ondulada que se divide nos quatro círculos: eles saem juntos, quase encostados, e se afastam um depois do outro;
// os "pescoços" entre eles afinam até romper (vidro::Gosma); ~400 ms no total.
bool g_reveal;               // os círculos já saíram (ponteiro passou em cima)
D2D1_POINT_2F g_spHover{ -1, -1 };  // o ponteiro (DIP), para o brilho dos círculos de vidro
vidro::LuzSegue g_spLuz;             // a luz, atrás do ponteiro com a mola de 0,3 s
double g_revealAt = -1e9;
float Since() { return (float)((anim::Now() - g_revealAt) / 1000.0); }
float CircE(int i) {  // 0 = ainda na gota, 1 = no lugar (medido no vídeo: se separam de ~170 a ~330 ms)
    if (!g_reveal) return 0;
    float t = Since() - 0.15f - i * 0.022f;
    if (t <= 0) return 0;
    if (t > 1.2f) return 1;
    return anim::Spring(t, 0.24f, 0.78f);
}
bool GooAnimating() { return g_reveal && anim::Now() - g_revealAt < 950; }
unsigned g_broken;           // pescoços da gosma que já romperam (não religam)
float PillW() { return W - 4 * (FIELD_H + CIRC_GAP); }
float MorphT() { return liquid::On() && !liquid::Golden() && g_pill >= 0 ? std::clamp((g_pill - PillW()) / (W - PillW()), 0.f, 1.f) : 1.f; }  // 0 cápsula, 1 painel
D2D1_RECT_F CircleRect(int i) { float x = PillW() + CIRC_GAP + i * (FIELD_H + CIRC_GAP); return D2D1::RectF(x, 0, x + FIELD_H, FIELD_H); }
// o círculo i agora (DIP): começa agrupado logo depois da ponta da cápsula (a gota), cresce e vai para o lugar
D2D1_RECT_F CircleNow(int i, float cdx) {
    D2D1_RECT_F c = CircleRect(i);
    float e = CircE(i), r = FIELD_H / 2;
    // a gota: os quatro já perto do lugar final, encostados (sobrepostos) e menores; a cauda presa à cápsula
    float xf = (c.left + c.right) / 2 + cdx;
    float x0 = (CircleRect(0).left + CircleRect(0).right) / 2 + cdx;
    float xs = x0 - r * 0.55f + (xf - x0) * 0.62f;
    float cx = xs + (xf - xs) * e;
    float grow = 0.70f + 0.30f * std::min(e, 1.f), sp = std::clamp(e * (1 - e) * 4, 0.f, 1.f);
    float rx = r * grow * (1 + 0.10f * sp), ry = r * grow * (1 - 0.05f * sp);
    return D2D1::RectF(cx - rx, FIELD_H / 2 - ry, cx + rx, FIELD_H / 2 + ry);
}
// a ponta direita da cápsula (DIP): comprida até o ponteiro chegar; depois recua rápido, passa um pouco e volta
float CapEnd(float pw) {
    if (!g_reveal) return W;
    float e = anim::Spring(Since(), 0.20f, 0.64f);  // recua ~50% em 70 ms, passa um pouco em ~170 ms e assenta em ~400
    return W - (W - pw) * e;
}
const wchar_t* ModeName(int m) { return m == 1 ? Tr(L"Aplicativos") : m == 2 ? Tr(L"Arquivos") : m == 3 ? Tr(L"Ações") : liquid::Golden() ? Tr(L"Pesquisar ou Perguntar") : Tr(L"Busca do Spotlight"); }
const wchar_t* ModeSymbol(int m) { return m == 1 ? L"square.grid.2x2" : m == 2 ? L"folder" : m == 3 ? L"slider.horizontal.3" : L"magnifyingglass"; }
bool ModeAllows(int k) {
    if (g_mode == 1) return k == K_APP || k == K_CALC;
    if (g_mode == 2) return k == K_DOC || k == K_FOLDER || k == K_IMAGE || k == K_MEDIA || k == K_OTHER;
    if (g_mode == 3) return k == K_SETTING;
    return true;
}

// Tahoe: o campo sozinho é uma cápsula; com resultados, o vidro cresce com cantos grandes
float SpotR(float H) {
    if (!liquid::On()) return RADIUS;
    float k = std::clamp((H - FIELD_H) / 40.f, 0.f, 1.f);
    return FIELD_H / 2 + (26.f - FIELD_H / 2) * k;  // cápsula recolhida (28) → 26 com resultados (guia, seção 28.2)
}

// Mac OS X Leopard: o menu do Spotlight do 10.5 (desenho próprio). Branco, preso na barra de menus (cantos de baixo
// redondos), "Spotlight" em negrito e o campo em pílula branca com o anel azul; a lista com a coluna cinza das
// categorias à esquerda, linhas de 20 com ícone de 16 e a escolhida em degradê azul com o texto branco
void PaintLeopard() {
    float sc = Sc();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(WIN_H * sc);
    if (g_back && IsWindowVisible(g_back)) ShowWindow(g_back, SW_HIDE);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    Pal p = Colors();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    if (!br) { ui::GpuEnd(g_s); return; }
    float fade, scale;
    Transition(fade, scale);
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), fade), nullptr);
    static IDWriteTextFormat* tfB = ui::FormatFace(L"Lucida Sans Unicode", 13, DWRITE_FONT_WEIGHT_BOLD);
    static IDWriteTextFormat* tfN = ui::FormatFace(L"Lucida Sans Unicode", 12, DWRITE_FONT_WEIGHT_NORMAL);
    static IDWriteTextFormat* tfH = [] { auto* f = ui::FormatFace(L"Lucida Sans Unicode", 11, DWRITE_FONT_WEIGHT_NORMAL); if (f) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); return f; }();
    float H = std::max(FIELD_H, g_h);
    const float RR = 5;
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));  // sombra curta
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1, 2, W - 1, H + 2), RR, RR), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.97f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), RR, RR), br);
    dc->FillRectangle(D2D1::RectF(0, 0, W, RR), br);  // em cima é reto (sai da barra de menus)
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, -RR, W - 0.5f, H - 0.5f), RR, RR), br, 1);
    // cabeçalho: "Spotlight" e o campo
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.88f));
    dc->DrawTextW(L"Spotlight", 9, tfB, D2D1::RectF(12, 0, 100, FIELD_H), br);
    D2D1_RECT_F f = D2D1::RectF(96, (FIELD_H - 20) / 2, W - 10, (FIELD_H + 20) / 2);
    float fr = (f.bottom - f.top) / 2;
    br->SetColor(D2D1::ColorF(0.36f, 0.60f, 0.95f, 0.75f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(f.left - 1.5f, f.top - 1.5f, f.right + 1.5f, f.bottom + 1.5f), fr + 1.5f, fr + 1.5f), br, 3);
    br->SetColor(D2D1::ColorF(1, 1, 1, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(f, fr, fr), br);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
    dc->FillRectangle(D2D1::RectF(f.left + fr * 0.6f, f.top + 1, f.right - fr * 0.6f, f.top + 3), br);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(f.left + 0.5f, f.top + 0.5f, f.right - 0.5f, f.bottom - 0.5f), fr - 0.5f, fr - 0.5f), br, 1);
    br->SetColor(D2D1::ColorF(0.25f, 0.25f, 0.27f, 1));  // a lupa
    float my = (f.top + f.bottom) / 2;
    dc->DrawEllipse(D2D1::Ellipse({ f.left + 12, my - 1 }, 4, 4), br, 1.5f);
    dc->DrawLine({ f.left + 15, my + 2 }, { f.left + 18, my + 5 }, br, 1.8f);
    D2D1_RECT_F tr = D2D1::RectF(f.left + 24, f.top, f.right - 8, f.bottom);
    float tw = ui::TextWidth(tfN, g_query);
    if (g_selAll && !g_query.empty()) {
        br->SetColor(D2D1::ColorF(0.36f, 0.60f, 0.95f, 0.35f));
        dc->FillRectangle(D2D1::RectF(tr.left, f.top + 3, tr.left + tw, f.bottom - 3), br);
    }
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.9f));
    dc->DrawTextW(g_query.c_str(), (UINT32)g_query.size(), tfN, tr, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (g_caretOn && !g_selAll) dc->FillRectangle(D2D1::RectF(tr.left + tw + 0.5f, f.top + 4, tr.left + tw + 1.5f, f.bottom - 4), br);
    // a lista
    if (H > FIELD_H + 2) {
        float top = ListTop();
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.14f));
        dc->FillRectangle(D2D1::RectF(0, top - 1, W, top), br);
        dc->PushAxisAlignedClip(D2D1::RectF(0, top, W, H - 1), D2D1_ANTIALIAS_MODE_ALIASED);
        br->SetColor(D2D1::ColorF(0.93f, 0.93f, 0.94f, 1));  // a coluna das categorias
        dc->FillRectangle(D2D1::RectF(0, top, LEO_COL, H), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
        dc->FillRectangle(D2D1::RectF(LEO_COL, top, LEO_COL + 1, H), br);
        std::wstring head;
        bool firstOfSect = false, firstSect = true;
        for (int i = 0; i < (int)g_rows.size(); i++) {
            auto& row = g_rows[i];
            if (row.type == R_HEAD) {
                head = row.head; firstOfSect = true;
                if (!firstSect) {  // linha entre as seções
                    float ly = top + row.r.top - g_scroll - 2.5f;
                    br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
                    dc->FillRectangle(D2D1::RectF(0, ly, W, ly + 1), br);
                }
                firstSect = false;
                continue;
            }
            if (row.res < 0 || row.res >= (int)g_res.size() || row.r.bottom <= row.r.top) continue;
            D2D1_RECT_F r = D2D1::RectF(row.r.left, top + row.r.top - g_scroll, row.r.right, top + row.r.bottom - g_scroll);
            if (r.bottom < top || r.top > H) { firstOfSect = false; continue; }
            bool sel = i == g_sel;
            if (sel) {
                D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0.39f, 0.56f, 0.96f, 1) }, { 1, D2D1::ColorF(0.13f, 0.35f, 0.88f, 1) } };
                ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
                if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
                    SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
                    dc->FillRectangle(D2D1::RectF(LEO_COL + 1, r.top, W, r.bottom), lb);
                if (lb) lb->Release();
                if (gs) gs->Release();
            }
            if (firstOfSect) {
                std::wstring h = row.type == R_CARD ? std::wstring(Tr(L"Melhor Resultado")) : head;
                br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
                dc->DrawTextW(h.c_str(), (UINT32)h.size(), tfH, D2D1::RectF(4, r.top, LEO_COL - 8, r.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                firstOfSect = row.type == R_CARD;  // o melhor resultado é sozinho: a seção de baixo também mostra o nome
            }
            const Result& res = g_res[row.res];
            DrawIcon(dc, br, res, D2D1::RectF(LEO_COL + 8, r.top + 2, LEO_COL + 24, r.top + 18), p);
            br->SetColor(sel ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, 0.9f));
            dc->DrawTextW(res.name.c_str(), (UINT32)res.name.size(), tfN, D2D1::RectF(LEO_COL + 30, r.top, W - 8, r.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        dc->PopAxisAlignedClip();
    }
    dc->PopLayer();
    br->Release();
    ui::GpuEnd(g_s);
}

void Paint() {
    if (!g_visible) return;
    if (liquid::Leopard()) { PaintLeopard(); return; }
    double tp0 = anim::Now();
    float sc = Sc();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(WIN_H * sc);
    // desenho dentro do vidro (mesmo quadro que ele); a janela de cima fica vazia, só para o mouse e o teclado
    ID2D1DeviceContext* dc = g_glass ? backdrop::ContentBegin(g_back, wpx, hpx, sc) : nullptr;
    const bool inGlass = dc != nullptr;
    static bool swapCleared;
    if (inGlass && !swapCleared) {  // apaga o que a janela de cima mostrava do jeito antigo
        if (auto* d0 = ui::GpuBegin(g_s, wpx, hpx, sc)) { (void)d0; ui::GpuEnd(g_s); }
        swapCleared = true;
    }
    if (!dc) dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    Pal p = Colors();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(p.text, &br);
    float H = g_h;
    float fade, scale;
    Transition(fade, scale);
    if (!inGlass) {  // dentro do vidro, a escala e a opacidade são as dele (backdrop::Fade)
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, { W / 2, FIELD_H / 2 }));
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), fade), nullptr);
    }
    // borda do vidro: 1 px claro por dentro (escuro) / escuro (claro)
    const float R = SpotR(H);
    const float mt = MorphT(), pw = liquid::On() ? std::max(PillW(), std::min(W, g_pill)) : W;
    const bool compact = liquid::On() && mt < 0.995f;  // cápsula + círculos (ou no meio da passagem para o painel)
    const float ca = 1 - mt, cdx = pw - PillW();     // círculos: somem e deslizam com a borda da cápsula
    if (compact) {  // cápsula + 4 círculos: contorno fino em cada peça e os símbolos
        bool dark = ui::Dark();
        const float capEnd = CapEnd(pw);
        if (GooAnimating() && ca > 0.01f) {  // gosma: um contorno só em volta da cápsula, da gota e dos pescoços
            std::vector<D2D1_ELLIPSE> cs;
            for (int i = 0; i < 4; i++) { auto c = CircleNow(i, cdx); cs.push_back(D2D1::Ellipse({ (c.left + c.right) / 2, (c.top + c.bottom) / 2 }, (c.right - c.left) / 2, (c.bottom - c.top) / 2)); }
            {   // pescoço que passou do ponto rompe de vez (no vídeo nada religa depois de soltar)
                float px = capEnd - H / 2, pr = H / 2;
                for (int i = 0; i < 4; i++) {
                    float gap = (cs[i].point.x - cs[i].radiusX) - (px + pr);
                    if (gap > vidro::GosmaRompe(std::min(pr, cs[i].radiusY))) g_broken |= 1u << i;
                    px = cs[i].point.x; pr = cs[i].radiusX;
                }
            }
            ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
            if (ID2D1Geometry* g = vidro::Gosma(fac, D2D1::RectF(0, 0, capEnd, H), H / 2, cs.data(), (int)cs.size(), g_broken)) {
                if (!backdrop::Real()) { br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.10f : 0.40f)); dc->FillGeometry(g, br); }
                br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.30f : 0.55f));
                dc->DrawGeometry(g, br, 1.0f);
                g->Release();
            }
            if (fac) fac->Release();
        } else liquid::Rim(dc, D2D1::RectF(0, 0, capEnd, H), std::min(SpotR(H), H / 2), dark, 0.8f);
        if (ca > 0.01f) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), ca * ca), nullptr);
        for (int i = 0; i < 4 && ca > 0.01f; i++) {
            float e = CircE(i);
            if (!g_reveal) continue;  // a cápsula ainda está comprida
            D2D1_RECT_F c = CircleNow(i, cdx);
            float cr = (c.bottom - c.top) / 2;
            if (!GooAnimating()) {  // assentados: cada círculo com a borda dele (durante a gosma o contorno é um só)
                if (!backdrop::Real()) liquid::Tile(dc, c, cr, dark, 1.4f);  // sem o vidro real: só a cápsula tem vidro
                // o brilho dos cantos só acende no círculo sob o ponteiro, vindo do lado dele (guia, seção 29.1)
                bool hov = g_spHover.x >= c.left && g_spHover.x < c.right && g_spHover.y >= c.top && g_spHover.y < c.bottom;
                float lpx = g_spLuz.x >= 0 ? g_spLuz.x : g_spHover.x, lpy = g_spLuz.y >= 0 ? g_spLuz.y : g_spHover.y;
                liquid::Rim(dc, c, cr, dark, 0.8f, hov ? 1.f : 0.f, hov ? std::clamp((lpx - c.left) / (c.right - c.left), 0.f, 1.f) : -1, hov ? std::clamp((lpy - c.top) / (c.bottom - c.top), 0.f, 1.f) : -1);
            }
            static const wchar_t* sym[4] = { L"square.grid.2x2", L"folder", L"slider.horizontal.3", L"rectangle.on.rectangle" };
            br->SetColor(D2D1::ColorF(p.text2.r, p.text2.g, p.text2.b, p.text2.a * std::clamp((e - 0.55f) / 0.4f, 0.f, 1.f)));  // o símbolo aparece quando o círculo se solta
            float cx = (c.left + c.right) / 2, s = 11;
            simbolo::Draw(dc, sym[i], D2D1::RectF(cx - s, FIELD_H / 2 - s, cx + s, FIELD_H / 2 + s), br);
        }
        if (ca > 0.01f) dc->PopLayer();
    } else if (liquid::On()) {
        if ((!backdrop::Liquid() || backdrop::Real()) && !liquid::Golden()) liquid::Rim(dc, D2D1::RectF(0, 0, W, H), R, ui::Dark());  // vidro real em peças: o contorno é nosso (Golden Gate: cada peça tem o seu)
    } else {
    br->SetColor(ui::Dark() ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.10f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R - 0.5f, R - 0.5f), br, 1);
    }

    // Golden Gate: a barra é uma cápsula escura e esfumada (preto 88 % em cima -> 62 % embaixo), com texto branco, e
    // fica centrada quando o painel é mais largo (Apps)
    const bool gg = liquid::Golden();
    const float fpw = gg ? std::min(W, GG_BAR_W) : pw, fx0 = gg ? (W - fpw) / 2 : 0;
    Pal pf = p;
    D2D1_MATRIX_3X2_F fieldOld; dc->GetTransform(&fieldOld);
    if (gg) {
        pf.text = D2D1::ColorF(1, 1, 1, 0.96f); pf.text2 = D2D1::ColorF(1, 1, 1, 0.62f); pf.place = D2D1::ColorF(1, 1, 1, 0.50f);
        D2D1_RECT_F bar = D2D1::RectF(fx0, 0, fx0 + fpw, FIELD_H);
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0, 0, 0, 0.88f) }, { 1, D2D1::ColorF(0.10f, 0.08f, 0.07f, 0.62f) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, FIELD_H }), gs, &lb)))
            dc->FillRoundedRectangle(D2D1::RoundedRect(bar, FIELD_H / 2, FIELD_H / 2), lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
        liquid::Rim(dc, bar, FIELD_H / 2, true, 0.8f);
        if (H > FIELD_H + 1.5f) liquid::Rim(dc, D2D1::RectF(0, FIELD_H + GG_GAP, W, H), 30, ui::Dark(), 0.8f);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(fx0 + 18, 0) * fieldOld);  // texto a 38 da borda (20 + 18)
    }
    // campo
    br->SetColor(pf.text2);
    // macOS 27: o símbolo do modo só aparece com o campo vazio (a 31,5 da borda, o texto a 76); digitando, sem símbolo
    // e o texto a 38 (texto de 22 pt)
    if (!gg) simbolo::Draw(dc, ModeSymbol(g_mode), D2D1::RectF(20, FIELD_H / 2 - 12, 44, FIELD_H / 2 + 12), br);
    else if (g_query.empty() && g_mode != 0) simbolo::Draw(dc, ModeSymbol(g_mode), D2D1::RectF(13.5f, FIELD_H / 2 - 12, 37.5f, FIELD_H / 2 + 12), br);
    D2D1_RECT_F field = gg ? D2D1::RectF(g_query.empty() && g_mode != 0 ? 58.f : 20.f, 0, fpw - 38, FIELD_H) : D2D1::RectF(56, 0, fpw - 20, FIELD_H);
    static IDWriteTextFormat* tfFieldGG = ui::Format(22, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* const tfF = gg && tfFieldGG ? tfFieldGG : g_tfField;
    if (g_query.empty() && gg && g_mode == 0) {  // macOS 27: microfone (contorno) a ~50 da borda direita, só com o campo vazio
        float mx = fpw - 50 - 18, my = FIELD_H / 2;
        br->SetColor(pf.text2);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(mx - 4.5f, my - 11, mx + 4.5f, my + 3), 4.5f, 4.5f), br, 1.6f);
        ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
        ID2D1PathGeometry* pg = nullptr;
        if (fac && SUCCEEDED(fac->CreatePathGeometry(&pg))) {
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(pg->Open(&sk))) {
                sk->BeginFigure({ mx - 8, my - 3 }, D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddArc(D2D1::ArcSegment({ mx + 8, my - 3 }, { 8, 8 }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close(); sk->Release();
            }
            dc->DrawGeometry(pg, br, 1.6f);
            pg->Release();
        }
        if (fac) fac->Release();
        dc->DrawLine({ mx, my + 5 }, { mx, my + 10 }, br, 1.6f);
        dc->DrawLine({ mx - 4, my + 10.5f }, { mx + 4, my + 10.5f }, br, 1.6f);
    }
    if (g_query.empty()) {
        Text(dc, ModeName(g_mode), tfF, field, br, pf.place);
        if (g_mode >= 1 && g_mode <= 3 && !gg) {  // o atalho do modo, à direita (o "⌘1" do Mac; aqui Ctrl+1); macOS 27: sem
            static IDWriteTextFormat* tfKey = ui::Format(13, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            wchar_t k[16]; swprintf_s(k, L"Ctrl %d", g_mode);
            float right = gg ? fpw - 56 : (ModePanel() && g_mode == 1) ? W - 60 : fpw - 20;  // Golden Gate: a 38 da borda (o desenho vai 18 para a direita)
            if (tfKey) Text(dc, k, tfKey, D2D1::RectF(right - 90, 0, right, FIELD_H), br, pf.place);
        }
    } else {
        float tw = ui::TextWidth(tfF, g_query);
        if (g_selAll) {  // tudo selecionado (Ctrl+A / ao reabrir)
            br->SetColor(D2D1::ColorF(p.sel.r, p.sel.g, p.sel.b, 0.45f));
            dc->FillRectangle(D2D1::RectF(field.left - 1, FIELD_H / 2 - 17, field.left + tw + 1, FIELD_H / 2 + 17), br);
        }
        Text(dc, g_query, tfF, field, br, pf.text);
        // completa o melhor resultado em cinza ("saf" → "ari"), como no Mac
        if (!g_selAll && g_sel >= 0 && g_rows[g_sel].res >= 0) {
            const Result& r = g_res[g_rows[g_sel].res];
            if (r.kind != K_WEB && r.kind != K_CALC && Lower(r.name).rfind(Lower(g_query), 0) == 0 && r.name.size() > g_query.size())
                Text(dc, r.name.substr(g_query.size()), tfF, D2D1::RectF(field.left + tw, 0, field.right, FIELD_H), br, pf.place);
            else if (r.kind == K_CALC) Text(dc, L" = " + r.name, tfF, D2D1::RectF(field.left + tw, 0, field.right, FIELD_H), br, pf.place);
        }
        if (g_caretOn && !g_selAll) {
            br->SetColor(p.sel);
            dc->FillRectangle(D2D1::RectF(field.left + tw + 1, FIELD_H / 2 - 16, field.left + tw + 3, FIELD_H / 2 + 16), br);
        }
    }

    if (ModePanel() && g_query.empty() && g_mode == 1) {  // botão ⋯ (grade ou lista), à direita do campo
        float mr = gg ? fpw - 18 - 30 : W - 20;  // Golden Gate: "⋯" solto, sem círculo, centrado a 45 da borda da barra
        g_moreR = D2D1::RectF(mr - 30, FIELD_H / 2 - 15, mr, FIELD_H / 2 + 15);
        float cx = (g_moreR.left + g_moreR.right) / 2;
        br->SetColor(pf.text2);
        if (!gg) dc->DrawEllipse(D2D1::Ellipse({ cx, FIELD_H / 2 }, 11, 11), br, 1.5f);
        for (int k = -1; k <= 1; k++) dc->FillEllipse(D2D1::Ellipse({ cx + k * 5.f, FIELD_H / 2 }, 1.5f, 1.5f), br);
        if (gg) { g_moreR.left += fx0 + 18; g_moreR.right += fx0 + 18; }  // o clique é sem o deslocamento do desenho
    } else g_moreR = {};
    dc->SetTransform(fieldOld);
    if (H > FIELD_H + 1.5f) {
        br->SetColor(p.sep);
        if (gg) {}  // Golden Gate: barra e painel separados, sem divisa
        else if (liquid::On()) dc->FillRectangle(D2D1::RectF(20, FIELD_H, W - 20, FIELD_H + 1), br);  // Tahoe: divisa solta das bordas
        else dc->FillRectangle(D2D1::RectF(0, FIELD_H, W, FIELD_H + 1), br);
        float listTop = ListTop();
        Layout();
        // conteúdo novo (trocou de modo): entra esmaecendo e subindo 10 pt, como no Mac
        float lin = anim::Ease(std::clamp((float)((anim::Now() - g_listIn) / 240.0), 0.f, 1.f));
        // macOS 27: o painel entra só esmaecendo (~140 ms), sem subir
        const float ggIn = gg ? anim::Ease(std::clamp((float)((anim::Now() - g_panelInAt) / 140.0), 0.f, 1.f)) : 1.f;
        D2D1_MATRIX_3X2_F listOld; dc->GetTransform(&listOld);
        const bool listAnim = lin < 0.999f || ggIn < 0.999f;
        if (listAnim) {
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0, gg ? 0.f : (1 - lin) * 10) * listOld);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), (gg ? 1.f : lin) * ggIn), nullptr);
        }
        if (HasChips()) {  // Tahoe: fileira de filtros (categorias / tipos), pílulas de vidro fracas
            auto chips = Chips();
            g_chipR.clear();
            float x = PAD + 4, cy = gg ? FIELD_H + GG_GAP + 33 : FIELD_H + 1 + GG_GAP + CHIPS_H / 2;  // macOS 27: centro a 33 do alto do painel
            bool dark = ui::Dark();
            static IDWriteTextFormat* tfChipGG = ui::Format(15, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            IDWriteTextFormat* tfC = gg && tfChipGG ? tfChipGG : g_tfChip;
            if (tfC) tfC->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (gg) x = 20;  // macOS 27: fichas de 26 com texto de 15, a 20 da borda do painel
            for (int i = 0; i < (int)chips.size(); i++) {
                float tw = ui::TextWidth(tfC, chips[i]), ch = gg ? 13.f : 12.f;
                D2D1_RECT_F c = D2D1::RectF(x, cy - ch, x + tw + (gg ? 24 : 26), cy + ch);  // pílulas de 24 de altura (guia, seção 39)
                if (c.right > W - PAD) break;
                bool on = g_chip == i + 1;
                br->SetColor(on ? D2D1::ColorF(p.sel.r, p.sel.g, p.sel.b, 0.85f) : dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(1, 1, 1, 0.38f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(c, ch, ch), br);
                if (!on) liquid::Rim(dc, c, ch, dark, 0.6f);
                Text(dc, chips[i], tfC, c, br, on ? D2D1::ColorF(1, 1, 1, 1) : gg ? p.text : p.text2);
                g_chipR.push_back(c);
                x = c.right + 8;
            }
        }
        dc->PushAxisAlignedClip(D2D1::RectF(0, listTop, W, H - 1), D2D1_ANTIALIAS_MODE_ALIASED);
        // macOS 27: o que rola some esmaecendo nos 26 de cima da lista (antes cortava seco, colado nas fichas)
        ID2D1LinearGradientBrush* edgeMask = nullptr;
        if (gg && g_scrollNow > 0.5f) {
            D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0, 0, 0, 0) }, { 1, D2D1::ColorF(0, 0, 0, 1) } };
            ID2D1GradientStopCollection* gs = nullptr;
            if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)))
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, listTop }, { 0, listTop + 26 }), gs, &edgeMask);
            if (gs) gs->Release();
            if (edgeMask) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1, edgeMask), nullptr);
        }
        float oy = listTop - g_scrollNow;
        // destaque que desliza entre as linhas
        if (g_sel >= 0 && g_rows[g_sel].type == R_ROW && g_selBot > g_selTop) {
            br->SetColor(gg ? (ui::Dark() ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(1, 1, 1, 0.30f)) : p.hi);  // Golden Gate: pílula clara
            float hr = gg ? std::min(22.f, (g_selBot - g_selTop) / 2) : liquid::On() ? std::min(16.f, (g_selBot - g_selTop) / 2) : 9.f;  // Tahoe: destaque em cápsula
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(PAD, g_selTop + oy, W - PAD, g_selBot + oy), hr, hr), br);
        }
        for (int i = 0; i < (int)g_rows.size(); i++) {
            const Row& row = g_rows[i];
            D2D1_RECT_F r = D2D1::RectF(row.r.left, row.r.top + oy, row.r.right, row.r.bottom + oy);
            if (r.bottom < listTop || r.top > H) continue;
            bool s = i == g_sel;
            if (row.type == R_MODE) {  // macOS 27: símbolo de 24 a 36 da borda, nome de 17 a 70 e o atalho em cinza à direita
                static IDWriteTextFormat* tfM = ui::Format(17, DWRITE_FONT_WEIGHT_NORMAL);
                static IDWriteTextFormat* tfK = ui::Format(17, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
                static const wchar_t* names[4] = { L"Aplicativos", L"Arquivos", L"Ações", L"Área de Transferência" };
                static const wchar_t* syms[4] = { L"square.grid.2x2", L"folder", L"slider.horizontal.3", L"rectangle.on.rectangle" };
                float cy = (r.top + r.bottom) / 2, ix = r.left + 24;
                if (g_spHover.x >= r.left && g_spHover.x < r.right && g_spHover.y >= r.top && g_spHover.y < r.bottom) { br->SetColor(D2D1::ColorF(1, 1, 1, 0.10f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 14, 14), br); }
                br->SetColor(p.text);
                simbolo::Draw(dc, syms[row.sect], D2D1::RectF(ix - 12, cy - 11, ix + 12, cy + 11), br);
                Text(dc, Tr(names[row.sect]), tfM, D2D1::RectF(r.left + 58, r.top, r.right - 90, r.bottom), br, p.text);
                wchar_t k[16]; swprintf_s(k, L"Ctrl %d", row.sect + 1);
                if (tfK) Text(dc, k, tfK, D2D1::RectF(r.right - 140, r.top, r.right - 28, r.bottom), br, p.text2);
                continue;
            }
            if (row.type == R_HEAD) {
                Text(dc, row.head, g_tfHead, D2D1::RectF(r.left + 10, r.top + 8, r.right - 10, r.bottom), br, p.text2);
                bool open = g_more.count(row.sect) > 0;
                if (row.sect >= 0 && (open || g_canMore.count(row.sect))) {
                    static IDWriteTextFormat* tfMore = ui::Format(12, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    Text(dc, Tr(open ? L"Mostrar Menos" : L"Mostrar Mais"), tfMore, D2D1::RectF(r.left + 10, r.top + 8, r.right - 10, r.bottom), br, p.text2);
                }
                continue;
            }
            const Result& res = g_res[row.res];
            if (row.type == R_APP) {  // ícone grande e o nome embaixo (duas linhas no máximo)
                if (gg && row.sect == 8) {  // macOS 27: a linha fina entre os mais usados e o resto
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.24f));
                    dc->FillRectangle(D2D1::RectF(18.5f, r.top - 8, W - 18.5f, r.top - 7), br);
                }
                if (g_dragging && i == g_press) continue;  // o arrastado vai por cima, no ponteiro
                if (auto it = g_cellAt.find(res.target); it != g_cellAt.end())  // deslizando até o lugar novo
                    r = D2D1::RectF(it->second.x, it->second.y + oy, it->second.x + (row.r.right - row.r.left), it->second.y + oy + (row.r.bottom - row.r.top));
                bool edit = g_edit && CanArrange();
                if (s && !edit && (!gg || g_keyNav)) { br->SetColor(p.hi); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 6, r.top + 4, r.right - 6, r.bottom - 4), 16, 16), br); }
                float cx = (r.left + r.right) / 2;
                D2D1_RECT_F ir = D2D1::RectF(cx - APP_ICON / 2, r.top + APP_ICON_TOP, cx + APP_ICON / 2, r.top + APP_ICON_TOP + APP_ICON);
                D2D1_MATRIX_3X2_F base; dc->GetTransform(&base);
                if (edit) {  // balança uns 2 graus, cada um no seu ritmo, como no Launchpad
                    float tt = (float)((anim::Now() - g_editAt) / 1000.0);
                    float ang = 2.0f * std::sin(tt * 6.2832f * 3.4f + i * 1.7f);
                    dc->SetTransform(D2D1::Matrix3x2F::Rotation(ang, { cx, (ir.top + ir.bottom) / 2 }) * base);
                }
                if (!IconFor(dc, res)) RequestIcon(res, 96);
                DrawIcon(dc, br, res, ir, p);
                if (edit) {  // o × no canto de cima à esquerda
                    D2D1_POINT_2F xc{ ir.left + 7, ir.top + 7 };
                    br->SetColor(D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.95f));
                    dc->FillEllipse(D2D1::Ellipse(xc, 10, 10), br);
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
                    dc->DrawEllipse(D2D1::Ellipse(xc, 10, 10), br, 1.2f);
                    dc->DrawLine({ xc.x - 4, xc.y - 4 }, { xc.x + 4, xc.y + 4 }, br, 1.8f);
                    dc->DrawLine({ xc.x - 4, xc.y + 4 }, { xc.x + 4, xc.y - 4 }, br, 1.8f);
                }
                {
                    static IDWriteTextFormat* tfCellGG = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
                    float nt = gg ? 78.f : APP_NAME_TOP;  // macOS 27: nome de 13 com o centro 47 abaixo do centro do ícone
                    static bool trimmed;
                    if (gg && tfCellGG && !trimmed) {  // uma linha só, terminando em "…" (System Informat…), como no macOS 27
                        trimmed = true;
                        tfCellGG->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        IDWriteInlineObject* dots = nullptr;
                        ui::dw->CreateEllipsisTrimmingSign(tfCellGG, &dots);
                        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                        tfCellGG->SetTrimming(&tr, dots);
                        if (dots) dots->Release();
                    }
                    Text(dc, res.name, gg && tfCellGG ? tfCellGG : g_tfCell, D2D1::RectF(r.left + 2, r.top + nt, r.right - 2, r.top + nt + 18), br, p.text);
                }
                if (edit) dc->SetTransform(base);
                continue;
            }
            if (row.type == R_ROW && !IconFor(dc, res)) RequestIcon(res, 64);
            if (row.type == R_TILE) {
                ID2D1RoundedRectangleGeometry* clip = nullptr;
                ID2D1Factory* fac = nullptr;
                dc->GetFactory(&fac);  // a geometria tem de ser da mesma fábrica do contexto da GPU
                if (fac) { fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, 10, 10), &clip); fac->Release(); }
                ID2D1Layer* layer = nullptr;
                dc->CreateLayer(&layer);
                if (clip && layer) dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
                br->SetColor(p.card);
                dc->FillRectangle(r, br);
                DrawIcon(dc, br, res, r, p);
                if (clip && layer) dc->PopLayer();
                if (layer) layer->Release();
                if (clip) clip->Release();
                if (s) { br->SetColor(p.sel); dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1.5f, r.top - 1.5f, r.right + 1.5f, r.bottom + 1.5f), 11.5f, 11.5f), br, 3); }
                continue;
            }
            bool card = row.type == R_CARD;
            if (card) {  // cartão do melhor resultado: fundo claro e borda fina
                float cr = liquid::On() ? 20.f : 14.f;
                br->SetColor(s ? p.hi : p.card);
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, cr, cr), br);
                if (liquid::On()) liquid::Rim(dc, r, cr, ui::Dark(), 0.7f);
                else {
                br->SetColor(p.sep);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 13.5f, 13.5f), br, 1);
                }
            }
            if (gg && !card) {  // macOS 27: ícone de 36 a 30 da borda, nome de 17 a 80 e o tipo · lugar em cinza de 15 embaixo
                static IDWriteTextFormat* tfT = ui::Format(17, DWRITE_FONT_WEIGHT_NORMAL);
                static IDWriteTextFormat* tfS = ui::Format(15, DWRITE_FONT_WEIGHT_NORMAL);
                float cy2 = (r.top + r.bottom) / 2;
                DrawIcon(dc, br, res, D2D1::RectF(r.left + 18, cy2 - 18, r.left + 54, cy2 + 18), p);
                std::wstring name = res.kind == K_CALC ? res.detail + L" = " + res.name : res.name;
                NameText(dc, name, tfT, D2D1::RectF(r.left + 68, r.top + 11, r.right - 14, r.top + 33), br, p.text);
                const wchar_t* kindName = res.kind == K_APP ? L"Aplicativo" : res.kind == K_FOLDER ? L"Pasta" : res.kind == K_DOC ? L"Documento" :
                                          res.kind == K_IMAGE ? L"Imagem" : res.kind == K_WEB ? L"Pesquisar na Web" : nullptr;
                std::wstring acc = Accessory(res), sub = kindName ? Tr(kindName) : L"";
                if (res.kind == K_WEB) acc.clear();
                if (!acc.empty()) sub += (sub.empty() ? L"" : L" · ") + acc;
                if (!sub.empty()) Text(dc, sub, tfS, D2D1::RectF(r.left + 68, r.top + 33, r.right - 14, r.top + 53), br, p.text2);
                continue;
            }
            float ic = card ? (liquid::On() ? 38.f : 40.f) : 24.f, cy = (r.top + r.bottom) / 2;
            if (card) {  // ícone num círculo, como no cartão do Mac (apps ficam com o ícone deles)
                if (res.kind != K_APP) {
                    br->SetColor(p.hi);
                    dc->FillEllipse(D2D1::Ellipse({ r.left + 12 + ic / 2, cy }, ic / 2, ic / 2), br);
                    DrawIcon(dc, br, res, D2D1::RectF(r.left + 12 + 8, cy - ic / 2 + 8, r.left + 12 + ic - 8, cy + ic / 2 - 8), p);
                } else DrawIcon(dc, br, res, D2D1::RectF(r.left + 12, cy - ic / 2, r.left + 12 + ic, cy + ic / 2), p);
            } else DrawIcon(dc, br, res, D2D1::RectF(r.left + 8, cy - ic / 2, r.left + 8 + ic, cy + ic / 2), p);
            std::wstring acc = Accessory(res);
            float accW = acc.empty() ? 0 : std::min(200.f, ui::TextWidth(g_tfSub, acc) + 4);
            float tx = r.left + (card ? 12 + ic + 14 : 8 + ic + 10);
            std::wstring name = res.kind == K_CALC ? res.detail + L" = " + res.name : res.name;
            NameText(dc, name, card ? g_tfTitle : g_tfRow, D2D1::RectF(tx, r.top, r.right - accW - 16, r.bottom), br, p.text);
            if (!acc.empty()) Text(dc, acc, g_tfSub, D2D1::RectF(r.right - accW - 12, r.top, r.right - 12, r.bottom), br, p.text2);
        }
        if (g_dragging && g_press >= 0 && g_press < (int)g_rows.size() && g_rows[g_press].res >= 0) {  // o app arrastado, no ponteiro
            float k = APP_ICON * 1.12f;
            DrawIcon(dc, br, g_res[g_rows[g_press].res], D2D1::RectF(g_dragPt.x - k / 2, g_dragPt.y - k / 2, g_dragPt.x + k / 2, g_dragPt.y + k / 2), p);
        }
        if (edgeMask) { dc->PopLayer(); edgeMask->Release(); }
        dc->PopAxisAlignedClip();
        if (listAnim) { dc->PopLayer(); dc->SetTransform(listOld); }
    }
    if (!inGlass) {
        dc->PopLayer();
        dc->SetTransform(D2D1::IdentityMatrix());
    }
    br->Release();
    if (inGlass) backdrop::ContentEnd(g_back);
    else ui::GpuEnd(g_s);
    double tp1 = anim::Now();
    struct TimeLog { double t0, &t1; ~TimeLog() { double t2 = anim::Now(); static int n; if (t2 - t0 > 10 && n++ < 60) Log(L"spotlight: quadro lento %.1f ms (desenho %.1f, vidro %.1f)", t2 - t0, t1 - t0, t2 - t1); } } tlog{ tp0, tp1 };
    // o vidro acompanha: mesma altura, opacidade e escala
    if (g_glass) {
        // Tahoe: Spotlight é vidro leitoso (branco ~80% no claro), como a cápsula das capturas do macOS 26
        // Tahoe (vídeo do macOS 26): vidro claro e leitoso que pega a cor do fundo (a cápsula fica azulada sobre o azul)
        // Tahoe: medido na tela (guia, seções 52 e 60): escuro 0,19 × fundo + 0,12 (81 % de 0,148); claro 0,26 × fundo
        // + 0,73 (74 % de 0,986): o Spotlight do macOS 26 é quase opaco, o "líquido" fica na borda
        D2D1_COLOR_F tint = !liquid::On() ? p.tint : ui::Dark() ? D2D1::ColorF(0.148f, 0.148f, 0.155f, 0.81f) : D2D1::ColorF(0.986f, 0.986f, 0.99f, 0.74f);
        if (compact && backdrop::Real()) {  // cada peça com a lente dela; os círculos saem da cápsula (e encolhem até sumir)
            float cw = CapEnd(pw);
            std::vector<backdrop::Piece> pcs;
            if (GooAnimating() && ca > 0.04f) {  // a cápsula e a gota numa peça só (vidro::Gosma)
                backdrop::Piece g{ D2D1::RectF(0, 0, W * sc, H * sc), H / 2 * sc };
                g.goo = true; g.cap = D2D1::RectF(0, 0, cw * sc, H * sc); g.capR = H / 2 * sc; g.broken = g_broken;
                for (int i = 0; i < 4; i++) {
                    D2D1_RECT_F c = CircleNow(i, cdx);
                    g.circ.push_back(D2D1::Ellipse({ (c.left + c.right) / 2 * sc, (c.top + c.bottom) / 2 * sc }, (c.right - c.left) / 2 * ca * sc, (c.bottom - c.top) / 2 * ca * sc));
                }
                pcs.push_back(g);
            } else {
                pcs.push_back({ D2D1::RectF(0, 0, cw * sc, H * sc), std::min(SpotR(H), H / 2) * sc });
                if (ca > 0.04f && g_reveal)
                    for (int i = 0; i < 4; i++) {
                        D2D1_RECT_F c = CircleNow(i, cdx);
                        float cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, hw = (c.right - c.left) / 2 * ca, hh = (c.bottom - c.top) / 2 * ca;
                        pcs.push_back({ D2D1::RectF((cx - hw) * sc, (cy - hh) * sc, (cx + hw) * sc, (cy + hh) * sc), hh * sc });
                    }
            }
            backdrop::SetPieces(g_back, W * sc, H * sc, pcs, tint, 10.f);
        } else if (gg && backdrop::Real()) {  // Golden Gate: a barra e o painel em duas peças de vidro
            tint = D2D1::ColorF(0.09f, 0.07f, 0.06f, 0.42f);  // tinta escura por cima do fundo bem desfocado (nunca transparente)
            const float fpw2 = std::min(W, GG_BAR_W), fx2 = (W - fpw2) / 2;
            std::vector<backdrop::Piece> pcs{ { D2D1::RectF(fx2 * sc, 0, (fx2 + fpw2) * sc, FIELD_H * sc), FIELD_H / 2 * sc } };
            if (H > FIELD_H + 1.5f) pcs.push_back({ D2D1::RectF(0, (FIELD_H + GG_GAP) * sc, W * sc, H * sc), 30 * sc });
            backdrop::SetPieces(g_back, W * sc, H * sc, pcs, tint, 10.f);  // o desfoque segue o controle (mínimo igual ao do Tahoe)
        } else if (liquid::On() && backdrop::Real()) {  // Tahoe: vidro claro atrás do texto, como o Spotlight do macOS 26
            std::vector<backdrop::Piece> pcs{ { D2D1::RectF(0, 0, pw * sc, H * sc), SpotR(H) * sc } };
            backdrop::SetPieces(g_back, W * sc, H * sc, pcs, tint, 10.f);
        } else backdrop::Set(g_back, pw * sc, H * sc, SpotR(H) * sc, tint);


        backdrop::Fade(g_back, fade, scale, W * sc / 2, FIELD_H * sc / 2);
    }
}

// As janelas ficam sempre do tamanho máximo; o que cresce e encolhe é o recorte do vidro e o desenho.
void Place() {
    float sc = Sc();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(WIN_H * sc);
    MONITORINFO mi{ sizeof(mi) };
    POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT m = mi.rcMonitor;
    // no Mac (Sequoia) o campo fica a ~22% da altura da tela; no Tahoe a 30 % (guia, seção 39); o painel cresce para baixo
    int x = m.left + (m.right - m.left - wpx) / 2, y = m.top + (int)((m.bottom - m.top) * (liquid::On() ? 0.30f : 0.22f));
    if (liquid::Leopard()) { x = m.right - wpx - 2; y = m.top + menubar::Height(); }  // Leopard: desce da lupa, na ponta direita
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    SetWindowPos(g_back, g_wnd, x, y, wpx, hpx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Um quadro da animação: aproxima altura, destaque e rolagem dos alvos (amortecimento exponencial,
// independente da taxa do monitor). Devolve true enquanto algo ainda se mexe.
bool Step() {
    double now = anim::Now();
    float dt = (float)std::clamp((now - g_lastFrame) / 1000.0, 0.0, 0.05);
    g_lastFrame = now;
    auto ease = [dt](float& v, float target, float tau) {
        float d = target - v;
        if (std::fabs(d) < 0.25f) { v = target; return false; }
        v += d * (1 - std::exp(-dt / tau));
        return true;
    };
    bool moving = false;
    if (liquid::On()) moving |= g_spLuz.Passo(g_spHover.y >= 0 && g_spHover.y < FIELD_H ? g_spHover.x : -1, g_spHover.y >= 0 && g_spHover.y < FIELD_H ? g_spHover.y : -1, dt);
    Layout();
    if (g_edit || !g_cellAt.empty()) {  // Apps tremendo: cada célula desliza até o lugar novo
        for (auto& row : g_rows) {
            if (row.type != R_APP || row.res < 0) continue;
            const std::wstring& t = g_res[row.res].target;
            auto it = g_cellAt.find(t);
            if (it == g_cellAt.end()) { g_cellAt[t] = { row.r.left, row.r.top }; continue; }
            moving |= ease(it->second.x, row.r.left, 0.07f);
            moving |= ease(it->second.y, row.r.top, 0.07f);
        }
        if (!g_edit && !moving) g_cellAt.clear();
        moving |= g_edit;  // tremendo: o relógio segue
    }
    {  // a cápsula alarga até o painel (ou volta), junto com a altura
        float tw = liquid::On() && Compact() && g_reveal ? PillW() : W;  // sem o ponteiro em cima, a cápsula fica comprida
        if (g_pill < 0) g_pill = tw;
        moving |= ease(g_pill, tw, 0.055f);
        moving |= now - g_listIn < 260;  // conteúdo entrando
    }
    float targetH = PanelH();
    float listH = std::max(0.f, targetH - ListTop());
    if (liquid::Golden()) {
        // macOS 27 (vídeo, 5:58,8–5:58,9, quadro a quadro): o painel não cresce; aparece já do tamanho final, esmaecido e
        // desfocado, e fica nítido em ~140 ms. Ao mudar de tamanho (digitando), troca direto
        if (g_h <= FIELD_H + 1.5f && targetH > FIELD_H + 1.5f) g_panelInAt = now + (now - g_openAt < 100 ? 110 : 0);  // ao abrir, a barra vem antes e a lista ~110 ms depois (vídeo)
        g_h = targetH;
        moving |= now - g_panelInAt < 160;  // (inclui a espera antes de começar)
    } else
    moving |= ease(g_h, targetH, 0.045f);  // cresce/encolhe rápido, como o do Mac
    // rolagem para mostrar o escolhido (com o cabeçalho dele)
    if (g_sel >= 0 && g_sel < (int)g_rows.size()) {
        float top = g_rows[g_sel].r.top, bot = g_rows[g_sel].r.bottom;
        if (g_sel > 0 && g_rows[g_sel - 1].type == R_HEAD) top = g_rows[g_sel - 1].r.top;
        // só quando a seleção muda (setas, digitar): antes valia a cada quadro e a roda do mouse não rolava, a
        // lista voltava sozinha para o escolhido (17/11/2026: não dava para ver o resto dos apps)
        static int shown = -2; static size_t shownRows = 0;
        if (g_sel != shown || g_rows.size() != shownRows) {
            shown = g_sel; shownRows = g_rows.size();
            if (bot - g_scroll > listH - 6) g_scroll = bot - listH + 6;
            if (top - g_scroll < 4) g_scroll = std::max(0.f, top - 4);
        }
        if (g_selTop < 0) { g_selTop = g_rows[g_sel].r.top; g_selBot = g_rows[g_sel].r.bottom; }  // primeira vez: sem deslizar
        moving |= ease(g_selTop, g_rows[g_sel].r.top, 0.05f);
        moving |= ease(g_selBot, g_rows[g_sel].r.bottom, 0.05f);
    } else g_selTop = g_selBot = -1;
    moving |= ease(g_scrollNow, g_scroll, 0.06f);
    if (g_open) {
        moving |= now - g_openAt < 520;  // a mola de abrir assenta
        if (liquid::On()) moving |= GooAnimating();  // os círculos saindo da cápsula
    } else if (g_visible) {
        // fechando. Antes este ramo também pegava o Spotlight ABERTO sem o Liquid Glass (Sequoia): a janela
        // sumia no 2º quadro e o relógio de quadros ficava ligado para sempre (11/11/2026, VM com liquid_glass=0)
        if (now - g_closeAt >= 160) {  // terminou de fechar
            g_visible = false;
            ShowWindow(g_wnd, SW_HIDE);
            ShowWindow(g_back, SW_HIDE);
            return false;
        }
        moving = true;
    }
    return moving;
}

// abrir: aparece com a curva do Mac e cresce de 96% a 100% numa mola ágil (passa um tiquinho e assenta);
// fechar: o caminho de volta, mais rápido e acelerando
void Transition(float& fade, float& scale) {
    double now = anim::Now();
    anim::Panel p = g_open ? anim::PanelIn(now - g_openAt) : anim::PanelOut(now - g_closeAt);  // a mola comum dos painéis
    fade = p.fade; scale = p.scale;
}

void Animate() {  // liga o relógio de quadros até tudo parar
    g_lastFrame = anim::Now();
    ui::FrameClock(g_wnd, WM_SP_FRAME, true);
}

void Refresh() {
    Rebuild();
    Animate();
    Paint();
}

void QueryChanged() {
    g_files.clear();
    g_more.clear();  // texto novo: as seções voltam ao resumo
    ++g_gen;
    Refresh();
    KillTimer(g_wnd, T_SEARCH);
    if (Fold(g_query).size() >= 2) SetTimer(g_wnd, T_SEARCH, 140, nullptr);  // espera parar de digitar
    g_caretOn = true;
    SetTimer(g_wnd, T_CARET, 530, nullptr);
}

// O teclado precisa vir para cá: o Windows só deixa trocar o primeiro plano a quem recebeu a última
// entrada; ligando a entrada da nossa thread à do app da frente por um instante, a troca passa
void Grab(HWND w) {
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(w);
    BringWindowToTop(w);
    SetFocus(w);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

void Close(bool restore = false) {
    if (!g_open) return;
    g_open = false;
    menubar::SoundChanged(false);  // a cápsula da lupa da barra sai
    g_edit = g_dragging = false; g_press = -1; g_cellAt.clear();
    KillTimer(g_wnd, T_REGRAB);
    // fechado pelo Esc ou pelo atalho: o app de antes volta para a frente (senão o Windows escolhia outro,
    // e o Notas sumia atrás do Chrome)
    if (restore && g_prevFg && IsWindow(g_prevFg) && GetForegroundWindow() == g_wnd) Grab(g_prevFg);
    g_closedAt = GetTickCount(); g_closeAt = anim::Now();
    ++g_gen;
    KillTimer(g_wnd, T_SEARCH);
    KillTimer(g_wnd, T_CARET);
    Animate();  // esmaece; o Step esconde as janelas no fim
}

void ShowInFinder(const std::wstring& path) {
    for (auto& a : apps::list)
        if (a.finder) { ShellExecuteW(nullptr, L"open", a.launch.c_str(), (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL); return; }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void Open(int row, bool reveal) {
    if (row < 0 || row >= (int)g_rows.size() || g_rows[row].res < 0) return;
    Result r = g_res[g_rows[row].res];
    if (r.kind == K_APP && !reveal) {  // aprende: este texto leva a este app
        std::wstring q = Fold(g_query);
        while (!q.empty() && q.back() == L' ') q.pop_back();
        std::wstring target = r.target;
        std::thread([target, q] { Learn(target, q); }).detach();
        std::lock_guard<std::mutex> l(g_mu);
        for (auto& a : g_apps) if (a.parsing == target) { a.use += 3; break; }
    }
    Close();
    switch (r.kind) {
    case K_APP: {
        std::wstring target = L"shell:AppsFolder\\" + r.target;
        if (!reveal && r.target.size() > 4 && r.target.find(L":\\") != std::wstring::npos && !_wcsicmp(r.target.c_str() + r.target.size() - 4, L".exe")) {
            // nossos apps (e outros pelo .exe): direto
            if (!_wcsicmp(r.target.c_str() + r.target.size() - wcslen(EXE_SETTINGS), EXE_SETTINGS)) shell::OpenSettings(L"");
            else ShellExecuteW(nullptr, L"open", r.target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        if (reveal && r.target.find(L":\\") != std::wstring::npos) { ShowInFinder(r.target); break; }  // app com .exe
        if (shell::OpenCalculator(target)) break;  // a Calculadora do Windows: abre a nossa
        if (apps::LaunchNoOcclusion(target)) break;
        if (apps::ActivatePackaged(target)) break;  // Segurança do Windows, ChatGPT...: sem o Explorador, só pelo ativador
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr))) {
            SHELLEXECUTEINFOW se{ sizeof(se) };
            se.fMask = SEE_MASK_IDLIST; se.lpIDList = pidl; se.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&se);
            CoTaskMemFree(pidl);
        }
        break;
    }
    case K_SETTING: shell::OpenSettings(r.target); break;
    case K_CALC:
        if (OpenClipboard(g_wnd)) {
            EmptyClipboard();
            size_t n = (r.name.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(g), r.name.c_str(), n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
            CloseClipboard();
        }
        break;
    case K_WEB: {
        std::wstring u = L"https://www.google.com/search?q=";
        for (wchar_t c : r.target) {
            if (iswalnum(c) && c < 128) u += c;
            else if (c == L' ') u += L'+';
            else {  // UTF-8 com %XX
                char b[8]; int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, b, 8, nullptr, nullptr);
                for (int i = 0; i < n; i++) { wchar_t h[4]; swprintf_s(h, L"%%%02X", (unsigned char)b[i]); u += h; }
            }
        }
        ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    default:
        if (reveal) ShowInFinder(r.target);
        else if (!AbrirNoAppMac(r.target)) ShellExecuteW(nullptr, L"open", r.target.c_str(), nullptr, r.detail.c_str(), SW_SHOWNORMAL);
        break;
    }
    Log(L"spotlight: %ls %ls", reveal ? L"mostrando" : L"abrindo", r.name.c_str());
}

// Caminho no disco do resultado da linha (arquivo, pasta, app com .exe/.lnk); vazio = não é arquivo
std::wstring RowPath(int i) {
    if (i < 0 || i >= (int)g_rows.size() || g_rows[i].res < 0) return {};
    const Result& r = g_res[g_rows[i].res];
    if (r.kind != K_APP && !(r.kind >= K_DOC && r.kind <= K_OTHER)) return {};
    if (r.target.find(L":\\") == std::wstring::npos) return {};
    if (GetFileAttributesW(r.target.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return r.target;
}

// Arrasta o arquivo da linha para fora (Finder, Mesa, outro app). Soltar copia, como no Spotlight do Mac.
void StartFileDrag(HWND h, int row) {
    std::wstring path = RowPath(row);
    if (path.empty()) return;
    IDataObject* d = nullptr;
    if (FAILED(ctxmenu::GetUIObject(h, { path }, IID_IDataObject, (void**)&d)) || !d) return;
    // a imagem do arrasto: o ícone do arquivo
    IDragSourceHelper* helper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) {
        HBITMAP hb = nullptr;
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            f->GetImage({ 64, 64 }, SIIGBF_BIGGERSIZEOK, &hb);
            f->Release();
        }
        if (hb) {
            SHDRAGIMAGE di{};
            BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
            di.sizeDragImage = { bm.bmWidth, std::abs(bm.bmHeight) };
            di.ptOffset = { bm.bmWidth / 2, std::abs(bm.bmHeight) / 2 };
            di.hbmpDragImage = hb;  // o helper passa a ser dono do bitmap
            di.crColorKey = CLR_NONE;
            if (FAILED(helper->InitializeFromBitmap(&di, d))) DeleteObject(hb);
        }
        helper->Release();
    }
    if (GetCapture() == h) ReleaseCapture();
    g_fpress = -1;
    g_fileDrag = true;
    DWORD eff = 0;
    HRESULT hr = SHDoDragDrop(h, d, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &eff);
    g_fileDrag = false;
    d->Release();
    Log(L"spotlight: arrastou %ls → 0x%08X efeito %lu", path.c_str(), (unsigned)hr, eff);
    if (hr == DRAGDROP_S_DROP && eff != DROPEFFECT_NONE) Close(false);  // soltou em algum lugar: sai da frente
    else if (GetForegroundWindow() != h) Close(false);
}

void Move(int dir) {
    if (g_rows.empty()) return;
    int i = g_sel;
    for (int n = 0; n < (int)g_rows.size(); n++) {
        i += dir;
        if (i < 0 || i >= (int)g_rows.size()) return;
        if (g_rows[i].res >= 0) { g_sel = i; Animate(); Paint(); return; }
    }
}

void Paste() {
    if (!OpenClipboard(g_wnd)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* s = (const wchar_t*)GlobalLock(h)) {
            std::wstring t = s;
            GlobalUnlock(h);
            for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            if (g_selAll) { g_query.clear(); g_selAll = false; }
            g_query += t.substr(0, 200);
        }
    }
    CloseClipboard();
    QueryChanged();
}

D2D1_POINT_2F Pt(LPARAM l) { float sc = Sc(); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }

int RowAt(D2D1_POINT_2F p) {  // linha/miniatura embaixo do ponto (coordenadas da janela)
    if (p.y <= ListTop()) return -1;
    float y = p.y - ListTop() + g_scroll;
    for (int i = 0; i < (int)g_rows.size(); i++) {
        auto& r = g_rows[i].r;
        if (g_rows[i].res >= 0 && p.x >= r.left && p.x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

void StartEdit() {
    g_edit = true;
    g_editAt = anim::Now();
    g_sel = -1;
    Animate();
    Paint();
}
void EndEdit() {
    g_edit = false;
    Animate();
    Paint();
}
void SaveOrder() {  // a ordem de agora (todos os apps não tirados)
    std::vector<std::wstring> v;
    for (auto& row : g_rows) if (row.type == R_APP && row.res >= 0 && row.sect != 7) v.push_back(g_res[row.res].name);  // sem a fileira dos mais usados
    shell::SetSetting(L"apps_ordem", JoinBar(v));
    Log(L"spotlight: ordem dos apps guardada (%zu)", v.size());
}
void RebuildKeep() {  // refaz a lista sem voltar a rolagem para o topo
    float sc = g_scroll;
    Rebuild();
    g_scroll = sc; g_scrollNow = sc;
    if (g_edit) g_sel = -1;
    Animate();
    Paint();
}
void HideApp(int row) {
    std::wstring name = g_res[g_rows[row].res].name;
    auto hid = SplitBar(shell::Setting(L"launchpad_ocultos", L""));
    hid.push_back(name);
    shell::SetSetting(L"launchpad_ocultos", JoinBar(hid));
    Log(L"spotlight: \"%ls\" tirado do painel Apps", name.c_str());
    RebuildKeep();
}
void AppsMenu(HWND h, LPARAM l) {
    auto mi = [](const std::wstring& t, int id, bool dis = false) { ctxmenu::Item i; i.label = t; i.id = id; i.disabled = dis; return i; };
    std::vector<ctxmenu::Item> items;
    items.push_back(mi(Tr(g_edit ? L"Concluir Edição" : L"Editar Apps"), 1, !g_edit && !CanArrange()));
    items.push_back(mi(Tr(L"Organizar por Nome"), 2, shell::Setting(L"apps_ordem", L"").empty()));
    bool semSis = shell::Setting(L"launchpad_sem_sistema", L"0") == L"1";
    ctxmenu::Item sys = mi(Tr(L"Ocultar Apps do Sistema"), 3);  // Painel de Controle, Mapa de Caracteres, Ferramentas do Windows...
    sys.checked = semSis;
    items.push_back(sys);
    auto hid = SplitBar(shell::Setting(L"launchpad_ocultos", L""));
    if (!hid.empty()) {
        ctxmenu::Item sub = mi(Tr(L"Trazer de Volta"), 0);
        for (int k = 0; k < (int)hid.size() && k < 40; k++) sub.sub.push_back(mi(hid[k], 100 + k));
        ctxmenu::Item sep; sep.separator = true; sub.sub.push_back(sep);
        sub.sub.push_back(mi(Tr(L"Trazer Todos"), 99));
        items.push_back(sub);
    }
    POINT sp{ (short)LOWORD(l), (short)HIWORD(l) };
    ClientToScreen(h, &sp);
    int id = ctxmenu::ShowItems(h, sp, items);
    if (id == 1) { if (g_edit) EndEdit(); else StartEdit(); }
    else if (id == 2) { shell::SetSetting(L"apps_ordem", L""); RebuildKeep(); }
    else if (id == 3) { shell::SetSetting(L"launchpad_sem_sistema", semSis ? L"0" : L"1"); RebuildKeep(); }
    else if (id == 99) { shell::SetSetting(L"launchpad_ocultos", L""); RebuildKeep(); }
    else if (id >= 100 && id - 100 < (int)hid.size()) {
        hid.erase(hid.begin() + (id - 100));
        shell::SetSetting(L"launchpad_ocultos", JoinBar(hid));
        RebuildKeep();
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INITMENUPOPUP || m == WM_DRAWITEM || m == WM_MEASUREITEM || m == WM_MENUCHAR) {  // submenus das extensões (Abrir com, Enviar para)
        LRESULT res = 0;
        if (ctxmenu::ForwardOwnerMsg(m, w, l, &res)) return res;
    }
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_SP_APPS: if (g_open && (!g_query.empty() || ModePanel())) Refresh(); return 0;
    case WM_SP_ICON:  // ícones chegam aos montes (a grade de apps): um redesenho só para vários
        SetTimer(h, T_ICONS, 24, nullptr);
        return 0;
    case WM_SP_FRAME:
        ui::FrameAck(h, WM_SP_FRAME);  // sem fila de quadros: o que se digita não espera atrás deles
        if (!Step()) ui::FrameClock(h, WM_SP_FRAME, false);
        Paint();
        return 0;
    case WM_SP_FILES: {
        auto* v = (std::vector<Result>*)l;
        if ((int)w == g_gen && g_open) {
            // mantém o item escolhido se ele continuar na lista
            std::wstring keep = g_sel >= 0 && g_rows[g_sel].res >= 0 ? IconKey(g_res[g_rows[g_sel].res]) : L"";
            bool moved = g_sel >= 0 && g_rows[g_sel].res >= 0 && g_rows[g_sel].res != 0;
            g_files = std::move(*v);
            Rebuild();
            if (moved) for (int i = 0; i < (int)g_rows.size(); i++) if (g_rows[i].res >= 0 && IconKey(g_res[g_rows[i].res]) == keep) { g_sel = i; break; }
            Animate();
            Paint();
        }
        delete v;
        return 0;
    }
    case WM_SP_SUGG: {
        auto* v = (std::pair<std::wstring, std::vector<std::wstring>>*)l;
        if ((int)w == g_gen && g_open) {
            std::wstring keep = g_sel >= 0 && g_rows[g_sel].res >= 0 ? IconKey(g_res[g_rows[g_sel].res]) : L"";
            bool moved = g_sel >= 0 && g_rows[g_sel].res >= 0 && g_rows[g_sel].res != 0;
            g_suggFor = v->first;
            g_sugg = std::move(v->second);
            Rebuild();
            if (moved) for (int i = 0; i < (int)g_rows.size(); i++) if (g_rows[i].res >= 0 && IconKey(g_res[g_rows[i].res]) == keep) { g_sel = i; break; }
            Animate();
            Paint();
        }
        delete v;
        return 0;
    }
    case WM_TIMER:
        if (w == T_SEARCH) {
            KillTimer(h, T_SEARCH);
            std::wstring q = g_query;
            while (!q.empty() && q.back() == L' ') q.pop_back();
            int gen = g_gen;
            std::thread(SearchFiles, gen, q).detach();
            if (!q.empty() && shell::Setting(L"spotlight_google", L"1") == L"1") std::thread(FetchSuggestions, gen, q).detach();
        } else if (w == T_CARET) { g_caretOn = !g_caretOn; Paint(); }
        else if (w == T_REGRAB) { KillTimer(h, T_REGRAB); if (g_open) Grab(h); }
        else if (w == T_ICONS) { KillTimer(h, T_ICONS); Paint(); }
        else if (w == T_HOLD) {  // segurou um app: a grade treme (como no Launchpad)
            KillTimer(h, T_HOLD);
            if (g_press >= 0 && !g_edit && CanArrange()) StartEdit();
        }
        return 0;
    case WM_CHAR: {
        wchar_t c = (wchar_t)w;
        if (c < 32 || c == 127) return 0;  // Backspace, Enter, Esc e Ctrl+letra vêm pelo WM_KEYDOWN
        if (g_selAll) { g_query.clear(); g_selAll = false; }
        if (g_query.size() < 200) g_query += c;
        QueryChanged();
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (w) {
        case VK_ESCAPE:
            if (g_edit) { EndEdit(); return 0; }
            if (!g_query.empty()) { g_query.clear(); g_selAll = false; QueryChanged(); }
            else if (g_mode) { g_mode = 0; g_chip = 0; Refresh(); return 0; }  // Esc: sai do modo do círculo antes de fechar

            else Close(true);
            return 0;
        case VK_RETURN: Open(g_sel, ctrl); return 0;
        case '1': case '2': case '3': case '4':  // ⌘1–⌘4 do Mac: Aplicativos, Arquivos, Ações, Área de Transferência
            if (ctrl) {
                if (w == '4') { Close(); INPUT in[4]{}; for (auto& x : in) x.type = INPUT_KEYBOARD;
                    in[0].ki.wVk = VK_LWIN; in[1].ki.wVk = 'V'; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP; in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(4, in, sizeof(INPUT)); return 0; }
                g_mode = (int)(w - '0'); g_chip = 0; g_scroll = 0; g_query.clear(); ModeChanged(); Refresh();
                return 0;
            }
            break;
        case 'Y':  // ⌘Y do Mac: Quick Look do arquivo escolhido
            if (ctrl && g_sel >= 0 && g_sel < (int)g_rows.size() && g_rows[g_sel].res >= 0) {
                const Result& r = g_res[g_rows[g_sel].res];
                if (r.kind == K_DOC || r.kind == K_IMAGE || r.kind == K_MEDIA || r.kind == K_OTHER) quicklook::Toggle(r.target);
                return 0;
            }
            break;
        case VK_UP: case VK_DOWN: {  // nas miniaturas, sobe/desce pula a fileira inteira
            g_keyNav = true;
            int dir = w == VK_UP ? -1 : 1;
            if (g_sel >= 0 && g_rows[g_sel].type == R_APP) {  // grade de apps: a mesma coluna na fileira de cima/baixo
                int i = g_sel + dir * 5;
                if (i >= 0 && i < (int)g_rows.size() && g_rows[i].type == R_APP) { g_sel = i; Animate(); Paint(); }
                return 0;
            }
            if (g_sel >= 0 && g_rows[g_sel].type == R_TILE) {
                int i = g_sel;
                while (i + dir >= 0 && i + dir < (int)g_rows.size() && g_rows[i + dir].type == R_TILE && g_rows[i + dir].r.top == g_rows[g_sel].r.top) i += dir;
                g_sel = i;
            }
            Move(dir);
            return 0;
        }
        case VK_LEFT: case VK_RIGHT:  // entre as miniaturas
            g_keyNav = true;
            if (g_sel >= 0 && (g_rows[g_sel].type == R_TILE || g_rows[g_sel].type == R_APP)) {
                int i = g_sel + (w == VK_LEFT ? -1 : 1);
                if (i >= 0 && i < (int)g_rows.size() && g_rows[i].type == g_rows[g_sel].type) { g_sel = i; Animate(); Paint(); }
            }
            return 0;
        case VK_BACK:
            if (g_selAll) { g_query.clear(); g_selAll = false; }
            else if (ctrl) {  // apaga a palavra
                while (!g_query.empty() && g_query.back() == L' ') g_query.pop_back();
                while (!g_query.empty() && g_query.back() != L' ') g_query.pop_back();
            } else if (!g_query.empty()) g_query.pop_back();
            QueryChanged();
            return 0;
        case 'A': if (ctrl && !g_query.empty()) { g_selAll = true; Paint(); } return 0;
        case 'V': if (ctrl) Paste(); return 0;
        case 'C':
            if (ctrl && g_selAll && OpenClipboard(h)) {
                EmptyClipboard();
                size_t n = (g_query.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(g), g_query.c_str(), n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                CloseClipboard();
            }
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        if (Compact() && g_reveal) {  // Tahoe: os círculos ao lado da cápsula
            auto pt = Pt(l);
            for (int i = 0; i < 4; i++) {
                auto c = CircleRect(i);
                if (pt.x < c.left || pt.x >= c.right || pt.y >= FIELD_H) continue;
                if (i == 0) { g_mode = 1; g_chip = 0; g_scroll = 0; ModeChanged(); Refresh(); return 0; }  // Aplicativos: a grade de apps
                if (i == 3) {  // Área de Transferência: o histórico do Windows (Win+V)
                    Close();
                    INPUT in[4]{};
                    for (auto& x : in) x.type = INPUT_KEYBOARD;
                    in[0].ki.wVk = VK_LWIN; in[1].ki.wVk = 'V'; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(4, in, sizeof(INPUT));
                    return 0;
                }
                g_mode = i == 1 ? 2 : 3;
                g_chip = 0; g_scroll = 0;
                ModeChanged();
                Refresh();
                return 0;
            }
            if (pt.x >= PillW() && pt.y < FIELD_H) return 0;  // entre os círculos
        }
        if (Pt(l).y > g_h) { Close(); return 0; }  // abaixo do painel (a janela é maior que ele): fecha, como clicar fora
        {
            auto pt = Pt(l);
            auto in = [&](D2D1_RECT_F r) { return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom; };
            if (g_moreR.right > g_moreR.left && in(g_moreR)) { g_appsList = !g_appsList; g_scroll = 0; ModeChanged(); Refresh(); return 0; }  // grade ↔ lista
            if (HasChips() && pt.y > FIELD_H && pt.y < ListTop()) {  // filtros
                for (int c = 0; c < (int)g_chipR.size(); c++)
                    if (in(g_chipR[c])) { g_chip = g_chip == c + 1 ? 0 : c + 1; g_scroll = 0; ModeChanged(); Refresh(); return 0; }
                return 0;
            }
        }
        {   // "Mostrar Mais" / "Mostrar Menos" à direita do cabeçalho da seção
            auto pt = Pt(l);
            float y = pt.y - ListTop() + g_scroll;
            if (pt.y > ListTop())
                for (auto& row : g_rows) {
                    if (row.type != R_HEAD || row.sect < 0 || y < row.r.top || y >= row.r.bottom || pt.x < row.r.right - 140) continue;
                    if (g_more.count(row.sect)) g_more.erase(row.sect);
                    else if (g_canMore.count(row.sect)) g_more.insert(row.sect);
                    else break;
                    Refresh();
                    return 0;
                }
        }
        int i = RowAt(Pt(l));
        if (i < 0) { if (g_edit) EndEdit(); return 0; }  // clicar no vazio termina a edição
        if (g_rows[i].type == R_MODE) {  // macOS 27: a linha do modo entra nele (Área de Transferência: o Win+V)
            int k = g_rows[i].sect;
            if (k == 3) {
                Close();
                INPUT in[4]{};
                for (auto& x : in) x.type = INPUT_KEYBOARD;
                in[0].ki.wVk = VK_LWIN; in[1].ki.wVk = 'V'; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
                in[3].ki.wVk = VK_LWIN; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(4, in, sizeof(INPUT));
                return 0;
            }
            g_mode = k + 1; g_chip = 0; g_scroll = 0; ModeChanged(); Refresh();
            return 0;
        }
        if (g_rows[i].type == R_APP && CanArrange()) {
            g_press = i; g_dragging = false; g_downPt = Pt(l); g_pressWasSel = i == g_sel;
            SetCapture(h);
            if (g_edit) return 0;  // o × é no soltar
            if (m == WM_LBUTTONDBLCLK) { ReleaseCapture(); g_press = -1; Open(i, GetKeyState(VK_CONTROL) < 0); return 0; }
            SetTimer(h, T_HOLD, 550, nullptr);
            if (g_sel != i) { g_sel = i; Paint(); }
            return 0;
        }
        if (!RowPath(i).empty()) {  // arquivo: segurar e arrastar leva para fora; o segundo clique abre ao soltar
            if (m == WM_LBUTTONDBLCLK) { Open(i, GetKeyState(VK_CONTROL) < 0); return 0; }
            g_fpress = i; g_downPt = Pt(l); g_pressWasSel = i == g_sel;
            SetCapture(h);
            if (g_sel != i) { g_sel = i; Paint(); }
            return 0;
        }
        if (m == WM_LBUTTONDBLCLK || i == g_sel) Open(i, GetKeyState(VK_CONTROL) < 0);  // o primeiro clique escolhe, o segundo abre
        else { g_sel = i; Paint(); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (liquid::On() && g_open) {  // o brilho do círculo sob o ponteiro segue o mouse
            D2D1_POINT_2F np = Pt(l);
            bool was = g_spHover.y >= 0 && g_spHover.y < FIELD_H, now = np.y < FIELD_H;
            g_spHover = np;
            if ((was || now) && g_reveal && !GooAnimating()) Animate();  // a luz persegue o ponteiro (Step)
        }
        if (liquid::On() && g_open && !g_reveal && g_query.empty() && g_mode == 0 && Pt(l).y < FIELD_H + 8) {
            // Tahoe: o ponteiro em cima da cápsula solta os círculos (a gosma)
            g_reveal = true;
            g_revealAt = anim::Now();
            g_broken = 0;
            g_pill = PillW();
            Animate();
        }
        if (g_fpress >= 0) {
            if (!(w & MK_LBUTTON)) { g_fpress = -1; if (GetCapture() == h) ReleaseCapture(); return 0; }
            auto q = Pt(l);
            if (std::hypot(q.x - g_downPt.x, q.y - g_downPt.y) >= 6) StartFileDrag(h, g_fpress);
            return 0;
        }
        if (g_press < 0 || !(w & MK_LBUTTON)) return 0;
        auto p = Pt(l);
        if (!g_dragging && std::hypot(p.x - g_downPt.x, p.y - g_downPt.y) < 6) return 0;
        if (!g_dragging) {  // arrastar já entra na edição (como no Mac)
            g_dragging = true;
            KillTimer(h, T_HOLD);
            if (!g_edit) StartEdit();
        }
        g_dragPt = p;
        float listH = PanelH() - ListTop();  // perto da borda de cima/baixo: rola
        if (p.y < ListTop() + 24) g_scroll = std::max(0.f, g_scroll - 14);
        else if (p.y > ListTop() + listH - 24) g_scroll = std::min(std::max(0.f, ListHeight() - listH), g_scroll + 14);
        float y = p.y - ListTop() + g_scroll;
        int t = -1; float best = 1e18f;
        for (int k = 0; k < (int)g_rows.size(); k++) {
            if (g_rows[k].type != R_APP) continue;
            float cx = (g_rows[k].r.left + g_rows[k].r.right) / 2, cy = (g_rows[k].r.top + g_rows[k].r.bottom) / 2;
            float d = (p.x - cx) * (p.x - cx) + (y - cy) * (y - cy);
            if (d < best) { best = d; t = k; }
        }
        if (t >= 0 && t != g_press && g_press < (int)g_rows.size()) {
            Row moved = g_rows[g_press];
            g_rows.erase(g_rows.begin() + g_press);
            g_rows.insert(g_rows.begin() + t, moved);
            g_press = t;
        }
        Animate();
        Paint();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_fpress >= 0) {
            int i = g_fpress; g_fpress = -1;
            if (GetCapture() == h) ReleaseCapture();
            if (RowAt(Pt(l)) == i && g_pressWasSel) Open(i, GetKeyState(VK_CONTROL) < 0);  // o segundo clique abre
            return 0;
        }
        if (g_press < 0) return 0;
        KillTimer(h, T_HOLD);
        if (GetCapture() == h) ReleaseCapture();
        int i = g_press; g_press = -1;
        if (g_dragging) {
            g_dragging = false;
            SaveOrder();
            Animate();
            Paint();
            return 0;
        }
        auto p = Pt(l);
        if (g_edit) {  // o ×: tira o app do painel
            if (i < (int)g_rows.size() && g_rows[i].res >= 0) {
                float cx = (g_rows[i].r.left + g_rows[i].r.right) / 2;
                float xx = cx - APP_ICON / 2 + 7, xy = g_rows[i].r.top + APP_ICON_TOP + 7, y = p.y - ListTop() + g_scroll;
                if ((p.x - xx) * (p.x - xx) + (y - xy) * (y - xy) <= 14 * 14) HideApp(i);
            }
            return 0;
        }
        if (RowAt(p) == i && g_pressWasSel) Open(i, GetKeyState(VK_CONTROL) < 0);  // o segundo clique abre
        return 0;
    }
    case WM_RBUTTONUP: {
        int i = RowAt(Pt(l));
        if ((i < 0 || g_rows[i].res < 0) && g_mode == 1 && Fold(g_query).empty()) { AppsMenu(h, l); return 0; }  // fundo do painel Apps
        if (i < 0 || g_rows[i].res < 0) return 0;
        g_sel = i; Animate(); Paint();
        const Result r = g_res[g_rows[i].res];
        std::wstring folder, path;
        if (r.kind == K_APP) {
            if (r.target.find(L":\\") != std::wstring::npos) { path = r.target; folder = path.substr(0, path.find_last_of(L'\\')); }
            else { folder = L"shell:AppsFolder"; path = folder + L"\\" + r.target; }  // app da lista do Iniciar (Store ou atalho)
        } else if (r.kind >= K_DOC && r.kind <= K_OTHER && r.target.find(L":\\") != std::wstring::npos) {
            path = r.target;
            size_t s = path.find_last_of(L'\\');
            folder = s == std::wstring::npos ? path : path.substr(0, s);
            if (folder.size() == 2) folder += L'\\';  // C:
        } else return 0;  // conta, Google, ajustes: sem menu
        POINT sp{ (short)LOWORD(l), (short)HIWORD(l) };
        ClientToScreen(h, &sp);
        // "Mostrar no Finder" (abrir o local do arquivo), como no menu do Spotlight do Mac
        bool onDisk = path.find(L":\\") != std::wstring::npos;
        std::vector<ctxmenu::Item> extra;
        if (onDisk) extra.push_back({ Tr(L"Mostrar no Finder"), 1, false, false, false, L"Ctrl+Enter", {}, 0xE838 });
        if (r.kind == K_APP && g_mode == 1) extra.push_back({ Tr(L"Ocultar do Launchpad"), 2, false, false, false, L"", {}, 0xED1A });  // painel Apps
        if (!extra.empty()) ctxmenu::ExtraNext(extra);
        auto res = ctxmenu::Show(h, sp, folder, { path }, GetKeyState(VK_SHIFT) < 0);
        ctxmenu::ExtraNext({});
        Log(L"spotlight: menu de %ls%ls", path.c_str(), res.chosen ? L" (comando escolhido)" : L"");
        if (res.extra == 1) { Open(i, true); return 0; }
        if (res.extra == 2) { HideApp(i); return 0; }
        if (res.chosen) Close();
        return 0;
    }
    case WM_CONTEXTMENU: return 0;
    case WM_MOUSEWHEEL: {
        float listH = PanelH() - ListTop();
        g_scroll = std::clamp(g_scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * ROW_H * 2, 0.f, std::max(0.f, ListHeight() - listH));
        Animate();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && g_fileDrag) return 0;  // arrastando um arquivo para fora: o StartFileDrag decide
        if (LOWORD(w) == WA_INACTIVE) {
            HWND to = (HWND)l; if (!to) to = GetForegroundWindow();
            wchar_t cls[64] = {}, exe[MAX_PATH] = {}; DWORD pid = 0, n = MAX_PATH;
            if (to) { GetClassNameW(to, cls, 64); GetWindowThreadProcessId(to, &pid); }
            if (HANDLE ph = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) { QueryFullProcessImageNameW(ph, 0, exe, &n); CloseHandle(ph); }
            const wchar_t* b = wcsrchr(exe, L'\\');
            // Logo depois de abrir, sem clique: o Windows tirou o foco (ativou outra janela por conta própria).
            // Pega de volta em vez de fechar; um clique fora fecha normalmente
            bool clicked = GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || GetAsyncKeyState(VK_MBUTTON) < 0;
            if (g_open && !clicked && anim::Now() - g_openAt < 1500 && g_regrabs < 3) {
                g_regrabs++;
                Log(L"spotlight: o Windows tirou o foco (%ls, %ls) %.0f ms depois de abrir; pegando de volta", cls, b ? b + 1 : exe, anim::Now() - g_openAt);
                SetTimer(h, T_REGRAB, 1, nullptr);
                return 0;
            }
            Log(L"spotlight: perdeu o foco para %ls (%ls) %.0f ms depois de abrir", cls, b ? b + 1 : exe, anim::Now() - g_openAt);
            Close();
        }
        else SetFocus(h);  // ativa sem foco, as teclas chegariam como teclas de sistema
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool spotlight::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellSpotlight";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellSpotlightFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Spotlight", WS_POPUP,
                            0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) return false;
    g_glass = backdrop::Attach(g_back);
    if (!g_glass) {  // sem a Composição: acrílico com o canto do DWM
        DestroyWindow(g_back);
        g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        DWORD corner = 2; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
        ui::Acrylic(g_back, ui::Dark() ? 0x40201E1Eu : 0x40F6F6F6u);
    } else {
        DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
    }
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    g_tfField = ui::Format(26, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfRow = ui::Format(14, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfHead = ui::Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfTitle = ui::Format(16, DWRITE_FONT_WEIGHT_MEDIUM);
    g_tfSub = ui::Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfCell = ui::Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfChip = ui::Format(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    for (auto* tf : { g_tfField, g_tfRow, g_tfHead, g_tfTitle, g_tfSub })
        if (tf) { tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); tf->SetTrimming(&tr, nullptr); }
    if (g_tfHead) g_tfHead->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (g_tfChip) { g_tfChip->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); g_tfChip->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    if (g_tfCell) {  // nome embaixo do ícone: até duas linhas, cortado com reticências
        g_tfCell->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        g_tfCell->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        g_tfCell->SetTrimming(&tr, nullptr);
    }
    ScanApps();
    return true;
}

void spotlight::Rescan() { ScanApps(); }

bool spotlight::IsOpen() { return g_open; }

static void OpenWith(int mode);

void spotlight::Toggle() {
    if (g_open) { Close(true); g_closedAt = 0; return; }  // fechado pelo atalho: pode reabrir na hora (Win+Espaço seguidos)
    if (GetTickCount() - g_closedAt < 300) return;  // o clique na lupa acabou de fechar (perdeu o foco): não reabre
    OpenWith(-1);
}

void spotlight::OpenMode(int mode) {
    if (g_open && g_mode == mode && g_query.empty()) { Close(); g_closedAt = 0; return; }
    if (!g_open && GetTickCount() - g_closedAt < 300) return;  // o clique no Dock acabou de fechar (perdeu o foco)
    g_query.clear();
    g_chip = 0;
    if (g_open) { g_mode = mode; g_scroll = 0; ModeChanged(); Refresh(); return; }
    OpenWith(mode);
}

static void OpenWith(int mode) {
    g_open = true;
    menubar::SoundChanged(false);  // o ícone da barra ganha a cápsula do Tahoe
    if (g_appsAt && GetTickCount64() - g_appsAt > 5 * 60 * 1000) ScanApps();  // por garantia: o vigia (instalados.cpp) já avisa na hora
    if (mode >= 0) g_mode = mode;
    else if (g_query.empty()) g_mode = 0;  // reabre na cápsula com os círculos
    if (!g_visible) Metricas();  // tema (ou o modo Apps, mais largo) pode ter mudado desde a última vez
    g_scroll = 0;
    g_openAt = anim::Now();
    g_keyNav = false;
    const bool wasVisible = g_visible;
    if (!g_visible) { g_h = FIELD_H; g_scrollNow = 0; }  // nasce só com o campo; os resultados crescem dele
    g_visible = true;
    g_selTop = g_selBot = -1;
    g_selAll = !g_query.empty();  // como no Mac: reabre com a última busca, toda selecionada
    Rebuild();
    if (!wasVisible) { g_pill = W; g_reveal = false; g_revealAt = -1e9; }  // nasce como a cápsula comprida (Tahoe), sem os círculos
    if (mode >= 1 && ModePanel()) { Layout(); g_h = PanelH(); g_scrollNow = 0; g_pill = W; ModeChanged(); }  // painel de modo (Apps): já nasce do tamanho final
    if (!g_query.empty() && Fold(g_query).size() >= 2) { ++g_gen; SetTimer(g_wnd, T_SEARCH, 10, nullptr); }
    Place();
    Animate();
    Paint();
    {   // quem estava na frente (não o próprio Shell): volta para ele ao fechar
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        g_prevFg = fg && pid != GetCurrentProcessId() ? fg : nullptr;
    }
    g_regrabs = 0;
    ShowWindow(g_wnd, SW_SHOW);
    Grab(g_wnd);
    g_caretOn = true;
    SetTimer(g_wnd, T_CARET, 530, nullptr);
}
