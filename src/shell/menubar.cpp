// Barra de menus no topo, como no macOS Sequoia: logo (menu do sistema), nome do app ativo em
// negrito, menus do app; à direita os ícones de status (bandeja), rede, Spotlight, Central de
// Controle e relógio. Ver docs/dock-e-barra-de-menus.md.
#include "shell.h"
#include "../comum/simbolos.h"
#include "../menu/context_menu.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shlobj.h>
#include <powrprof.h>
#include <wlanapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <bluetoothapis.h>
#include <propsys.h>
#pragma comment(lib, "bthprops.lib")
#include <shellscalingapi.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#include <algorithm>
#include <cmath>
#include <map>
#include "../comum/idioma.h"

namespace {

const float BAR_PT_SEQUOIA = 26;  // altura da barra (telas sem entalhe; um pouco maior que os 24 pt do Mac, para telas grandes)
// Tahoe: 30 pt, medido no macOS 26 (guia macOS26-Theme-Reference, seções 13 e 35: frame.maxY − visibleFrame.maxY)
// Leopard: 22 pt, a barra do Mac OS X 10.5
#define BAR_PT (liquid::On() ? 30.f : liquid::Leopard() ? 22.f : BAR_PT_SEQUOIA)
const float PAD_SEQUOIA = 10;     // espaço de cada lado de um ícone de status (Mac: ~10 pt, um item a cada ~38 pt)
// Tahoe (guia, seção 35): cada item da direita = o ícone + ~8 pt de respiro de cada lado (31–42 pt por item)
#define PAD (liquid::On() ? 8.f : PAD_SEQUOIA)

bool g_overflow;  // macOS 27: algum ícone da bandeja não coube na barra (mostra a »)
enum Kind { K_LOGO, K_APP, K_MENU, K_TRAY, K_NET, K_SPOT, K_CC, K_CLOCK, K_MORE, K_BATT, K_SOUND, K_BT, K_INPUT };
struct Hit { D2D1_RECT_F r; Kind kind; int index; };

HWND g_bar, g_owner;            // g_bar: a barra do monitor principal
// Uma barra em cada monitor (como no Mac), todas com o mesmo conteúdo. Cada uma tem a sua superfície e os
// seus itens clicáveis; g_cur é a que está sendo desenhada/clicada agora (nullptr = a principal), e g_s/g_hits
// apontam para as dela.
struct BarState { ui::Surface s; std::vector<Hit> hits; };
std::map<HWND, BarState> g_state;
std::vector<HWND> g_extras;     // barras dos outros monitores
HWND g_cur, g_openBar;
HWND Cur() { return g_cur ? g_cur : g_bar; }
#define g_s (g_state[Cur()].s)
#define g_hits (g_state[Cur()].hits)
int g_open = -1;                 // índice em g_hits do item com menu aberto (na barra g_openBar)
HWND g_fg;                       // janela do app ativo (fora as nossas)
std::wstring g_appName = L"Finder";
std::vector<std::wstring> g_titles;   // menus do app ativo
HMENU g_hmenu;                   // menu Win32 real do app (se tiver)
bool g_finderActive = true;
IDWriteTextFormat *g_tf, *g_tfBold, *g_tfIcon, *g_tfBadge;

// Como no Mac, a barra mostra pouca coisa: os ícones de bandeja só aparecem se o usuário fixar
// ("Mostrar na barra de menus"); os outros ficam na lista da setinha. Guardados pelo nome (dica).
std::vector<std::wstring> Shown() {
    std::vector<std::wstring> v;
    std::wstring s = shell::Setting(L"barra_icones", L"");
    size_t a = 0, b;
    while ((b = s.find(L'|', a)) != std::wstring::npos) { if (b > a) v.push_back(s.substr(a, b - a)); a = b + 1; }
    if (a < s.size()) v.push_back(s.substr(a));
    return v;
}
std::wstring TrayKey(const TrayIcon& t) {  // nome estável: a dica até a primeira quebra de linha ou ":"
    std::wstring k = t.tip.substr(0, t.tip.find_first_of(L"\r\n:"));
    while (!k.empty() && k.back() == L' ') k.pop_back();
    return k;
}
bool IsShown(const TrayIcon& t) {
    if (shell::Setting(L"barra_icones_todos", L"0") == L"1") return true;  // Central de Controle: "Sempre na barra de menus"
    std::wstring k = TrayKey(t);
    for (auto& s : Shown()) if (!_wcsicmp(s.c_str(), k.c_str())) return true;
    return false;
}
void SetShown(const TrayIcon& t, bool on) {
    auto v = Shown();
    std::wstring k = TrayKey(t);
    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::wstring& x) { return !_wcsicmp(x.c_str(), k.c_str()); }), v.end());
    if (on) v.push_back(k);
    std::wstring out;
    for (auto& x : v) { if (!out.empty()) out += L"|"; out += x; }
    shell::SetSetting(L"barra_icones", out);
}

std::wstring Clean(const wchar_t* s, std::wstring* shortcut = nullptr) {
    std::wstring out;
    for (const wchar_t* p = s; *p; p++) {
        if (*p == L'\t') { if (shortcut) *shortcut = p + 1; break; }
        if (*p == L'&') { if (p[1] == L'&') { out += L'&'; p++; } continue; }
        out += *p;
    }
    return out;
}

// ---- teclas para os menus genéricos (apps sem menu Win32): mandadas ao app ativo
void SendKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i{ INPUT_KEYBOARD }; i.ki.wVk = k; in.push_back(i); }
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) { INPUT i{ INPUT_KEYBOARD }; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

struct KeyCmd { int id; const wchar_t* label; const wchar_t* shortcut; std::initializer_list<WORD> keys; };
// ids 1000+: atalhos de teclado mandados ao app
const KeyCmd g_keyCmds[] = {
    { 1001, L"Nova Janela", L"Ctrl+N", { VK_CONTROL, 'N' } },
    { 1002, L"Abrir…", L"Ctrl+O", { VK_CONTROL, 'O' } },
    { 1003, L"Fechar Janela", L"Ctrl+W", { VK_CONTROL, 'W' } },
    { 1004, L"Salvar", L"Ctrl+S", { VK_CONTROL, 'S' } },
    { 1005, L"Imprimir…", L"Ctrl+P", { VK_CONTROL, 'P' } },
    { 1010, L"Desfazer", L"Ctrl+Z", { VK_CONTROL, 'Z' } },
    { 1011, L"Refazer", L"Ctrl+Y", { VK_CONTROL, 'Y' } },
    { 1012, L"Recortar", L"Ctrl+X", { VK_CONTROL, 'X' } },
    { 1013, L"Copiar", L"Ctrl+C", { VK_CONTROL, 'C' } },
    { 1014, L"Colar", L"Ctrl+V", { VK_CONTROL, 'V' } },
    { 1015, L"Selecionar Tudo", L"Ctrl+A", { VK_CONTROL, 'A' } },
    { 1016, L"Buscar", L"Ctrl+F", { VK_CONTROL, 'F' } },
    { 1020, L"Nova Aba", L"Ctrl+T", { VK_CONTROL, 'T' } },
    { 1021, L"Nova Pasta", L"Ctrl+Shift+N", { VK_CONTROL, VK_SHIFT, 'N' } },
    { 1022, L"Obter Informações", L"Ctrl+I", { VK_CONTROL, 'I' } },
    { 1023, L"Mover para o Lixo", L"Delete", { VK_DELETE } },
    { 1030, L"como Ícones", L"Ctrl+1", { VK_CONTROL, '1' } },
    { 1031, L"como Lista", L"Ctrl+2", { VK_CONTROL, '2' } },
    { 1032, L"como Colunas", L"Ctrl+3", { VK_CONTROL, '3' } },
    { 1033, L"como Galeria", L"Ctrl+4", { VK_CONTROL, '4' } },
    { 1034, L"Mostrar Opções de Visualização", L"Ctrl+J", { VK_CONTROL, 'J' } },
    { 1040, L"Voltar", L"Alt+←", { VK_MENU, VK_LEFT } },
    { 1041, L"Avançar", L"Alt+→", { VK_MENU, VK_RIGHT } },
    { 1042, L"Pasta Superior", L"Ctrl+↑", { VK_CONTROL, VK_UP } },
    { 1043, L"Ir para a Pasta…", L"Ctrl+Shift+G", { VK_CONTROL, VK_SHIFT, 'G' } },
    { 1050, L"Ajuda", L"F1", { VK_F1 } },
    { 1051, L"Ajustes…", L"Ctrl+,", { VK_CONTROL, VK_OEM_COMMA } },
    // menus próprios dos apps sem menu Win32 (AppMenus): os atalhos do Windows de cada um
    { 5001, L"Nova Janela Anônima", L"Ctrl+Shift+N", { VK_CONTROL, VK_SHIFT, 'N' } },
    { 5002, L"Reabrir Aba Fechada", L"Ctrl+Shift+T", { VK_CONTROL, VK_SHIFT, 'T' } },
    { 5003, L"Abrir Arquivo…", L"Ctrl+O", { VK_CONTROL, 'O' } },
    { 5004, L"Abrir Local…", L"Ctrl+L", { VK_CONTROL, 'L' } },
    { 5005, L"Fechar Janela", L"Ctrl+Shift+W", { VK_CONTROL, VK_SHIFT, 'W' } },
    { 5006, L"Fechar Aba", L"Ctrl+W", { VK_CONTROL, 'W' } },
    { 5007, L"Salvar Página Como…", L"Ctrl+S", { VK_CONTROL, 'S' } },
    { 5010, L"Sempre Mostrar a Barra de Favoritos", L"Ctrl+Shift+B", { VK_CONTROL, VK_SHIFT, 'B' } },
    { 5011, L"Parar", L"Esc", { VK_ESCAPE } },
    { 5012, L"Recarregar Esta Página", L"Ctrl+R", { VK_CONTROL, 'R' } },
    { 5013, L"Forçar Recarregamento", L"Ctrl+Shift+R", { VK_CONTROL, VK_SHIFT, 'R' } },
    { 5014, L"Entrar em Tela Cheia", L"F11", { VK_F11 } },
    { 5015, L"Tamanho Real", L"Ctrl+0", { VK_CONTROL, '0' } },
    { 5016, L"Ampliar", L"Ctrl+=", { VK_CONTROL, VK_OEM_PLUS } },
    { 5017, L"Reduzir", L"Ctrl+-", { VK_CONTROL, VK_OEM_MINUS } },
    { 5018, L"Ferramentas do Desenvolvedor", L"Ctrl+Shift+I", { VK_CONTROL, VK_SHIFT, 'I' } },
    { 5019, L"Ver Código-Fonte", L"Ctrl+U", { VK_CONTROL, 'U' } },
    { 5020, L"Página Inicial", L"Alt+Home", { VK_MENU, VK_HOME } },
    { 5021, L"Mostrar Histórico Completo", L"Ctrl+H", { VK_CONTROL, 'H' } },
    { 5022, L"Limpar Dados de Navegação…", L"Ctrl+Shift+Del", { VK_CONTROL, VK_SHIFT, VK_DELETE } },
    { 5030, L"Gerenciador de Favoritos", L"Ctrl+Shift+O", { VK_CONTROL, VK_SHIFT, 'O' } },
    { 5031, L"Adicionar Esta Página aos Favoritos…", L"Ctrl+D", { VK_CONTROL, 'D' } },
    { 5032, L"Adicionar Todas as Abas aos Favoritos…", L"Ctrl+Shift+D", { VK_CONTROL, VK_SHIFT, 'D' } },
    { 5040, L"Selecionar Próxima Aba", L"Ctrl+Tab", { VK_CONTROL, VK_TAB } },
    { 5041, L"Selecionar Aba Anterior", L"Ctrl+Shift+Tab", { VK_CONTROL, VK_SHIFT, VK_TAB } },
    { 5050, L"Downloads", L"Ctrl+J", { VK_CONTROL, 'J' } },
    { 5051, L"Gerenciador de Tarefas", L"Shift+Esc", { VK_SHIFT, VK_ESCAPE } },
    { 5060, L"Central de Ajuda", L"F1", { VK_F1 } },
    { 5070, L"Refazer", L"Ctrl+Shift+Z", { VK_CONTROL, VK_SHIFT, 'Z' } },
    { 5071, L"Recarregar", L"Ctrl+R", { VK_CONTROL, 'R' } },
    { 5072, L"Busca Rápida", L"Ctrl+K", { VK_CONTROL, 'K' } },
    { 5073, L"Atalhos de Teclado", L"Ctrl+/", { VK_CONTROL, VK_DIVIDE } },
    { 5080, L"Nova Conversa", L"Ctrl+N", { VK_CONTROL, 'N' } },
    { 5081, L"Novo Grupo", L"Ctrl+Shift+N", { VK_CONTROL, VK_SHIFT, 'N' } },
    // Spotify
    { 5100, L"Nova Playlist", L"Ctrl+N", { VK_CONTROL, 'N' } },
    { 5101, L"Reproduzir/Pausar", L"Espaço", { VK_SPACE } },
    { 5102, L"Próxima", L"Ctrl+→", { VK_CONTROL, VK_RIGHT } },
    { 5103, L"Anterior", L"Ctrl+←", { VK_CONTROL, VK_LEFT } },
    { 5104, L"Aumentar Volume", L"Ctrl+↑", { VK_CONTROL, VK_UP } },
    { 5105, L"Diminuir Volume", L"Ctrl+↓", { VK_CONTROL, VK_DOWN } },
    { 5106, L"Repetir", L"Ctrl+R", { VK_CONTROL, 'R' } },
    { 5107, L"Aleatório", L"Ctrl+S", { VK_CONTROL, 'S' } },
    { 5108, L"Buscar", L"Ctrl+K", { VK_CONTROL, 'K' } },
    // VS Code
    { 5120, L"Novo Arquivo", L"Ctrl+N", { VK_CONTROL, 'N' } },
    { 5121, L"Nova Janela", L"Ctrl+Shift+N", { VK_CONTROL, VK_SHIFT, 'N' } },
    { 5122, L"Salvar Como…", L"Ctrl+Shift+S", { VK_CONTROL, VK_SHIFT, 'S' } },
    { 5123, L"Fechar Editor", L"Ctrl+W", { VK_CONTROL, 'W' } },
    { 5124, L"Substituir", L"Ctrl+H", { VK_CONTROL, 'H' } },
    { 5125, L"Buscar nos Arquivos", L"Ctrl+Shift+F", { VK_CONTROL, VK_SHIFT, 'F' } },
    { 5126, L"Paleta de Comandos…", L"Ctrl+Shift+P", { VK_CONTROL, VK_SHIFT, 'P' } },
    { 5127, L"Explorador", L"Ctrl+Shift+E", { VK_CONTROL, VK_SHIFT, 'E' } },
    { 5128, L"Controle do Código-Fonte", L"Ctrl+Shift+G", { VK_CONTROL, VK_SHIFT, 'G' } },
    { 5129, L"Extensões", L"Ctrl+Shift+X", { VK_CONTROL, VK_SHIFT, 'X' } },
    { 5130, L"Ir para Arquivo…", L"Ctrl+P", { VK_CONTROL, 'P' } },
    { 5131, L"Ir para Linha…", L"Ctrl+G", { VK_CONTROL, 'G' } },
    { 5132, L"Iniciar Depuração", L"F5", { VK_F5 } },
    { 5133, L"Executar sem Depurar", L"Ctrl+F5", { VK_CONTROL, VK_F5 } },
    { 5134, L"Barra Lateral", L"Ctrl+B", { VK_CONTROL, 'B' } },
};

ctxmenu::Item K(int id) {
    for (auto& k : g_keyCmds) if (k.id == id) { ctxmenu::Item i; i.label = Tr(k.label); i.id = id; i.shortcut = k.shortcut; return i; }
    return {};
}
ctxmenu::Item I(const std::wstring& label, int id, bool disabled = false, bool checked = false) {
    ctxmenu::Item i; i.label = label; i.id = id; i.disabled = disabled; i.checked = checked; return i;
}
ctxmenu::Item Sep() { ctxmenu::Item i; i.separator = true; return i; }

// ---- menu Win32 real do app → itens do nosso menu
std::vector<ctxmenu::Item> FromHmenu(HMENU m, int depth) {
    std::vector<ctxmenu::Item> out;
    int n = GetMenuItemCount(m);
    for (int k = 0; k < n; k++) {
        wchar_t buf[256] = {};
        MENUITEMINFOW mi{ sizeof(mi) };
        mi.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU;
        mi.dwTypeData = buf; mi.cch = 255;
        if (!GetMenuItemInfoW(m, k, TRUE, &mi)) continue;
        if (mi.fType & MFT_SEPARATOR) { if (!out.empty() && !out.back().separator) out.push_back(Sep()); continue; }
        if (mi.fType & MFT_OWNERDRAW || !buf[0]) continue;
        ctxmenu::Item it;
        it.label = Clean(buf, &it.shortcut);
        it.id = (int)mi.wID;
        it.disabled = (mi.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
        it.checked = (mi.fState & MFS_CHECKED) != 0;
        if (mi.hSubMenu) {
            if (g_fg) {
                // alguns apps só preenchem o submenu quando o item dele é destacado (o Ableton deixa "*" nos
                // sets recentes até receber WM_MENUSELECT); depois espera um pouco se ainda for o marcador
                SendMessageTimeoutW(g_fg, WM_MENUSELECT, MAKEWPARAM(k, MF_POPUP | MF_HILITE), (LPARAM)m, SMTO_ABORTIFHUNG, 200, nullptr);
                SendMessageTimeoutW(g_fg, WM_INITMENUPOPUP, (WPARAM)mi.hSubMenu, k, SMTO_ABORTIFHUNG, 200, nullptr);
                for (int t = 0; t < 12; t++) {
                    wchar_t first[8] = {};
                    GetMenuStringW(mi.hSubMenu, 0, first, 8, MF_BYPOSITION);
                    if (wcscmp(first, L"*")) break;
                    SendMessageTimeoutW(g_fg, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 100, nullptr);
                    Sleep(15);
                }
            }
            if (depth == 0) it.sub = FromHmenu(mi.hSubMenu, 1);
            else {  // nosso menu tem um nível de submenu: os mais fundos entram achatados
                out.push_back(Sep());
                for (auto& c : FromHmenu(mi.hSubMenu, 2)) out.push_back(c);
                out.push_back(Sep());
                continue;
            }
        }
        out.push_back(it);
    }
    while (!out.empty() && out.back().separator) out.pop_back();
    return out;
}

App* ActiveApp() {
    if (g_finderActive || !g_fg) for (auto& a : apps::list) if (a.finder) return &a;
    return g_fg ? apps::OfWindow(g_fg) : nullptr;
}

// ---- estado do sistema para a direita da barra
bool WifiName(std::wstring& ssid) {
    HANDLE h = nullptr; DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return false;
    bool ok = false;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < list->dwNumberOfItems && !ok; i++) {
            auto& info = list->InterfaceInfo[i];
            if (info.isState != wlan_interface_state_connected) continue;
            PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; DWORD sz = 0;
            if (WlanQueryInterface(h, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &sz, (PVOID*)&ca, nullptr) == ERROR_SUCCESS) {
                auto& s = ca->wlanAssociationAttributes.dot11Ssid;
                ssid.assign(s.ucSSID, s.ucSSID + s.uSSIDLength);
                ok = true;
                WlanFreeMemory(ca);
            }
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(h, nullptr);
    return ok;
}

// Estado do sistema para os ícones da direita (relido a cada segundo; o Bluetooth a cada 10 s)
struct Status {
    bool wlan = false, wifiOn = false, wired = false;
    bool batt = false, charging = false; int pct = 100;
    bool bt = false;
    bool sound = false, muted = false, phones = false; float vol = 0;
    HKL hkl = nullptr;
    DWORD btAt = 0;
} g_st;

bool WifiName(std::wstring& ssid);
bool g_audioStale;  // a saída padrão mudou: relê o objeto de áudio já

void RefreshStatus() {
    static DWORD wifiAt, audioAt;
    static IAudioEndpointVolume* vol;  // o mesmo objeto de áudio entre as leituras (antes: criado e solto a cada segundo)
    DWORD now = GetTickCount();
    // Wi-Fi: existe placa? está conectada? (a cada 5 s)
    if (!wifiAt || now - wifiAt > 5000) {
        wifiAt = now;
        g_st.wlan = false;
        HANDLE h = nullptr; DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &h) == ERROR_SUCCESS) {
            PWLAN_INTERFACE_INFO_LIST list = nullptr;
            if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS) { g_st.wlan = list->dwNumberOfItems > 0; WlanFreeMemory(list); }
            WlanCloseHandle(h, nullptr);
        }
        std::wstring ssid;
        g_st.wifiOn = g_st.wlan && WifiName(ssid);
        // cabo ligado (placa Ethernet com rede)? Aí o Wi-Fi desconectado não aparece na barra
        g_st.wired = false;
        ULONG sz = 16 * 1024;
        std::vector<BYTE> buf(sz);
        DWORD rc = GetAdaptersInfo((IP_ADAPTER_INFO*)buf.data(), &sz);
        if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(sz); rc = GetAdaptersInfo((IP_ADAPTER_INFO*)buf.data(), &sz); }
        if (rc == NO_ERROR)  // placa Ethernet com gateway = cabo conectado à rede
            for (auto* a = (IP_ADAPTER_INFO*)buf.data(); a; a = a->Next)
                if (a->Type == MIB_IF_TYPE_ETHERNET && strcmp(a->GatewayList.IpAddress.String, "0.0.0.0") && a->GatewayList.IpAddress.String[0]) { g_st.wired = true; break; }
    }
    // bateria (só em quem tem)
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100) {
        g_st.batt = true; g_st.pct = ps.BatteryLifePercent; g_st.charging = ps.ACLineStatus == 1;
    } else g_st.batt = false;
    // Bluetooth: tem rádio?
    if (!g_st.btAt || GetTickCount() - g_st.btAt > 10000) {
        g_st.btAt = GetTickCount();
        BLUETOOTH_FIND_RADIO_PARAMS fp{ sizeof(fp) }; HANDLE radio = nullptr;
        HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&fp, &radio);
        g_st.bt = f != nullptr;
        if (radio) CloseHandle(radio);
        if (f) BluetoothFindRadioClose(f);
    }
    // som: volume e mudo a cada segundo (barato, no objeto guardado); a saída padrão e se é um fone a cada 10 s
    bool fresh = false;
    if (g_audioStale) { g_audioStale = false; audioAt = 0; }
    if (vol && audioAt && now - audioAt < 10000) {
        BOOL mute = FALSE;
        if (SUCCEEDED(vol->GetMute(&mute)) && SUCCEEDED(vol->GetMasterVolumeLevelScalar(&g_st.vol))) { g_st.muted = mute; fresh = true; }
    }
    if (!fresh) {
    audioAt = now;
    if (vol) { vol->Release(); vol = nullptr; }
    g_st.sound = false;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol))) {
                BOOL mute = FALSE; vol->GetMute(&mute); vol->GetMasterVolumeLevelScalar(&g_st.vol);
                g_st.muted = mute; g_st.sound = true;
            }
            IPropertyStore* st = nullptr;
            g_st.phones = false;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &st))) {
                static const PROPERTYKEY formFactor = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(st->GetValue(formFactor, &v)) && v.vt == VT_UI4) g_st.phones = v.ulVal == 3 || v.ulVal == 5;  // fone / headset
                PropVariantClear(&v);
                st->Release();
            }
            dev->Release();
        }
        en->Release();
    }
    }
    // fonte de entrada (teclado) do app da frente
    HWND fg = GetForegroundWindow();
    g_st.hkl = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);
}

// "A" para o teclado americano (o "ABC" do Mac), senão a sigla do país do layout (BR, PT, ES…)
std::wstring InputBadge(HKL hkl) {
    WORD layout = HIWORD((ULONG_PTR)hkl), lang = LOWORD((ULONG_PTR)hkl);
    if ((layout & 0xF000) == 0xF000) layout = lang;  // layout alternativo: usa o idioma
    if (layout == 0x0409) return L"A";
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(layout, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) ||
        LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0)) {
        std::wstring n = name;
        size_t d = n.find(L'-');
        std::wstring c = d != std::wstring::npos ? n.substr(d + 1, 2) : n.substr(0, 2);
        for (auto& ch : c) ch = towupper(ch);
        return c;
    }
    return L"?";
}
std::wstring LayoutName(HKL hkl) {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {}, disp[128] = {};
    LCIDToLocaleName(MAKELCID(LOWORD((ULONG_PTR)hkl), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0);
    GetLocaleInfoEx(name, LOCALE_SLOCALIZEDDISPLAYNAME, disp, 128);
    std::wstring r = disp[0] ? disp : name;
    if (HIWORD((ULONG_PTR)hkl) == 0x0416) r += L" (ABNT2)";
    return r;
}

IAudioEndpointVolume* Volume() {
    IMMDeviceEnumerator* en = nullptr;
    IAudioEndpointVolume* vol = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
            dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol);
            dev->Release();
        }
        en->Release();
    }
    return vol;
}

std::wstring ClockText() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t day[16] = {}, mon[16] = {};
    const wchar_t* loc = idioma::Locale();
    GetDateFormatEx(loc, 0, &st, L"ddd", day, 16, nullptr);
    GetDateFormatEx(loc, 0, &st, L"MMM", mon, 16, nullptr);
    std::wstring d = day, m = mon;
    if (idioma::Pt() && !d.empty() && d.back() != L'.') d += L'.';
    if (idioma::Pt() && !m.empty() && m.back() != L'.') m += L'.';
    wchar_t buf[64];
    if (liquid::Leopard()) {
        // Leopard: só o dia da semana e a hora ("Mon 6:45 PM" na captura do 10.5), no formato de hora do usuário
        wchar_t tm[32] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, tm, 32);
        std::wstring dd = day;
        if (!dd.empty() && dd.back() == L'.') dd.pop_back();
        if (!dd.empty()) dd[0] = towupper(dd[0]);
        swprintf_s(buf, L"%ls %ls", dd.c_str(), tm);
        return buf;
    }
    if (shell::Setting(L"relogio_curto", L"0") == L"1") swprintf_s(buf, L"%02d:%02d", st.wHour, st.wMinute);
    else if (idioma::En()) swprintf_s(buf, L"%ls %ls %d  %02d:%02d", d.c_str(), m.c_str(), st.wDay, st.wHour, st.wMinute);  // Tue Nov 10
    else if (idioma::Pt()) swprintf_s(buf, L"%ls %d de %ls  %02d:%02d", d.c_str(), st.wDay, m.c_str(), st.wHour, st.wMinute);
    else swprintf_s(buf, L"%ls  %02d:%02d", idioma::Date(st, idioma::D_WEEKDAY_SHORT).c_str(), st.wHour, st.wMinute);  // ven. 20 nov., 11月20日(金)
    return buf;
}

// ---- ícones de status: HICON → BGRA pré-multiplicado (desenhado sobre preto e sobre branco)
struct IconCache { HICON icon; UINT px; std::vector<BYTE> bgra; };
std::vector<IconCache> g_iconCache;

const std::vector<BYTE>& IconPixels(HICON ic, UINT px) {
    for (auto& c : g_iconCache) if (c.icon == ic && c.px == px) return c.bgra;
    if (g_iconCache.size() > 64) g_iconCache.clear();
    // desenhado 4× maior e reduzido pela média de cada bloco 4×4 (anti-aliasing): em 1080p (17 px) o DrawIconEx
    // esticava o ícone de 16/32 px pelo vizinho mais próximo e a borda saía serrilhada
    const UINT SS = 4, R = px * SS;
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)R, -(LONG)R, 1, 32, BI_RGB };
    void *pb, *pw;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &pb, nullptr, 0);
    HBITMAP w = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &pw, nullptr, 0);
    IconCache c{ ic, px, std::vector<BYTE>(px * px * 4) };
    if (!b || !w || !pb || !pw) {
        if (b) DeleteObject(b);
        if (w) DeleteObject(w);
        DeleteDC(dc);
        g_iconCache.push_back(std::move(c));
        return g_iconCache.back().bgra;
    }
    memset(pw, 0xFF, R * R * 4);
    HGDIOBJ old = SelectObject(dc, b); DrawIconEx(dc, 0, 0, ic, R, R, 0, nullptr, DI_NORMAL);
    SelectObject(dc, w); DrawIconEx(dc, 0, 0, ic, R, R, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    auto* B = (BYTE*)pb; auto* W = (BYTE*)pw;
    for (UINT y = 0; y < px; y++)
        for (UINT x = 0; x < px; x++) {
            UINT sb = 0, sg = 0, sr = 0, sa = 0;
            for (UINT dy = 0; dy < SS; dy++)
                for (UINT dx = 0; dx < SS; dx++) {
                    size_t i = ((size_t)(y * SS + dy) * R + (x * SS + dx)) * 4;
                    int a = 255 - std::clamp((W[i + 1] - B[i + 1]), 0, 255);  // alfa = 1 - (branco - preto)
                    sb += B[i + 0]; sg += B[i + 1]; sr += B[i + 2]; sa += (UINT)a;
                }
            const UINT n = SS * SS;
            size_t o = ((size_t)y * px + x) * 4;
            c.bgra[o + 3] = (BYTE)((sa + n / 2) / n);
            c.bgra[o + 0] = (BYTE)std::min<UINT>((sb + n / 2) / n, c.bgra[o + 3]);
            c.bgra[o + 1] = (BYTE)std::min<UINT>((sg + n / 2) / n, c.bgra[o + 3]);
            c.bgra[o + 2] = (BYTE)std::min<UINT>((sr + n / 2) / n, c.bgra[o + 3]);
        }
    DeleteObject(b); DeleteObject(w); DeleteDC(dc);
    g_iconCache.push_back(std::move(c));
    return g_iconCache.back().bgra;
}

// ---- desenho
void DrawLogo(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, float cx, float cy) {
    // logo próprio (não usamos a marca da Apple): quatro quadradinhos arredondados
    float s = 5.4f, g = 1.6f;
    for (int i = 0; i < 4; i++) {
        float x = cx - s - g / 2 + (i % 2) * (s + g), y = cy - s - g / 2 + (i / 2) * (s + g);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + s, y + s), 1.5f, 1.5f), br);
    }
}

void DrawCC(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, float cx, float cy) {
    // Central de Controle: duas chavinhas
    for (int k = 0; k < 2; k++) {
        float y = cy - 3.6f + k * 7.2f;
        D2D1_ROUNDED_RECT rr{ D2D1::RectF(cx - 7.5f, y - 2.8f, cx + 7.5f, y + 2.8f), 2.8f, 2.8f };
        rt->DrawRoundedRectangle(rr, br, 1.2f);
        float kx = k == 0 ? cx - 4.7f : cx + 4.7f;
        rt->FillEllipse(D2D1::Ellipse({ kx, y }, 2.0f, 2.0f), br);
    }
}

// Ajustes > Transparência e Efeitos > "Mostrar fundo da barra de menus" (desligado: como no Tahoe)
// No Tahoe (Liquid Glass) a barra de menus não tem fundo nenhum, como no macOS 26: só o papel de parede atrás.
bool BarBackground() {
    if (backdrop::Current().reduce || liquid::Leopard()) return true;  // Leopard: a barra translúcida clara sempre tem fundo
    if (backdrop::Liquid()) return false;
    return shell::Setting(L"barra_fundo", L"1") != L"0";
}

void BarGlass(HWND bar) {
    if (!bar) return;
    // Leopard: o degradê branco translúcido vem do Render. Translúcida (padrão): transparência de verdade, SEM desfoque
    // (o 10.5 deixava o papel de parede aparecer nítido através da barra); opaca: acrílico cheio por baixo
    const bool leoClear = liquid::Leopard() && shell::Setting(L"barra_translucida", L"1") == L"1";
    if (liquid::Leopard() && !leoClear) { ui::Acrylic(bar, 0xFFE6E6E6u); return; }
    if (!leoClear)
    if (BarBackground()) { ui::Acrylic(bar, ui::Dark() ? 0x40141414u : 0x55F4F4F4u); return; }  // vidro bem claro: o papel de parede aparece, como no Mac
    // sem fundo: o DWM só respeita o alfa do que a barra desenha (nada de desfoque)
    struct ACCENT_POLICY { int state, flags; DWORD gradient; int anim; };
    struct WCA_DATA { int attr; PVOID data; SIZE_T size; };
    using Fn = BOOL(WINAPI*)(HWND, WCA_DATA*);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!fn) return;
    ACCENT_POLICY ap{ 2 /*ACCENT_ENABLE_TRANSPARENTGRADIENT*/, 2, 0x00000000u, 0 };
    WCA_DATA d{ 19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap) };
    fn(bar, &d);
}

// Barra sem fundo (Tahoe): o texto e os ícones seguem a claridade do que está atrás deles, como no Mac
// (branco sobre papel de parede escuro, preto sobre claro), não o tema do sistema. A barra fica sempre
// em cima do papel de parede (a área de trabalho começa abaixo dela): mede o topo da imagem.
float g_topR = 0.5f, g_topG = 0.5f, g_topB = 0.5f;  // cor média do topo do papel de parede (vibrancy do Tahoe)
bool WallpaperTopDarkFile(bool fallback);
// a cor do que está atrás da barra: a faixa que a mesa desenhou (muda na hora com o papel de parede); sem a mesa, o
// arquivo do papel de parede
bool WallpaperTopDark(bool fallback) {
    float r, g, b;
    if (!mesa::TopColor(menubar::Height(), r, g, b)) return WallpaperTopDarkFile(fallback);
    g_topR = r; g_topG = g; g_topB = b;
    double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    static int adapt = -1;  // -1 ainda não decidido, 0 texto claro, 1 texto escuro
    static double lastLog = -1;
    bool dark;
    if (liquid::On()) {  // histerese 0,40/0,60 (guia, seção 2.1)
        if (luma > 0.60) adapt = 1; else if (luma < 0.40) adapt = 0; else if (adapt < 0) adapt = luma >= 0.5 ? 1 : 0;
        dark = adapt == 0;
    } else dark = luma < 0.72;
    if (std::fabs(luma - lastLog) > 0.05) { lastLog = luma; Log(L"barra de menus: atrás dela claridade %.2f -> texto %ls", luma, dark ? L"branco" : L"preto"); }
    return dark;
}
bool WallpaperTopDarkFile(bool fallback) {
    static std::wstring lastKey;
    static bool lastDark = true;
    // o papel de parede que a mesa desenha (do registro); sem ele, o do Windows
    wchar_t path[MAX_PATH] = {};
    DWORD cb = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallPaper", RRF_RT_REG_SZ, nullptr, path, &cb) != ERROR_SUCCESS || !path[0]
        || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!path[0] || !GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        static bool warned;
        if (!warned) { warned = true; Log(L"barra de menus: papel de parede não encontrado (%ls)", path); }
        return fallback;
    }
    std::wstring key = std::wstring(path) + L"|" + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime);
    if (key == lastKey) return lastDark;
    lastKey = key;
    lastDark = fallback;
    IWICImagingFactory* wic = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) { Log(L"barra de menus: WIC indisponível (0x%08X)", (unsigned)hr); return fallback; }
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    const UINT W = 96, H = 54;
    int step = 0;
    if (++step, SUCCEEDED(hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && (++step, SUCCEEDED(hr = dec->GetFrame(0, &fr))) &&
        (++step, SUCCEEDED(hr = wic->CreateBitmapScaler(&sc))) && (++step, SUCCEEDED(hr = sc->Initialize(fr, W, H, WICBitmapInterpolationModeFant))) &&
        (++step, SUCCEEDED(hr = wic->CreateFormatConverter(&cv))) && (++step, SUCCEEDED(hr = cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))) {
        std::vector<BYTE> px(W * 3 * 4);  // as 3 primeiras linhas (~5% de cima, onde fica a barra)
        WICRect top{ 0, 0, (INT)W, 3 };
        if (SUCCEEDED(hr = cv->CopyPixels(&top, W * 4, (UINT)px.size(), px.data()))) {
            double sum = 0, sr = 0, sg = 0, sb = 0;
            for (UINT i = 0; i < W * 3; i++) {
                sum += (0.0722 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.2126 * px[i * 4 + 2]) / 255.0;
                sb += px[i * 4] / 255.0; sg += px[i * 4 + 1] / 255.0; sr += px[i * 4 + 2] / 255.0;
            }
            double luma = sum / (W * 3);
            g_topR = (float)(sr / (W * 3)); g_topG = (float)(sg / (W * 3)); g_topB = (float)(sb / (W * 3));
            // Tahoe: adaptação com histerese do vidro do macOS 26 (glassLumaHysteresisRange, guia seção 2.1): acima de
            // 0,60 vira texto escuro, abaixo de 0,40 texto claro, entre os dois fica como estava. Sequoia: limiar 0,72.
            static int adapt = -1;  // -1 ainda não decidido, 0 texto claro, 1 texto escuro
            if (liquid::On()) {
                if (luma > 0.60) adapt = 1; else if (luma < 0.40) adapt = 0; else if (adapt < 0) adapt = luma >= 0.5 ? 1 : 0;
                lastDark = adapt == 0;
            } else lastDark = luma < 0.72;  // o Mac só usa texto escuro sobre fundo bem claro
            Log(L"barra de menus: topo do papel de parede com claridade %.2f -> texto %ls", luma, lastDark ? L"branco" : L"preto");
        }
    }
    else Log(L"barra de menus: não deu para ler o papel de parede (etapa %d, 0x%08X)", step, (unsigned)hr);
    if (cv) cv->Release(); if (sc) sc->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    wic->Release();
    return lastDark;
}

void Render() {
    if (!g_bar) return;
    HWND bar = Cur();
    g_s.hwnd = bar;
    RECT rc; GetClientRect(bar, &rc);
    float sc = ui::Scale(bar);
    auto* rt = ui::Begin(g_s, rc.right, rc.bottom, sc);
    if (!rt) return;
    float W = rc.right / sc, H = rc.bottom / sc;
    bool dark = ui::Dark();
    // sem fundo, ou no Tahoe (o vidro da barra é tão leve que o papel de parede aparece quase inteiro):
    // a cor do texto e dos ícones segue o papel de parede atrás
    const bool bgOn0 = BarBackground();
    if (!bgOn0 || liquid::On()) dark = WallpaperTopDark(dark);
    const bool leo = liquid::Leopard();
    // Leopard: Lucida Grande 14 px no Mac; no Windows, a Lucida Sans Unicode (o mesmo desenho de origem)
    static IDWriteTextFormat* tfLeo = ui::FormatFace(L"Lucida Sans Unicode", 14, DWRITE_FONT_WEIGHT_NORMAL);
    static IDWriteTextFormat* tfLeoB = ui::FormatFace(L"Lucida Sans Unicode", 14, DWRITE_FONT_WEIGHT_BOLD);
    IDWriteTextFormat* tfN = leo ? tfLeo : g_tf;
    IDWriteTextFormat* tfB = leo ? tfLeoB : g_tfBold;
    if (leo) dark = false;  // Leopard: a barra é sempre clara, com texto preto (o 10.5 não tinha modo escuro)
    // Menubar Image do macOS 26 (guia, seção 31: catálogos Aqua/DarkAqua): branco 90 % no escuro, preto 85 % no claro
    D2D1_COLOR_F fgc = dark ? D2D1::ColorF(1, 1, 1, liquid::On() ? 0.90f : 0.92f) : D2D1::ColorF(0, 0, 0, 0.85f);
    // Liquid Glass sem fundo: só branco ou preto, mais firmes. Antes puxava a cor do papel de parede saturada 3×
    // (a "vibrancy" do Mac) e em papel de parede azul o texto e os ícones ficavam ciano.
    if (liquid::On() && !bgOn0) fgc = dark ? D2D1::ColorF(1, 1, 1, 0.96f) : D2D1::ColorF(0, 0, 0, 0.90f);
    ID2D1SolidColorBrush* br = nullptr;
    bool liquid = backdrop::Liquid(), bgOn = BarBackground();
    D2D1_COLOR_F veil = dark ? D2D1::ColorF(0.06f, 0.06f, 0.07f, 0.18f) : D2D1::ColorF(1, 1, 1, 0.26f);
    if (liquid) veil.a = backdrop::Current().tinted ? veil.a : veil.a * 0.35f;
    if (!bgOn || leo) veil.a = 0;  // sem fundo (Tahoe): só o papel de parede atrás; Leopard: o degradê do Render é o fundo todo
    rt->CreateSolidColorBrush(veil, &br);
    rt->FillRectangle(D2D1::RectF(0, 0, W, H), br);  // tinta leve por cima do acrílico
    if (leo) {
        // Leopard: a barra de menus translúcida do 10.5 — branco leitoso que deixa o papel de parede passar um pouco,
        // um tom mais cinza embaixo, o fio de luz no alto e a linha escura embaixo (a "sombra" da barra)
        float px = 1 / sc;
        // medido numa captura do 10.5 a 1x: 248 no alto -> 204 embaixo, a linha de baixo quase preta
        // Aparência > "Barra de menus translúcida" desligada (o 10.5.2 trouxe a opção): opaca
        const float op = shell::Setting(L"barra_translucida", L"1") == L"1" ? 1.f : 1.12f;
        // translúcida: ~72 % de branco (o papel de parede passa nítido, como nas capturas do 10.5); opaca: cheia
        const float a0 = op > 1 ? 1.f : 0.74f, a1 = op > 1 ? 1.f : 0.70f;
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(248 / 255.f, 248 / 255.f, 248 / 255.f, a0) }, { 1, D2D1::ColorF(214 / 255.f, 214 / 255.f, 214 / 255.f, a1) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, H }), gs, &lb)))
            rt->FillRectangle(D2D1::RectF(0, 0, W, H), lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.75f));
        rt->FillRectangle(D2D1::RectF(0, 0, W, px), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.62f));
        rt->FillRectangle(D2D1::RectF(0, H - px, W, H), br);
    }
    if (liquid && bgOn) {  // Liquid Glass: sem degradê branco; só um fio fraco embaixo, como no Tahoe
        float px = 1 / sc;
        br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.08f : 0.18f));
        rt->FillRectangle(D2D1::RectF(0, H - px, W, H), br);
    }
    g_hits.clear();
    bool litNow = false;  // Leopard: o item aceso fica com o texto branco sobre o azul
    auto highlight = [&](D2D1_RECT_F r) {
        if (leo) {  // Leopard: o destaque azul do Aqua de ponta a ponta na altura da barra, sem cantos
            litNow = true;
            D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0.39f, 0.56f, 0.95f, 1) }, { 1, D2D1::ColorF(0.13f, 0.35f, 0.86f, 1) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, H }), gs, &lb)))
                rt->FillRectangle(D2D1::RectF(r.left, 0, r.right, H - 1 / sc), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            return;
        }
        if (liquid::On()) {  // Tahoe (vídeo do macOS 26): o item aberto ganha uma cápsula de vidro clara
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.24f) : D2D1::ColorF(1, 1, 1, 0.55f));
            float rad = (H - 4) / 2;
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, 2, r.right - 1, H - 2), rad, rad), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.18f : 0.45f));  // o fio de luz da borda
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1.5f, 2.5f, r.right - 1.5f, H - 2.5f), rad - 0.5f, rad - 0.5f), br, 0.8f);
            return;
        }
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.20f) : D2D1::ColorF(0, 0, 0, 0.10f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, 2, r.right, H - 2), 4, 4), br);
    };
    auto add = [&](D2D1_RECT_F r, Kind k, int i) {
        litNow = false;
        g_hits.push_back({ r, k, i });
        bool lit = (k == K_SOUND && (som::IsOpen() || som::HudLit()))  // menu Som aberto / aviso de volume do Tahoe
                || (k == K_CC && central::IsOpen()) || (k == K_CLOCK && (notificacoes::IsOpen() || (liquid::On() && notificacoes::BannerVisible()))) || (k == K_SPOT && spotlight::IsOpen());
        if (lit || ((int)g_hits.size() - 1 == g_open && bar == g_openBar)) highlight(r);
    };

    // esquerda (Tahoe, guia seção 35: o menu Apple fica em x 10, com 34 de largura)
    float x = liquid::On() ? 10.f : 8.f;
    add(D2D1::RectF(x, 0, x + 34, H), K_LOGO, 0);
    br->SetColor(litNow ? D2D1::ColorF(1, 1, 1) : fgc);
    if (leo && !litNow) {
        // Leopard: a maçã grafite brilhante do 10.5 (a azul era a do Tiger): cinza claro em cima, quase preto embaixo;
        // aberta, fica branca
        D2D1_GRADIENT_STOP st[3] = { { 0, D2D1::ColorF(0.62f, 0.62f, 0.63f, 1) }, { 0.45f, D2D1::ColorF(0.36f, 0.36f, 0.37f, 1) }, { 1, D2D1::ColorF(0.16f, 0.16f, 0.17f, 1) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        bool ok = SUCCEEDED(rt->CreateGradientStopCollection(st, 3, &gs)) &&
                  SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, H / 2 - 7.5f }, { 0, H / 2 + 6.5f }), gs, &lb));
        if (!ok || !simbolo::Draw(rt, L"apple.logo", D2D1::RectF(x + 10, H / 2 - 7.5f, x + 24, H / 2 + 6.5f), lb)) {
            br->SetColor(D2D1::ColorF(0.30f, 0.30f, 0.31f, 1));
            DrawLogo(rt, br, x + 17, H / 2);
        }
        if (lb) lb->Release();
        if (gs) gs->Release();
    } else
    if (!simbolo::Draw(rt, L"apple.logo", D2D1::RectF(x + 10, H / 2 - 7.5f, x + 24, H / 2 + 6.5f), (br->SetColor(litNow ? D2D1::ColorF(1, 1, 1) : fgc), br)))  // a maçã do SF Symbols
        DrawLogo(rt, br, x + 17, H / 2);
    x += 34;
    {
        float w = ui::TextWidth(tfB, g_appName) + 20;
        add(D2D1::RectF(x, 0, x + w, H), K_APP, 0);
        br->SetColor(litNow ? D2D1::ColorF(1, 1, 1) : fgc);
        rt->DrawTextW(g_appName.c_str(), (UINT32)g_appName.size(), tfB, D2D1::RectF(x + 10, 0, x + w, H), br);
        x += w;
    }
    float leftEnd = x;

    // direita (da borda para dentro), na ordem do Mac: relógio, Central de Controle, Spotlight, fonte de
    // entrada, Wi-Fi, bateria, som, Bluetooth, ícones fixados dos apps e a setinha dos guardados.
    // Cada item é o símbolo com 10 pt de cada lado (no Mac fica um a cada ~38 pt).
    float r = W - 10;
    auto slot = [&](float iconW, Kind k, int i) {  // reserva o espaço e devolve a caixa do símbolo
        float w = iconW + 2 * PAD;
        add(D2D1::RectF(r - w, 0, r, H), k, i);
        D2D1_RECT_F box = D2D1::RectF(r - w + PAD, 0, r - PAD, H);
        r -= w;
        br->SetColor(litNow ? D2D1::ColorF(1, 1, 1) : fgc);
        return box;
    };
    auto sym = [&](const wchar_t* name, D2D1_RECT_F box, float h) {
        return simbolo::Draw(rt, name, D2D1::RectF(box.left, H / 2 - h / 2, box.right, H / 2 + h / 2), br);
    };
    // Leopard (captura do 10.5): a lupa do Spotlight é o último item, na ponta direita, e o relógio vem antes dela;
    // (a Central de Controle continua, à esquerda do relógio: o usuário quer o acesso a ela)
    if (leo && shell::Setting(L"barra_spotlight", L"1") == L"1") {
        auto bx = slot(15, K_SPOT, 0);
        if (!sym(L"magnifyingglass", bx, 15)) rt->DrawTextW(L"", 1, g_tfIcon, bx, br);
    }
    {
        std::wstring t = ClockText();
        float w = ui::TextWidth(tfN, t) + 2 * PAD;
        add(D2D1::RectF(r - w, 0, r, H), K_CLOCK, 0);
        br->SetColor(litNow ? D2D1::ColorF(1, 1, 1) : fgc);
        rt->DrawTextW(t.c_str(), (UINT32)t.size(), tfN, D2D1::RectF(r - w + PAD, 0, r, H), br);
        r -= w;
    }
    { auto bx = slot(18, K_CC, 0); if (!sym(L"switch.2", bx, 14)) DrawCC(rt, br, (bx.left + bx.right) / 2, H / 2); }
    if (!leo && shell::Setting(L"barra_spotlight", L"1") == L"1") {
        auto bx = slot(15, K_SPOT, 0);
        if (!sym(L"magnifyingglass", bx, 15)) rt->DrawTextW(L"", 1, g_tfIcon, bx, br);
    }
    if (shell::Setting(L"barra_teclado", L"1") == L"1") {  // fonte de entrada: a sigla num quadradinho (como o "A" do ABC)
        std::wstring t = InputBadge(g_st.hkl);
        float tw = std::max(15.f, ui::TextWidth(g_tfBadge, t) + 7);
        auto bx = slot(tw, K_INPUT, 0);
        D2D1_RECT_F k = D2D1::RectF(bx.left, H / 2 - 7.5f, bx.right, H / 2 + 7.5f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(k, 3.5f, 3.5f), br);
        br->SetColor(dark ? D2D1::ColorF(0.13f, 0.13f, 0.14f) : D2D1::ColorF(0.96f, 0.96f, 0.96f));
        rt->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfBadge, D2D1::RectF(k.left, k.top - 0.5f, k.right, k.bottom - 0.5f), br);
    }
    // como no Mac: só quem tem Wi-Fi; no cabo, o Wi-Fi desconectado não aparece
    if (g_st.wlan && (g_st.wifiOn || !g_st.wired) && shell::Setting(L"barra_wifi", L"1") == L"1") {
        auto bx = slot(18, K_NET, 0);
        if (!sym(g_st.wifiOn ? L"wifi" : L"wifi.slash", bx, 14)) rt->DrawTextW(L"", 1, g_tfIcon, bx, br);
    }
    if (g_st.batt && shell::Setting(L"barra_bateria", L"1") == L"1") {
        const wchar_t* name = g_st.charging ? L"battery.100percent.bolt" : g_st.pct > 87 ? L"battery.100percent" : g_st.pct > 62 ? L"battery.75percent"
                            : g_st.pct > 37 ? L"battery.50percent" : g_st.pct > 12 ? L"battery.25percent" : L"battery.0percent";
        if (liquid::Golden()) {
            // macOS 27 (vídeo do MacVince, 2:44): a porcentagem DENTRO do ícone — corpo cheio de 25 × 12 com canto 4,
            // o número vazado (e o raio quando carrega) e a ponta à direita
            auto bx = slot(29, K_BATT, 0);
            float cy = H / 2;
            D2D1_RECT_F body = D2D1::RectF(bx.left, cy - 6, bx.left + 25, cy + 6);
            rt->FillRoundedRectangle(D2D1::RoundedRect(body, 4, 4), br);
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.right + 1, cy - 2.5f, body.right + 2.8f, cy + 2.5f), 1, 1), br);
            static IDWriteTextFormat* tfB = ui::Format(9.5f, DWRITE_FONT_WEIGHT_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            wchar_t num[8]; swprintf_s(num, L"%d", std::clamp(g_st.pct, 0, 100));
            br->SetColor(dark ? D2D1::ColorF(0.15f, 0.13f, 0.12f, 0.92f) : D2D1::ColorF(1, 1, 1, 0.95f));
            D2D1_RECT_F tr = D2D1::RectF(body.left, body.top - 0.5f, body.right - (g_st.charging ? 6.f : 0.f), body.bottom - 0.5f);
            if (tfB) { tfB->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); rt->DrawTextW(num, (UINT32)wcslen(num), tfB, tr, br); }
            if (g_st.charging) {  // raio
                float bx0 = body.right - 6.5f;
                ID2D1PathGeometry* g = nullptr;
                if (SUCCEEDED(ui::d2d->CreatePathGeometry(&g))) {
                    ID2D1GeometrySink* sk = nullptr; g->Open(&sk);
                    sk->BeginFigure({ bx0 + 2.6f, cy - 4.5f }, D2D1_FIGURE_BEGIN_FILLED);
                    D2D1_POINT_2F pts[] = { { bx0, cy + 0.6f }, { bx0 + 1.9f, cy + 0.6f }, { bx0 + 1.1f, cy + 4.5f }, { bx0 + 3.8f, cy - 0.8f }, { bx0 + 1.9f, cy - 0.8f } };
                    sk->AddLines(pts, 5); sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
                    rt->FillGeometry(g, br);
                    g->Release();
                }
            }
            br->SetColor(litNow ? D2D1::ColorF(1, 1, 1) : fgc);
        } else {
        auto bx = slot(27, K_BATT, 0);
        sym(name, bx, 13);
        }
    }
    // Som e Bluetooth: no Mac ficam fora da barra por padrão (Ajustes > Central de Controle liga)
    if (g_st.sound && shell::Setting(L"barra_som", L"0") == L"1") {
        const wchar_t* name = g_st.muted ? L"speaker.slash.fill" : g_st.phones ? L"headphones" : g_st.vol < 0.01f ? L"speaker.fill"
                            : g_st.vol < 0.34f ? L"speaker.wave.1.fill" : g_st.vol < 0.67f ? L"speaker.wave.2.fill" : L"speaker.wave.3.fill";
        auto bx = slot(g_st.phones ? 16 : 21, K_SOUND, 0);
        sym(name, bx, g_st.phones ? 16 : 14);
    }
    if (g_st.bt && shell::Setting(L"barra_bluetooth", L"0") == L"1") {  // o símbolo do Bluetooth do Mac (não está no SF Symbols público)
        auto bx = slot(9, K_BT, 0);
        float cx = (bx.left + bx.right) / 2, cy = H / 2, hh = 7.2f, ww = 3.9f;
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(ui::d2d->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* sk = nullptr; g->Open(&sk);
            sk->BeginFigure({ cx - ww, cy - hh * 0.48f }, D2D1_FIGURE_BEGIN_HOLLOW);
            D2D1_POINT_2F pts[] = { { cx + ww, cy + hh * 0.48f }, { cx, cy + hh }, { cx, cy - hh }, { cx + ww, cy - hh * 0.48f }, { cx - ww, cy + hh * 0.48f } };
            sk->AddLines(pts, 5); sk->EndFigure(D2D1_FIGURE_END_OPEN); sk->Close(); sk->Release();
            ID2D1StrokeStyle* st = nullptr;
            ui::d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &st);
            rt->DrawGeometry(g, br, 1.5f, st);
            if (st) st->Release();
            g->Release();
        }
    }
    UINT px = (UINT)std::lround(17 * sc);
    for (int i = (int)tray::icons.size() - 1; i >= 0; i--) {
        auto& t = tray::icons[i];
        if (t.hidden || !t.icon || !IsShown(t)) continue;
        if (r - 37 < leftEnd + 60) { g_overflow = true; break; }  // sem espaço: some, como no Mac (macOS 27: aparece a »)
        auto bx = slot(17, K_TRAY, i);
        std::vector<BYTE> mono = IconPixels(t.icon, px);
        // como os ícones-modelo do Mac: uma cor só, a mesma do texto da barra (branco no escuro, preto no claro);
        // o desenho de dentro (ícone de fundo cheio: Discord, WhatsApp) sai pela transparência, nunca por cinza
        // ou pela cor do original, que deixava o ícone azulado/acinzentado em cima do papel de parede.
        float lmin = 1, lmax = 0;
        for (size_t k = 0; k < mono.size(); k += 4) {
            float a = mono[k + 3] / 255.f;
            if (a < 0.5f) continue;
            float l = std::clamp((0.2126f * mono[k + 2] + 0.7152f * mono[k + 1] + 0.0722f * mono[k]) / 255.f / a, 0.f, 1.f);
            lmin = std::min(lmin, l); lmax = std::max(lmax, l);
        }
        bool detail = lmax - lmin > 0.25f;  // tem desenho de dentro (senão é só a silhueta)
        BYTE ink = dark ? 255 : 0;
        for (size_t k = 0; k < mono.size(); k += 4) {
            float a = mono[k + 3] / 255.f;
            if (a <= 0.004f) { mono[k] = mono[k + 1] = mono[k + 2] = mono[k + 3] = 0; continue; }
            float oa = a;
            if (detail) {
                float l = std::clamp((0.2126f * mono[k + 2] + 0.7152f * mono[k + 1] + 0.0722f * mono[k]) / 255.f / a, 0.f, 1.f);
                float t = std::clamp((l - lmin) / (lmax - lmin), 0.f, 1.f);  // 0 = parte mais escura do original
                oa = a * (dark ? 0.30f + 0.70f * t : 1.f - 0.70f * t);
            }
            oa *= dark ? 0.96f : 0.88f;
            BYTE c = (BYTE)std::lround(ink * oa);
            mono[k] = mono[k + 1] = mono[k + 2] = c;
            mono[k + 3] = (BYTE)std::lround(oa * 255);
        }
        if (ID2D1Bitmap* bmp = ui::Bitmap(rt, px, px, mono)) {
            // no pixel inteiro (1:1 com o bitmap): fora da grade o bitmap era reamostrado e borrava/serrilhava
            float cxp = (bx.left + bx.right) / 2 * sc, cyp = H / 2 * sc;
            float l0 = std::round(cxp - px / 2.f) / sc, t0 = std::round(cyp - px / 2.f) / sc;
            rt->DrawBitmap(bmp, D2D1::RectF(l0, t0, l0 + px / sc, t0 + px / sc), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            bmp->Release();
        }
    }
    bool anyHidden = false;
    for (auto& t : tray::icons) if (!t.hidden && t.icon && !IsShown(t)) anyHidden = true;
    if (liquid::Golden() && (g_overflow || anyHidden)) {
        // macOS 27 (vídeo, 2:16): quando os ícones não cabem, uma seta dupla » mostra os escondidos
        auto bx = slot(14, K_MORE, 0);
        float cx = (bx.left + bx.right) / 2, cy = H / 2;
        for (int k = 0; k < 2; k++) {
            float ox = cx - 3.5f + k * 5.5f;
            rt->DrawLine({ ox - 2.2f, cy - 4.5f }, { ox + 2.2f, cy }, br, 1.5f);
            rt->DrawLine({ ox + 2.2f, cy }, { ox - 2.2f, cy + 4.5f }, br, 1.5f);
        }
        anyHidden = false;  // já desenhada
    }
    g_overflow = false;
    // setinha dos ícones guardados: não existe no Mac, desligada por padrão (barra_seta=1 liga); a lista abre
    // também no botão direito da Central de Controle
    if (anyHidden && shell::Setting(L"barra_seta", L"0") == L"1") {  // setinha dos ícones guardados: o chevron do SF Symbols, apontando para onde eles abrem
        auto bx = slot(8, K_MORE, 0);
        if (!sym(L"chevron.left", bx, 12)) {
            float cx = (bx.left + bx.right) / 2, cy = H / 2;
            rt->DrawLine({ cx + 2, cy - 5 }, { cx - 2.5f, cy }, br, 1.6f);
            rt->DrawLine({ cx - 2.5f, cy }, { cx + 2, cy + 5 }, br, 1.6f);
        }
    }

    // menus do app (entre o nome e os ícones)
    for (int i = 0; i < (int)g_titles.size(); i++) {
        float w = ui::TextWidth(tfN, g_titles[i]) + 20;
        if (x + w > r - 8) break;
        add(D2D1::RectF(x, 0, x + w, H), K_MENU, i);
        br->SetColor(litNow ? D2D1::ColorF(1, 1, 1) : fgc);
        rt->DrawTextW(g_titles[i].c_str(), (UINT32)g_titles[i].size(), tfN, D2D1::RectF(x + 10, 0, x + w, H), br);
        x += w;
    }
    br->Release();
    ui::End(g_s, false);
}

// ---- menus
// Menus próprios, como os do app no Mac, para os apps sem menu Win32 (os itens mandam o atalho do Windows do
// app). Ids: atalhos de g_keyCmds; 0 = separador. No "Janela" os itens vêm antes da lista de janelas.
struct MenuDef { const wchar_t* title; std::vector<int> ids; };
const std::vector<MenuDef>* AppMenus(App* app) {
    static const std::vector<MenuDef> browser = {
        { L"Arquivo", { 1020, 1001, 5001, 5002, 0, 5003, 5004, 0, 5005, 5006, 5007, 0, 1005 } },
        { L"Editar", { 1010, 5070, 0, 1012, 1013, 1014, 1015, 0, 1016 } },
        { L"Visualizar", { 5010, 0, 5011, 5012, 5013, 0, 5014, 5015, 5016, 5017, 0, 5018, 5019 } },
        { L"Histórico", { 5020, 1040, 1041, 0, 5021, 5022 } },
        { L"Favoritos", { 5030, 0, 5031, 5032 } },
        { L"Aba", { 5040, 5041 } },
        { L"Janela", { 5050, 5051 } },
        { L"Ajuda", { 5060 } },
    };
    static const std::vector<MenuDef> discord = {
        { L"Editar", { 1010, 5070, 0, 1012, 1013, 1014, 1015 } },
        { L"Visualizar", { 5071, 5072, 0, 5015, 5016, 5017 } },
        { L"Janela", {} },
        { L"Ajuda", { 5073 } },
    };
    static const std::vector<MenuDef> whatsapp = {
        { L"Arquivo", { 5080, 5081, 0, 1003 } },
        { L"Editar", { 1010, 1011, 0, 1012, 1013, 1014, 1015, 0, 1016 } },
        { L"Visualizar", { 5015, 5016, 5017 } },
        { L"Janela", {} },
        { L"Ajuda", {} },
    };
    static const std::vector<MenuDef> electron = {  // Claude e afins: editar e zoom (o padrão do Electron)
        { L"Arquivo", { 1001, 0, 1003 } },
        { L"Editar", { 1010, 5070, 0, 1012, 1013, 1014, 1015 } },
        { L"Visualizar", { 5071, 0, 5015, 5016, 5017 } },
        { L"Janela", {} },
        { L"Ajuda", {} },
    };
    // o programa da janela ativa (o App do Chrome casa pelo AUMID e pode vir sem o .exe)
    static HWND s_fg; static std::wstring s_path;  // a barra pergunta a cada atualização: lido uma vez por janela
    std::wstring path = app ? app->exe : L"";
    DWORD pid = 0;
    if (g_fg && g_fg == s_fg) { if (!s_path.empty()) path = s_path; }
    else if (g_fg && GetWindowThreadProcessId(g_fg, &pid) && pid) {
        s_fg = g_fg; s_path.clear();
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, buf, &n)) path = s_path = buf;
            CloseHandle(p);
        }
    }
    if (path.empty()) return nullptr;
    std::wstring n = path.substr(path.find_last_of(L"\\/") + 1);
    for (auto& c : n) c = towlower(c);
    if (n == L"chrome.exe" || n == L"msedge.exe" || n == L"brave.exe" || n == L"vivaldi.exe") return &browser;
    if (n == L"discord.exe" || n == L"discordptb.exe" || n == L"discordcanary.exe") return &discord;
    if (n.find(L"whatsapp") != std::wstring::npos) return &whatsapp;
    if (n == L"claude.exe") return &electron;
    static const std::vector<MenuDef> spotify = {
        { L"Arquivo", { 5100, 0, 1003 } },
        { L"Editar", { 1010, 1011, 0, 1012, 1013, 1014, 1015, 0, 5108 } },
        { L"Visualizar", { 5015, 5016, 5017 } },
        { L"Reprodução", { 5101, 5102, 5103, 0, 5104, 5105, 0, 5106, 5107 } },
        { L"Janela", {} },
        { L"Ajuda", {} },
    };
    static const std::vector<MenuDef> vscode = {
        { L"Arquivo", { 5120, 5121, 0, 1002, 0, 1004, 5122, 0, 5123 } },
        { L"Editar", { 1010, 1011, 0, 1012, 1013, 1014, 0, 1016, 5124, 0, 5125 } },
        { L"Seleção", { 1015 } },
        { L"Visualizar", { 5126, 0, 5127, 5125, 5128, 5129, 0, 5134, 0, 5016, 5017, 5015 } },
        { L"Ir", { 5130, 5131 } },
        { L"Executar", { 5132, 5133 } },
        { L"Janela", {} },
        { L"Ajuda", {} },
    };
    if (n == L"spotify.exe") return &spotify;
    if (n == L"code.exe" || n == L"cursor.exe" || n == L"code - insiders.exe") return &vscode;
    return nullptr;
}

std::vector<std::wstring> GenericTitles(App* app) {
    if (!g_finderActive)
        if (auto* defs = AppMenus(app)) {
            std::vector<std::wstring> t;
            for (auto& d : *defs) t.push_back(Tr(d.title));
            return t;
        }
    if (g_finderActive) return { Tr(L"Arquivo"), Tr(L"Editar"), Tr(L"Visualizar"), Tr(L"Ir"), Tr(L"Janela"), Tr(L"Ajuda") };
    return { Tr(L"Arquivo"), Tr(L"Editar"), Tr(L"Janela"), Tr(L"Ajuda") };
}

std::vector<ctxmenu::Item> GenericMenu(const std::wstring& title, App* app) {
    std::vector<ctxmenu::Item> pre;  // Janela: itens do app antes dos comuns (minimizar, zoom, lista de janelas)
    if (auto* defs = g_finderActive ? nullptr : AppMenus(app))
        for (auto& d : *defs) {
            if (title != Tr(d.title)) continue;
            std::vector<ctxmenu::Item> v;
            for (int id : d.ids) v.push_back(id ? K(id) : Sep());
            if (title != Tr(L"Janela")) {
                if (v.empty()) v.push_back(I(Tr(L"Nenhum item"), 0, true));
                return v;
            }
            if (!v.empty()) { pre = v; pre.push_back(Sep()); }
            break;
        }
    if (title == Tr(L"Arquivo")) {
        if (g_finderActive) return { K(1001), K(1020), K(1021), Sep(), K(1002), K(1003), Sep(), K(1022), Sep(), K(1023) };
        return { K(1001), K(1002), Sep(), K(1003), K(1004), Sep(), K(1005) };
    }
    if (title == Tr(L"Editar")) return { K(1010), K(1011), Sep(), K(1012), K(1013), K(1014), K(1015), Sep(), K(1016) };
    if (title == Tr(L"Visualizar")) return { K(1030), K(1031), K(1032), K(1033), Sep(), K(1034) };
    if (title == Tr(L"Ir")) return { K(1040), K(1041), K(1042), Sep(), K(1043), Sep(), I(Tr(L"Recentes"), 1100), I(Tr(L"Documentos"), 1101), I(Tr(L"Mesa"), 1102), I(Tr(L"Downloads"), 1103), I(Tr(L"Início"), 1104), I(Tr(L"Este Computador"), 1105), I(Tr(L"Aplicativos"), 1106) };
    if (title == Tr(L"Janela")) {
        std::vector<ctxmenu::Item> v{ I(Tr(L"Minimizar"), 1200), I(Tr(L"Zoom"), 1201), Sep(), I(Tr(L"Trazer Tudo para a Frente"), 1202) };
        v.insert(v.begin(), pre.begin(), pre.end());
        if (app && !app->wins.empty()) {
            v.push_back(Sep());
            for (int k = 0; k < (int)app->wins.size() && k < 20; k++) {
                auto it = I(app->wins[k].title, 3000 + k);
                it.checked = app->wins[k].hwnd == g_fg;
                v.push_back(it);
            }
        }
        return v;
    }
    if (title == Tr(L"Ajuda")) return { K(1050) };
    return {};
}

std::vector<ctxmenu::Item> LogoMenu() {
    std::vector<ctxmenu::Item> recent;
    {  // Itens Recentes: os atalhos mais novos de shell:Recent
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &p))) {
            std::wstring dir = p; CoTaskMemFree(p);
            std::vector<std::pair<ULONGLONG, std::wstring>> files;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do files.push_back({ ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, dir + L"\\" + fd.cFileName });
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });
            static std::vector<std::wstring> s_recent;
            s_recent.clear();
            for (size_t k = 0; k < files.size() && k < 10; k++) {
                std::wstring n = files[k].second.substr(files[k].second.find_last_of(L'\\') + 1);
                n.resize(n.size() - 4);
                s_recent.push_back(files[k].second);
                recent.push_back(I(n, 4000 + (int)k));
            }
            if (recent.empty()) recent.push_back(I(Tr(L"Nenhum item"), 0, true));
        }
    }
    wchar_t user[128] = L""; DWORD n = 128; GetUserNameW(user, &n);
    ctxmenu::Item rec = I(Tr(L"Itens Recentes"), 0); rec.sub = recent;
    return { I(Tr(L"Sobre Este Mac"), 100), Sep(), I(Tr(L"Ajustes do Sistema…"), 101), I(Tr(L"Microsoft Store…"), 102), Sep(),
             rec, Sep(), I(Tr(L"Forçar Encerrar…"), 103), Sep(), I(Tr(L"Repousar"), 104), I(Tr(L"Reiniciar…"), 105), I(Tr(L"Desligar…"), 106), Sep(),
             I(Tr(L"Bloquear Tela"), 107), I(Tr(L"Encerrar Sessão de ") + std::wstring(user) + L"…", 108), Sep(), I(Tr(L"Sair do Shell (voltar à barra do Windows)"), 109) };
}

std::vector<ctxmenu::Item> AppMenu(App* a) {
    std::wstring n = a ? a->name : g_appName;
    return { I(Tr(L"Sobre ") + n, 200), Sep(), K(1051), Sep(), I(Tr(L"Ocultar ") + n, 201), I(Tr(L"Ocultar Outros"), 202), I(Tr(L"Mostrar Tudo"), 203), Sep(), I(Tr(L"Encerrar ") + n, 204) };
}


void Shutdown(UINT how) {
    HANDLE tok;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{ 1 };
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    ExitWindowsEx(how, SHTDN_REASON_FLAG_PLANNED);
}

void Open(const wchar_t* what, const wchar_t* args = nullptr) {
    if (!wcsncmp(what, L"ms-settings:", 12)) { shell::OpenSettings(what); return; }  // o nosso Ajustes do Sistema
    ShellExecuteW(nullptr, L"open", what, args, nullptr, SW_SHOWNORMAL);
}

void OpenFinder(const std::wstring& args) {
    if (shell::nativo) { shell::AbrirFinder(args); return; }
    for (auto& a : apps::list) if (a.finder) { ShellExecuteW(nullptr, L"open", a.launch.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL); return; }
}

void Run(int id, App* app) {
    for (auto& k : g_keyCmds) if (k.id == id) {  // atalho de teclado para o app ativo
        if (g_fg) SetForegroundWindow(g_fg);
        Sleep(30);
        SendKeys(k.keys);
        return;
    }
    if (id >= 1000 && id < 1100) return;
    switch (id) {
    case 100: sobre::Show(); break;
    case 101: shell::OpenSettings(L""); break;
    case 102: Open(L"ms-windows-store:"); break;
    case 103: shell::OpenForceQuit(false); break;
    case 104: SetSuspendState(FALSE, FALSE, FALSE); break;
    // como no Mac: alerta com o ícone do computador, Cancelar | Reiniciar/Desligar (antes era a caixa do Windows)
    case 105: alertas::Confirm(Tr(L"Tem certeza de que deseja reiniciar o computador agora?"), L"", Tr(L"Reiniciar"), [] { Shutdown(EWX_REBOOT); }, L"pack:macstudio"); break;
    case 106: alertas::Confirm(Tr(L"Tem certeza de que deseja desligar o computador agora?"), L"", Tr(L"Desligar"), [] { Shutdown(EWX_POWEROFF); }, L"pack:macstudio"); break;
    case 107: LockWorkStation(); break;
    case 108: alertas::Confirm(Tr(L"Tem certeza de que deseja encerrar todos os aplicativos e sair agora?"), L"", Tr(L"Encerrar Sessão"), [] { ExitWindowsEx(EWX_LOGOFF, 0); }, L"pack:macstudio"); break;
    case 109: shell::Quit(L"menu do logo"); break;
    case 200: if (app && !app->exe.empty()) ShellExecuteW(nullptr, L"properties", app->exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case 201: if (app) apps::Hide(*app); break;
    case 202: for (auto& a : apps::list) if (&a != app) apps::Hide(a); break;
    case 203: for (auto& a : apps::list) for (auto& w : a.wins) if (w.minimized) ShowWindow(w.hwnd, SW_SHOWNOACTIVATE); break;
    case 204: if (app) apps::Quit(*app); break;
    case 1100: OpenFinder(L"shell:Recent"); break;
    case 1101: OpenFinder(L"shell:Personal"); break;
    case 1102: OpenFinder(L"shell:Desktop"); break;
    case 1103: OpenFinder(L"shell:Downloads"); break;
    case 1104: OpenFinder(L"shell:Profile"); break;
    case 1105: OpenFinder(L"shell:MyComputerFolder"); break;
    case 1106: OpenFinder(L"shell:Common Programs"); break;
    case 1200: if (g_fg) ShowWindow(g_fg, SW_MINIMIZE); break;
    case 1201: if (g_fg) ShowWindow(g_fg, IsZoomed(g_fg) ? SW_RESTORE : SW_MAXIMIZE); break;
    case 1202: if (app) apps::Activate(*app); break;
    }
    if (id >= 3000 && id < 3100 && app && id - 3000 < (int)app->wins.size()) {
        HWND w = app->wins[id - 3000].hwnd;
        if (IsIconic(w)) shell::Restaurar(w);
        SetForegroundWindow(w);
    }
}

int g_closedHit = -1; DWORD g_closedHitAt;  // o título cujo menu acabou de fechar (clicar nele de novo só fecha)
void ShowAt(int hit, const std::vector<ctxmenu::Item>& items, std::function<void(int)> done) {
    g_open = hit;
    g_openBar = Cur();
    Render();
    float sc = ui::Scale(Cur());
    RECT wr; GetWindowRect(Cur(), &wr);
    POINT pt{ wr.left + (LONG)std::lround(g_hits[hit].r.left * sc), wr.bottom };
    // Tahoe (guia, seção 35): o menu abre 1 pt abaixo da barra e 4 pt à esquerda do título (o texto fica a 10 da caixa)
    if (liquid::On()) pt = { wr.left + (LONG)std::lround((g_hits[hit].r.left + 6) * sc), wr.bottom + (LONG)std::lround(sc) };
    SetForegroundWindow(g_owner);  // o menu fecha se a janela dona não estiver na frente
    int id = ctxmenu::ShowItems(g_owner, pt, items);
    g_closedHit = hit; g_closedHitAt = GetTickCount();
    g_open = -1;
    Render();
    if (g_fg && IsWindow(g_fg)) SetForegroundWindow(g_fg);
    if (id) done(id);
}

void Click(D2D1_POINT_2F p, bool right) {
    int hit = -1;
    for (int i = 0; i < (int)g_hits.size(); i++)
        if (p.x >= g_hits[i].r.left && p.x < g_hits[i].r.right) { hit = i; break; }
    if (hit < 0) return;
    if (!right && hit == g_closedHit && GetTickCount() - g_closedHitAt < 400) { g_closedHit = -1; return; }  // clique no mesmo título: só fechou
    g_closedHit = -1;
    Hit h = g_hits[hit];
    App* app = ActiveApp();
    float sc = ui::Scale(Cur());
    RECT wr; GetWindowRect(Cur(), &wr);
    POINT spt{ wr.left + (LONG)std::lround((h.r.left + h.r.right) / 2 * sc), wr.bottom };
    switch (h.kind) {
    case K_LOGO: ShowAt(hit, LogoMenu(), [&](int id) {
        if (id >= 4000 && id < 4010) {  // item recente
            PWSTR p = nullptr; SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &p);
            std::wstring dir = p; CoTaskMemFree(p);
            std::vector<std::pair<ULONGLONG, std::wstring>> files;
            WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) { do files.push_back({ ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, dir + L"\\" + fd.cFileName }); while (FindNextFileW(fh, &fd)); FindClose(fh); }
            std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });
            if (id - 4000 < (int)files.size()) Open(files[id - 4000].second.c_str());
        } else Run(id, app);
    }); break;
    case K_APP: ShowAt(hit, AppMenu(app), [&](int id) { Run(id, app); }); break;
    case K_MENU:
        if (g_hmenu && IsMenu(g_hmenu)) {
            HMENU sub = GetSubMenu(g_hmenu, h.index);
            if (!sub) break;
            SendMessageTimeoutW(g_fg, WM_INITMENU, (WPARAM)g_hmenu, 0, SMTO_ABORTIFHUNG, 200, nullptr);
            SendMessageTimeoutW(g_fg, WM_INITMENUPOPUP, (WPARAM)sub, h.index, SMTO_ABORTIFHUNG, 200, nullptr);
            HWND target = g_fg;
            ShowAt(hit, FromHmenu(sub, 0), [&](int id) { PostMessageW(target, WM_COMMAND, MAKEWPARAM(id, 0), 0); });
        } else ShowAt(hit, GenericMenu(g_titles[h.index], app), [&](int id) { Run(id, app); });
        break;
    case K_TRAY: tray::Click(h.index, right ? WM_RBUTTONUP : WM_LBUTTONUP, spt); break;
    case K_CC:
        if (!right) { central::Toggle(); break; }
        [[fallthrough]];  // botão direito: a lista dos ícones guardados (a setinha fica desligada, como no Mac)
    case K_MORE: {  // lista dos ícones guardados: abrir, menu do app, mostrar na barra
        std::vector<ctxmenu::Item> items;
        std::vector<int> idx;
        for (int i = 0; i < (int)tray::icons.size(); i++) {
            auto& t = tray::icons[i];
            if (t.hidden || !t.icon) continue;
            std::wstring name = TrayKey(t);
            if (name.empty()) name = Tr(L"(sem nome)");
            ctxmenu::Item it = I(name, 0);
            it.sub = { I(Tr(L"Abrir"), 1000 + i * 4), I(Tr(L"Menu do App"), 1001 + i * 4), Sep(), I(Tr(L"Mostrar na barra de menus"), 1002 + i * 4, false, IsShown(t)) };
            items.push_back(it);
        }
        ShowAt(hit, items, [&](int id) {
            int i = (id - 1000) / 4, op = (id - 1000) % 4;
            if (i < 0 || i >= (int)tray::icons.size()) return;
            if (op == 2) { SetShown(tray::icons[i], !IsShown(tray::icons[i])); return; }
            tray::Click(i, op == 1 ? WM_RBUTTONUP : WM_LBUTTONUP, spt);
        });
        break;
    }
    case K_SPOT: spotlight::Toggle(); break;
    case K_NET: {
        std::wstring ssid;
        bool wifi = WifiName(ssid);
        ShowAt(hit, { I(wifi ? L"Wi-Fi: " + ssid : L"Ethernet", 0, true), Sep(), I(Tr(L"Ajustes de Rede…"), 1) }, [](int) { Open(L"ms-settings:network"); });
        break;
    }
    case K_SOUND: {  // menu "Som" do Mac (som.cpp): volume, saídas e Ajustes de Som
        RECT ir{ wr.left + (LONG)std::lround(h.r.left * sc), wr.top, wr.left + (LONG)std::lround(h.r.right * sc), wr.bottom };
        som::Toggle(ir);
        break;
    }
    case K_BT: ShowAt(hit, { I(Tr(L"Bluetooth"), 0, true), Sep(), I(Tr(L"Ajustes de Bluetooth…"), 1) }, [](int) { shell::OpenSettings(L"bluetooth"); }); break;
    case K_BATT: {
        wchar_t b[64]; swprintf_s(b, Tr(L"Bateria: %d%%"), g_st.pct);
        ShowAt(hit, { I(b, 0, true), I(g_st.charging ? Tr(L"Fonte de Energia: Carregador") : Tr(L"Fonte de Energia: Bateria"), 0, true), Sep(), I(Tr(L"Ajustes de Bateria…"), 1) },
               [](int) { shell::OpenSettings(L"energia"); });
        break;
    }
    case K_INPUT: {  // as fontes de entrada instaladas; a escolhida vai para o app da frente
        HKL list[32]; int n = GetKeyboardLayoutList(32, list);
        std::vector<ctxmenu::Item> items;
        for (int i = 0; i < n; i++) items.push_back(I(InputBadge(list[i]) + L"   " + LayoutName(list[i]), 100 + i, false, list[i] == g_st.hkl));
        items.push_back(Sep());
        items.push_back(I(Tr(L"Abrir Ajustes de Teclado…"), 1));
        HWND target = g_fg;
        ShowAt(hit, items, [&](int id) {
            if (id == 1) { shell::OpenSettings(L"teclado"); return; }
            if (id >= 100 && id - 100 < n && target) PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)list[id - 100]);
            RefreshStatus(); Render();
        });
        break;
    }  // painel da Central de Controle (central.cpp)
    case K_CLOCK: {
        if (!right) { notificacoes::Toggle(); break; }  // clique: Central de Notificações; botão direito: data e ajustes
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t full[128] = {};
        GetDateFormatEx(idioma::Locale(), DATE_LONGDATE, &st, nullptr, full, 128, nullptr);
        ShowAt(hit, { I(full, 0, true), Sep(), I(Tr(L"Ajustes de Data e Hora…"), 1) }, [](int) { Open(L"ms-settings:dateandtime"); });
        break;
    }
    }
}

// Desenha todas as barras (a principal por último: os itens dela ficam em g_hits de quem chama sem g_cur)
void RenderAll() {
    HWND keep = g_cur;
    for (HWND b : g_extras) if (IsWindowVisible(b)) { g_cur = b; Render(); }
    g_cur = nullptr;
    Render();
    g_cur = keep;
}

// Redesenha só quando algo visível mudou (antes: a barra inteira a cada segundo)
void RenderIfChanged() {
    static std::wstring last;
    std::wstring sig = g_appName + L"|" + ClockText() + L"|" + std::to_wstring(ui::Dark()) + L"|" + std::to_wstring(g_titles.size());
    for (auto& t : g_titles) sig += t;
    wchar_t b[128];
    swprintf_s(b, L"|%d%d%d%d%d%d%d|%d|%d|%p|%d", g_st.wlan, g_st.wifiOn, g_st.batt, g_st.charging, g_st.bt, g_st.sound, g_st.muted,
               g_st.pct, (int)std::lround(g_st.vol * 3), (void*)g_st.hkl, g_st.phones);
    sig += b;
    for (auto& t : tray::icons) { swprintf_s(b, L"|%p%d%d", (void*)t.icon, t.hidden, IsShown(t)); sig += b; }
    RECT rc; GetClientRect(g_bar, &rc);
    sig += std::to_wstring(rc.right) + L"x" + std::to_wstring(rc.bottom);
    for (HWND b : g_extras) { GetClientRect(b, &rc); sig += L"|" + std::to_wstring(rc.right) + L"x" + std::to_wstring(IsWindowVisible(b)); }
    if (sig == last) return;
    last = sig;
    RenderAll();
}

const UINT WM_BARDISPLAYS = WM_APP + 41;  // monitores mudaram: refaz as barras (fora do WndProc que recebeu o aviso)

LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); HWND keep = g_cur; g_cur = h; Render(); g_cur = keep; return 0; }
    case WM_LBUTTONDOWN: case WM_RBUTTONUP: {
        float sc = ui::Scale(h);
        HWND keep = g_cur;
        g_cur = h;  // os menus abrem embaixo da barra clicada (no monitor dela)
        Click({ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }, m == WM_RBUTTONUP);
        g_cur = keep;
        return 0;
    }
    case WM_BARDISPLAYS: menubar::Displays(); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER: RefreshStatus(); menubar::Update(); return 0;
    case WM_SETTINGCHANGE:  // Windows trocou entre claro e escuro
        if (l && !lstrcmpiW((LPCWSTR)l, L"ImmersiveColorSet")) { menubar::ApplyTheme(); dock::ApplyTheme(); }
        break;
    case WM_DPICHANGED: case WM_DISPLAYCHANGE:
        // todas as barras recebem o aviso: só a principal refaz (depois, fora deste WndProc)
        PostMessageW(g_bar, WM_BARDISPLAYS, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

HWND menubar::Hwnd() { return g_bar; }
void menubar::SoundChanged(bool device) {
    if (!g_bar) return;
    if (device) g_audioStale = true;
    RefreshStatus();
    if (g_open < 0) RenderAll();
}
bool menubar::SoundIcon(RECT* out) {
    if (!g_bar || !IsWindowVisible(Cur())) return false;
    for (auto& h : g_hits)
        if (h.kind == K_SOUND) {
            float sc = ui::Scale(Cur());
            RECT wr; GetWindowRect(Cur(), &wr);
            *out = { wr.left + (LONG)std::lround(h.r.left * sc), wr.top, wr.left + (LONG)std::lround(h.r.right * sc), wr.bottom };
            return true;
        }
    return false;
}
void menubar::ApplyTheme() {
    if (!g_bar) return;
    {   // tema trocado (Sequoia 26 pt / Tahoe 30 pt): a barra muda de altura
        static float lastPt = 0;
        if (lastPt && lastPt != BAR_PT) menubar::Displays();
        lastPt = BAR_PT;
    }
    BarGlass(g_bar);
    InvalidateRect(g_bar, nullptr, FALSE);
    for (HWND b : g_extras) { BarGlass(b); InvalidateRect(b, nullptr, FALSE); }
}
static bool g_fsOn;
static HMONITOR g_fsMon;
void menubar::SetFullscreen(bool on, HMONITOR mon) {
    if (!g_bar) return;
    g_fsOn = on; g_fsMon = mon;
    std::vector<HWND> all{ g_bar };
    all.insert(all.end(), g_extras.begin(), g_extras.end());
    for (HWND b : all) {
        bool hide = on && (!mon || MonitorFromWindow(b, MONITOR_DEFAULTTONEAREST) == mon);  // só a barra do monitor em tela cheia
        if (hide == !IsWindowVisible(b)) continue;
        ShowWindow(b, hide ? SW_HIDE : SW_SHOWNOACTIVATE);
        if (!hide) SetWindowPos(b, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}
int menubar::Height() { return g_bar ? (int)std::lround(BAR_PT * ui::Scale(g_bar)) : (int)std::lround(BAR_PT * GetDpiForSystem() / 96.f); }
int menubar::HeightOn(HMONITOR mon) {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = GetDpiForSystem();
    return (int)std::lround(BAR_PT * dx / 96.f);
}

// Uma barra em cada monitor (como no Mac): a principal no monitor principal e uma igual em cada um dos outros.
void menubar::Displays() {
    if (!g_bar) return;
    struct Mon { HMONITOR hm; RECT r; };
    std::vector<Mon> others;
    Mon prim{};
    std::pair<std::vector<Mon>*, Mon*> ctx{ &others, &prim };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hm, &mi)) return TRUE;
        auto* v = (std::pair<std::vector<Mon>*, Mon*>*)p;
        if (mi.dwFlags & MONITORINFOF_PRIMARY) *v->second = { hm, mi.rcMonitor };
        else v->first->push_back({ hm, mi.rcMonitor });
        return TRUE;
    }, (LPARAM)&ctx);
    if (!prim.hm) { prim.hm = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(prim.hm, &mi); prim.r = mi.rcMonitor; }
    SetWindowPos(g_bar, HWND_TOPMOST, prim.r.left, prim.r.top, prim.r.right - prim.r.left, HeightOn(prim.hm), SWP_NOACTIVATE);
    // sobrando (monitor desligado): fecha
    while (g_extras.size() > others.size()) {
        HWND b = g_extras.back();
        g_extras.pop_back();
        apps::ours.erase(std::remove(apps::ours.begin(), apps::ours.end(), b), apps::ours.end());
        if (g_cur == b) g_cur = nullptr;
        if (g_openBar == b) g_openBar = nullptr;
        DestroyWindow(b);
        g_state.erase(b);
    }
    // faltando (monitor novo): cria
    while (g_extras.size() < others.size()) {
        HWND b = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ShellBarraDeMenus", L"Barra de menus", WS_POPUP,
                                 0, 0, 100, 20, nullptr, nullptr, (HINSTANCE)GetWindowLongPtrW(g_bar, GWLP_HINSTANCE), nullptr);
        if (!b) break;
        g_extras.push_back(b);
        apps::ours.push_back(b);
        g_state[b].s.hwnd = b;
        DWORD corner = 1;
        DwmSetWindowAttribute(b, 33, &corner, sizeof(corner));
        BarGlass(b);
    }
    for (size_t i = 0; i < g_extras.size(); i++) {
        const RECT& r = others[i].r;
        bool hide = g_fsOn && (!g_fsMon || g_fsMon == others[i].hm);
        SetWindowPos(g_extras[i], HWND_TOPMOST, r.left, r.top, r.right - r.left, HeightOn(others[i].hm),
                     SWP_NOACTIVATE | (hide ? 0 : SWP_SHOWWINDOW));
    }
    static size_t lastN = (size_t)-1;
    if (lastN != g_extras.size()) { lastN = g_extras.size(); Log(L"barra de menus: %zu monitor(es) a mais", lastN); }
    RenderAll();
    shell::ApplyWorkArea();
}

bool menubar::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = BarProc; wc.hInstance = hi; wc.lpszClassName = L"ShellBarraDeMenus";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW oc{}; oc.lpfnWndProc = DefWindowProcW; oc.hInstance = hi; oc.lpszClassName = L"ShellDonoDosMenus";
    RegisterClassW(&oc);
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, oc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, -32000, -32000, 1, 1, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(g_owner, 0, 0, LWA_ALPHA);
    g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Barra de menus", WS_POPUP,
                            0, 0, GetSystemMetrics(SM_CXSCREEN), menubar::Height(), nullptr, nullptr, hi, nullptr);
    if (!g_bar) return false;
    apps::ours.push_back(g_bar);
    apps::ours.push_back(g_owner);
    g_s.hwnd = g_bar;
    DWORD corner = 1;  // sem cantos arredondados
    DwmSetWindowAttribute(g_bar, 33, &corner, sizeof(corner));
    BarGlass(g_bar);
    g_tf = ui::Format(13.5f, DWRITE_FONT_WEIGHT_MEDIUM);  // o Mac desenha a barra com vibrância: as letras parecem mais cheias
    g_tfBold = ui::Format(13.5f, DWRITE_FONT_WEIGHT_BOLD);
    g_tfBadge = ui::Format(10, DWRITE_FONT_WEIGHT_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (g_tfBadge) g_tfBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_tfIcon = ui::Format(14, DWRITE_FONT_WEIGHT_NORMAL, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    RefreshStatus();
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    SetTimer(g_bar, 1, 1000, nullptr);
    Update();
    Displays();  // uma barra igual em cada um dos outros monitores
    return true;
}

void menubar::Update() {
    HWND fg = GetForegroundWindow();
    if (fg && !apps::IsShellWindow(fg)) {
        HWND root = GetAncestor(fg, GA_ROOTOWNER);
        if (root) fg = root;
        wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
        bool desktop = mesa::IsDeskClass(cls) || !wcscmp(cls, L"Shell_TrayWnd");
        if (desktop) { g_finderActive = true; g_fg = nullptr; }
        else if (fg != g_fg) {
            g_fg = fg;
            App* a = apps::OfWindow(fg);
            g_finderActive = a && a->finder;
        }
    }
    App* a = ActiveApp();
    g_appName = a ? a->name : L"Finder";
    if (wchar_t cls[32] = {}; g_fg && GetClassNameW(g_fg, cls, 32) && !wcscmp(cls, L"AjustesJanela")) g_appName = TrPick(L"Ajustes do Sistema", L"System Settings");
    g_hmenu = (g_fg && !g_finderActive) ? GetMenu(g_fg) : nullptr;
    if (!g_hmenu && g_fg && !g_finderActive) g_hmenu = (HMENU)GetPropW(g_fg, L"MacJanelaMenu");  // tirada da janela pela MacJanela.dll
    g_titles.clear();
    if (g_hmenu && IsMenu(g_hmenu)) {
        int n = GetMenuItemCount(g_hmenu);
        for (int k = 0; k < n; k++) {
            wchar_t buf[128] = {};
            GetMenuStringW(g_hmenu, k, buf, 127, MF_BYPOSITION);
            if (buf[0]) g_titles.push_back(Clean(buf));
            else g_titles.push_back(L"?");
        }
    } else {
        g_hmenu = nullptr;
        g_titles = GenericTitles(a);
    }
    if (g_open < 0) RenderIfChanged();
}
