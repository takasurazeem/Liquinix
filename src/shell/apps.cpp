// Apps do Dock: os fixos (dock.txt; na primeira vez, os fixados na barra de tarefas do Windows),
// os abertos (janelas que apareceriam na barra de tarefas) e os recentes. Abrir, ativar, ocultar,
// encerrar, fixar, abrir ao iniciar sessão.
#include "shell.h"
#include <memory>
#include <thread>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <algorithm>
#include <map>

std::vector<App> apps::list;
std::vector<std::wstring> apps::recent;
std::vector<HWND> apps::ours;
static std::wstring g_finderExe;

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
static std::wstring VerKey(const std::wstring& name);   // apps que se atualizam numa pasta nova (mais abaixo, perto de Launch)
static std::vector<unsigned> VerNums(const std::wstring& name);
static bool Heal(App& a);

static std::wstring DataDir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell"; CoTaskMemFree(p); }
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// Um app nosso aberto antes de uma compilação continua rodando do .exe antigo, que foi renomeado/movido
// (LiquinixFinder-000312.exe na pasta de antigos, LiquinixFinder.run-..., .old, .bak): para o Dock ele é o
// mesmo app do bin (antes aparecia um segundo Finder no fim do Dock).
static std::wstring OurCanonical(const std::wstring& path) {
    size_t sl = path.find_last_of(L'\\');
    std::wstring file = path.substr(sl == std::wstring::npos ? 0 : sl + 1);
    std::wstring low = file; for (auto& c : low) c = towlower(c);
    if (low.rfind(L"sequoia", 0) != 0 || low.size() < 12) return path;
    size_t cut = low.find_first_of(L"-.");
    if (cut == std::wstring::npos || low.compare(cut, std::wstring::npos, L".exe") == 0) return path;  // já é o nome certo
    std::wstring canon = shell::dir + L"\\" + file.substr(0, cut) + L".exe";
    return GetFileAttributesW(canon.c_str()) != INVALID_FILE_ATTRIBUTES ? canon : path;
}

static std::wstring ExeOfProcess(DWORD pid) {
    std::wstring out;
    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(hp, 0, buf, &n)) out = buf;
        CloseHandle(hp);
    }
    return OurCanonical(out);
}

static std::wstring ExeOfWindow(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    return ExeOfProcess(pid);
}

static std::wstring AumidOfWindow(HWND h) {
    IPropertyStore* ps = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    if (out.empty()) {  // app da Loja cuja janela não traz o AUMID (WhatsApp, WinUI/WebView2): o do processo
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH] = {}; UINT32 n = APPLICATION_USER_MODEL_ID_MAX_LENGTH;
            if (GetApplicationUserModelId(hp, &n, buf) == ERROR_SUCCESS) out = buf;  // sem pacote: APPMODEL_ERROR_NO_APPLICATION
            CloseHandle(hp);
        }
    }
    return out;
}

// Os nossos apps aparecem só com o nome do app (como no Mac), na língua escolhida, nunca "Sequoia ...".
// Vazio: não é um dos nossos.
static std::wstring OurName(const std::wstring& exe) {
    std::wstring f = Lower(exe.substr(exe.find_last_of(L'\\') + 1));
    struct N { const wchar_t* exe; const wchar_t* pt; const wchar_t* en; };
    static const N names[] = {
        { L"liquinixfinder.exe", L"Finder", L"Finder" },
        { L"liquinixsettings.exe", L"Ajustes do Sistema", L"System Settings" },
        { L"liquinixactivitymonitor.exe", L"Monitor de Atividade", L"Activity Monitor" },
        { L"liquinixterminal.exe", L"Terminal", L"Terminal" },
        { L"liquinixcalculator.exe", L"Calculadora", L"Calculator" },
        { L"liquinixnotes.exe", L"Notas", L"Notes" },
        { L"liquinixscreenshot.exe", L"Captura de Tela", L"Screenshot" },
    };
    for (auto& n : names) if (f == n.exe) return TrPick(n.pt, n.en);
    return L"";
}

void apps::Relabel() {
    for (auto& a : list) {
        if (a.finder) continue;
        if (std::wstring o = OurName(a.exe.empty() ? a.launch : a.exe); !o.empty()) a.name = o;
    }
}

// Nome amigável do programa (descrição do arquivo), ou o nome do .exe.
static std::wstring ExeName(const std::wstring& exe) {
    if (std::wstring o = OurName(exe); !o.empty()) return o;
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (n) {
        std::vector<BYTE> buf(n);
        if (GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) {
            struct LangCp { WORD lang, cp; } *tr = nullptr; UINT len = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LangCp)) {
                wchar_t key[80];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
                wchar_t* desc = nullptr; UINT dl = 0;
                if (VerQueryValueW(buf.data(), key, (void**)&desc, &dl) && dl > 1 && desc[0]) return desc;
            }
        }
    }
    std::wstring f = exe.substr(exe.find_last_of(L'\\') + 1);
    if (f.size() > 4) f.resize(f.size() - 4);
    return f;
}

static std::wstring DisplayNameOf(const std::wstring& parsing) {
    IShellItem* si = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
        PWSTR n = nullptr;
        if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &n))) { out = n; CoTaskMemFree(n); }
        si->Release();
    }
    return out;
}

// Destino (.exe) e AUMID de um atalho.
static void ReadLink(const std::wstring& lnk, std::wstring& target, std::wstring& aumid) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
            wchar_t p[MAX_PATH] = {};
            if (SUCCEEDED(sl->GetPath(p, MAX_PATH, nullptr, 0))) target = p;
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) aumid = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
        }
        pf->Release();
    }
    sl->Release();
}

bool apps::IsShellWindow(HWND h) { return std::find(ours.begin(), ours.end(), h) != ours.end(); }

std::wstring apps::IdOfWindow(HWND h) {
    std::wstring exe = Lower(ExeOfWindow(h));
    if (!g_finderExe.empty() && exe == Lower(g_finderExe)) return Lower(g_finderExe);
    wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
    if ((!wcscmp(cls, L"CabinetWClass") || !wcscmp(cls, L"ArquivosJanela")) && !g_finderExe.empty()) return Lower(g_finderExe);  // janela do Explorador (e o Finder nativo, dentro do explorer.exe) conta como do Finder
    std::wstring a = AumidOfWindow(h);
    if (!a.empty()) return L"aumid:" + Lower(a);
    return exe;
}

App* apps::FindById(const std::wstring& id) {
    for (auto& a : list) if (a.id == id) return &a;
    return nullptr;
}

// Janela com dona que fica sozinha: a dona está escondida (ou é só uma tela de abertura de ferramenta),
// como o aviso "recuperar o trabalho?" do Ableton ao abrir depois de fechar sem querer. O Windows mostra
// essas no Alt+Tab; antes ela ficava de fora do Dock, do seletor e do Mission Control e se perdia atrás de tudo.
bool apps::LoneOwned(HWND h) {
    HWND owner = GetWindow(h, GW_OWNER);
    if (!owner || !IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE), st = GetWindowLongPtrW(h, GWL_STYLE);
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || !(st & WS_CAPTION) && !GetPropW(h, L"MacJanela")) return false;
    // a janela de dona mais de cima: a dona dela não pode aparecer como app
    HWND root = GetAncestor(h, GA_ROOTOWNER);
    if (!root || root == h) return false;
    for (HWND o = owner; o; o = GetWindow(o, GW_OWNER)) {
        if (!IsWindowVisible(o)) continue;
        DWORD cl = 0; DwmGetWindowAttribute(o, DWMWA_CLOAKED, &cl, sizeof(cl));
        if (cl) continue;
        LONG_PTR oex = GetWindowLongPtrW(o, GWL_EXSTYLE);
        if ((oex & WS_EX_APPWINDOW) || (!(oex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && !GetWindow(o, GW_OWNER) && GetWindowTextLengthW(o) > 0))
            return false;  // a dona já aparece como app (ela representa este aviso)
    }
    return true;
}

// Janela que o Windows mostraria na barra de tarefas (as mesmas regras do Alt+Tab).
static bool IsTaskbarWindow(HWND h) {
    if (!IsWindowVisible(h) || apps::IsShellWindow(h)) return false;
    // nenhuma janela do próprio Shell é "app" (no Alt+Tab rápido o "app anterior" chegava a ser o Shell,
    // e trocar para ele não fazia nada)
    if (shell::Nossa(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    HWND owner = GetWindow(h, GW_OWNER);
    bool lone = false;
    if (ex & WS_EX_APPWINDOW) {}
    else if (owner && apps::LoneOwned(h)) lone = true;
    else if (owner || (ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE)) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (mesa::IsDeskClass(cls)) return false;
    for (auto c : { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" }) if (!wcscmp(cls, c)) return false;
    if (!wcscmp(cls, L"OperationStatusWindow") && progresso::Owns(h)) return false;  // trocada pela janela de progresso do Mac
    return GetWindowTextLengthW(h) > 0 || lone;  // aviso sem título (Ableton): vale também
}

bool apps::IsAppWindow(HWND h) { return IsTaskbarWindow(h); }

App* apps::OfWindow(HWND h) {
    std::wstring id = IdOfWindow(h);
    if (App* a = FindById(id)) return a;
    std::wstring exe = Lower(ExeOfWindow(h));
    for (auto& a : list) if (!a.exe.empty() && a.exe == exe) return &a;
    return nullptr;
}

void apps::Save() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (DataDir() + L"\\dock.txt").c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& a : list) if (a.pinned && !a.finder) fwprintf(f, L"%ls\t%ls\t%ls\t%ls\n", a.id.c_str(), a.launch.c_str(), a.name.c_str(), a.exe.c_str());
    fclose(f);
}

void apps::Load(const std::wstring& finderExe) {
    g_finderExe = finderExe;
    list.clear();
    App fin;
    fin.id = Lower(finderExe); fin.exe = fin.id; fin.launch = finderExe; fin.name = L"Finder";
    fin.pinned = fin.finder = true;
    list.push_back(fin);

    FILE* f = nullptr;
    if (!_wfopen_s(&f, (DataDir() + L"\\dock.txt").c_str(), L"r, ccs=UTF-8") && f) {
        wchar_t line[2048];
        while (fgetws(line, 2048, f)) {
            std::wstring s = line;
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            std::vector<std::wstring> p;
            size_t a = 0, b;
            while ((b = s.find(L'\t', a)) != std::wstring::npos) { p.push_back(s.substr(a, b - a)); a = b + 1; }
            p.push_back(s.substr(a));
            if (p.size() < 3) continue;
            if (p[0] == L"aumid:microsoft.windows.explorer") continue;  // o Finder já está no Dock
            App x; x.id = p[0]; x.launch = p[1]; x.name = p[2]; x.exe = p.size() > 3 ? p[3] : L""; x.pinned = true;
            if (std::wstring o = OurName(x.exe.empty() ? x.launch : x.exe); !o.empty()) x.name = o;
            list.push_back(x);
        }
        fclose(f);
        Log(L"dock: %zu apps fixos", list.size() - 1);
        bool healed = false;
        for (auto& a : list) healed |= Heal(a);  // apps que se atualizaram desde a última vez
        if (healed) Save();
        return;
    }
    // primeira vez: o que está fixado na barra de tarefas do Windows
    std::wstring pins = DataDir();
    PWSTR rp = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &rp))) {
        pins = std::wstring(rp) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
        CoTaskMemFree(rp);
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((pins + L"\\*.lnk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring lnk = pins + L"\\" + fd.cFileName, target, aumid;
            ReadLink(lnk, target, aumid);
            std::wstring t = Lower(target);
            if ((t.size() >= 12 && t.substr(t.size() - 12) == L"explorer.exe") || Lower(aumid) == L"microsoft.windows.explorer") continue;  // o Finder já está no Dock
            App x;
            x.exe = t;
            x.id = !aumid.empty() ? L"aumid:" + Lower(aumid) : t;
            x.launch = lnk;
            x.name = fd.cFileName; x.name.resize(x.name.size() - 4);
            x.pinned = true;
            list.push_back(x);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Log(L"dock: importados %zu apps fixos da barra de tarefas", list.size() - 1);
    Save();
}

bool apps::Refresh() {
    struct Ctx {
        std::map<std::wstring, std::vector<AppWin>> found;  // id -> janelas (ordem Z)
        std::map<std::wstring, std::wstring> exeOf;
        std::vector<std::wstring> order;
    } ctx;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        if (!IsTaskbarWindow(h)) return TRUE;
        auto& c = *(Ctx*)p;
        std::wstring id = IdOfWindow(h);
        // janela de um app fixo pelo .exe (inclusive app da Loja fixado pelo caminho dentro de WindowsApps, que
        // agora chega com o AUMID do processo): usa o id do fixo, senão aparecia duas vezes no Dock
        std::wstring exe = Lower(ExeOfWindow(h));
        for (auto& a : list)
            if (a.id != id && !a.exe.empty() && Lower(a.exe) == exe && (id.rfind(L"aumid:", 0) != 0 || a.id.rfind(L"aumid:", 0) != 0)) { id = a.id; break; }
        // app fixo que se atualizou enquanto o Shell rodava (o .exe novo está numa pasta de versão nova): é o mesmo
        // ícone do Dock, e o caminho guardado passa a ser o novo
        if (id.rfind(L"aumid:", 0) != 0 && exe.find_first_of(L"0123456789") != std::wstring::npos)
            for (auto& a : list)
                if (a.pinned && !a.finder && a.id != id && !a.exe.empty() && Lower(a.exe) != exe && VerKey(a.exe) == VerKey(exe) &&
                    VerNums(exe) > VerNums(Lower(a.exe))) {
                    Log(L"dock: %ls atualizou: %ls -> %ls", a.name.c_str(), a.exe.c_str(), exe.c_str());
                    bool idWasExe = a.id == Lower(a.exe);
                    if (Lower(a.launch) == Lower(a.exe)) a.launch = ExeOfWindow(h);
                    a.exe = exe;
                    if (idWasExe) a.id = exe;
                    id = a.id;
                    Save();
                    break;
                }
        // o nosso Ajustes do Sistema fica no ícone do Ajustes do Windows fixado (não aparece um segundo)
        if (exe.size() >= 12 && exe.compare(exe.size() - 12, 12, L"\\ajustes.exe") == 0)
            for (auto& a : list) {
                std::wstring l = Lower(a.launch);
                if (a.pinned && l.find(L"immersivecontrolpanel") != std::wstring::npos) { id = a.id; break; }
            }
        // a nossa Calculadora fica no ícone da Calculadora do Windows fixada
        if (exe.size() >= 16 && exe.compare(exe.size() - 16, 16, L"\\calculadora.exe") == 0)
            for (auto& a : list)
                if (a.pinned && Lower(a.id + a.launch).find(L"windowscalculator") != std::wstring::npos) { id = a.id; break; }
        wchar_t t[256]; GetWindowTextW(h, t, 256);
        if (!c.found.count(id)) c.order.push_back(id);
        c.found[id].push_back({ h, t, IsIconic(h) != FALSE });
        c.exeOf[id] = exe;
        return TRUE;
    }, (LPARAM)&ctx);
    auto& found = ctx.found;
    auto& exeOf = ctx.exeOf;
    auto& order = ctx.order;

    std::wstring before;
    for (auto& a : list) before += a.id + L"|" + std::to_wstring(a.wins.size()) + (a.recent ? L"r" : L"") + L";";
    for (auto& a : list) for (auto& w : a.wins) if (w.minimized) before += L"m" + std::to_wstring((ULONG_PTR)w.hwnd);

    for (auto& a : list) {
        auto it = found.find(a.id);
        bool was = !a.wins.empty();
        a.wins = it != found.end() ? it->second : std::vector<AppWin>{};
        if (!a.wins.empty()) { a.launchedAt = 0; a.recent = false; }
        (void)was;
    }
    for (auto& id : order) {  // abertos que não estão no Dock
        if (FindById(id)) continue;
        App x;
        x.id = id;
        x.exe = exeOf[id];
        x.wins = found[id];
        if (id.rfind(L"aumid:", 0) == 0) {
            std::wstring aumid = AumidOfWindow(x.wins[0].hwnd);
            x.launch = L"shell:AppsFolder\\" + aumid;
            x.name = DisplayNameOf(x.launch);
            if (x.name.empty()) x.name = ExeName(x.exe);
        } else {
            x.launch = ExeOfWindow(x.wins[0].hwnd);
            x.name = ExeName(x.launch);
        }
        list.push_back(x);
        Log(L"dock: aberto %ls (%ls)", x.name.c_str(), x.id.c_str());
    }
    // sem seção de recentes: o app que não está fixo sai do Dock assim que fecha (só fica enquanto abre)
    recent.clear();
    list.erase(std::remove_if(list.begin(), list.end(), [](const App& a) {
        return !a.pinned && a.wins.empty() && (a.launchedAt == 0 || GetTickCount() - a.launchedAt > 20000);
    }), list.end());
    // ordem: fixos (na ordem do usuário), depois abertos/recentes
    std::stable_partition(list.begin(), list.end(), [](const App& a) { return a.pinned; });

    std::wstring after;
    for (auto& a : list) after += a.id + L"|" + std::to_wstring(a.wins.size()) + (a.recent ? L"r" : L"") + L";";
    for (auto& a : list) for (auto& w : a.wins) if (w.minimized) after += L"m" + std::to_wstring((ULONG_PTR)w.hwnd);  // minimizar/restaurar também conta
    return before != after;
}

// Ícone do pacote do Sequoia para o app: pelo nome, pelo .exe ou por um apelido conhecido.
static std::wstring Norm(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) if (iswalnum(c)) o += towlower(c);
    return o;
}

static std::wstring PackIconFor(const App& a) {
    static std::map<std::wstring, std::wstring> pack;  // nome normalizado -> arquivo
    static int loaded = -1;  // tema com que a lista foi montada (Tahoe usa icones\tahoe antes do Sequoia)
    int tema = liquid::Leopard() ? 3 : liquid::Golden() ? 2 : liquid::On() ? 1 : 0;  // Golden Gate usa icones\goldengate antes do Tahoe; Leopard, icones\leopard antes do Sequoia
    if (loaded != tema) {
        loaded = tema;
        pack.clear();
        // icones\extra (ícones escolhidos pelo usuário, ex.: baixados do macOSicons.com) tem prioridade
        // sobre o pacote do Sequoia; o nome do arquivo é o nome do app ("Discord.png", "Steam.png").
        // Tema Tahoe: icones\tahoe (tools\baixar-icones.ps1) vem antes do Sequoia; o que faltar, segue o Sequoia
        std::wstring root = shell::dir.substr(0, shell::dir.find_last_of(L'\\')) + L"\\icones";
        // Leopard: icones\leopard vem ANTES do extra (o extra tem os ícones modernos; o Leopard tem a versão Aqua deles)
        for (const wchar_t* sub : { loaded == 3 ? L"\\leopard" : L"", L"\\extra", loaded == 2 ? L"\\goldengate" : L"", loaded == 1 || loaded == 2 ? L"\\tahoe" : L"", L"\\sequoia" }) {
            if (!*sub) continue;
            std::wstring dir = root + sub;
            if (std::wstring custom = shell::Setting(L"icones", L""); !custom.empty() && !wcscmp(sub, L"\\sequoia")) dir = custom;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring n = fd.cFileName; n.resize(n.size() - 4);
                pack.emplace(Norm(n), dir + L"\\" + fd.cFileName);  // emplace: o primeiro (extra) vence
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (pack.empty()) return L"";
    if (a.finder) return pack.count(L"finder") ? pack[L"finder"] : L"";
    static const std::map<std::wstring, std::wstring> alias = {
        { L"msedge", L"edge" }, { L"code", L"vscode" }, { L"winword", L"word" }, { L"powerpnt", L"powerpoint" },
        { L"chrome", L"googlechrome" }, { L"notepad", L"textedit" }, { L"mspaint", L"paint" }, { L"calculatorapp", L"calculator" }, { L"calculadora", L"calculator" },
        { L"windowsterminal", L"terminal" }, { L"wt", L"terminal" }, { L"taskmgr", L"activitymonitor" }, { L"monitoratividade", L"activitymonitor" }, { L"monitordeatividade", L"activitymonitor" }, { L"notas", L"notes" }, { L"sequoiacalculator", L"calculator" }, { L"sequoiaterminal", L"terminal" }, { L"sequoiaactivitymonitor", L"activitymonitor" }, { L"sequoianotes", L"notes" }, { L"sequoiasettings", L"settings" }, { L"systemsettings", L"settings" }, { L"ajustes", L"settings" }, { L"ajustesdosistema", L"settings" },
        { L"mbam", L"malwarebytes" }, { L"steamwebhelper", L"steam" }, { L"ms-teams", L"teams" }, { L"olk", L"outlook" },
        { L"photoshop", L"photoshop" }, { L"illustrator", L"illustrator" }, { L"sublime_text", L"sublimetext" },
    };
    std::wstring exeBase = a.exe.substr(a.exe.find_last_of(L'\\') + 1);
    if (exeBase.size() > 4) exeBase.resize(exeBase.size() - 4);
    for (const std::wstring& k : { Norm(a.name), Norm(exeBase) }) {
        if (k.empty()) continue;
        auto it = pack.find(k);
        if (it != pack.end()) return it->second;
        auto al = alias.find(k);
        if (al != alias.end() && pack.count(al->second)) return pack[al->second];
    }
    return L"";
}

std::wstring apps::PackIconByName(const std::wstring& name, const std::wstring& exe) {
    App a; a.name = name; a.exe = exe;
    return PackIconFor(a);
}

void apps::EnsureIcon(App& a, int px) {
    if (!a.icon.empty() && (a.mac || (int)std::max(a.iw, a.ih) >= px)) return;
    std::wstring png = PackIconFor(a);
    if (!png.empty() && ui::LoadPng(png, a.iw, a.ih, a.icon)) { a.mac = true; return; }
    if (a.finder) return;
    if (!ui::ShellImage(a.launch, px, a.iw, a.ih, a.icon, true))
        if (!ui::ShellImage(a.launch, px, a.iw, a.ih, a.icon, false) && !a.exe.empty())
            ui::ShellImage(a.exe, px, a.iw, a.ih, a.icon, true);
    if (!a.icon.empty()) ui::TrimToSquare(a.iw, a.ih, a.icon);
}

// Apps feitos com Chromium/Electron param de desenhar quando estão cobertos por outra janela, e a
// miniatura do Mission Control fica preta. Abertos por aqui, eles recebem a opção que desliga isso
// (não muda nenhum atalho; vale enquanto o app estiver aberto). Ajuste "chromium_sem_oclusao".
static const wchar_t* NO_OCCLUSION = L"--disable-features=CalculateNativeWinOcclusion";

static bool IsChromium(const std::wstring& exe) {
    std::wstring base = exe.substr(exe.find_last_of(L'\\') + 1);
    for (auto& ch : base) ch = towlower(ch);
    for (auto n : { L"chrome.exe", L"msedge.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe", L"arc.exe", L"thorium.exe" })
        if (base == n) return true;
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    return GetFileAttributesW((dir + L"\\chrome_100_percent.pak").c_str()) != INVALID_FILE_ATTRIBUTES ||  // Electron
           GetFileAttributesW((dir + L"\\resources\\app.asar").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// "{GUID-de-pasta-conhecida}\resto\app.exe" (nome do shell:AppsFolder de apps de desktop) → caminho real
static std::wstring ExpandKnownFolder(const std::wstring& p) {
    if (p.size() < 40 || p[0] != L'{') return p;
    size_t close = p.find(L'}');
    GUID g;
    if (close == std::wstring::npos || FAILED(CLSIDFromString(p.substr(0, close + 1).c_str(), &g))) return p;
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(g, 0, nullptr, &base))) return p;
    std::wstring r = std::wstring(base) + p.substr(close + 1);
    CoTaskMemFree(base);
    return r;
}

// O ativador segura quem chama até o app terminar de abrir (a Segurança do Windows leva vários segundos):
// numa thread separada, senão a interface do Shell para e o vigia o reinicia achando que travou.
bool apps::ActivatePackaged(const std::wstring& target, const std::wstring& args) {
    if (target.rfind(L"shell:AppsFolder\\", 0) != 0) return false;
    std::wstring id = target.substr(17);
    if (id.find(L'!') == std::wstring::npos) return false;  // não é empacotado
    // App da Loja antigo (UWP) sem o Explorador: o ativador fica esperando para sempre (a janela dele vem de
    // uma peça do explorer.exe). Sem resposta em 2,5 s, o Shell abre o Explorador escondido para ele.
    auto done = std::make_shared<std::atomic<bool>>(false);
    if (mesa::Standalone())
        std::thread([id, done] {
            Sleep(2500);
            if (!*done && mesa::Standalone()) mesa::NeedExplorer(id);
        }).detach();
    std::thread([id, args, done] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD t0 = GetTickCount();
        IApplicationActivationManager* am = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am));
        DWORD pid = 0;
        if (SUCCEEDED(hr)) {
            hr = am->ActivateApplication(id.c_str(), args.empty() ? nullptr : args.c_str(), AO_NONE, &pid);
            am->Release();
        }
        *done = true;
        if (FAILED(hr)) Log(L"app empacotado não abriu pelo ativador (0x%08X, %lu ms): %ls", (unsigned)hr, GetTickCount() - t0, id.c_str());
        else Log(L"app empacotado aberto em %lu ms (pid %lu): %ls", GetTickCount() - t0, pid, id.c_str());
        CoUninitialize();
    }).detach();
    return true;
}

bool apps::LaunchNoOcclusion(const std::wstring& target, const std::wstring& args, const std::wstring& exeHint) {
    if (shell::Setting(L"chromium_sem_oclusao", L"1") != L"1") return false;
    std::wstring exe, cmdArgs, dir;
    std::wstring t = target;
    std::wstring hint = exeHint;
    if (hint.empty())  // Launchpad: o .exe das janelas, se o app já apareceu no Dock
        for (auto& a : list) if (a.launch == target && !a.exe.empty()) { hint = a.exe; break; }
    if (t.rfind(L"shell:AppsFolder\\", 0) == 0) {
        std::wstring id = t.substr(17);
        if (id.find(L'!') != std::wstring::npos) {  // app empacotado (AUMID): o .exe não abre direto, só pelo ativador
            if (hint.empty() || !IsChromium(hint)) return false;
            IApplicationActivationManager* am = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am)))) return false;
            std::wstring all = std::wstring(NO_OCCLUSION) + (args.empty() ? L"" : L" " + args);
            DWORD pid = 0;
            HRESULT hr = am->ActivateApplication(id.c_str(), all.c_str(), AO_NONE, &pid);
            am->Release();
            if (SUCCEEDED(hr)) Log(L"abrindo sem oclusão (miniatura viva no Mission Control): %ls", id.c_str());
            return SUCCEEDED(hr);
        }
        t = ExpandKnownFolder(id);
        if (t.find(L'\\') == std::wstring::npos) {
            // ID de app de desktop ("Chrome", "com.squirrel.Discord.Discord"): o item do shell sabe o atalho por trás
            IShellItem2* si = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
                PWSTR p = nullptr, a = nullptr;
                if (SUCCEEDED(si->GetString(PKEY_Link_TargetParsingPath, &p)) && p) { exe = p; CoTaskMemFree(p); }
                if (SUCCEEDED(si->GetString(PKEY_Link_Arguments, &a)) && a) { cmdArgs = a; CoTaskMemFree(a); }
                si->Release();
            }
        }
    }
    std::wstring low = t;
    for (auto& ch : low) ch = towlower(ch);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, L".lnk") == 0) {
        IShellLinkW* sl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(t.c_str(), STGM_READ))) {
                wchar_t p[MAX_PATH] = {}, a[1024] = {}, w[MAX_PATH] = {};
                sl->GetPath(p, MAX_PATH, nullptr, 0);
                sl->GetArguments(a, 1024);
                sl->GetWorkingDirectory(w, MAX_PATH);
                exe = p; cmdArgs = a; dir = w;
            }
            if (pf) pf->Release();
            sl->Release();
        }
    } else if (low.size() > 4 && low.compare(low.size() - 4, 4, L".exe") == 0) exe = t;
    else if (exe.empty() && !hint.empty()) exe = hint;  // sem caminho: usa o .exe das janelas do app
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring base = exe.substr(exe.find_last_of(L'\\') + 1);
    for (auto& ch : base) ch = towlower(ch);
    if (base != L"update.exe" && !IsChromium(exe)) {
        // lançador do Squirrel (Discord, Slack...): o app de verdade está em app-x.y.z\ ao lado do Update.exe
        std::wstring dir2 = exe.substr(0, exe.find_last_of(L'\\'));
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir2 + L"\\app-*").c_str(), &fd);
        bool electron = false;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (IsChromium(dir2 + L"\\" + fd.cFileName + L"\\" + exe.substr(exe.find_last_of(L'\\') + 1))) electron = true;
            } while (!electron && FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (electron && GetFileAttributesW((dir2 + L"\\Update.exe").c_str()) != INVALID_FILE_ATTRIBUTES) {
            cmdArgs = L"--processStart " + exe.substr(exe.find_last_of(L'\\') + 1);
            exe = dir2 + L"\\Update.exe";
            base = L"update.exe";
        }
    }
    std::wstring all;
    if (base == L"update.exe" && cmdArgs.find(L"--processStart") != std::wstring::npos) {
        // Discord e outros apps Squirrel: o Update.exe repassa os argumentos ao app
        all = cmdArgs + L" --process-start-args \"" + NO_OCCLUSION + L"\"";
    } else {
        if (!IsChromium(exe)) return false;
        all = cmdArgs + (cmdArgs.empty() ? L"" : L" ") + NO_OCCLUSION;
    }
    if (!args.empty()) all += L" " + args;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), all.c_str(), dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) return false;
    Log(L"abrindo sem oclusão (miniatura viva no Mission Control): %ls %ls", exe.c_str(), all.c_str());
    return true;
}

// App da Loja guardado pelo caminho do .exe (fixado enquanto estava aberto): o Windows não deixa abrir o
// .exe de dentro de WindowsApps direto (o Spotify não abria). A identidade do pacote sai do nome da pasta
// (Nome_Versão_Arq__Editor -> Nome_Editor) e o Id do app, do AppxManifest.xml (o <Application> deste .exe).
static std::wstring PackagedAumid(const std::wstring& path) {
    std::wstring low = Lower(path);
    size_t w = low.find(L"\\windowsapps\\");
    if (w == std::wstring::npos) return L"";
    size_t f0 = w + 13, f1 = path.find(L'\\', f0);
    if (f1 == std::wstring::npos) return L"";
    std::wstring folder = path.substr(f0, f1 - f0);
    size_t u = folder.find(L'_'), pub = folder.rfind(L"__");
    if (u == std::wstring::npos || pub == std::wstring::npos || pub <= u) return L"";
    std::wstring pfn = folder.substr(0, u) + L"_" + folder.substr(pub + 2);
    std::wstring exe = Lower(path.substr(path.find_last_of(L'\\') + 1)), appId;
    HANDLE fh = CreateFileW((path.substr(0, f1) + L"\\AppxManifest.xml").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(fh, nullptr), rd = 0;
        std::string x(size, 0);
        ReadFile(fh, x.data(), size, &rd, nullptr);
        CloseHandle(fh);
        std::string lx = x; for (auto& c : lx) c = (char)tolower((unsigned char)c);
        std::string want(exe.begin(), exe.end());
        for (size_t at = lx.find("<application "); at != std::string::npos; at = lx.find("<application ", at + 1)) {
            size_t end = lx.find('>', at);
            std::string tag = lx.substr(at, end - at);
            size_t i = tag.find(" id=\"");
            if (i == std::string::npos) continue;
            std::string id = x.substr(at + i + 5, x.find('"', at + i + 5) - (at + i + 5));
            if (appId.empty() || tag.find(want) != std::string::npos) appId = std::wstring(id.begin(), id.end());
            if (tag.find(want) != std::string::npos) break;
        }
    }
    if (appId.empty()) appId = L"App";
    return pfn + L"!" + appId;
}

// ---- apps que se atualizam numa pasta nova (Discord e outros do Squirrel: ...\Discord\app-1.0.9268\Discord.exe;
// alguns guardam a versão em ...\Application\131.0.6778.86\...). O Dock guardava o caminho da versão em que o app
// foi fixado e, depois de atualizar, abria a versão velha (que o atualizador às vezes ainda deixa na pasta) ou
// nada. Agora, ao abrir e ao carregar o Dock, cada pedaço do caminho com número de versão é trocado pela pasta
// irmã mais nova que tenha o mesmo resto do caminho; o dock.txt é regravado com o caminho novo.

// o nome com a versão trocada por '#': "app-1.0.9268" -> "app-#", "claude_5.11492.5.0_x64__fnn" -> "claude_#_x64__fnn"
static std::wstring VerKey(const std::wstring& name) {
    std::wstring r;
    size_t i = 0;
    while (i < name.size()) {
        if (iswdigit(name[i])) {
            size_t j = i; bool dot = false;
            // só conta o ponto seguido de número ("1.0.9"), não o da extensão ("cubase15.exe")
            while (j < name.size() && (iswdigit(name[j]) || (name[j] == L'.' && j + 1 < name.size() && iswdigit(name[j + 1])))) { if (name[j] == L'.') dot = true; j++; }
            if (dot) { r += L'#'; i = j; continue; }
            r.append(name, i, j - i); i = j; continue;
        }
        r += (wchar_t)towlower(name[i++]);
    }
    return r;
}

static std::vector<unsigned> VerNums(const std::wstring& name) {
    std::vector<unsigned> v; unsigned cur = 0; bool in = false;
    for (wchar_t c : name) {
        if (iswdigit(c)) { cur = cur * 10 + (c - L'0'); in = true; }
        else { if (in) v.push_back(cur); cur = 0; in = false; }
    }
    if (in) v.push_back(cur);
    return v;
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// o mesmo caminho na versão mais nova instalada (ou o próprio caminho, se não houver outra)
static std::wstring NewestVersion(const std::wstring& path) {
    if (path.size() < 4 || path[1] != L':') return path;  // só caminhos de disco (não shell:, não aumid)
    std::vector<std::wstring> parts;
    size_t a = 0, b;
    while ((b = path.find(L'\\', a)) != std::wstring::npos) { parts.push_back(path.substr(a, b - a)); a = b + 1; }
    parts.push_back(path.substr(a));
    std::wstring prefix = parts[0];
    for (size_t i = 1; i + 1 < parts.size(); i++) {
        const std::wstring& comp = parts[i];
        std::wstring key = VerKey(comp);
        if (key.find(L'#') != std::wstring::npos) {
            std::wstring rest;
            for (size_t k = i + 1; k < parts.size(); k++) rest += L"\\" + parts[k];
            std::wstring best = comp; std::vector<unsigned> bestV = Exists(prefix + L"\\" + comp + rest) ? VerNums(comp) : std::vector<unsigned>{};
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((prefix + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                    if (VerKey(fd.cFileName) != key) continue;
                    std::vector<unsigned> v = VerNums(fd.cFileName);
                    if (v > bestV && Exists(prefix + L"\\" + fd.cFileName + rest)) { best = fd.cFileName; bestV = v; }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            parts[i] = best;
        }
        prefix += L"\\" + parts[i];
    }
    std::wstring r = prefix + L"\\" + parts.back();
    return Exists(r) ? r : path;
}

// troca o caminho velho pelo da versão nova (no exe, no jeito de abrir e no id, se o id era o caminho)
// Os nossos apps fixados (Ajustes, Notas...) apontam para o programa da pasta de onde o Shell roda, com o nome de
// agora: o nome velho (SequoiaSettings.exe) não existe mais depois da troca para Liquinix, e outra pasta (a de
// desenvolvimento) não recebe as atualizações do GitHub (17/11/2026: o Ajustes fixado não abria pelo Dock)
static std::wstring OurApp(const std::wstring& path) {
    std::wstring base = Lower(path.substr(path.find_last_of(L'\\') + 1));
    size_t n = base.rfind(L"sequoia", 0) == 0 ? 7 : base.rfind(L"liquinix", 0) == 0 ? 5 : 0;
    if (!n || base.size() < 5 || base.compare(base.size() - 4, 4, L".exe")) return path;
    std::wstring mine = shell::dir + L"\\Liquinix" + base.substr(n);
    if (GetFileAttributesW(mine.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    return Lower(mine) == Lower(path) ? path : mine;
}

static bool Heal(App& a) {
    if (a.finder) return false;
    std::wstring exe = a.exe.empty() ? L"" : OurApp(NewestVersion(a.exe));
    bool launchIsPath = a.launch.size() > 4 && a.launch[1] == L':' && Lower(a.launch.substr(a.launch.size() - 4)) == L".exe";
    std::wstring launch = launchIsPath ? OurApp(NewestVersion(a.launch)) : a.launch;
    if (Lower(exe) == Lower(a.exe) && launch == a.launch) return false;
    Log(L"dock: %ls atualizou: %ls -> %ls", a.name.c_str(), (launchIsPath ? a.launch : a.exe).c_str(), (launchIsPath ? launch : exe).c_str());
    if (!a.id.empty() && a.id == Lower(a.exe)) a.id = Lower(exe);
    a.exe = Lower(exe);
    a.launch = launch;
    return true;
}

void apps::Launch(App& a, const std::wstring& args) {
    a.launchedAt = GetTickCount();
    if (a.pinned && Heal(a)) Save();
    for (const std::wstring* c : { &a.launch, &a.exe }) {
        std::wstring aumid = PackagedAumid(*c);
        if (aumid.empty()) continue;
        std::wstring target = L"shell:AppsFolder\\" + aumid;
        // Spotify e outros feitos sobre o Chromium: abertos sem a "oclusão" (senão param de desenhar quando a
        // janela nasce escondida para a animação de abrir e ficam parados)
        if (LaunchNoOcclusion(target, args, a.exe.empty() ? *c : a.exe) || ActivatePackaged(target, args)) {
            Log(L"dock: abrindo %ls (pacote %ls)", a.name.c_str(), aumid.c_str());
            return;
        }
    }
    if (shell::OpenCalculator(a.id + L"|" + a.launch + L"|" + a.exe)) return;  // a Calculadora do Windows fixada: abre a nossa
    if (LaunchNoOcclusion(a.launch, args, a.exe)) return;
    std::wstring lower = a.launch + L"|" + a.exe;
    for (auto& ch : lower) ch = towlower(ch);
    if (lower.find(L"immersivecontrolpanel") != std::wstring::npos || lower.find(L"systemsettings.exe") != std::wstring::npos) {
        shell::OpenSettings(L"");  // Ajustes do Windows fixado no Dock: abre o nosso
        return;
    }
    if (ActivatePackaged(a.launch, args)) { Log(L"dock: abrindo %ls", a.name.c_str()); return; }  // empacotado: sem segurar a interface
    ShellExecuteW(nullptr, L"open", a.launch.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
    Log(L"dock: abrindo %ls", a.name.c_str());
}

static void ForceForeground(HWND h) {
    if (SetForegroundWindow(h)) return;
    // o clique foi para o nosso processo, então costuma funcionar; senão, o truque do Alt
    keybd_event(VK_MENU, 0, 0, 0);
    SetForegroundWindow(h);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
}

// Ocultar: por baixo é minimizar (o Windows não deixa esconder janela de outro programa com segurança),
// mas o Shell marca as janelas: sem o gênio, sem a miniatura no Dock, e o clique no ícone traz todas.
static std::map<HWND, DWORD> g_hiddenWins;  // janela -> quando foi oculta

bool apps::IsHidden(HWND h) {
    auto it = g_hiddenWins.find(h);
    if (it == g_hiddenWins.end()) return false;
    if (GetTickCount() - it->second < 1500) return true;  // ainda minimizando
    if (!IsWindow(h) || !IsIconic(h)) { g_hiddenWins.erase(it); return false; }  // voltou por outro caminho (Alt+Tab)
    return true;
}

void apps::Activate(App& a) {
    if (a.wins.empty()) { Launch(a); return; }
    {   // estava oculto: todas as janelas voltam, na ordem em que estavam
        std::vector<HWND> back;
        for (auto& w : a.wins) if (IsHidden(w.hwnd)) back.push_back(w.hwnd);
        if (!back.empty()) {
            for (int i = (int)back.size() - 1; i >= 0; i--) { g_hiddenWins.erase(back[i]); ShowWindow(back[i], SW_SHOWNOACTIVATE); }
            ForceForeground(back[0]);
            Log(L"dock: %ls volta (%zu janelas)", a.name.c_str(), back.size());
            return;
        }
    }
    bool anyShown = false;
    for (auto& w : a.wins) if (!w.minimized) anyShown = true;
    if (!anyShown) {  // tudo minimizado: volta a mais recente
        shell::Restaurar(a.wins[0].hwnd);
        ForceForeground(a.wins[0].hwnd);
        return;
    }
    for (int i = (int)a.wins.size() - 1; i >= 0; i--)  // de trás para frente: a ordem entre elas se mantém
        if (!a.wins[i].minimized) SetWindowPos(a.wins[i].hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    for (auto& w : a.wins) if (!w.minimized) { ForceForeground(w.hwnd); break; }
}

void apps::Hide(App& a) {
    DWORD now = GetTickCount();
    std::vector<HWND> hs;
    for (auto& w : a.wins) if (!w.minimized && !IsIconic(w.hwnd)) { g_hiddenWins[w.hwnd] = now; hs.push_back(w.hwnd); }
    for (HWND h : hs) ShowWindow(h, SW_MINIMIZE);  // somem na hora (a animação do Windows está desligada); a frente passa para o app de trás
    Log(L"dock: %ls oculto (%zu janelas)", a.name.c_str(), hs.size());
}
// Encerrar = como o "Finalizar tarefa": mata os processos das janelas do app na hora, sem perguntar.
// Apps da loja vivem dentro do ApplicationFrameHost (que hospeda todos eles): ali vale o processo da
// janela de dentro, nunca o hospedeiro. O Arquivos (o Finder) e o Explorer só recebem o pedido de fechar.
void apps::Quit(App& a) {
    std::vector<DWORD> pids;
    for (auto& w : a.wins) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w.hwnd, &pid);
        std::wstring exe = ExeOfWindow(w.hwnd);
        exe = exe.substr(exe.find_last_of(L'\\') + 1);  // só o nome do arquivo, minúsculo e sem ".exe"
        for (auto& c : exe) c = towlower(c);
        if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, L".exe") == 0) exe.resize(exe.size() - 4);
        if (exe == L"applicationframehost") {
            struct Find { DWORD host, inner; } f{ pid, 0 };
            EnumChildWindows(w.hwnd, [](HWND c, LPARAM l) -> BOOL {
                auto* f = (Find*)l;
                DWORD p = 0; GetWindowThreadProcessId(c, &p);
                if (p && p != f->host) { f->inner = p; return FALSE; }
                return TRUE;
            }, (LPARAM)&f);
            pid = f.inner;
        }
        if (a.finder || !pid || pid == GetCurrentProcessId() || exe == L"explorer" || exe == L"shell" || exe == L"sequoiashell") { PostMessageW(w.hwnd, WM_CLOSE, 0, 0); continue; }
        if (std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
    }
    for (DWORD pid : pids) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
    }
    Log(L"dock: encerrado %ls (%zu processos)", a.name.c_str(), pids.size());
}

void apps::SetPinned(App& a, bool pin) {
    a.pinned = pin;
    if (!pin && a.wins.empty()) {
        std::wstring id = a.id;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const App& x) { return x.id == id; }), list.end());
    }
    std::stable_partition(list.begin(), list.end(), [](const App& x) { return x.pinned; });
    Save();
}

void apps::ForgetRecent(const std::wstring& id) {
    recent.erase(std::remove(recent.begin(), recent.end(), id), recent.end());
    list.erase(std::remove_if(list.begin(), list.end(), [&](const App& x) { return x.id == id && !x.pinned && x.wins.empty(); }), list.end());
}

void apps::Move(int from, int to) {
    if (from == to || from <= 0 || to <= 0 || from >= (int)list.size() || to >= (int)list.size()) return;  // o Arquivos não sai do lugar
    App a = list[from];
    list.erase(list.begin() + from);
    list.insert(list.begin() + to, a);
    Save();
}

static std::wstring StartupLink(const App& a) {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p))) { d = p; CoTaskMemFree(p); }
    std::wstring n = a.name;
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return d + L"\\" + n + L".lnk";
}

bool apps::OpenAtLogin(const App& a) { return GetFileAttributesW(StartupLink(a).c_str()) != INVALID_FILE_ATTRIBUTES; }

void apps::SetOpenAtLogin(const App& a, bool on) {
    std::wstring lnk = StartupLink(a);
    if (!on) { DeleteFileW(lnk.c_str()); return; }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(a.launch.c_str(), nullptr, &pidl, 0, nullptr))) return;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetIDList(pidl);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
        sl->Release();
    }
    CoTaskMemFree(pidl);
}

void apps::ShowInFinder(const App& a) {
    std::wstring target = a.exe;
    if (target.empty() && a.launch.size() > 4 && Lower(a.launch.substr(a.launch.size() - 4)) == L".lnk") {
        std::wstring aumid; ReadLink(a.launch, target, aumid);
    }
    if (target.empty()) target = a.launch;
    ShellExecuteW(nullptr, L"open", g_finderExe.c_str(), (L"\"" + target + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
