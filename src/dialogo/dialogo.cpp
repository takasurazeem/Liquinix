// DialogoMac.dll: troca a janela de Abrir/Salvar do Windows (IFileOpenDialog / IFileSaveDialog)
// pelo Finder em modo escolher (Arquivos.exe --escolher ...).
//
// Registrada só para o usuário (HKCU\Software\Classes\CLSID\{...}\InprocServer32), no lugar das
// classes do comdlg32. Para cada pedido:
//   - programa fora da lista (%APPDATA%\Shell\dialogo.txt) → entrega o diálogo original na hora;
//   - na lista → cria o diálogo original por dentro e repassa a ele todas as chamadas (tipos,
//     controles extras, eventos...); só o Show abre o Finder, e depois GetResult/GetResults/
//     GetFileName devolvem o que foi escolhido. Shift segurado no Show, ou Finder ausente,
//     mostra o diálogo original.
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cstdio>

static HMODULE g_self;
#ifdef DIALOGO_NATIVO
// MacDialogo.dll (barra-nativa): troca as classes no HKLM e traz o Finder dentro dela (FinderEscolher, src\finder):
// o escolher roda no próprio programa, sem WacOSFinder.exe. Vale para todos os programas (sem dialogo.txt);
// C:\Users\Public\MacDialogo-desligado.txt devolve o diálogo do Windows.
int FinderEscolher(HINSTANCE hi, const std::wstring& dirBin, const std::wstring& cmd);
static const wchar_t* PASTA_SHELL = L"C:\\WacOS\\bin";
#endif
static const CLSID CLSID_Open = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
static const CLSID CLSID_Save = { 0xC0B4E2F3, 0xBA21, 0x4773, { 0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B } };

static std::wstring SelfDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s = p; return s.substr(0, s.find_last_of(L'\\'));
}

static void Log(const wchar_t* fmt, ...) {
    FILE* f = nullptr;
#ifdef DIALOGO_NATIVO
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    if (_wfopen_s(&f, (std::wstring(tmp) + L"MacDialogo-com.log").c_str(), L"a, ccs=UTF-8") || !f) return;
#else
    if (_wfopen_s(&f, (SelfDir() + L"\\dialogo.log").c_str(), L"a, ccs=UTF-8") || !f) return;
#endif
    va_list a; va_start(a, fmt); vfwprintf(f, fmt, a); va_end(a);
    fputwc(L'\n', f); fclose(f);
}

// O original do Windows (comdlg32.dll) para esta classe.
static HRESULT RealFactory(REFCLSID clsid, IClassFactory** out) {
    static HMODULE comdlg = LoadLibraryW(L"comdlg32.dll");
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    auto fn = comdlg ? (Fn)GetProcAddress(comdlg, "DllGetClassObject") : nullptr;
    if (!fn) return CLASS_E_CLASSNOTAVAILABLE;
    return fn(clsid, IID_IClassFactory, (void**)out);
}

// Programa na lista? (%APPDATA%\Shell\dialogo.txt, um .exe por linha; "*" = todos)
static bool Allowed() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = PathFindFileNameW(exe);
    for (auto& c : name) c = towlower(c);
    if (name == L"arquivos.exe" || name == L"wacosfinder.exe") return false;
#ifdef DIALOGO_NATIVO
    return GetFileAttributesW(L"C:\\Users\\Public\\MacDialogo-desligado.txt") == INVALID_FILE_ATTRIBUTES;
#endif
    wchar_t list[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", list, MAX_PATH)) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(list) + L"\\Shell\\dialogo.txt").c_str(), L"r, ccs=UTF-8") || !f) return name == L"dialogoteste.exe";
    wchar_t line[260];
    bool ok = false;
    while (fgetws(line, 260, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r' || l.back() == L' ')) l.pop_back();
        for (auto& c : l) c = towlower(c);
        if (l == L"*" || l == name) { ok = true; break; }
    }
    fclose(f);
    return ok;
}

static IShellItem* ItemFromPath(const std::wstring& path) {  // vale também para arquivo que ainda não existe
    PIDLIST_ABSOLUTE pidl = SHSimpleIDListFromPath(path.c_str());
    IShellItem* it = nullptr;
    if (pidl) { SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&it)); CoTaskMemFree(pidl); }
    return it;
}

static std::wstring PathOf(IShellItem* it) {
    PWSTR p = nullptr; std::wstring s;
    if (it && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { s = p; CoTaskMemFree(p); }
    return s;
}

// IFileDialog2 também: o Bloco de Notas (e outros) pede ela e chama o Show por ela; repassada ao original,
// o diálogo do Windows aparecia
class Wrap : public IFileOpenDialog, public IFileSaveDialog, public IFileDialog2 {
    LONG ref = 1;
    bool save;
    IFileDialog* inner;              // o original
    IFileOpenDialog* innerOpen = nullptr;
    IFileSaveDialog* innerSave = nullptr;
    // o que o programa configurou
    std::vector<std::pair<std::wstring, std::wstring>> types;
    UINT typeIndex = 1;
    FILEOPENDIALOGOPTIONS opts = 0;
    std::wstring folder, defFolder, fileName, okLabel, defExt;
    IFileDialogEvents* events = nullptr;
    // o que foi escolhido no Finder
    bool chosen = false;
    std::vector<std::wstring> results;

    IFileDialog* Self() { return save ? (IFileDialog*)(IFileSaveDialog*)this : (IFileDialog*)(IFileOpenDialog*)this; }

public:
    Wrap(bool isSave, IFileDialog* in) : save(isSave), inner(in) {
        inner->QueryInterface(IID_PPV_ARGS(&innerOpen));
        inner->QueryInterface(IID_PPV_ARGS(&innerSave));
    }
    ~Wrap() {
        if (events) events->Release();
        if (innerOpen) innerOpen->Release();
        if (innerSave) innerSave->Release();
        inner->Release();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** o) override {
        if (iid == IID_IUnknown || iid == IID_IModalWindow || iid == IID_IFileDialog) { *o = Self(); AddRef(); return S_OK; }
        if (iid == IID_IFileOpenDialog && !save) { *o = (IFileOpenDialog*)this; AddRef(); return S_OK; }
        if (iid == IID_IFileSaveDialog && save) { *o = (IFileSaveDialog*)this; AddRef(); return S_OK; }
        if (iid == IID_IFileDialog2) {  // só se o original também tem (para o repasse das duas funções dela)
            IFileDialog2* d2 = nullptr;
            if (FAILED(inner->QueryInterface(IID_PPV_ARGS(&d2)))) { *o = nullptr; return E_NOINTERFACE; }
            d2->Release();
            *o = (IFileDialog2*)this; AddRef(); return S_OK;
        }
        return inner->QueryInterface(iid, o);  // controles extras, IObjectWithSite... ficam com o original
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    // IModalWindow
    STDMETHODIMP Show(HWND owner) override {
#ifdef DIALOGO_NATIVO
        std::wstring finder = std::wstring(PASTA_SHELL) + L"\\WacOSFinder.exe";
        if (GetKeyState(VK_SHIFT) & 0x8000) return inner->Show(owner);  // escape: o original
#else
        std::wstring finder = SelfDir() + L"\\WacOSFinder.exe";
        if ((GetKeyState(VK_SHIFT) & 0x8000) || GetFileAttributesW(finder.c_str()) == INVALID_FILE_ATTRIBUTES)
            return inner->Show(owner);  // escape: o original
#endif
        chosen = false; results.clear();
        std::wstring start = !folder.empty() ? folder : !defFolder.empty() ? defFolder : L"";
        if (start.empty()) { PWSTR d = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &d))) { start = d; CoTaskMemFree(d); } }
        wchar_t tmp[MAX_PATH], res[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        GetTempFileNameW(tmp, L"dlg", 0, res);
        auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
        std::wstring cmd = q(finder) + L" " + q(start) + L" --lista --escolher " +
                           ((opts & FOS_PICKFOLDERS) ? L"pasta" : save ? L"salvar" : L"abrir");
        if (opts & FOS_ALLOWMULTISELECT) cmd += L" --multi";
        if (save && !(opts & FOS_OVERWRITEPROMPT)) cmd += L" --sem-confirmar";
        if (!types.empty()) {
            std::wstring t;
            for (auto& [d, sp] : types) { if (!t.empty()) t += L"||"; t += d + L"|" + sp; }
            cmd += L" --tipos " + q(t) + L" --tipo " + std::to_wstring(typeIndex > 0 ? typeIndex - 1 : 0);
        }
        if (!fileName.empty()) cmd += L" --nome " + q(fileName);
        if (!okLabel.empty()) cmd += L" --ok " + q(okLabel);
        if (!defExt.empty()) cmd += L" --ext " + q(defExt);
        cmd += L" --resultado " + q(res);
        if (owner) cmd += L" --pai " + std::to_wstring((ULONG_PTR)GetAncestor(owner, GA_ROOT));
        Log(L"Show: %ls", cmd.c_str());
        HWND root = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
        DWORD code = 1;
        bool feito = false;
#ifdef DIALOGO_NATIVO
        {   // o Finder aqui dentro, na thread do programa (a linha sem o caminho do exe)
            std::wstring args = cmd.substr(q(finder).size() + 1);
            if (root) EnableWindow(root, FALSE);  // modal, como o original
            int r = FinderEscolher(g_self, PASTA_SHELL, args);
            if (root) { EnableWindow(root, TRUE); SetForegroundWindow(root); }
            if (r >= 0) { code = (DWORD)r; feito = true; }
            else Log(L"  Finder embutido indisponível (outra thread?): pelo exe");
        }
#endif
        if (!feito) {
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                Log(L"  não abriu o Finder (%lu): diálogo original", GetLastError());
                DeleteFileW(res);
                return inner->Show(owner);
            }
            AllowSetForegroundWindow(pi.dwProcessId);
            if (root) EnableWindow(root, FALSE);  // modal, como o original
            for (;;) {  // espera o Finder fechar sem travar o programa (mensagens continuam)
                DWORD w = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
                if (w == WAIT_OBJECT_0) break;
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            }
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            if (root) { EnableWindow(root, TRUE); SetForegroundWindow(root); }
        }
        if (code == 0) {
            FILE* f = nullptr;
            if (!_wfopen_s(&f, res, L"r, ccs=UTF-8") && f) {
                wchar_t line[2048];
                while (fgetws(line, 2048, f)) {
                    std::wstring l = line;
                    while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
                    if (l.rfind(L"tipo=", 0) == 0) typeIndex = (UINT)_wtoi(l.c_str() + 5) + 1;
                    else if (!l.empty()) results.push_back(l);
                }
                fclose(f);
            }
        }
        DeleteFileW(res);
        if (results.empty()) { Log(L"  cancelado"); return HRESULT_FROM_WIN32(ERROR_CANCELLED); }
        chosen = true;
        Log(L"  escolhido: %ls", results[0].c_str());
        // o original fica coerente, para quem perguntar a ele (ex.: controles extras)
        inner->SetFileTypeIndex(typeIndex);
        std::wstring first = results[0];
        inner->SetFileName(PathFindFileNameW(first.c_str()));
        if (events) events->OnFileOk(Self());
        return S_OK;
    }

    // IFileDialog: guarda o que interessa e repassa tudo ao original
    STDMETHODIMP SetFileTypes(UINT n, const COMDLG_FILTERSPEC* s) override {
        types.clear();
        for (UINT i = 0; i < n; i++) types.push_back({ s[i].pszName ? s[i].pszName : L"", s[i].pszSpec ? s[i].pszSpec : L"*.*" });
        return inner->SetFileTypes(n, s);
    }
    STDMETHODIMP SetFileTypeIndex(UINT i) override { typeIndex = i; return inner->SetFileTypeIndex(i); }
    STDMETHODIMP GetFileTypeIndex(UINT* i) override { if (chosen && i) { *i = typeIndex; return S_OK; } return inner->GetFileTypeIndex(i); }
    STDMETHODIMP Advise(IFileDialogEvents* e, DWORD* c) override {
        if (e) { if (events) events->Release(); events = e; e->AddRef(); }
        return inner->Advise(e, c);
    }
    STDMETHODIMP Unadvise(DWORD c) override { if (events) { events->Release(); events = nullptr; } return inner->Unadvise(c); }
    STDMETHODIMP SetOptions(FILEOPENDIALOGOPTIONS o) override { opts = o; return inner->SetOptions(o); }
    STDMETHODIMP GetOptions(FILEOPENDIALOGOPTIONS* o) override { return inner->GetOptions(o); }
    STDMETHODIMP SetDefaultFolder(IShellItem* s) override { defFolder = PathOf(s); return inner->SetDefaultFolder(s); }
    STDMETHODIMP SetFolder(IShellItem* s) override { folder = PathOf(s); return inner->SetFolder(s); }
    STDMETHODIMP GetFolder(IShellItem** s) override {
        if (chosen && s) {
            std::wstring p = results[0];
            if (!(opts & FOS_PICKFOLDERS)) { p.resize(p.find_last_of(L'\\')); if (p.size() == 2) p += L"\\"; }
            *s = ItemFromPath(p);
            return *s ? S_OK : E_FAIL;
        }
        return inner->GetFolder(s);
    }
    STDMETHODIMP GetCurrentSelection(IShellItem** s) override { if (chosen) return GetResult(s); return inner->GetCurrentSelection(s); }
    STDMETHODIMP SetFileName(LPCWSTR n) override { fileName = n ? n : L""; return inner->SetFileName(n); }
    STDMETHODIMP GetFileName(LPWSTR* n) override {
        if (chosen && n) return SHStrDupW(PathFindFileNameW(results[0].c_str()), n);
        return inner->GetFileName(n);
    }
    STDMETHODIMP SetTitle(LPCWSTR t) override { return inner->SetTitle(t); }
    STDMETHODIMP SetOkButtonLabel(LPCWSTR t) override { okLabel = t ? t : L""; return inner->SetOkButtonLabel(t); }
    STDMETHODIMP SetFileNameLabel(LPCWSTR t) override { return inner->SetFileNameLabel(t); }
    STDMETHODIMP GetResult(IShellItem** s) override {
        if (!s) return E_POINTER;
        if (!chosen) return inner->GetResult(s);
        *s = ItemFromPath(results[0]);
        return *s ? S_OK : E_FAIL;
    }
    STDMETHODIMP AddPlace(IShellItem* s, FDAP a) override { return inner->AddPlace(s, a); }
    STDMETHODIMP SetDefaultExtension(LPCWSTR e) override { defExt = e ? e : L""; return inner->SetDefaultExtension(e); }
    STDMETHODIMP Close(HRESULT h) override { return inner->Close(h); }
    STDMETHODIMP SetClientGuid(REFGUID g) override { return inner->SetClientGuid(g); }
    STDMETHODIMP ClearClientData() override { return inner->ClearClientData(); }
    STDMETHODIMP SetFilter(IShellItemFilter* f) override { return inner->SetFilter(f); }

    // IFileDialog2
    STDMETHODIMP SetCancelButtonLabel(LPCWSTR t) override {
        IFileDialog2* d2 = nullptr;
        if (FAILED(inner->QueryInterface(IID_PPV_ARGS(&d2)))) return E_NOTIMPL;
        HRESULT hr = d2->SetCancelButtonLabel(t); d2->Release(); return hr;
    }
    STDMETHODIMP SetNavigationRoot(IShellItem* i) override {
        if (!folder.size() && !defFolder.size()) defFolder = PathOf(i);
        IFileDialog2* d2 = nullptr;
        if (FAILED(inner->QueryInterface(IID_PPV_ARGS(&d2)))) return E_NOTIMPL;
        HRESULT hr = d2->SetNavigationRoot(i); d2->Release(); return hr;
    }

    // IFileOpenDialog
    STDMETHODIMP GetResults(IShellItemArray** a) override {
        if (!a) return E_POINTER;
        if (!chosen) return innerOpen ? innerOpen->GetResults(a) : E_UNEXPECTED;
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (auto& r : results) if (auto p = SHSimpleIDListFromPath(r.c_str())) pidls.push_back(p);
        HRESULT hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), a);
        for (auto p : pidls) CoTaskMemFree(p);
        return hr;
    }
    STDMETHODIMP GetSelectedItems(IShellItemArray** a) override { if (chosen) return GetResults(a); return innerOpen ? innerOpen->GetSelectedItems(a) : E_UNEXPECTED; }

    // IFileSaveDialog
    STDMETHODIMP SetSaveAsItem(IShellItem* s) override {
        std::wstring p = PathOf(s);
        if (!p.empty()) { fileName = PathFindFileNameW(p.c_str()); std::wstring d = p; d.resize(d.find_last_of(L'\\')); folder = d; }
        return innerSave ? innerSave->SetSaveAsItem(s) : E_UNEXPECTED;
    }
    STDMETHODIMP SetProperties(IPropertyStore* s) override { return innerSave ? innerSave->SetProperties(s) : E_UNEXPECTED; }
    STDMETHODIMP SetCollectedProperties(IPropertyDescriptionList* l, BOOL b) override { return innerSave ? innerSave->SetCollectedProperties(l, b) : E_UNEXPECTED; }
    STDMETHODIMP GetProperties(IPropertyStore** s) override { return innerSave ? innerSave->GetProperties(s) : E_UNEXPECTED; }
    STDMETHODIMP ApplyProperties(IShellItem* i, IPropertyStore* s, HWND h, IFileOperationProgressSink* k) override { return innerSave ? innerSave->ApplyProperties(i, s, h, k) : E_UNEXPECTED; }
};

class Factory : public IClassFactory {
    LONG ref = 1;
    CLSID clsid;
public:
    explicit Factory(REFCLSID c) : clsid(c) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** o) override {
        if (iid == IID_IUnknown || iid == IID_IClassFactory) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** o) override {
        *o = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        IClassFactory* real = nullptr;
        HRESULT hr = RealFactory(clsid, &real);
        if (FAILED(hr)) return hr;
        IFileDialog* inner = nullptr;
        hr = real->CreateInstance(nullptr, IID_PPV_ARGS(&inner));
        real->Release();
        if (FAILED(hr)) return hr;
        auto* w = new Wrap(clsid == CLSID_Save, inner);
        hr = w->QueryInterface(iid, o);
        w->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** o) {
    if (clsid != CLSID_Open && clsid != CLSID_Save) return CLASS_E_CLASSNOTAVAILABLE;
    if (!Allowed()) {  // fora da lista: o original, direto
        IClassFactory* real = nullptr;
        HRESULT hr = RealFactory(clsid, &real);
        if (FAILED(hr)) return hr;
        hr = real->QueryInterface(iid, o);
        real->Release();
        return hr;
    }
    auto* f = new Factory(clsid);
    HRESULT hr = f->QueryInterface(iid, o);
    f->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

#ifdef DIALOGO_NATIVO
// para o Shell nativo (dialogs.cpp, dentro do explorer): o escolher para programas que usam a API antiga
// (GetOpenFileName/GetSaveFileName não passam pelas classes do registro). Sempre da mesma thread.
extern "C" int WINAPI MacEscolher(const wchar_t* args) { return FinderEscolher(g_self, PASTA_SHELL, args ? args : L""); }
#ifdef _WIN64
#pragma comment(linker, "/EXPORT:MacEscolher")
#else
#pragma comment(linker, "/EXPORT:MacEscolher=_MacEscolher@4")
#endif
#endif  // fica carregada: mais seguro

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

#ifdef _WIN64
#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
#else  // 32 bits: __stdcall decora os nomes
#pragma comment(linker, "/EXPORT:DllGetClassObject=_DllGetClassObject@12,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow=_DllCanUnloadNow@0,PRIVATE")
#endif
