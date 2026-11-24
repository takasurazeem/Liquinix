// Janela de Abrir/Salvar do Windows trocada pelo Finder, POR FORA, para os programas que não deixam
// a DialogoMac.dll entrar (Chrome, Discord, WhatsApp e outros que bloqueiam DLLs de terceiros).
//  1. O Windows avisa (WinEvent, fora do processo) quando um diálogo aparece; é o de arquivos se
//     tem o campo de nome (controle 1148), o botão OK e a área de itens do Explorador.
//  2. Lemos o modo (abrir/salvar/pasta), os tipos (combo 1136), o nome e a pasta atual; a janela
//     original sai da tela (continua aberta) e o Finder abre em modo escolher.
//  3. Escolheu: escrevemos o caminho no campo de nome da original e mandamos OK para ela; o
//     programa recebe o arquivo pelo caminho de sempre. Cancelou: Cancelar na original.
// Shift segurado quando a janela aparece deixa a original. Programas elevados ficam com a original.
#include "shell.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <UIAutomation.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <map>
#include <thread>

namespace {

HWINEVENTHOOK g_hook, g_objHook;
// Sem piscar: todo diálogo novo (#32770) nasce invisível (alfa 0) até sabermos o que é. Se não for
// de arquivos, volta na hora (no evento de aparecer, ou no máximo em 700 ms).
struct Veil { DWORD at; bool hadLayered; };
std::map<HWND, Veil> g_veiled;
UINT_PTR g_veilTimer;
DWORD g_selfPid;
std::mutex g_mu;
std::set<HWND> g_busy;  // diálogos sendo atendidos
std::map<HWND, RECT> g_origPos;      // onde a original estava antes de sair da tela
std::map<HWND, DWORD> g_finderTid;
std::map<HWND, HWND> g_finderWnd;    // ou a janela do Finder reserva que atende
std::atomic<bool> g_stopping{ false };  // o Shell está saindo: não responde mais nada às originais   // o Finder que atende cada um (para fechar se o Shell sair)

std::wstring Text(HWND h) {
    wchar_t buf[2048] = {};
    SendMessageTimeoutW(h, WM_GETTEXT, 2048, (LPARAM)buf, SMTO_ABORTIFHUNG, 300, nullptr);
    return buf;
}

// Controle pelo id em qualquer nível. Nas janelas antigas com modelo próprio (GetSaveFileName com
// template, ex.: o Ableton), o campo do nome (1148) e o dos tipos (1136) ficam dentro da área do
// Explorador (DUIViewWndClassName), não como filhos diretos: GetDlgItem não acha.
HWND Item(HWND dlg, int id) {
    if (HWND c = GetDlgItem(dlg, id)) return c;
    struct Find { int id; HWND found; } f{ id, nullptr };
    EnumChildWindows(dlg, [](HWND c, LPARAM l) -> BOOL {
        auto* f = (Find*)l;
        if (GetDlgCtrlID(c) == f->id) { f->found = c; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.found;
}

// As caixas da faixa de baixo das janelas novas montadas pelo programa (ex.: o Ableton): ficam dentro de
// FloatNotifySink e sem número; a do nome tem um campo de texto (1001) dentro, a dos tipos não.
void FloatCombos(HWND dlg, HWND& nameEdit, HWND& typeCombo) {
    struct F { HWND edit, type; } f{ nullptr, nullptr };
    EnumChildWindows(dlg, [](HWND c, LPARAM l) -> BOOL {
        auto* f = (F*)l;
        wchar_t cls[32] = {}, pcls[32] = {};
        GetClassNameW(c, cls, 32);
        if (wcscmp(cls, L"ComboBox")) return TRUE;
        GetClassNameW(GetParent(c), pcls, 32);
        if (wcscmp(pcls, L"FloatNotifySink")) return TRUE;
        if (HWND e = FindWindowExW(c, nullptr, L"Edit", nullptr)) { if (!f->edit) f->edit = e; }
        else if (!f->type) f->type = c;
        return TRUE;
    }, (LPARAM)&f);
    nameEdit = f.edit; typeCombo = f.type;
}

HWND TypeCombo(HWND dlg) {
    if (HWND c = Item(dlg, 1136)) return c;
    HWND e, t; FloatCombos(dlg, e, t);
    return t;
}

// O campo de texto de verdade do "Nome do arquivo" (ComboBoxEx32 > ComboBox > Edit, ou só Edit).
HWND NameEdit(HWND dlg) {
    HWND c = Item(dlg, 1148);
    if (!c) c = Item(dlg, 1152);  // janela antiga de abrir/salvar
    if (!c) { HWND e, t; FloatCombos(dlg, e, t); return e; }  // janela nova montada pelo programa
    wchar_t cls[32]; GetClassNameW(c, cls, 32);
    if (!wcscmp(cls, L"Edit")) return c;
    if (HWND cb = FindWindowExW(c, nullptr, L"ComboBox", nullptr))
        if (HWND e = FindWindowExW(cb, nullptr, L"Edit", nullptr)) return e;
    return FindWindowExW(c, nullptr, L"Edit", nullptr);
}

bool HasChildClass(HWND parent, const wchar_t* cls) {
    struct C { const wchar_t* cls; bool found; } c{ cls, false };
    EnumChildWindows(parent, [](HWND w, LPARAM p) -> BOOL {
        auto* c = (C*)p;
        wchar_t n[64]; GetClassNameW(w, n, 64);
        if (!wcscmp(n, c->cls)) { c->found = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.found;
}

bool IsFileDialog(HWND dlg) {
    wchar_t cls[32] = {}; GetClassNameW(dlg, cls, 32);
    if (wcscmp(cls, L"#32770")) return false;
    if (GetDlgItem(dlg, IDOK) && NameEdit(dlg) && (HasChildClass(dlg, L"DirectUIHWND") || HasChildClass(dlg, L"SHELLDLL_DefView"))) return true;
    // "Procurar Pasta" (SHBrowseForFolder) com campo de texto: dá para preencher
    return GetDlgItem(dlg, IDOK) && GetDlgItem(dlg, 0x3744) && HasChildClass(dlg, L"SysTreeView32");
}

HWND FolderEdit(HWND dlg) { return GetDlgItem(dlg, 0x3744); }

// marca para a barra do Mac (LiquinixWindow.dll, dentro do programa) esconder o que ela desenha em volta
// da janela; o reposicionamento sem mudar nada faz ela reavaliar na hora
void Mark(HWND h, bool on) {
    if (on) SetPropW(h, L"ShellDialogoPorFora", (HANDLE)1); else RemovePropW(h, L"ShellDialogoPorFora");
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void Unveil(HWND h) {
    auto it = g_veiled.find(h);
    if (it == g_veiled.end()) return;
    if (IsWindow(h)) Mark(h, false);
    if (IsWindow(h)) {
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        if (!it->second.hadLayered) SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    g_veiled.erase(it);
}

void Veil(HWND h) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool had = (ex & WS_EX_LAYERED) != 0;
    if (!had && !SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED)) return;  // programa elevado: deixa
    if (!SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA)) { if (!had) SetWindowLongPtrW(h, GWL_EXSTYLE, ex); return; }
    g_veiled[h] = { GetTickCount(), had };
    Mark(h, true);
}

void CALLBACK VeilTimer(HWND, UINT, UINT_PTR, DWORD) {  // segurança: nada fica invisível por engano
    DWORD now = GetTickCount();
    std::vector<HWND> late;
    for (auto& [h, v] : g_veiled) {
        if (!IsWindow(h)) { late.push_back(h); continue; }
        bool busy;
        { std::lock_guard<std::mutex> lk(g_mu); busy = g_busy.count(h) > 0; }
        if (!busy && now - v.at > 700) late.push_back(h);
    }
    for (HWND h : late) Unveil(h);
    if (g_veiled.empty()) { KillTimer(nullptr, g_veilTimer); g_veilTimer = 0; }
}

// Pasta onde a janela original está: a barra de endereço tem o texto "Endereço: C:\..." / "Address: ...".
std::wstring CurrentFolder(HWND dlg) {
    struct C { std::wstring found; } c;
    EnumChildWindows(dlg, [](HWND w, LPARAM p) -> BOOL {
        wchar_t cls[32]; GetClassNameW(w, cls, 32);
        if (wcscmp(cls, L"ToolbarWindow32")) return TRUE;
        std::wstring t = Text(w);
        size_t colon = t.find(L": ");
        if (colon == std::wstring::npos) return TRUE;
        std::wstring path = t.substr(colon + 2);
        DWORD a = GetFileAttributesW(path.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { ((C*)p)->found = path; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.found;
}

struct Request {
    HWND dlg, owner;
    int mode = 0;  // 0 abrir, 1 salvar, 2 pasta
    std::vector<std::pair<std::wstring, std::wstring>> types;
    int type = 0;
    std::wstring name, folder;
};

Request Read(HWND dlg) {
    Request r;
    r.dlg = dlg;
    r.owner = GetWindow(dlg, GW_OWNER);
    std::wstring ok = Text(GetDlgItem(dlg, IDOK));
    for (auto& ch : ok) ch = towlower(ch);
    HWND combo = TypeCombo(dlg);
    if (ok.find(L"salv") != std::wstring::npos || ok.find(L"save") != std::wstring::npos) r.mode = 1;
    else if (ok.find(L"pasta") != std::wstring::npos || ok.find(L"folder") != std::wstring::npos || !combo) r.mode = 2;
    if (combo) {  // "Imagens (*.png;*.jpg)" → descrição + padrões
        int n = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < n && i < 64; i++) {
            wchar_t buf[512] = {};
            if (SendMessageW(combo, CB_GETLBTEXTLEN, i, 0) >= 511) continue;
            SendMessageW(combo, CB_GETLBTEXT, i, (LPARAM)buf);
            std::wstring t = buf, desc = t, spec = L"*.*";
            size_t o = t.rfind(L'('), c = t.rfind(L')');
            if (o != std::wstring::npos && c != std::wstring::npos && c > o) {
                spec = t.substr(o + 1, c - o - 1);
                desc = t.substr(0, o);
                while (!desc.empty() && desc.back() == L' ') desc.pop_back();
            }
            for (auto& ch : spec) if (ch == L',' ) ch = L';';
            r.types.push_back({ desc.empty() ? t : desc, spec });
        }
        r.type = std::max(0, (int)SendMessageW(combo, CB_GETCURSEL, 0, 0));
    }
    if (!NameEdit(dlg) && FolderEdit(dlg)) r.mode = 2;
    r.name = NameEdit(dlg) ? Text(NameEdit(dlg)) : L"";
    r.folder = CurrentFolder(dlg);
    return r;
}

std::mutex g_lastMu;
std::wstring g_lastSaved;
ULONGLONG g_lastSavedAt = 0;

void Finish(const Request& r, const std::vector<std::wstring>& paths, int type) {
    if (!IsWindow(r.dlg)) return;  // o programa já fechou a janela
    if (paths.empty()) { PostMessageW(r.dlg, WM_COMMAND, IDCANCEL, 0); return; }
    if (r.mode == 1) { std::lock_guard<std::mutex> lk(g_lastMu); g_lastSaved = paths[0]; g_lastSavedAt = GetTickCount64(); }
    if (HWND combo = TypeCombo(r.dlg); combo && type >= 0 && type != r.type) {
        SendMessageTimeoutW(combo, CB_SETCURSEL, type, 0, SMTO_ABORTIFHUNG, 300, nullptr);
        SendMessageTimeoutW(r.dlg, WM_COMMAND, MAKEWPARAM(1136, CBN_SELCHANGE), (LPARAM)combo, SMTO_ABORTIFHUNG, 300, nullptr);
    }
    HWND edit = NameEdit(r.dlg);
    if (!edit) edit = FolderEdit(r.dlg);
    std::wstring value = paths[0];
    if (paths.size() > 1) {  // vários: entra na pasta e depois lista os nomes entre aspas
        std::wstring dir = paths[0].substr(0, paths[0].find_last_of(L'\\'));
        SendMessageTimeoutW(edit, WM_SETTEXT, 0, (LPARAM)dir.c_str(), SMTO_ABORTIFHUNG, 300, nullptr);
        SendMessageTimeoutW(r.dlg, WM_COMMAND, IDOK, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
        value.clear();
        for (auto& p : paths) value += L"\"" + std::wstring(PathFindFileNameW(p.c_str())) + L"\" ";
    }
    SendMessageTimeoutW(edit, WM_SETTEXT, 0, (LPARAM)value.c_str(), SMTO_ABORTIFHUNG, 300, nullptr);
    PostMessageW(r.dlg, WM_COMMAND, IDOK, (LPARAM)GetDlgItem(r.dlg, IDOK));
}

// O programa aceita vários arquivos? A lista de itens da janela original diz (SelectionPattern).
// Sem isto o Finder recebia sempre "um só" e o envio de vários ficava com o primeiro.
bool AllowsMultiple(HWND dlg) {
    bool multi = false;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) && uia) {
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(uia->ElementFromHandle(dlg, &root)) && root) {
            VARIANT v; v.vt = VT_I4; v.lVal = UIA_ListControlTypeId;
            IUIAutomationCondition* cond = nullptr;
            uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
            IUIAutomationElementArray* arr = nullptr;
            if (cond && SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
                int n = 0; arr->get_Length(&n);
                for (int i = 0; i < n && !multi; i++) {
                    IUIAutomationElement* e = nullptr; arr->GetElement(i, &e);
                    if (!e) continue;
                    IUIAutomationSelectionPattern* sp = nullptr;
                    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&sp))) && sp) {
                        BOOL can = FALSE;
                        if (SUCCEEDED(sp->get_CurrentCanSelectMultiple(&can)) && can) multi = true;
                        sp->Release();
                    }
                    e->Release();
                }
                arr->Release();
            }
            if (cond) cond->Release();
            root->Release();
        }
        uia->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return multi;
}

// Nativo (Shell dentro do explorer): o Finder em modo escolher roda numa thread do próprio explorer, pela
// MacDialogo.dll (a mesma que troca o Abrir/Salvar dentro dos programas), sem LiquinixFinder.exe. Uma thread
// só, sempre a mesma (a janela do escolher mora nela); um pedido de cada vez. -1 = indisponível.
struct PedidoEmbutido { std::wstring args; int code = -1; HANDLE done = nullptr; };
PedidoEmbutido* g_pedido;
HANDLE g_evPedido;
std::mutex g_embMu;
DWORD g_embTid;
int(WINAPI* g_escolher)(const wchar_t*);

int Embutido(HWND dlg, const std::wstring& args) {
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE m = LoadLibraryW(L"MacDialogo.dll");
        g_escolher = m ? (int(WINAPI*)(const wchar_t*))GetProcAddress(m, "MacEscolher") : nullptr;
        if (!g_escolher) { Log(L"MacDialogo.dll indisponível: Abrir/Salvar por fora pelo exe"); return; }
        g_evPedido = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread([] {
            g_embTid = GetCurrentThreadId();
            OleInitialize(nullptr);
            for (;;) {  // espera pedidos sem deixar de atender mensagens (a janela escondida recebe avisos do sistema)
                DWORD w = MsgWaitForMultipleObjects(1, &g_evPedido, FALSE, INFINITE, QS_ALLINPUT);
                if (w == WAIT_OBJECT_0) {
                    if (PedidoEmbutido* p = g_pedido) { p->code = g_escolher(p->args.c_str()); SetEvent(p->done); }
                    continue;
                }
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            }
        }).detach();
    });
    if (!g_escolher) return -1;
    std::lock_guard<std::mutex> lk(g_embMu);
    PedidoEmbutido p;
    p.args = args;
    p.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_pedido = &p;
    SetEvent(g_evPedido);
    bool fechou = false;
    while (WaitForSingleObject(p.done, 250) == WAIT_TIMEOUT)
        if (!fechou && (!IsWindow(dlg) || g_stopping)) {  // o programa desistiu (ou o Shell está saindo): cancela
            for (HWND w = FindWindowW(L"ArquivosEscolher", nullptr); w; w = FindWindowExW(nullptr, w, L"ArquivosEscolher", nullptr))
                if (GetWindowThreadProcessId(w, nullptr) == g_embTid) PostMessageW(w, WM_CLOSE, 0, 0);
            fechou = true;
        }
    g_pedido = nullptr;
    CloseHandle(p.done);
    return p.code;
}

void LerResultado(const wchar_t* res, std::vector<std::wstring>& paths, int& type) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, res, L"r, ccs=UTF-8") || !f) return;
    wchar_t line[2048];
    while (fgetws(line, 2048, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
        if (l.rfind(L"tipo=", 0) == 0) type = _wtoi(l.c_str() + 5);
        else if (!l.empty()) paths.push_back(l);
    }
    fclose(f);
}

void Serve(Request r) {
    std::wstring finder = shell::dir + L"\\" EXE_FINDER;
    wchar_t tmp[MAX_PATH], res[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"dlg", 0, res);
    auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
    std::wstring start = r.folder;
    if (start.empty()) { PWSTR d = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &d))) { start = d; CoTaskMemFree(d); } }
    std::wstring cmd = q(finder) + L" " + q(start) + L" --lista --escolher " + (r.mode == 1 ? L"salvar" : r.mode == 2 ? L"pasta" : L"abrir");
    if (r.mode == 1) cmd += L" --sem-confirmar";  // quem confirma a substituição é a janela original
    if (r.mode == 0 && AllowsMultiple(r.dlg)) cmd += L" --multi";  // vários arquivos de uma vez (upload do navegador etc.)
    if (!r.types.empty() && r.mode != 2) {
        std::wstring t;
        for (auto& [d, sp] : r.types) { if (!t.empty()) t += L"||"; t += d + L"|" + sp; }
        cmd += L" --tipos " + q(t) + L" --tipo " + std::to_wstring(r.type);
    }
    if (!r.name.empty()) cmd += L" --nome " + q(r.name);
    cmd += L" --resultado " + q(res);
    if (r.owner) cmd += L" --pai " + std::to_wstring((ULONG_PTR)r.owner);
    Log(L"diálogo por fora: %ls", cmd.c_str());
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::vector<std::wstring> paths;
    int type = -1;
    // Direto para o Finder reserva (já aberto e escondido): sem abrir um processo só para repassar o
    // pedido a ele, a janela aparece na hora. Sem reserva, abre o Finder como antes.
    int embutido = shell::nativo ? Embutido(r.dlg, cmd.substr(q(finder).size() + 1)) : -1;  // nativo: aqui no explorer
    if (embutido >= 0) Log(L"  embutido no explorer (%d)", embutido);
    HANDLE proc = nullptr; HWND spare = shell::nativo ? nullptr : FindWindowW(L"ArquivosJanela", L"\u200BArquivosReserva");
    if (spare) {
        DWORD rpid = 0; GetWindowThreadProcessId(spare, &rpid);
        proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rpid);
        AllowSetForegroundWindow(rpid);
        COPYDATASTRUCT cd{ 0x41525156 /*'ARQV'*/, (DWORD)((cmd.size() + 1) * sizeof(wchar_t)), (void*)cmd.c_str() };
        DWORD_PTR ok = 0;
        // Reserva ocupada (ainda se preparando): o pedido fica na fila dela e é atendido depois. Esgotar o tempo
        // não é recusa; antes abria outro Finder e apareciam DUAS janelas de salvar no Ableton (16/11/2026).
        if (proc) {
            SetLastError(0);
            LRESULT sent = SendMessageTimeoutW(spare, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &ok);
            bool tarde = !sent && GetLastError() == ERROR_TIMEOUT && IsWindow(spare);
            if (tarde) Log(L"  reserva do Finder demorou; o pedido segue com ela");
            else if (!sent || !ok) { CloseHandle(proc); proc = nullptr; }
        }
        if (proc) Log(L"  pela reserva do Finder");
    }
    if (embutido < 0 && !proc && CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        spare = nullptr;
    }
    if (embutido >= 0) {
        if (embutido == 0) LerResultado(res, paths, type);
    } else if (proc) {
        { std::lock_guard<std::mutex> lk(g_mu); g_finderTid[r.dlg] = spare ? 0 : pi.dwThreadId; g_finderWnd[r.dlg] = spare; }
        while (WaitForSingleObject(proc, 250) == WAIT_TIMEOUT)
            if (!IsWindow(r.dlg)) {  // o programa desistiu
                if (spare) PostMessageW(spare, WM_CLOSE, 0, 0); else PostThreadMessageW(pi.dwThreadId, WM_QUIT, 0, 0);
                break;
            }
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        if (code == 0) {
            FILE* f = nullptr;
            if (!_wfopen_s(&f, res, L"r, ccs=UTF-8") && f) {
                wchar_t line[2048];
                while (fgetws(line, 2048, f)) {
                    std::wstring l = line;
                    while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
                    if (l.rfind(L"tipo=", 0) == 0) type = _wtoi(l.c_str() + 5);
                    else if (!l.empty()) paths.push_back(l);
                }
                fclose(f);
            }
        }
    } else {  // sem Finder: devolve a janela original para a tela
        SetWindowPos(r.dlg, nullptr, 100, 100, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        SetLayeredWindowAttributes(r.dlg, 0, 255, LWA_ALPHA);
    }
    DeleteFileW(res);
    if (g_stopping) return;  // a original já foi devolvida à tela por Stop: fica com o usuário
    Finish(r, paths, type);
    Log(L"  %ls", paths.empty() ? L"cancelado" : paths[0].c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_busy.erase(r.dlg);
    g_origPos.erase(r.dlg);
    g_finderTid.erase(r.dlg);
    g_finderWnd.erase(r.dlg);
}

void CALLBACK OnDialog(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD, DWORD);

// Janelas montadas pelo próprio programa (GetSaveFileName com modelo, ex.: o Ableton) ganham a área do
// Explorador e o campo do nome um pouco DEPOIS de aparecer: na primeira olhada ainda não são "de
// arquivos". Ficam invisíveis e são olhadas de novo a cada 15 ms por até 600 ms.
std::map<HWND, DWORD> g_recheck;
UINT_PTR g_recheckTimer;
void CALLBACK RecheckTimer(HWND, UINT, UINT_PTR, DWORD) {
    DWORD now = GetTickCount();
    std::vector<HWND> ready, gone;
    for (auto& [h, at] : g_recheck) {
        if (!IsWindow(h)) { gone.push_back(h); continue; }
        if (IsFileDialog(h)) ready.push_back(h);
        else if (now - at > 600) gone.push_back(h);
    }
    for (HWND h : gone) {
        g_recheck.erase(h);
        if (IsWindow(h)) {
            wchar_t t[80] = {}; GetWindowTextW(h, t, 80);
            Log(L"diálogos por fora: \"%ls\" não é de arquivos (nome %d, ok %d, área %d)", t, Item(h, 1148) != nullptr, GetDlgItem(h, IDOK) != nullptr,
                HasChildClass(h, L"DirectUIHWND") || HasChildClass(h, L"SHELLDLL_DefView"));
            Unveil(h);
        }
    }
    for (HWND h : ready) {
        g_recheck.erase(h);
        Log(L"diálogos por fora: reconhecida na segunda olhada");
        OnDialog(nullptr, EVENT_SYSTEM_DIALOGSTART, h, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    }
    if (g_recheck.empty() && g_recheckTimer) { KillTimer(nullptr, g_recheckTimer); g_recheckTimer = 0; }
}

void CALLBACK OnDialog(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !hwnd) return;
    wchar_t cls[16] = {}; GetClassNameW(hwnd, cls, 16);
    if (wcscmp(cls, L"#32770")) return;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == g_selfPid) return;
    // Seletor dos apps da Loja (PickerHost.exe, ex.: "Adicionar exclusão" da Segurança do Windows): a resposta
    // escrita no diálogo não volta para o app, e o original escondido deixava a janela do app desativada (não
    // dava para clicar, 09/11/2026). Fica o diálogo do Windows.
    {
        static std::map<DWORD, bool> picker;
        auto it = picker.find(pid);
        if (it == picker.end()) {
            bool is = false;
            if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(hp, 0, path, &n)) is = !_wcsicmp(PathFindFileNameW(path), L"PickerHost.exe");
                CloseHandle(hp);
            }
            if (picker.size() > 512) picker.clear();
            it = picker.emplace(pid, is).first;
        }
        if (it->second) return;
    }
    if (ev == EVENT_OBJECT_CREATE) {  // nasce invisível até sabermos o que é
        if (GetKeyState(VK_SHIFT) & 0x8000) return;
        Veil(hwnd);
        if (!g_veilTimer) g_veilTimer = SetTimer(nullptr, 0, 120, VeilTimer);
        return;
    }
    if (!IsFileDialog(hwnd)) {
        // tem os controles do modelo das janelas de arquivos (lista 1120, textos 1091...): ainda montando, olha
        // de novo; caixas de mensagem e outros diálogos não esperam
        if (GetDlgItem(hwnd, IDOK) && (GetDlgItem(hwnd, 1120) || GetDlgItem(hwnd, 1091) || HasChildClass(hwnd, L"DUIViewWndClassName")) && !g_recheck.count(hwnd) && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            if (!g_veiled.count(hwnd)) Veil(hwnd);
            if (auto v = g_veiled.find(hwnd); v != g_veiled.end()) v->second.at = GetTickCount();  // o VeilTimer não solta antes
            g_recheck[hwnd] = GetTickCount();
            if (!g_recheckTimer) g_recheckTimer = SetTimer(nullptr, 0, 15, RecheckTimer);
            return;
        }
        if (!g_recheck.count(hwnd)) Unveil(hwnd);
        return;
    }
    if (GetKeyState(VK_SHIFT) & 0x8000) { Unveil(hwnd); return; }  // escape: a janela original
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_busy.count(hwnd)) return;
        g_busy.insert(hwnd);
    }
    Request r = Read(hwnd);
    Mark(hwnd, true);  // a barra do Mac do programa some com ela
    RECT orig{}; GetWindowRect(hwnd, &orig);
    if (orig.left > -30000) { std::lock_guard<std::mutex> lk(g_mu); g_origPos[hwnd] = orig; }
    // tira a original da tela; se não der (programa elevado), ela fica
    if (!SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        { std::lock_guard<std::mutex> lk(g_mu); g_busy.erase(hwnd); }
        Unveil(hwnd);
        return;
    }
    std::thread(Serve, r).detach();
}

}  // namespace

std::wstring dialogs::LastSaved() {
    std::lock_guard<std::mutex> lk(g_lastMu);
    return GetTickCount64() - g_lastSavedAt < 20000 ? g_lastSaved : L"";
}

void dialogs::Start() {
    if (shell::Setting(L"dialogo_por_fora", L"1") != L"1") return;
    g_selfPid = GetCurrentProcessId();
    g_hook = SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART, nullptr, OnDialog, 0, 0,
                             WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    g_objHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, OnDialog, 0, 0,
                                WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    Log(L"diálogos por fora: %ls", g_hook ? L"ok" : L"falhou");
    // janelas de abrir/salvar que já estavam abertas (abertas enquanto o Shell reiniciava, ou deixadas fora
    // da tela por um Shell que caiu no meio): o Finder atende agora
    std::vector<HWND> open;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != g_selfPid && IsWindowVisible(h) && IsFileDialog(h)) ((std::vector<HWND>*)l)->push_back(h);
        return TRUE;
    }, (LPARAM)&open);
    for (HWND h : open) {
        RECT r; GetWindowRect(h, &r);
        Log(L"diálogos por fora: janela de arquivos já aberta%ls", r.left <= -30000 ? L" (estava fora da tela)" : L"");
        OnDialog(nullptr, EVENT_SYSTEM_DIALOGSTART, h, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    }
}

void dialogs::Stop() {
    g_stopping = true;
    if (g_hook) UnhookWinEvent(g_hook);
    if (g_objHook) UnhookWinEvent(g_objHook);
    g_hook = g_objHook = nullptr;
    std::vector<HWND> all;
    for (auto& [h, v] : g_veiled) all.push_back(h);
    for (HWND h : all) Unveil(h);  // nada fica invisível quando o shell sai
    // as que estavam sendo atendidas voltam para a tela (senão o app fica com uma janela invisível e
    // parece travado) e o Finder delas fecha
    std::lock_guard<std::mutex> lk(g_mu);
    for (HWND h : g_busy) {
        if (!IsWindow(h)) continue;
        auto p = g_origPos.find(h);
        RECT r = p != g_origPos.end() ? p->second : RECT{ 100, 100, 100, 100 };
        SetWindowPos(h, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        auto f = g_finderTid.find(h);
        if (f != g_finderTid.end() && f->second) PostThreadMessageW(f->second, WM_QUIT, 0, 0);
        auto fw = g_finderWnd.find(h);
        if (fw != g_finderWnd.end() && fw->second) PostMessageW(fw->second, WM_CLOSE, 0, 0);
    }
}
