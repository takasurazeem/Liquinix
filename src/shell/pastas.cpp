// Pastas no Finder, venham de onde vierem: "Mostrar na pasta" dos downloads do Chrome/Opera, "abrir pasta"
// de outros apps (Claude, Discord...) chamam o Explorador direto (explorer.exe /select ou
// SHOpenFolderAndSelectItems), sem passar pelo verbo de pasta do registro (que o Finder já assume). Aqui a
// janela de pasta do Explorador (CabinetWClass) é escondida assim que aparece, a pasta e os itens
// selecionados são lidos dela (IShellWindows) e ela é trocada pelo Finder no mesmo lugar.
// Pastas que não são de disco (Lixeira, Este Computador, Painel de Controle, dentro de um .zip, rede)
// voltam a aparecer no Explorador. Ajuste "finder_padrao" (Ajustes > Geral > Abrir pastas no Finder).
#include "shell.h"
#include "../comum/nomes.h"
#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <thread>
#include <algorithm>

namespace {

HWINEVENTHOOK g_hook;

bool On() { return shell::Setting(L"finder_padrao", L"1") == L"1"; }

// pasta e seleção de uma janela do Explorador (true se achou a janela)
bool Read(HWND h, std::wstring& folder, std::vector<std::wstring>& sel, bool& fileSystem) {
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw)))) return false;
    bool found = false;
    long n = 0; sw->get_Count(&n);
    for (long i = 0; i < n && !found; i++) {
        VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = i;
        IDispatch* d = nullptr;
        if (FAILED(sw->Item(v, &d)) || !d) continue;
        IWebBrowserApp* wb = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&wb)))) {
            SHANDLE_PTR hw = 0; wb->get_HWND(&hw);
            if ((HWND)hw == h) {
                found = true;
                IServiceProvider* sp = nullptr; IShellBrowser* sb = nullptr; IShellView* sv = nullptr; IFolderView* fv = nullptr;
                if (SUCCEEDED(wb->QueryInterface(IID_PPV_ARGS(&sp))) && SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) &&
                    SUCCEEDED(sb->QueryActiveShellView(&sv)) && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv)))) {
                    IPersistFolder2* pf = nullptr;
                    if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf)))) {
                        PIDLIST_ABSOLUTE pidl = nullptr;
                        wchar_t path[MAX_PATH] = {};
                        if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
                            fileSystem = SHGetPathFromIDListW(pidl, path) != FALSE;
                            folder = path;
                            PWSTR pn = nullptr;  // pasta virtual: o nome dela (::{CLSID}) para saber se o Finder também mostra
                            if (!fileSystem && SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &pn)) && pn) { folder = pn; CoTaskMemFree(pn); }
                            CoTaskMemFree(pidl);
                        }
                        pf->Release();
                    }
                    IShellItemArray* items = nullptr;
                    if (SUCCEEDED(fv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&items))) && items) {
                        DWORD c = 0; items->GetCount(&c);
                        for (DWORD k = 0; k < c && k < 64; k++) {
                            IShellItem* it = nullptr;
                            if (SUCCEEDED(items->GetItemAt(k, &it))) {
                                PWSTR p = nullptr;
                                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { sel.push_back(p); CoTaskMemFree(p); }
                                it->Release();
                            }
                        }
                        items->Release();
                    }
                }
                for (IUnknown* u : { (IUnknown*)fv, (IUnknown*)sv, (IUnknown*)sb, (IUnknown*)sp }) if (u) u->Release();
            }
            wb->Release();
        }
        d->Release();
    }
    sw->Release();
    return found;
}

// A janela do Explorador fica invisível (totalmente transparente) em vez de escondida: escondida, a
// vez de ficar na frente voltava para quem pediu (o Chrome, ao fechar o painel de downloads, reativa a janela
// dele) e o Finder abria e ia para trás ("piscava"). Invisível, ela segura a frente até o Finder aparecer.
void Invisivel(HWND h, bool on) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (on) {
        // só transparente: com WS_EX_TRANSPARENT junto o Windows tirava a frente dela na hora
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
        // e fora da tela: a animação de abrir do Windows ainda mostrava o quadro escuro por ~2 quadros
        RECT r{}; GetWindowRect(h, &r);
        SetPropW(h, L"ShellPastaX", (HANDLE)(INT_PTR)r.left); SetPropW(h, L"ShellPastaY", (HANDLE)(INT_PTR)r.top);
        SetWindowPos(h, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    } else {
        if (GetPropW(h, L"ShellPastaX") || GetPropW(h, L"ShellPastaY"))  // volta ao lugar (ficou com o Explorador)
            SetWindowPos(h, nullptr, (int)(INT_PTR)GetPropW(h, L"ShellPastaX"), (int)(INT_PTR)GetPropW(h, L"ShellPastaY"), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
}

// janelas do Finder visíveis agora (para saber quando a nova apareceu)
std::vector<HWND> Finders() {
    std::vector<HWND> v;
    for (HWND o = FindWindowW(L"ArquivosJanela", nullptr); o; o = FindWindowExW(nullptr, o, L"ArquivosJanela", nullptr))
        if (IsWindowVisible(o)) v.push_back(o);
    return v;
}

void Handle(HWND h) {
    std::thread([h] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring folder; std::vector<std::wstring> sel; bool fs = false, found = false;
        // a pasta e a seleção (do /select) chegam um instante depois da janela aparecer
        for (int t = 0; t < 30 && IsWindow(h); t++) {
            folder.clear(); sel.clear(); fs = false;
            found = Read(h, folder, sel, fs);
            if (found && !folder.empty()) {
                if (!sel.empty() || t >= 8) break;  // com seleção: pronto; sem: espera um pouco pelo /select
            }
            Sleep(50);
        }
        DWORD at = folder.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(folder.c_str());
        bool ok = found && fs && at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
        if (!ok && shell::nativo && found) {  // pastas virtuais que o Finder mostra: Início, Acesso rápido, Este Computador, Lixeira
            const wchar_t* arg = nullptr;
            if (!_wcsicmp(folder.c_str(), L"::{F874310E-B6B7-47DC-BC84-B9E6B38F5903}") || !_wcsicmp(folder.c_str(), L"::{679F85CB-0220-4080-B29B-5540CC05AAB6}")) arg = L"";
            else if (!_wcsicmp(folder.c_str(), L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}") || !_wcsicmp(folder.c_str(), L"::{645FF040-5081-101B-9F08-00AA002F954E}")) arg = folder.c_str();
            if (arg) {
                shell::AbrirFinder(arg);
                if (IsWindow(h)) { ShowWindowAsync(h, SW_HIDE); PostMessageW(h, WM_CLOSE, 0, 0); }
                Log(L"pastas: Explorador (pasta virtual %ls) trocado pelo Finder nativo", folder.c_str());
                CoUninitialize();
                return;
            }
        }
        if (!ok) {  // não é pasta de disco (Lixeira, Painel de Controle, .zip, rede): fica com o Explorador
            if (IsWindow(h)) { Invisivel(h, false); ShowWindowAsync(h, SW_SHOWNORMAL); }
            Log(L"pastas: deixei no Explorador (%ls)", folder.empty() ? L"pasta especial" : folder.c_str());
            CoUninitialize();
            return;
        }
        std::wstring arg = sel.size() == 1 ? sel[0] : folder;  // um item selecionado: o Finder abre a pasta com ele marcado
        std::wstring exe = shell::dir + L"\\" EXE_FINDER;
        // o Explorador (que está na frente) só fecha depois que o Finder aparecer, para a frente passar para ele
        auto antes = Finders();
        AllowSetForegroundWindow(ASFW_ANY);
        if (shell::nativo) {  // o Finder do explorer responde na hora (a janela já está na tela): fecha o Explorador já
            shell::AbrirFinder(L"\"" + arg + L"\"");
            if (IsWindow(h)) { ShowWindowAsync(h, SW_HIDE); PostMessageW(h, WM_CLOSE, 0, 0); }
            Log(L"pastas: Explorador trocado pelo Finder nativo (%ls)", arg.c_str());
            CoUninitialize();
            return;
        }
        ShellExecuteW(nullptr, L"open", exe.c_str(), (L"\"" + arg + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        HWND novo = nullptr;
        for (int t = 0; t < 60 && !novo; t++) {  // até 3 s
            Sleep(50);
            for (HWND f : Finders()) if (std::find(antes.begin(), antes.end(), f) == antes.end()) { novo = f; break; }
        }
        // só fecha o Explorador depois que o Finder pegou a frente (ele a pega do Explorador, que a segura);
        // fechando antes, a frente ia para outra janela e o Windows não deixava o Finder tomá-la de volta
        for (int t = 0; novo && t < 30 && GetForegroundWindow() != novo; t++) Sleep(50);
        if (IsWindow(h)) { ShowWindowAsync(h, SW_HIDE); PostMessageW(h, WM_CLOSE, 0, 0); }
        HWND fg = GetForegroundWindow(); wchar_t fc[64] = {}; if (fg) GetClassNameW(fg, fc, 64);
        Log(L"pastas: Explorador trocado pelo Finder (%ls); Finder %ls, na frente: %ls", arg.c_str(), novo ? L"apareceu" : L"NÃO apareceu em 3 s", fc);
        CoUninitialize();
    }).detach();
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (ev == EVENT_OBJECT_DESTROY || obj != OBJID_WINDOW || child != CHILDID_SELF || !h || !On()) return;
    wchar_t c[32] = {};
    if (!GetClassNameW(h, c, 32) || wcscmp(c, L"CabinetWClass") || GetAncestor(h, GA_ROOT) != h) return;
    if (GetPropW(h, L"ShellPastaVista")) return;  // já olhada (ficou no Explorador de propósito)
    if (GetFileAttributesW((shell::dir + L"\\" EXE_FINDER).c_str()) == INVALID_FILE_ATTRIBUTES) return;  // sem o Finder: nada a fazer
    // ao ser criada (antes de aparecer) já fica invisível: sem o quadro do Explorador piscando na tela
    if (!GetPropW(h, L"ShellPastaInvisivel")) { SetPropW(h, L"ShellPastaInvisivel", (HANDLE)1); Invisivel(h, true); }
    if (ev != EVENT_OBJECT_SHOW) return;
    SetPropW(h, L"ShellPastaVista", (HANDLE)1);
    Handle(h);
}

}  // namespace

void pastas::Start() {
    // CREATE (0x8000) até SHOW (0x8002): pega a janela já ao nascer
    // No modo nativo o Shell roda DENTRO do explorer.exe, e é o próprio explorer que cria a janela de pasta quando um
    // programa pede "mostrar na pasta" (SHOpenFolderAndSelectItems; ex.: "Mostrar no Explorer" do Claude): pulando o
    // próprio processo, essas janelas passavam direto e abria o Explorador antigo (relato do usuário no PC, 12/11/2026).
    // As janelas do Finder são de outra classe (ArquivosJanela), não há risco de pegar as nossas.
    if (!g_hook) g_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, OnShow, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | (shell::nativo ? 0 : WINEVENT_SKIPOWNPROCESS));
}

void pastas::Stop() {
    if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
}
