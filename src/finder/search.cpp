// Busca no índice do Windows Search (o "Spotlight" do Windows), via OLE DB/ADO.
// Pastas fora do índice caem numa varredura simples pelo nome.
#include "finder.h"
#include <shlwapi.h>
#include <shlobj.h>
#import "C:\\Program Files\\Common Files\\System\\ado\\msado15.dll" no_namespace rename("EOF", "AdoEOF")

namespace search {

// Aspas simples dobradas; curingas do LIKE escapados entre colchetes.
static std::wstring LikeEscape(const std::wstring& q) {
    std::wstring o;
    for (wchar_t c : q) {
        if (c == L'\'') o += L"''";
        else if (c == L'%' || c == L'_' || c == L'[') { o += L'['; o += c; o += L']'; }
        else o += c;
    }
    return o;
}

// Termo para CONTAINS: só letras, números e espaços (o resto quebra a sintaxe).
static std::wstring ContainsTerm(const std::wstring& q) {
    std::wstring o;
    for (wchar_t c : q) if (iswalnum(c) || c == L' ') o += c;
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}

// data local -> texto UTC para o SQL do índice ('AAAA-MM-DD HH:MM:SS')
static std::wstring UtcText(SYSTEMTIME local) {
    SYSTEMTIME u{};
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &u);
    wchar_t b[32]; swprintf_s(b, L"%04d-%02d-%02d %02d:%02d:%02d", u.wYear, u.wMonth, u.wDay, u.wHour, u.wMinute, u.wSecond);
    return b;
}
static SYSTEMTIME DaysAgo(int d) {  // meia-noite (hora local) de d dias atrás
    SYSTEMTIME st; GetLocalTime(&st);
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    u.QuadPart -= (ULONGLONG)d * 24 * 3600 * 10000000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    return st;
}

int Smart(int kind, int max, const std::function<bool(const std::wstring&)>& onPath) {
    int n = 0;
    wchar_t prof[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) return -1;
    std::wstring scope = prof;
    for (auto& c : scope) if (c == L'\\') c = L'/';
    if (scope.back() != L'/') scope += L'/';
    std::wstring cond;
    switch (kind) {
    case 0: cond = L"System.DateModified >= '" + UtcText(DaysAgo(0)) + L"'"; break;
    case 1: cond = L"System.DateModified >= '" + UtcText(DaysAgo(1)) + L"' AND System.DateModified < '" + UtcText(DaysAgo(0)) + L"'"; break;
    case 2: cond = L"System.DateModified >= '" + UtcText(DaysAgo(7)) + L"'"; break;
    case 3: cond = L"System.Kind = 'picture'"; break;
    case 4: cond = L"System.Kind = 'video'"; break;
    default: cond = L"System.Kind = 'document'"; break;
    }
    try {
        _ConnectionPtr con;
        if (FAILED(con.CreateInstance(__uuidof(Connection)))) return -1;
        con->Open(L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';", L"", L"", adConnectUnspecified);
        std::wstring sql = L"SELECT TOP " + std::to_wstring(max) + L" System.ItemUrl FROM SystemIndex WHERE SCOPE='file:" + scope + L"' AND " + cond +
                           L" AND System.ItemType <> 'Directory' AND System.FileAttributes <> 2 ORDER BY System.DateModified DESC";
        _RecordsetPtr rs;
        rs.CreateInstance(__uuidof(Recordset));
        rs->Open(sql.c_str(), _variant_t((IDispatch*)con, true), adOpenForwardOnly, adLockReadOnly, adCmdText);
        while (!rs->AdoEOF) {
            _variant_t v = rs->Fields->GetItem(0L)->Value;
            if (v.vt == VT_BSTR) {
                std::wstring url = v.bstrVal;
                if (url.rfind(L"file:", 0) == 0) {
                    std::wstring p = url.substr(5);
                    for (auto& c : p) if (c == L'/') c = L'\\';
                    n++;
                    if (!onPath(p)) break;
                }
            }
            rs->MoveNext();
        }
        rs->Close();
        con->Close();
    } catch (...) { return n ? n : -1; }
    return n;
}

int Index(const std::wstring& q, const std::wstring& folder, int max, const std::function<bool(const std::wstring&)>& onPath) {
    int n = 0;
    try {
        _ConnectionPtr con;
        if (FAILED(con.CreateInstance(__uuidof(Connection)))) return -1;
        con->Open(L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';", L"", L"", adConnectUnspecified);
        std::wstring sql = L"SELECT TOP " + std::to_wstring(max) + L" System.ItemUrl FROM SystemIndex WHERE (System.FileName LIKE '%" + LikeEscape(q) + L"%'";
        std::wstring ct = ContainsTerm(q);
        if (!ct.empty()) sql += L" OR CONTAINS(System.Search.Contents, '\"" + ct + L"*\"')";  // conteúdo, como o Spotlight
        sql += L")";
        if (!folder.empty()) {
            std::wstring f = folder;
            for (auto& c : f) if (c == L'\\') c = L'/';
            if (f.back() != L'/') f += L'/';
            for (size_t i = 0; (i = f.find(L'\'', i)) != std::wstring::npos; i += 2) f.insert(i, 1, L'\'');
            sql += L" AND SCOPE='file:" + f + L"'";
        }
        _RecordsetPtr rs;
        rs.CreateInstance(__uuidof(Recordset));
        rs->Open(sql.c_str(), _variant_t((IDispatch*)con, true), adOpenForwardOnly, adLockReadOnly, adCmdText);
        while (!rs->AdoEOF) {
            _variant_t v = rs->Fields->GetItem(0L)->Value;
            if (v.vt == VT_BSTR) {
                std::wstring url = v.bstrVal;  // "file:C:/Users/..."
                if (url.rfind(L"file:", 0) == 0) {
                    std::wstring p = url.substr(5);
                    for (auto& c : p) if (c == L'/') c = L'\\';
                    n++;
                    if (!onPath(p)) break;
                }
            }
            rs->MoveNext();
        }
        rs->Close();
        con->Close();
    } catch (_com_error&) {
        return n ? n : -1;
    }
    return n;
}

// Varredura pelo nome (subpastas incluídas), para pastas que o índice não cobre.
int Walk(const std::wstring& q, const std::wstring& folder, int max, const std::function<bool(const std::wstring&)>& onPath) {
    int n = 0;
    bool stop = false;
    std::vector<std::wstring> stack{ folder };
    while (!stack.empty() && !stop) {
        std::wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        std::wstring pat = dir + (dir.back() == L'\\' ? L"*" : L"\\*");
        HANDLE f = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (fd.cFileName[0] == L'.') continue;
            std::wstring p = dir + (dir.back() == L'\\' ? L"" : L"\\") + fd.cFileName;
            if (StrStrIW(fd.cFileName, q.c_str())) { n++; if (!onPath(p) || n >= max) { stop = true; break; } }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return n;
}

}  // namespace search

// ---------------------------------------------------------------------------
// O índice do Spotlight do Shell (%APPDATA%\Shell\indice.db, ver src\shell\indice.cpp): nomes de tudo
// na pasta do usuário e nos discos. Lido uma vez (na primeira busca) e relido quando o Shell o refaz.
#include <mutex>
#include <cstdint>
#include <algorithm>

namespace {

struct Node { uint32_t parent, name; uint16_t len, dir; uint64_t mtime; };
std::mutex g_ixMu;
std::vector<Node> g_nodes;
std::wstring g_names;
FILETIME g_loadedTime{};

std::wstring IndexFile() {
    wchar_t* p = nullptr;
    std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) f = std::wstring(p) + L"\\Shell\\indice.db";
    if (p) CoTaskMemFree(p);
    return f;
}

bool LoadIndex() {  // com g_ixMu
    std::wstring file = IndexFile();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa)) return !g_nodes.empty();
    if (!g_nodes.empty() && CompareFileTime(&fa.ftLastWriteTime, &g_loadedTime) == 0) return true;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") || !f) return !g_nodes.empty();
    uint32_t m = 0, v = 0; uint64_t n = 0, c = 0;
    bool ok = fread(&m, 4, 1, f) && fread(&v, 4, 1, f) && fread(&n, 8, 1, f) && fread(&c, 8, 1, f) && m == 0x58444E49 && v == 2 && n < 50000000 && c < 2000000000;
    std::vector<Node> nodes; std::wstring names;
    if (ok) {
        nodes.resize(n); names.resize(c);
        ok = fread(nodes.data(), sizeof(Node), n, f) == n && (c == 0 || fread(&names[0], 2, c, f) == c);
    }
    fclose(f);
    if (ok) { g_nodes = std::move(nodes); g_names = std::move(names); g_loadedTime = fa.ftLastWriteTime; }
    return !g_nodes.empty();
}

wchar_t Fold(wchar_t c) {
    static const std::wstring from = L"áàâãäéèêëíìîïóòôõöúùûüçñ", to = L"aaaaaeeeeiiiiooooouuuucn";
    c = towlower(c);
    size_t i = from.find(c);
    return i != std::wstring::npos ? to[i] : c;
}

std::wstring PathOf(uint32_t id) {
    std::vector<uint32_t> chain;
    for (uint32_t i = id; i != UINT32_MAX && i < g_nodes.size(); i = g_nodes[i].parent) chain.push_back(i);
    std::wstring p;
    for (size_t k = chain.size(); k-- > 0;) {
        if (!p.empty()) p += L'\\';
        p.append(g_names, g_nodes[chain[k]].name, g_nodes[chain[k]].len);
    }
    return p;
}

}  // namespace

int search::Spotlight(const std::wstring& q, const std::wstring& folder, int max, const std::function<bool(const std::wstring&)>& onPath) {
    std::wstring fq;
    for (wchar_t c : q) fq += Fold(c);
    while (!fq.empty() && fq.back() == L' ') fq.pop_back();
    if (fq.empty()) return -1;
    std::lock_guard<std::mutex> l(g_ixMu);
    if (!LoadIndex()) return -1;
    // tabela de dobra (minúscula sem acento) para comparar sem copiar os nomes
    static std::vector<wchar_t> fold;
    if (fold.empty()) { fold.resize(65536); for (int i = 0; i < 65536; i++) fold[i] = Fold((wchar_t)i); }
    std::wstring scope;
    for (wchar_t c : folder) scope += towlower(c);
    if (!scope.empty() && scope.back() != L'\\') scope += L'\\';
    struct Cand { uint32_t id; int score; uint64_t mtime; };
    std::vector<Cand> cands;
    size_t ql = fq.size();
    for (uint32_t i = 0; i < g_nodes.size(); i++) {
        const Node& n = g_nodes[i];
        if (n.parent == UINT32_MAX || n.len < ql) continue;
        const wchar_t* s = g_names.data() + n.name; const wchar_t* e = s + n.len;
        const wchar_t* hit = std::search(s, e, fq.begin(), fq.end(), [&](wchar_t a, wchar_t b) { return fold[a] == b; });
        if (hit == e) continue;
        int score = hit == s ? (n.len == ql ? 120 : 100) : (hit[-1] == L' ' || hit[-1] == L'-' || hit[-1] == L'_' || hit[-1] == L'.') ? 80 : 50;
        cands.push_back({ i, score, n.mtime });
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score != b.score ? a.score > b.score : a.mtime > b.mtime; });
    int count = 0;
    for (auto& c : cands) {
        std::wstring p = PathOf(c.id);
        if (!scope.empty()) {  // busca na pasta: só o que está dentro dela
            std::wstring lp = p; for (auto& ch : lp) ch = towlower(ch);
            if (lp.compare(0, scope.size(), scope) != 0) continue;
        }
        count++;
        if (!onPath(p) || count >= max) break;
    }
    return count;
}
