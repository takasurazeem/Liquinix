// Notas: pastas e notas gravadas em %APPDATA%\Shell\Notas (um arquivo .nota por nota, pastas.txt),
// e as Notas Matemáticas (expressões terminadas em "=" ganham o resultado; "nome = expr" cria variável).
#include "notas.h"
#include <shlobj.h>
#include <cwctype>
#include <cmath>
#include <algorithm>
#include "../comum/idioma.h"

namespace store {
std::vector<std::unique_ptr<Note>> notes;
std::vector<NFolder> folders;
std::wstring g_dir;
}

extern std::wstring g_dataDirOverride;  // --dados=<pasta> (testes)

ULONGLONG store::Now() { FILETIME f; GetSystemTimeAsFileTime(&f); return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// ---------------------------------------------------------------------------
// Note
std::wstring Note::Title() const {
    for (auto& p : paras) {
        size_t a = p.text.find_first_not_of(L" \t");
        if (a != std::wstring::npos) return p.text.substr(a, 200);
    }
    return Tr(L"Nova Nota");
}
std::wstring Note::Preview() const {
    bool first = true;
    for (auto& p : paras) {
        size_t a = p.text.find_first_not_of(L" \t");
        if (a == std::wstring::npos) continue;
        if (first) { first = false; continue; }
        return p.text.substr(a, 200);
    }
    return Tr(L"Nenhum texto adicional");
}
bool Note::Empty() const {
    for (auto& p : paras) if (p.text.find_first_not_of(L" \t") != std::wstring::npos) return false;
    return true;
}
std::wstring Note::PlainText() const {
    std::wstring s;
    for (size_t i = 0; i < paras.size(); i++) { if (i) s += L"\r\n"; s += paras[i].text; }
    return s;
}

// ---------------------------------------------------------------------------
// serialização: uma linha por parágrafo  "estilo,lista,recuo,marcado,recolhido|atributos|texto"
static std::wstring Esc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else o += c;
    }
    return o;
}
static std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            o += n == L'n' ? L'\n' : n == L'r' ? L'\r' : n;
        } else o += s[i];
    }
    return o;
}

std::wstring store::Serialize(const std::vector<Para>& ps) {
    std::wstring out;
    wchar_t b[64];
    for (auto& p : ps) {
        swprintf_s(b, L"%d,%d,%d,%d,%d|", p.style, p.list, p.indent, p.checked ? 1 : 0, p.collapsed ? 1 : 0);
        out += b;
        // atributos em corridas: "aa:n;" (hex do atributo, quantidade)
        size_t i = 0;
        while (i < p.at.size()) {
            size_t j = i;
            while (j < p.at.size() && p.at[j] == p.at[i]) j++;
            if (p.at[i] || j < p.at.size()) { swprintf_s(b, L"%x:%zu;", p.at[i], j - i); out += b; }
            i = j;
        }
        out += L"|";
        out += Esc(p.text);
        out += L"\n";
    }
    return out;
}

std::vector<Para> store::Deserialize(const std::wstring& s) {
    std::vector<Para> ps;
    size_t a = 0;
    while (a < s.size()) {
        size_t e = s.find(L'\n', a);
        if (e == std::wstring::npos) e = s.size();
        std::wstring line = s.substr(a, e - a);
        a = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t b1 = line.find(L'|');
        if (b1 == std::wstring::npos) continue;
        size_t b2 = line.find(L'|', b1 + 1);
        if (b2 == std::wstring::npos) continue;
        Para p;
        int st = 3, li = 0, in = 0, ck = 0, co = 0;
        swscanf_s(line.substr(0, b1).c_str(), L"%d,%d,%d,%d,%d", &st, &li, &in, &ck, &co);
        p.style = (uint8_t)std::clamp(st, 0, 4); p.list = (uint8_t)std::clamp(li, 0, 4); p.indent = (uint8_t)std::clamp(in, 0, 8);
        p.checked = ck != 0; p.collapsed = co != 0;
        p.text = Unesc(line.substr(b2 + 1));
        std::wstring runs = line.substr(b1 + 1, b2 - b1 - 1);
        size_t r = 0;
        while (r < runs.size()) {
            size_t semi = runs.find(L';', r);
            if (semi == std::wstring::npos) break;
            unsigned v = 0; size_t n = 0;
            if (swscanf_s(runs.substr(r, semi - r).c_str(), L"%x:%zu", &v, &n) == 2) p.at.insert(p.at.end(), n, (uint8_t)v);
            r = semi + 1;
        }
        p.at.resize(p.text.size(), 0);
        ps.push_back(std::move(p));
    }
    if (ps.empty()) ps.push_back(Para{});
    return ps;
}

// ---------------------------------------------------------------------------
// arquivos
static std::wstring DataDir() {
    if (!store::g_dir.empty()) return store::g_dir;
    if (!g_dataDirOverride.empty()) store::g_dir = g_dataDirOverride;
    else {
        PWSTR p = nullptr;
        std::wstring base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { base = p; CoTaskMemFree(p); }
        CreateDirectoryW((base + L"\\Shell").c_str(), nullptr);
        store::g_dir = base + L"\\Shell\\Notas";
    }
    CreateDirectoryW(store::g_dir.c_str(), nullptr);
    return store::g_dir;
}

static bool ReadFileW(const std::wstring& path, std::wstring& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    std::string raw; char buf[16384]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);
    if (raw.size() >= 3 && (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF) raw.erase(0, 3);
    int wn = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    out.assign(wn, 0);
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), out.data(), wn);
    return true;
}

// grava num .tmp e troca (nunca deixa um arquivo pela metade)
static bool WriteFileW(const std::wstring& path, const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string raw(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), raw.data(), n, nullptr, nullptr);
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) { Log(L"erro: gravar %ls", tmp.c_str()); return false; }
    fwrite(raw.data(), 1, raw.size(), f);
    fclose(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { Log(L"erro: trocar %ls (%lu)", path.c_str(), GetLastError()); return false; }
    return true;
}

static ULONGLONG Ft(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// lê um .nota (nullptr se estiver estragado ou se já passou 30 dias em Apagadas Recentemente: aí apaga)
static std::unique_ptr<Note> ReadNote(const std::wstring& path, const WIN32_FIND_DATAW& fd) {
    std::wstring txt;
    if (!ReadFileW(path, txt)) return nullptr;
    size_t sep = txt.find(L"\n---\n");
    if (sep == std::wstring::npos) return nullptr;
    auto n = std::make_unique<Note>();
    n->id = fd.cFileName; n->id.resize(n->id.size() - 5);
    n->diskTime = Ft(fd.ftLastWriteTime);
    std::wstring head = txt.substr(0, sep);
    size_t a = 0;
    while (a < head.size()) {
        size_t e = head.find(L'\n', a); if (e == std::wstring::npos) e = head.size();
        std::wstring line = head.substr(a, e - a); a = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == L"pasta") n->folder = v;
        else if (k == L"anterior") n->prevFolder = v;
        else if (k == L"criada") n->created = _wcstoui64(v.c_str(), nullptr, 10);
        else if (k == L"modificada") n->modified = _wcstoui64(v.c_str(), nullptr, 10);
        else if (k == L"apagada") n->deleted = _wcstoui64(v.c_str(), nullptr, 10);
        else if (k == L"fixada") n->pinned = v == L"1";
    }
    n->paras = store::Deserialize(txt.substr(sep + 5));
    if (n->folder.empty() || (n->folder != L"apagadas" && !store::FindFolder(n->folder))) n->folder = L"notas";
    ULONGLONG now = store::Now(), month = 30ULL * 24 * 3600 * 10000000ULL;
    if (n->folder == L"apagadas" && n->deleted && now > n->deleted && now - n->deleted > month) { DeleteFileW(path.c_str()); return nullptr; }
    return n;
}

// outro processo (a Nota Rápida do canto da tela) gravou notas: traz as novas e as mudadas
bool store::Sync(const Note* keep) {
    std::wstring dir = DataDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.nota").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool changed = false;
    std::vector<std::wstring> seen;
    do {
        std::wstring id = fd.cFileName; id.resize(id.size() - 5);
        seen.push_back(id);
        Note* cur = nullptr;
        for (auto& n : notes) if (n->id == id) cur = n.get();
        if (cur && (cur->dirty || cur->diskTime >= Ft(fd.ftLastWriteTime))) continue;
        auto n = ReadNote(dir + L"\\" + fd.cFileName, fd);
        if (!n) continue;
        if (!cur) { notes.push_back(std::move(n)); changed = true; }
        else if (cur != keep) {  // a nota aberta no editor não é trocada debaixo do cursor
            cur->paras = n->paras; cur->folder = n->folder; cur->prevFolder = n->prevFolder; cur->modified = n->modified;
            cur->deleted = n->deleted; cur->pinned = n->pinned; cur->diskTime = n->diskTime; cur->undo.clear(); cur->redo.clear();
            changed = true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // apagadas por fora (Nota Rápida vazia que sumiu ao fechar)
    for (size_t i = 0; i < notes.size();) {
        Note* n = notes[i].get();
        if (n != keep && !n->dirty && n->diskTime && std::find(seen.begin(), seen.end(), n->id) == seen.end()) { notes.erase(notes.begin() + i); changed = true; }
        else i++;
    }
    return changed;
}

static std::wstring NotePath(const Note* n) { return DataDir() + L"\\" + n->id + L".nota"; }

void store::SaveNote(Note* n) {
    if (!n || !n->dirty) return;
    wchar_t h[512];
    swprintf_s(h, L"pasta=%ls\nanterior=%ls\ncriada=%llu\nmodificada=%llu\napagada=%llu\nfixada=%d\n---\n",
               n->folder.c_str(), n->prevFolder.c_str(), n->created, n->modified, n->deleted, n->pinned ? 1 : 0);
    if (WriteFileW(NotePath(n), h + Serialize(n->paras))) {
        n->dirty = false;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(NotePath(n).c_str(), GetFileExInfoStandard, &a)) n->diskTime = Ft(a.ftLastWriteTime);
    }
}
void store::SaveAllDirty() { for (auto& n : notes) SaveNote(n.get()); }
void store::DeleteNoteFile(Note* n) { DeleteFileW(NotePath(n).c_str()); }

void store::SaveFolders() {
    std::wstring s;
    for (auto& f : folders) s += f.id + L"\t" + f.name + L"\t" + std::to_wstring(f.sort) + L"\t" + (f.group ? L"1" : L"0") + L"\n";
    WriteFileW(DataDir() + L"\\pastas.txt", s);
}

NFolder* store::FindFolder(const std::wstring& id) {
    for (auto& f : folders) if (f.id == id) return &f;
    return nullptr;
}
std::wstring store::FolderName(const std::wstring& id) {
    if (id == L"apagadas") return Tr(L"Apagadas Recentemente");
    if (id == L"todas") return Tr(L"Todo o iCloud");
    if (NFolder* f = FindFolder(id)) return Tr(f->name);
    return Tr(L"Notas");
}

Note* store::NewNote(const std::wstring& folder) {
    static int counter;
    auto n = std::make_unique<Note>();
    n->created = n->modified = Now();
    wchar_t id[64]; swprintf_s(id, L"%016llx%02x", n->created, (++counter) & 0xFF);
    n->id = id;
    n->folder = folder;
    Para t; t.style = S_TITLE;  // "a primeira linha é o título", como no Mac
    n->paras.push_back(t);
    Note* raw = n.get();
    notes.push_back(std::move(n));
    return raw;
}

void store::RemoveNote(Note* n) {
    DeleteNoteFile(n);
    for (size_t i = 0; i < notes.size(); i++) if (notes[i].get() == n) { notes.erase(notes.begin() + i); break; }
}

void store::Load() {
    notes.clear(); folders.clear();
    std::wstring dir = DataDir(), s;
    if (ReadFileW(dir + L"\\pastas.txt", s)) {
        size_t a = 0;
        while (a < s.size()) {
            size_t e = s.find(L'\n', a); if (e == std::wstring::npos) e = s.size();
            std::wstring line = s.substr(a, e - a); a = e + 1;
            std::vector<std::wstring> f; size_t x = 0, y;
            while ((y = line.find(L'\t', x)) != std::wstring::npos) { f.push_back(line.substr(x, y - x)); x = y + 1; }
            f.push_back(line.substr(x));
            if (f.size() < 2 || f[0].empty()) continue;
            NFolder fo; fo.id = f[0]; fo.name = f[1];
            if (f.size() > 2) fo.sort = _wtoi(f[2].c_str());
            if (f.size() > 3) fo.group = f[3] != L"0";
            folders.push_back(fo);
        }
    }
    // pastas fixas sempre existem
    auto ensure = [](const wchar_t* id, const wchar_t* name, bool front) {
        if (FindFolder(id)) return;
        NFolder f; f.id = id; f.name = name;
        if (front) folders.insert(folders.begin(), f); else folders.push_back(f);
    };
    ensure(L"notas", L"Notas", true);
    ensure(L"math", L"Notas Matemáticas", true);
    ensure(L"rapidas", L"Notas Rápidas", false);
    ensure(L"mac", L"Notas", false);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.nota").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (auto n = ReadNote(dir + L"\\" + fd.cFileName, fd)) notes.push_back(std::move(n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Log(L"dados: %ls, %zu notas, %zu pastas", dir.c_str(), notes.size(), folders.size());
}

// ---------------------------------------------------------------------------
// Notas Matemáticas
namespace {

struct Tok { enum K { NUM, ID, OP, LP, RP, END, BAD } k; double v = 0; std::wstring s; wchar_t op = 0; int pos = 0, len = 0; bool cur = false; };

bool IsIdStart(wchar_t c) { return iswalpha(c) || c == L'_'; }
bool IsIdChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

// número em pt-BR ou en: "1.500", "1500,5", "1,800" (milhar), "2.5"
bool ParseNumber(const std::wstring& s, size_t& i, double& out) {
    size_t a = i;
    std::wstring raw;
    while (i < s.size() && (iswdigit(s[i]) || ((s[i] == L'.' || s[i] == L',') && i + 1 < s.size() && iswdigit(s[i + 1])))) raw += s[i++];
    if (raw.empty()) { i = a; return false; }
    // separadores: se os dois aparecem, o último é o decimal; um só: decimal, a não ser que venha
    // seguido de exatamente 3 dígitos (milhar), e repetido vira milhar sempre
    size_t lastDot = raw.rfind(L'.'), lastCom = raw.rfind(L',');
    wchar_t dec = 0;
    if (lastDot != std::wstring::npos && lastCom != std::wstring::npos) dec = lastDot > lastCom ? L'.' : L',';
    else {
        wchar_t c = lastDot != std::wstring::npos ? L'.' : lastCom != std::wstring::npos ? L',' : 0;
        if (c) {
            size_t cnt = std::count(raw.begin(), raw.end(), c), last = raw.rfind(c);
            bool thousands = cnt > 1 || (raw.size() - last - 1 == 3 && last <= 3 && c == L'.');
            if (c == L',' && cnt == 1 && raw.size() - last - 1 == 3 && last <= 3) thousands = true;  // "1,800"
            dec = thousands ? 0 : c;
        }
    }
    std::string num;
    for (wchar_t c : raw) {
        if (iswdigit(c)) num += (char)c;
        else if (c == dec) num += '.';
    }
    out = atof(num.c_str());
    return true;
}

std::vector<Tok> Lex(const std::wstring& s, int start, int end) {
    std::vector<Tok> t;
    size_t i = start;
    while ((int)i < end) {
        wchar_t c = s[i];
        if (c == L' ' || c == L'\t' || c == L' ') { i++; continue; }
        Tok k; k.pos = (int)i;
        if (c == L'R' && i + 1 < s.size() && s[i + 1] == L'$') { i += 2; continue; }   // R$ 10
        if (c == L'$' || c == L'€' || c == L'£') { i++; t.push_back({ Tok::OP, 0, L"", L'$', (int)i - 1, 1, true }); continue; }
        if (iswdigit(c)) {
            double v; ParseNumber(s, i, v);
            k.k = Tok::NUM; k.v = v; k.len = (int)i - k.pos;
            if ((int)i < end && s[i] == L'%') { k.v /= 100; i++; k.len++; }
            t.push_back(k); continue;
        }
        if (IsIdStart(c)) {
            while ((int)i < end && IsIdChar(s[i])) i++;
            k.k = Tok::ID; k.s = s.substr(k.pos, i - k.pos); k.len = (int)i - k.pos;
            t.push_back(k); continue;
        }
        if (c == L'(') { k.k = Tok::LP; i++; t.push_back(k); continue; }
        if (c == L')') { k.k = Tok::RP; i++; t.push_back(k); continue; }
        wchar_t op = 0;
        switch (c) {
        case L'+': op = L'+'; break;
        case L'-': case L'−': case L'–': op = L'-'; break;
        case L'*': case L'×': case L'⋅': case L'·': op = L'*'; break;
        case L'/': case L'÷': op = L'/'; break;
        case L'^': op = L'^'; break;
        case L'√': op = L'r'; break;  // √
        }
        if (!op) { k.k = Tok::BAD; t.push_back(k); return t; }
        k.k = Tok::OP; k.op = op; i++; t.push_back(k);
    }
    t.push_back({ Tok::END });
    return t;
}

std::wstring LowerS(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

struct Parser {
    std::vector<Tok>& t; size_t i = 0;
    const std::map<std::wstring, double>& vars;
    bool ok = true, money = false, usedOp = false;
    std::vector<std::pair<int, int>>* hits;
    double Expr() {
        double v = Term();
        while (ok && t[i].k == Tok::OP && (t[i].op == L'+' || t[i].op == L'-')) { wchar_t o = t[i++].op; usedOp = true; double r = Term(); v = o == L'+' ? v + r : v - r; }
        return v;
    }
    double Term() {
        double v = Pow();
        for (;;) {
            if (!ok) return v;
            if (t[i].k == Tok::OP && (t[i].op == L'*' || t[i].op == L'/')) { wchar_t o = t[i++].op; usedOp = true; double r = Pow(); v = o == L'*' ? v * r : v / r; continue; }
            if (t[i].k == Tok::ID && LowerS(t[i].s) == L"x" && !vars.count(L"x")) { i++; usedOp = true; v *= Pow(); continue; }  // 2 x 3
            if (t[i].k == Tok::LP || (t[i].k == Tok::ID && vars.count(LowerS(t[i].s)))) { usedOp = true; v *= Pow(); continue; }  // 2(3) / 2 largura
            return v;
        }
    }
    double Pow() {
        double b = Unary();
        if (ok && t[i].k == Tok::OP && t[i].op == L'^') { i++; usedOp = true; double e = Pow(); return std::pow(b, e); }
        return b;
    }
    double Unary() {
        if (t[i].k == Tok::OP && t[i].op == L'-') { i++; return -Unary(); }
        if (t[i].k == Tok::OP && t[i].op == L'+') { i++; return Unary(); }
        if (t[i].k == Tok::OP && t[i].op == L'$') { i++; money = true; return Unary(); }
        if (t[i].k == Tok::OP && t[i].op == L'r') { i++; usedOp = true; return std::sqrt(Unary()); }
        return Atom();
    }
    double Atom() {
        Tok& k = t[i];
        if (k.k == Tok::NUM) { i++; return k.v; }
        if (k.k == Tok::LP) { i++; double v = Expr(); if (t[i].k == Tok::RP) i++; else ok = false; return v; }
        if (k.k == Tok::ID) {
            std::wstring n = LowerS(k.s);
            static const std::map<std::wstring, double (*)(double)> fn = {
                { L"sqrt", [](double x) { return std::sqrt(x); } }, { L"raiz", [](double x) { return std::sqrt(x); } },
                { L"sin", [](double x) { return std::sin(x); } }, { L"sen", [](double x) { return std::sin(x); } },
                { L"cos", [](double x) { return std::cos(x); } }, { L"tan", [](double x) { return std::tan(x); } },
                { L"ln", [](double x) { return std::log(x); } }, { L"log", [](double x) { return std::log10(x); } },
                { L"abs", [](double x) { return std::fabs(x); } },
            };
            auto f = fn.find(n);
            if (f != fn.end() && t[i + 1].k == Tok::LP) { i++; usedOp = true; return f->second(Atom()); }
            if (n == L"pi" || n == L"π") { i++; return 3.14159265358979323846; }
            auto it = vars.find(n);
            if (it != vars.end()) { if (hits) hits->push_back({ k.pos, k.len }); i++; usedOp = true; return it->second; }
        }
        ok = false;
        return 0;
    }
};

// número no formato do Brasil: 1.800 / 2,5 / 0,333333
std::wstring Format(double v, bool money) {
    if (!std::isfinite(v)) return L"Erro";
    wchar_t b[64];
    if (money) swprintf_s(b, L"%.2f", v);
    else {
        swprintf_s(b, L"%.10f", v);
        std::wstring s = b;
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
        wcscpy_s(b, s.c_str());
        if (std::fabs(v) >= 1e15) swprintf_s(b, L"%g", v);
    }
    std::wstring s = b, ip = s, fp;
    bool neg = !ip.empty() && ip[0] == L'-';
    if (neg) ip.erase(0, 1);
    size_t dot = ip.find(L'.');
    if (dot != std::wstring::npos) { fp = ip.substr(dot + 1); ip.resize(dot); }
    if (ip.find(L'e') == std::wstring::npos) for (int k = (int)ip.size() - 3; k > 0; k -= 3) ip.insert(k, L".");
    std::wstring r = (neg ? L"-" : L"") + (money ? std::wstring(L"R$ ") : L"") + ip + (fp.empty() ? L"" : L"," + fp);
    return r;
}

}  // namespace

std::vector<MathLine> EvalMath(const std::vector<Para>& ps) {
    std::vector<MathLine> out(ps.size());
    std::map<std::wstring, double> vars;
    for (size_t li = 0; li < ps.size(); li++) {
        const std::wstring& s = ps[li].text;
        if (s.empty() || s.size() > 400 || ps[li].style == S_MONO) continue;
        MathLine& m = out[li];
        int end = (int)s.size();
        while (end > 0 && (s[end - 1] == L' ' || s[end - 1] == L'\t')) end--;
        bool wantsResult = end > 0 && s[end - 1] == L'=';
        int exprEnd = wantsResult ? end - 1 : end;
        // "nome = expr": a variável
        int start = 0;
        std::wstring defName;
        int defPos = -1, defLen = 0;
        {
            size_t i = 0;
            while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) i++;
            size_t a = i;
            if (i < s.size() && IsIdStart(s[i])) {
                while (i < s.size() && IsIdChar(s[i])) i++;
                size_t idEnd = i;
                while (i < s.size() && s[i] == L' ') i++;
                if (i < (size_t)exprEnd && s[i] == L'=') {
                    defName = LowerS(s.substr(a, idEnd - a));
                    defPos = (int)a; defLen = (int)(idEnd - a);
                    start = (int)i + 1;
                }
            }
        }
        if (start >= exprEnd && !wantsResult) continue;
        std::vector<Tok> toks = Lex(s, start, exprEnd);
        if (toks.empty() || toks.back().k != Tok::END || toks.size() == 1) continue;
        std::vector<std::pair<int, int>> hits;
        Parser p{ toks, 0, vars, true, false, false, &hits };
        double v = p.Expr();
        if (!p.ok || toks[p.i].k != Tok::END) continue;
        if (!defName.empty()) { vars[defName] = v; m.math = true; m.vars.push_back({ defPos, defLen }); }
        if (wantsResult && (p.usedOp || !defName.empty() || !hits.empty())) { m.math = true; m.result = Format(v, p.money); }
        if (m.math) m.vars.insert(m.vars.end(), hits.begin(), hits.end());
    }
    return out;
}
