// Índice de arquivos próprio do Shell (para o Spotlight): nomes, pastas e datas de tudo na pasta do
// usuário (sem a AppData) e nos outros discos fixos (sem as pastas do sistema), em memória e salvo em
// %APPDATA%\Shell\indice.db. Ao abrir o Shell, o arquivo salvo é lido (busca pronta na hora) e uma
// varredura nova roda em segundo plano; depois, mudanças nas pastas refazem o índice (no máximo a
// cada 2 minutos). A busca é uma passada na memória: milissegundos mesmo com centenas de milhares de itens.
#include "shell.h"
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <thread>
#include <map>
#include <set>

namespace {

struct Node {
    uint32_t parent;   // índice do pai (UINT32_MAX = raiz: o nome é o caminho inteiro)
    uint32_t name;     // posição em names
    uint16_t len;
    uint16_t dir;
    uint64_t mtime;    // FILETIME
};

struct Index {
    std::vector<Node> nodes;
    std::wstring names;          // todos os nomes, um atrás do outro
};

std::shared_mutex g_mx;
Index g_idx;
std::atomic<bool> g_ready{ false };
size_t g_dead;  // itens apagados (nome vazio) desde a última varredura completa
std::wstring g_file;

// tabela caractere → minúscula sem acento (a busca compara por ela, sem guardar uma cópia dos nomes)
const wchar_t* FoldTable() {
    static wchar_t* t = [] {
        auto* t = new wchar_t[65536];
        static const std::wstring from = L"áàâãäéèêëíìîïóòôõöúùûüçñ", to = L"aaaaaeeeeiiiiooooouuuucn";
        for (int i = 0; i < 65536; i++) {
            wchar_t c = towlower((wchar_t)i);
            size_t k = from.find(c);
            t[i] = k != std::wstring::npos ? to[k] : c;
        }
        return t;
    }();
    return t;
}

bool SkipDir(const wchar_t* n, bool top, bool profile) {
    if (n[0] == L'.' ) return true;  // .git, .cache, .vscode...
    for (auto s : { L"node_modules", L"__pycache__", L"$Recycle.Bin", L"System Volume Information", L"$WinREAgent", L"$SysReset",
                    L"Config.Msi", L"Recovery" })
        if (!_wcsicmp(n, s)) return true;
    if (top && !profile)
        for (auto s : { L"Windows", L"Program Files", L"Program Files (x86)", L"ProgramData", L"PerfLogs", L"Users", L"MSOCache", L"Intel", L"AMD",
                        L"Windows.old", L"$WINDOWS.~BT", L"$Windows.~WS", L"$GetCurrent", L"OneDriveTemp", L"inetpub", L"XboxGames" })
            if (!_wcsicmp(n, s)) return true;
    if (top && profile && (!_wcsicmp(n, L"AppData") || !_wcsicmp(n, L"MicrosoftEdgeBackups"))) return true;
    return false;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    if (p) CoTaskMemFree(p);
    return s;
}

// raízes: a pasta do usuário e todos os discos fixos (SSDs, HDs, o externo). No disco do Windows ficam
// de fora as pastas do sistema e a pasta Users (a do usuário já é uma raiz, sem a AppData)
std::vector<std::pair<std::wstring, bool>> Roots() {
    std::vector<std::pair<std::wstring, bool>> r;
    std::wstring prof = KnownFolder(FOLDERID_Profile);
    if (!prof.empty()) r.push_back({ prof, true });
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; d++) {
        if (!(mask & (1u << d))) continue;
        wchar_t root[] = { (wchar_t)(L'A' + d), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        (void)win;
        r.push_back({ std::wstring(root, 2), false });
    }
    return r;
}

// filhos de uma pasta, com os filtros da varredura (sem pastas do sistema, junções e as puladas)
template <class F> void List(const std::wstring& path, bool top, bool profile, F&& f) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if ((n[0] == L'.' && !n[1]) || (n[0] == L'.' && n[1] == L'.' && !n[2])) continue;
        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM && dir) continue;
        if (dir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;  // junções (evita voltas)
        if (dir && SkipDir(n, top, profile)) continue;
        size_t len = wcslen(n);
        if (len > 0xFFFF) continue;
        f(n, len, dir, ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// subárvore de `path` no índice `ix`, com os filhos pendurados em `parentRef`
void CrawlFrom(Index& ix, uint32_t parentRef, const std::wstring& path, bool top, bool profile) {
    struct Todo { uint32_t id; std::wstring path; bool top; };
    std::vector<Todo> stack{ { parentRef, path, top } };
    while (!stack.empty()) {
        Todo t = std::move(stack.back());
        stack.pop_back();
        List(t.path, t.top, profile, [&](const wchar_t* n, size_t len, bool dir, uint64_t mt) {
            uint32_t id = (uint32_t)ix.nodes.size();
            ix.nodes.push_back({ t.id, (uint32_t)ix.names.size(), (uint16_t)len, (uint16_t)dir, mt });
            ix.names.append(n, len);
            if (dir) stack.push_back({ id | (t.id & 0x80000000u), t.path + L"\\" + n, false });
        });
    }
}

void Crawl(Index& ix, const std::wstring& root, bool profile) {
    uint32_t rootId = (uint32_t)ix.nodes.size();
    ix.nodes.push_back({ UINT32_MAX, (uint32_t)ix.names.size(), (uint16_t)root.size(), 1, 0 });
    ix.names += root;
    CrawlFrom(ix, rootId, root, true, profile);
}

std::wstring PathOf(const Index& ix, uint32_t id) {
    std::vector<uint32_t> chain;
    for (uint32_t i = id; i != UINT32_MAX; i = ix.nodes[i].parent) chain.push_back(i);
    std::wstring p;
    for (size_t k = chain.size(); k-- > 0;) {
        const Node& n = ix.nodes[chain[k]];
        if (!p.empty()) p += L'\\';
        p.append(ix.names, n.name, n.len);
    }
    return p;
}

// ---- arquivo salvo: cabeçalho, nós e os dois textos
const uint32_t MAGIC = 0x58444E49, VERSION = 2;  // 'INDX'

void Save(const Index& ix) {
    std::wstring tmp = g_file + L".novo";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return;
    uint64_t n = ix.nodes.size(), c = ix.names.size();
    fwrite(&MAGIC, 4, 1, f); fwrite(&VERSION, 4, 1, f); fwrite(&n, 8, 1, f); fwrite(&c, 8, 1, f);
    fwrite(ix.nodes.data(), sizeof(Node), n, f);
    fwrite(ix.names.data(), 2, c, f);
    bool ok = !ferror(f);
    fclose(f);
    if (ok) MoveFileExW(tmp.c_str(), g_file.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

bool Load(Index& ix) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_file.c_str(), L"rb") || !f) return false;
    uint32_t m = 0, v = 0; uint64_t n = 0, c = 0;
    bool ok = fread(&m, 4, 1, f) && fread(&v, 4, 1, f) && fread(&n, 8, 1, f) && fread(&c, 8, 1, f) && m == MAGIC && v == VERSION && n < 50000000 && c < 2000000000;
    if (ok) {
        ix.nodes.resize(n); ix.names.resize(c);
        ok = fread(ix.nodes.data(), sizeof(Node), n, f) == n && fread(&ix.names[0], 2, c, f) == c;
    }
    fclose(f);
    return ok;
}

void Rebuild() {
    DWORD t0 = GetTickCount();
    Index ix;
    ix.nodes.reserve(200000);
    std::wstring perRoot;
    for (auto& [root, profile] : Roots()) {
        size_t before = ix.nodes.size();
        Crawl(ix, root, profile);
        wchar_t b[64]; swprintf_s(b, L" %ls %zu", profile ? L"usuário" : root.c_str(), ix.nodes.size() - before);
        perRoot += b;
    }
    size_t n = ix.nodes.size();
    ix.nodes.shrink_to_fit();   // sem a folga do crescimento (o texto dobrava de capacidade: dezenas de MB a mais)
    ix.names.shrink_to_fit();
    Save(ix);
    { std::unique_lock<std::shared_mutex> l(g_mx); g_idx = std::move(ix); g_dead = 0; }
    g_ready = true;
    Log(L"índice: %zu itens em %lu ms (%ls)", n, GetTickCount() - t0, perRoot.c_str());
}

// ---- Atualização só do que mudou
// Antes: qualquer mudança nas pastas observadas (até dentro de .git, node_modules, temporários do navegador,
// que nem entram no índice) refazia o índice inteiro, todos os discos (~930 mil itens, ~4 s de CPU), a cada
// 2 minutos. Agora o aviso diz QUAL pasta mudou: só os filhos dela são relidos e comparados com o índice.
// O que sumiu fica apagado (nome vazio: a busca pula); o que é novo entra no fim (pasta nova com tudo dentro).
// A varredura completa só volta para limpar (apagados passando de 1/6 do índice), ao ligar/desligar um
// disco ou se o Windows perder avisos (estouro do buffer).

std::wstring LowerPath(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

bool NoiseFile(const std::wstring& n) {  // temporários: vão e voltam o tempo todo
    if (n.empty() || n[0] == L'~' || n.back() == L'~') return true;
    std::wstring l = LowerPath(n);
    for (auto e : { L".tmp", L".crdownload", L".part", L".partial", L".download", L".lock", L".swp", L".temp", L".etl" }) {
        size_t el = wcslen(e);
        if (l.size() > el && !l.compare(l.size() - el, el, e)) return true;
    }
    return false;
}

// a mudança em `rel` (caminho a partir da raiz) importa para o índice?
bool Relevant(const std::wstring& rel, bool profile) {
    size_t pos = 0;
    bool top = true;
    for (;;) {
        size_t e = rel.find(L'\\', pos);
        std::wstring comp = rel.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        if (e == std::wstring::npos) return !NoiseFile(comp) && !SkipDir(comp.c_str(), top, profile);
        if (comp.empty() || SkipDir(comp.c_str(), top, profile)) return false;
        top = false;
        pos = e + 1;
    }
}

// relê os filhos destas pastas e aplica a diferença no índice. false: não deu (refazer tudo)
bool Incremental(const std::vector<std::wstring>& dirs) {
    DWORD t0 = GetTickCount();
    auto roots = Roots();
    struct Target { uint32_t id; std::wstring path; bool top, profile; };
    std::vector<Target> targets;
    struct Kid { uint32_t id; uint64_t mtime; bool dir; };
    std::map<uint32_t, std::map<std::wstring, Kid>> kids;  // pasta -> filhos (nome em minúsculas)
    {
        std::shared_lock<std::shared_mutex> l(g_mx);
        const Index& ix = g_idx;
        const uint32_t N = (uint32_t)ix.nodes.size();
        std::map<std::wstring, uint32_t> rootIds;
        for (uint32_t i = 0; i < N; i++)
            if (ix.nodes[i].parent == UINT32_MAX && ix.nodes[i].len) rootIds[LowerPath(ix.names.substr(ix.nodes[i].name, ix.nodes[i].len))] = i;
        for (auto& D : dirs) {
            size_t best = 0; int ri = -1;
            for (int r = 0; r < (int)roots.size(); r++) {
                const std::wstring& rp = roots[r].first;
                if (D.size() >= rp.size() && !_wcsnicmp(D.c_str(), rp.c_str(), rp.size()) && (D.size() == rp.size() || D[rp.size()] == L'\\') && rp.size() > best) { best = rp.size(); ri = r; }
            }
            if (ri < 0) continue;
            auto it = rootIds.find(LowerPath(roots[ri].first));
            if (it == rootIds.end()) return false;  // raiz nova (disco): varredura completa
            uint32_t cur = it->second;
            std::wstring curPath = roots[ri].first;
            size_t pos = best + 1;
            while (pos < D.size()) {  // desce pelos nomes até onde o índice conhece
                size_t e = D.find(L'\\', pos);
                std::wstring comp = D.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
                uint32_t next = UINT32_MAX;
                for (uint32_t j = cur + 1; j < N; j++) {
                    const Node& n = ix.nodes[j];
                    if (n.parent == cur && n.dir && n.len == comp.size() && !_wcsnicmp(ix.names.data() + n.name, comp.c_str(), comp.size())) { next = j; break; }
                }
                if (next == UINT32_MAX) break;  // pasta nova: relê a de cima, que já conhece (a nova entra inteira)
                cur = next;
                curPath += L"\\" + comp;
                if (e == std::wstring::npos) break;
                pos = e + 1;
            }
            bool dup = false;
            for (auto& t : targets) if (t.id == cur) dup = true;
            if (!dup) targets.push_back({ cur, curPath, ix.nodes[cur].parent == UINT32_MAX, roots[ri].second });
        }
        if (targets.empty()) return true;
        for (auto& t : targets) kids[t.id];
        for (uint32_t i = 0; i < N; i++) {
            const Node& n = ix.nodes[i];
            if (!n.len || n.parent == UINT32_MAX) continue;
            auto it = kids.find(n.parent);
            if (it != kids.end()) it->second[LowerPath(ix.names.substr(n.name, n.len))] = { i, n.mtime, n.dir != 0 };
        }
    }
    // sem trava: lê as pastas no disco e monta a diferença
    const uint32_t LOCAL = 0x80000000u;
    Index add;
    std::vector<uint32_t> gone;
    std::vector<std::pair<uint32_t, uint64_t>> touch;
    for (auto& t : targets) {
        auto& old = kids[t.id];
        std::set<std::wstring> seen;
        List(t.path, t.top, t.profile, [&](const wchar_t* n, size_t len, bool dir, uint64_t mt) {
            std::wstring key = LowerPath(std::wstring(n, len));
            seen.insert(key);
            auto it = old.find(key);
            if (it != old.end() && it->second.dir == dir) { if (it->second.mtime != mt) touch.push_back({ it->second.id, mt }); return; }
            if (it != old.end()) gone.push_back(it->second.id);  // virou pasta/arquivo
            uint32_t id = (uint32_t)add.nodes.size();
            add.nodes.push_back({ t.id, (uint32_t)add.names.size(), (uint16_t)len, (uint16_t)dir, mt });
            add.names.append(n, len);
            if (dir) CrawlFrom(add, id | LOCAL, t.path + L"\\" + std::wstring(n, len), false, t.profile);
        });
        for (auto& [k, v] : old) if (!seen.count(k)) gone.push_back(v.id);
    }
    if (gone.empty() && add.nodes.empty() && touch.empty()) return true;
    {
        std::unique_lock<std::shared_mutex> l(g_mx);
        Index& ix = g_idx;
        const uint32_t N = (uint32_t)ix.nodes.size();
        size_t dead = 0;
        if (!gone.empty()) {  // apaga a subárvore (o pai sempre vem antes do filho no índice)
            std::vector<bool> d(N, false);
            for (uint32_t g : gone) if (g < N) d[g] = true;
            for (uint32_t i = 0; i < N; i++) {
                Node& n = ix.nodes[i];
                if (!d[i] && n.parent != UINT32_MAX && n.parent < i && d[n.parent]) d[i] = true;
                if (d[i] && n.len) { n.len = 0; n.dir = 0; dead++; }
            }
        }
        for (auto& [id, mt] : touch) if (id < N) ix.nodes[id].mtime = mt;
        uint32_t base = N, nameBase = (uint32_t)ix.names.size();
        for (auto n : add.nodes) {
            if (n.parent != UINT32_MAX && (n.parent & LOCAL)) n.parent = (n.parent & ~LOCAL) + base;
            n.name += nameBase;
            ix.nodes.push_back(n);
        }
        ix.names += add.names;
        g_dead += dead;
        Log(L"índice: %zu pasta(s) relidas em %lu ms (+%zu, -%zu)", targets.size(), GetTickCount() - t0, add.nodes.size(), dead);
    }
    return true;
}

// Observa as pastas de cima de cada raiz (as que entram no índice; AppData, Windows etc. ficam de fora)
struct Watch {
    HANDLE dir = INVALID_HANDLE_VALUE, ev = nullptr;
    OVERLAPPED ov{};
    std::wstring base, root;
    bool profile = false, recursive = false;
    std::vector<BYTE> buf;
};
const DWORD WATCH_WHAT = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;

bool Arm(Watch& w) {
    ResetEvent(w.ev);
    w.ov = {}; w.ov.hEvent = w.ev;
    return ReadDirectoryChangesW(w.dir, w.buf.data(), (DWORD)w.buf.size(), w.recursive, WATCH_WHAT, nullptr, &w.ov, nullptr) != 0;
}

std::vector<Watch*> MakeWatches() {
    std::vector<Watch*> ws;
    auto add = [&](const std::wstring& path, const std::wstring& root, bool profile, bool rec) {
        if (ws.size() >= MAXIMUM_WAIT_OBJECTS) return;
        HANDLE d = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (d == INVALID_HANDLE_VALUE) return;
        auto* w = new Watch;
        w->dir = d; w->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w->base = path; w->root = root; w->profile = profile; w->recursive = rec;
        w->buf.resize(64 * 1024);
        if (!Arm(*w)) { CloseHandle(w->dir); CloseHandle(w->ev); delete w; return; }
        ws.push_back(w);
    };
    for (auto& [root, profile] : Roots()) {
        add(root + L"\\", root, profile, false);  // a própria raiz, sem subpastas
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM))) continue;
            if ((fd.cFileName[0] == L'.' && (!fd.cFileName[1] || fd.cFileName[1] == L'.')) || SkipDir(fd.cFileName, true, profile)) continue;
            add(root + L"\\" + fd.cFileName, root, profile, true);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return ws;
}

void FreeWatches(std::vector<Watch*>& ws) {
    for (auto* w : ws) { CancelIo(w->dir); CloseHandle(w->dir); CloseHandle(w->ev); delete w; }
    ws.clear();
}

void Watcher() {
    std::vector<Watch*> ws = MakeWatches();
    DWORD drives = GetLogicalDrives();
    DWORD lastChange = 0, lastBuild = GetTickCount(), lastSave = GetTickCount();
    std::set<std::wstring> pending;  // pastas que mudaram (caminho completo)
    bool full = false, unsaved = false;
    for (;;) {
        std::vector<HANDLE> evs;
        for (auto* w : ws) evs.push_back(w->ev);
        DWORD r = WAIT_TIMEOUT;
        if (evs.empty()) Sleep(2000);
        else r = WaitForMultipleObjects((DWORD)evs.size(), evs.data(), FALSE, 2000);
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + evs.size()) {
            Watch& w = *ws[r - WAIT_OBJECT_0];
            DWORD got = 0;
            if (!GetOverlappedResult(w.dir, &w.ov, &got, FALSE) || got == 0) full = true;  // estourou: perdeu avisos
            else {
                size_t off = 0;
                for (;;) {
                    auto* fi = (FILE_NOTIFY_INFORMATION*)(w.buf.data() + off);
                    std::wstring rel(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
                    std::wstring fullPath = w.base + (w.base.back() == L'\\' ? L"" : L"\\") + rel;
                    std::wstring fromRoot = fullPath.size() > w.root.size() + 1 ? fullPath.substr(w.root.size() + 1) : L"";
                    if (!fromRoot.empty() && Relevant(fromRoot, w.profile)) {
                        std::wstring parent = fullPath.substr(0, fullPath.find_last_of(L'\\'));
                        if (parent.size() <= w.root.size()) parent = w.root;  // mudou direto na raiz ("C:")
                        pending.insert(parent);
                        lastChange = GetTickCount();
                    }
                    if (!fi->NextEntryOffset) break;
                    off += fi->NextEntryOffset;
                }
            }
            if (!Arm(w)) full = true;
            continue;
        }
        DWORD now = GetTickCount(), d = GetLogicalDrives();
        bool drivesChanged = d != drives;
        bool calm = now - lastChange > 3000;
        // modo jogo (jogo.cpp): o jogo escreve sem parar (logs, cache de shaders) e cada leva de mudanças virava uma
        // reindexação no meio da partida; as pastas ficam anotadas em `pending` e entram quando o jogo fechar
        if (jogo::Ativo() && !drivesChanged) continue;
        if (drivesChanged || (full && calm && now - lastBuild > 120000) || (!full && !pending.empty() && calm)) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // CPU e disco em prioridade de fundo
            bool ok = !drivesChanged && !full && pending.size() <= 300;
            if (ok) {
                ok = Incremental(std::vector<std::wstring>(pending.begin(), pending.end()));
                size_t total; { std::shared_lock<std::shared_mutex> l(g_mx); total = g_idx.nodes.size(); }
                if (ok && g_dead * 6 > total) ok = false;  // muito apagado: limpa com a varredura completa
                if (ok) unsaved = true;
            }
            if (!ok) {
                if (drivesChanged) Sleep(3000);  // o disco recém-ligado ainda está montando
                Rebuild();
                lastBuild = lastSave = GetTickCount();
                unsaved = false;
            }
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            pending.clear();
            full = false;
            if (drivesChanged) { drives = d; FreeWatches(ws); ws = MakeWatches(); }
        }
        if (unsaved && GetTickCount() - lastSave > 10 * 60 * 1000) {  // o arquivo salvo, no máximo a cada 10 min
            std::shared_lock<std::shared_mutex> l(g_mx);
            Save(g_idx);
            lastSave = GetTickCount();
            unsaved = false;
        }
    }
}

}  // namespace

void indice::Start() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { g_file = std::wstring(p) + L"\\Shell\\indice.db"; CoTaskMemFree(p); }
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Index ix;
        bool loaded = Load(ix);
        if (loaded) {
            size_t n = ix.nodes.size();
            { std::unique_lock<std::shared_mutex> l(g_mx); g_idx = std::move(ix); }
            g_ready = true;
            Log(L"índice: %zu itens lidos do disco", n);
            // com o índice salvo a busca já funciona: a varredura nova espera o login assentar (antes: corria
            // junto com o boot por ~3 minutos, disputando disco com o Windows e os apps)
            Sleep(GetTickCount64() < 10 * 60 * 1000 ? 4 * 60 * 1000 : 30 * 1000);
        }
        while (jogo::Ativo()) Sleep(5000);  // a varredura da abertura (disco inteiro) espera o jogo fechar
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        Rebuild();  // atualiza o que mudou enquanto o Shell estava fechado
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        Watcher();
    }).detach();
}

bool indice::Ready() { return g_ready; }

void indice::Search(const std::wstring& foldedQuery, size_t max, const std::function<bool()>& cancelled, std::vector<indice::Hit>& out) {
    if (foldedQuery.empty() || !g_ready) return;
    std::shared_lock<std::shared_mutex> l(g_mx);
    const Index& ix = g_idx;
    // candidatos: os que têm o termo no nome; nota pelo lugar onde casou, depois os mais recentes
    struct Cand { uint32_t id; int score; uint64_t mtime; };
    std::vector<Cand> cands;
    const wchar_t* q = foldedQuery.c_str();
    size_t ql = foldedQuery.size();
    const wchar_t* fold = FoldTable();
    auto eq = [fold](wchar_t a, wchar_t b) { return fold[a] == b; };
    for (uint32_t i = 0; i < ix.nodes.size(); i++) {
        const Node& n = ix.nodes[i];
        if (n.parent == UINT32_MAX || n.len < ql) continue;
        if ((i & 0xFFFF) == 0 && cancelled()) return;
        const wchar_t* s = ix.names.data() + n.name;
        const wchar_t* e = s + n.len;
        const wchar_t* hit = std::search(s, e, q, q + ql, eq);
        if (hit == e) continue;
        int score = hit == s ? (n.len == ql ? 120 : 100) : (hit[-1] == L' ' || hit[-1] == L'-' || hit[-1] == L'_' || hit[-1] == L'.') ? 80 : 50;
        cands.push_back({ i, score, n.mtime });
    }
    size_t k = std::min(max, cands.size());
    std::partial_sort(cands.begin(), cands.begin() + k, cands.end(), [](const Cand& a, const Cand& b) {
        return a.score != b.score ? a.score > b.score : a.mtime > b.mtime;
    });
    for (size_t i = 0; i < k; i++) {
        const Node& n = ix.nodes[cands[i].id];
        out.push_back({ PathOf(ix, cands[i].id), std::wstring(ix.names, n.name, n.len), n.dir != 0, cands[i].score });
    }
}

size_t indice::Count() {
    std::shared_lock<std::shared_mutex> l(g_mx);
    return g_idx.nodes.size();
}
