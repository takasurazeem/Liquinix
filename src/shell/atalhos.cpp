// Itens do próprio app no menu do Dock (o que a barra de tarefas do Windows chama de "lista de atalhos"):
//  - tarefas do app ("Nova janela", "Nova janela anônima" do Chrome; Firefox, Edge, AnyDesk...). Ficam em
//    %APPDATA%\Microsoft\Windows\Recent\CustomDestinations\*.customDestinations-ms: cada item é o CLSID do
//    ShellLink seguido do atalho gravado. Não há API pública para ler; o nome do arquivo é um hash da
//    identidade do app, então o arquivo é ligado ao app pelo programa que os atalhos abrem (o .exe dele);
//    com vários (perfis do Chrome), vale o mais recente.
//  - recentes do app (IApplicationDocumentLists, API documentada) pela identidade dele: a do app (AUMID) ou,
//    sem ela, a que o Windows deriva do caminho do .exe ({pasta conhecida}\resto\do\caminho).
#include "shell.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propkey.h>
#include <knownfolders.h>
#include <map>
#include <thread>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <algorithm>

namespace {

struct Cached { FILETIME mt{}; std::vector<atalhos::Item> items; std::wstring exe; };
std::map<std::wstring, Cached> g_files;  // arquivo -> tarefas lidas

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

void ReadCustom(const std::wstring& file, Cached& c) {
    c.items.clear(); c.exe.clear();
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, nullptr), rd = 0;
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) { CloseHandle(f); return; }
    std::vector<BYTE> b(size);
    ReadFile(f, b.data(), size, &rd, nullptr);
    CloseHandle(f);
    const CLSID cl = CLSID_ShellLink;
    for (size_t i = 0; i + 16 < b.size(); i++) {
        if (memcmp(&b[i], &cl, 16)) continue;
        IStream* st = SHCreateMemStream(&b[i + 16], (UINT)(b.size() - i - 16));
        IShellLinkW* sl = nullptr; IPersistStream* ps = nullptr;
        if (st && SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
            SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps))) && SUCCEEDED(ps->Load(st))) {
            atalhos::Item it;
            wchar_t p[MAX_PATH] = {}, a[2048] = {}, d[MAX_PATH] = {};
            sl->GetPath(p, MAX_PATH, nullptr, SLGP_RAWPATH);
            sl->GetArguments(a, 2048);
            sl->GetWorkingDirectory(d, MAX_PATH);
            it.path = p; it.args = a; it.dir = d;
            IPropertyStore* pst = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pst)))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(pst->GetValue(PKEY_Title, &v)) && v.vt == VT_LPWSTR && v.pwszVal) it.title = v.pwszVal;
                PropVariantClear(&v);
                pst->Release();
            }
            // título indireto ("@C:\...\app.dll,-123"): o texto de verdade
            if (!it.title.empty() && it.title[0] == L'@') {
                wchar_t out[256] = {};
                if (SUCCEEDED(SHLoadIndirectString(it.title.c_str(), out, 256, nullptr))) it.title = out;
            }
            if (!it.title.empty() && !it.path.empty()) {
                if (c.exe.empty()) c.exe = Lower(it.path);
                c.items.push_back(it);
            }
        }
        if (ps) ps->Release();
        if (sl) sl->Release();
        if (st) st->Release();
    }
}

std::wstring RecentDir() {
    wchar_t* rec = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &rec))) out = rec;
    CoTaskMemFree(rec);
    return out;
}

// a identidade que o Windows dá a um .exe sem AUMID: a pasta conhecida trocada pelo GUID dela
std::vector<std::wstring> ImplicitIds(const std::wstring& exe) {
    std::vector<std::wstring> ids;
    const KNOWNFOLDERID* kfs[] = { &FOLDERID_ProgramFilesX64, &FOLDERID_ProgramFilesX86, &FOLDERID_System, &FOLDERID_Windows, &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData, &FOLDERID_Profile };
    std::wstring le = Lower(exe);
    for (auto* kf : kfs) {
        wchar_t* p = nullptr;
        if (FAILED(SHGetKnownFolderPath(*kf, 0, nullptr, &p))) continue;
        std::wstring base = Lower(p); CoTaskMemFree(p);
        if (!base.empty() && le.rfind(base + L"\\", 0) == 0) {
            wchar_t g[64]; StringFromGUID2(*kf, g, 64);
            ids.push_back(std::wstring(g) + exe.substr(base.size()));
        }
    }
    ids.push_back(exe);
    return ids;
}

// Ableton Live não usa a lista do Windows: os projetos recentes ("Open Recent Set") ficam dentro do
// Preferences.cfg dele, como caminhos completos em UTF-16 (em posições pares e ímpares do arquivo). Lidos na
// ordem em que estão gravados (o mais recente primeiro), sem repetir e só os que ainda existem.
std::vector<atalhos::Item> AbletonRecent(int max) {
    std::vector<atalhos::Item> out;
    wchar_t* app = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app))) return out;
    std::wstring base = std::wstring(app) + L"\\Ableton\\"; CoTaskMemFree(app);
    // a versão mais usada por último (Live 12.4, Live 12.3...)
    std::wstring cfg; FILETIME best{};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"Live *").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring c = base + fd.cFileName + L"\\Preferences\\Preferences.cfg";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(c.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &best) > 0) { best = a.ftLastWriteTime; cfg = c; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (cfg.empty()) return out;
    HANDLE f = CreateFileW(cfg.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(f, nullptr), rd = 0;
    if (size == INVALID_FILE_SIZE || size > 8 * 1024 * 1024) { CloseHandle(f); return out; }
    std::vector<BYTE> b(size);
    ReadFile(f, b.data(), size, &rd, nullptr);
    CloseHandle(f);
    // ".als" em UTF-16, em qualquer posição: volta até o começo do caminho (letra + ":/")
    std::vector<std::pair<size_t, std::wstring>> found;
    const BYTE pat[8] = { '.', 0, 'a', 0, 'l', 0, 's', 0 };
    for (size_t i = 8; i + 8 <= b.size(); i++) {
        if (memcmp(&b[i], pat, 8)) continue;
        size_t st = i, lim = i > 1040 ? i - 1040 : 0;
        while (st >= 2 && st > lim) {
            wchar_t c = (wchar_t)(b[st - 2] | (b[st - 1] << 8));
            if (c < 0x20 || c == L'"' || c == L'<' || c == L'>' || c == L'|') break;
            st -= 2;
        }
        std::wstring path;
        for (size_t k = st; k < i + 8; k += 2) path.push_back((wchar_t)(b[k] | (b[k + 1] << 8)));
        size_t colon = path.find(L":/");
        if (colon == std::wstring::npos) colon = path.find(L":\\");
        if (colon == std::wstring::npos || colon < 1) continue;
        path = path.substr(colon - 1);
        for (auto& c : path) if (c == L'/') c = L'\\';
        found.push_back({ st, path });
    }
    std::sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.first < y.first; });
    for (auto& [pos, path] : found) {
        if ((int)out.size() >= max) break;
        bool dup = false;
        for (auto& o : out) if (!_wcsicmp(o.path.c_str(), path.c_str())) dup = true;
        if (dup || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        atalhos::Item it;
        it.path = path; it.recent = true;
        std::wstring name = PathFindFileNameW(path.c_str());
        if (name.size() > 4) name.resize(name.size() - 4);
        it.title = name;
        out.push_back(it);
    }
    return out;
}

void AddRecent(std::vector<atalhos::Item>& out, const std::wstring& path, int max) {
    if ((int)out.size() >= max || path.empty()) return;
    for (auto& o : out) if (!_wcsicmp(o.path.c_str(), path.c_str())) return;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    atalhos::Item it;
    it.path = path; it.recent = true;
    std::wstring name = PathFindFileNameW(path.c_str());
    if (const wchar_t* ext = PathFindExtensionW(name.c_str()); ext && *ext) name.resize(ext - name.c_str());
    it.title = name;
    out.push_back(it);
}

// FL Studio: HKCU\Software\Image-Line\FL Studio <versão>\MRU, valores "0", "1"... (o mais recente primeiro)
std::vector<atalhos::Item> FlRecent(int max) {
    std::vector<atalhos::Item> out;
    HKEY il;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Image-Line", 0, KEY_READ, &il)) return out;
    wchar_t sub[256];
    for (DWORD i = 0; (int)out.size() < max; i++) {
        DWORD n = 256;
        if (RegEnumKeyExW(il, i, sub, &n, nullptr, nullptr, nullptr, nullptr)) break;
        if (_wcsnicmp(sub, L"FL Studio", 9)) continue;
        std::wstring mru = std::wstring(sub) + L"\\MRU";
        for (int k = 0; k < 20 && (int)out.size() < max; k++) {
            wchar_t v[MAX_PATH * 2] = {}; DWORD cb = sizeof(v);
            if (RegGetValueW(il, mru.c_str(), std::to_wstring(k).c_str(), RRF_RT_REG_SZ, nullptr, v, &cb) == ERROR_SUCCESS) AddRecent(out, v, max);
        }
    }
    RegCloseKey(il);
    return out;
}

// Cubase: %APPDATA%\Steinberg\Cubase <versão>_64\Defaults.xml, lista "GRecentDocumentPaths" (Name + Path)
std::vector<atalhos::Item> CubaseRecent(int max) {
    std::vector<atalhos::Item> out;
    wchar_t* app = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app))) return out;
    std::wstring base = std::wstring(app) + L"\\Steinberg\\"; CoTaskMemFree(app);
    std::wstring xml; FILETIME best{};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"Cubase*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring c = base + fd.cFileName + L"\\Defaults.xml";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(c.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &best) > 0) { best = a.ftLastWriteTime; xml = c; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    HANDLE f = xml.empty() ? INVALID_HANDLE_VALUE : CreateFileW(xml.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(f, nullptr), rd = 0;
    std::string t(size, 0);
    ReadFile(f, t.data(), size, &rd, nullptr);
    CloseHandle(f);
    size_t at = t.find("GRecentDocumentPaths");
    if (at == std::string::npos) return out;
    auto value = [&](size_t from, const char* name, size_t& end) -> std::string {
        std::string key = std::string("name=\"") + name + "\" value=\"";
        size_t k = t.find(key, from);
        if (k == std::string::npos) return {};
        k += key.size();
        size_t e = t.find('"', k);
        end = e;
        return e == std::string::npos ? std::string() : t.substr(k, e - k);
    };
    size_t pos = at;
    while ((int)out.size() < max) {
        size_t e1 = 0, e2 = 0;
        std::string nm = value(pos, "Name", e1);
        if (nm.empty()) break;
        if (nm.size() < 5 || _stricmp(nm.c_str() + nm.size() - 4, ".cpr")) { pos = e1; continue; }  // outro campo "Name" (o tipo do arquivo)
        std::string dir = value(e1, "Path", e2);
        if (dir.empty()) break;
        pos = e2;
        int n = MultiByteToWideChar(CP_UTF8, 0, (dir + nm).c_str(), -1, nullptr, 0);
        std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, (dir + nm).c_str(), -1, w.data(), n);
        if (!w.empty() && !w.back()) w.pop_back();
        AddRecent(out, w, max);
    }
    return out;
}

// Plano B para qualquer app: os "Itens recentes" do Windows (a pasta Recent), só os arquivos que abrem
// com este .exe, do mais novo para o mais velho
std::vector<atalhos::Item> ShellRecent(const std::wstring& exe, int max) {
    std::vector<atalhos::Item> out;
    std::wstring dir = RecentDir() + L"\\";
    std::vector<std::pair<FILETIME, std::wstring>> lnks;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do lnks.push_back({ fd.ftLastWriteTime, dir + fd.cFileName }); while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(lnks.begin(), lnks.end(), [](auto& x, auto& y) { return CompareFileTime(&x.first, &y.first) > 0; });
    std::wstring want = Lower(exe);
    std::map<std::wstring, bool> extOk;
    int looked = 0;
    for (auto& [t, l] : lnks) {
        if ((int)out.size() >= max || ++looked > 200) break;
        IShellLinkW* sl = nullptr; IPersistFile* pf = nullptr;
        std::wstring target;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) && SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) &&
            SUCCEEDED(pf->Load(l.c_str(), STGM_READ))) {
            wchar_t p[MAX_PATH] = {}; sl->GetPath(p, MAX_PATH, nullptr, SLGP_RAWPATH); target = p;
        }
        if (pf) pf->Release();
        if (sl) sl->Release();
        const wchar_t* ext = target.empty() ? nullptr : PathFindExtensionW(target.c_str());
        if (!ext || !*ext) continue;
        std::wstring e = Lower(ext);
        auto f = extOk.find(e);
        if (f == extOk.end()) {
            wchar_t h2[MAX_PATH] = {}; DWORD n = MAX_PATH;
            bool ok = SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, e.c_str(), nullptr, h2, &n)) && Lower(h2) == want;
            f = extOk.emplace(e, ok).first;
        }
        if (f->second) AddRecent(out, target, max);
    }
    return out;
}

}  // namespace

std::vector<atalhos::Item> atalhos::Tasks(const std::wstring& exePath) {
    std::vector<Item> out;
    if (exePath.empty()) return out;
    std::wstring want = Lower(exePath);
    std::wstring dir = RecentDir() + L"\\CustomDestinations\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.customDestinations-ms").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    FILETIME best{};
    do {
        std::wstring file = dir + fd.cFileName;
        auto& c = g_files[file];
        if (CompareFileTime(&c.mt, &fd.ftLastWriteTime) != 0) { c.mt = fd.ftLastWriteTime; ReadCustom(file, c); }
        if (c.exe == want && !c.items.empty() && CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; out = c.items; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<atalhos::Item> atalhos::Recent(const std::wstring& aumid, const std::wstring& exePath, int max) {
    std::vector<Item> out;
    std::wstring file = Lower(PathFindFileNameW(exePath.c_str()));
    // apps que guardam a lista por conta própria (não usam a do Windows)
    if (file.rfind(L"ableton live", 0) == 0) out = AbletonRecent(max);
    else if (file == L"fl64.exe" || file == L"fl.exe") out = FlRecent(max);
    else if (file.rfind(L"cubase", 0) == 0) out = CubaseRecent(max);
    if (!out.empty()) return out;
    IApplicationDocumentLists* dl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ApplicationDocumentLists, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dl)))) return out;
    std::vector<std::wstring> ids;
    if (!aumid.empty()) ids.push_back(aumid);
    if (!exePath.empty()) for (auto& i : ImplicitIds(exePath)) ids.push_back(i);
    for (auto& id : ids) {
        if (FAILED(dl->SetAppID(id.c_str()))) continue;
        IObjectArray* arr = nullptr;
        if (FAILED(dl->GetList(ADLT_RECENT, max, IID_PPV_ARGS(&arr))) || !arr) continue;
        UINT n = 0; arr->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IShellItem* si = nullptr;
            if (SUCCEEDED(arr->GetAt(i, IID_PPV_ARGS(&si))) && si) {
                PWSTR p = nullptr, nm = nullptr;
                Item it;
                if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) it.path = p;
                if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &nm)) && nm) it.title = nm;
                CoTaskMemFree(p); CoTaskMemFree(nm);
                if (!it.path.empty() && !it.title.empty()) { it.recent = true; out.push_back(it); }
                si->Release();
                continue;
            }
            IShellLinkW* sl = nullptr;
            if (SUCCEEDED(arr->GetAt(i, IID_PPV_ARGS(&sl))) && sl) {
                wchar_t p[MAX_PATH] = {}; sl->GetPath(p, MAX_PATH, nullptr, 0);
                Item it; it.path = p; it.recent = true;
                it.title = PathFindFileNameW(p);
                if (!it.path.empty()) out.push_back(it);
                sl->Release();
            }
        }
        arr->Release();
        if (!out.empty()) { Log(L"atalhos: %zu recentes pela identidade %ls", out.size(), id.c_str()); break; }
    }
    dl->Release();
    if (out.empty() && !exePath.empty()) out = ShellRecent(exePath, max);  // plano B: os Itens recentes do Windows
    return out;
}

std::wstring atalhos::AumidOf(HWND h) {
    std::wstring out;
    IPropertyStore* ps = nullptr;
    if (h && SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return out;
}

namespace wmc = winrt::Windows::Media::Control;

// a sessão de mídia deste app: pela identidade (AUMID) ou pelo nome do .exe dentro da identidade da sessão
// ("Spotify.exe", "SpotifyAB.SpotifyMusic_...!Spotify", "Chrome"...)
static wmc::GlobalSystemMediaTransportControlsSession SessionOf(const std::wstring& exePath, const std::wstring& aumid) {
    std::wstring base = Lower(PathFindFileNameW(exePath.c_str()));
    if (base.size() > 4) base.resize(base.size() - 4);
    auto mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    for (auto s : mgr.GetSessions()) {
        std::wstring src = Lower(s.SourceAppUserModelId().c_str());
        if ((!aumid.empty() && src == Lower(aumid)) || (!base.empty() && src.find(base) != std::wstring::npos)) return s;
    }
    return nullptr;
}

atalhos::Media atalhos::MediaOf(const std::wstring& exePath, const std::wstring& aumid) {
    Media m;
    if (exePath.empty() && aumid.empty()) return m;
    // WinRT síncrono não pode na thread da janela (STA): uma thread rápida faz a consulta
    std::thread t([&] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            auto s = SessionOf(exePath, aumid);
            if (s) {
                auto info = s.GetPlaybackInfo();
                auto c = info.Controls();
                m.has = true;
                m.playing = info.PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                m.canNext = c.IsNextEnabled(); m.canPrev = c.IsPreviousEnabled();
                auto p = s.TryGetMediaPropertiesAsync().get();
                m.title = p.Title().c_str(); m.artist = p.Artist().c_str();
            }
        } catch (...) {}
        winrt::uninit_apartment();
    });
    t.join();
    return m;
}

void atalhos::MediaCommand(const std::wstring& exePath, const std::wstring& aumid, int cmd) {
    std::thread([exePath, aumid, cmd] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            if (auto s = SessionOf(exePath, aumid)) {
                if (cmd == 0) s.TryTogglePlayPauseAsync().get();
                else if (cmd == 1) s.TrySkipNextAsync().get();
                else s.TrySkipPreviousAsync().get();
            }
        } catch (...) {}
    }).detach();
}

void atalhos::Run(const Item& it) {
    if (it.recent) { ShellExecuteW(nullptr, L"open", it.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
    ShellExecuteW(nullptr, L"open", it.path.c_str(), it.args.empty() ? nullptr : it.args.c_str(), it.dir.empty() ? nullptr : it.dir.c_str(), SW_SHOWNORMAL);
    Log(L"atalhos: \"%ls\" (%ls %ls)", it.title.c_str(), it.path.c_str(), it.args.c_str());
}
