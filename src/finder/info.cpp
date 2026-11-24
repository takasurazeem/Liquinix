// "Obter Informações" (Ctrl+I): a janela de informações do Finder, uma por item.
// Cabeçalho com ícone, nome, tamanho e data; campo de etiquetas; seções que abrem e fecham com o
// triângulo (Geral, Mais Informações, Nome e Extensão, Comentários, Abrir com, Pré-visualização,
// Compartilhamento e Permissões). O tamanho de uma pasta é calculado numa thread de fundo.
// "Bloqueado" é o atributo somente leitura do arquivo (reversível pela própria caixa).
// Compartilhamento e Permissões: quem tem acesso, lido das permissões de verdade (DACL). O cadeado pede
// a senha de administrador (UAC) e reabre a janela como administrador, onde cada privilégio pode ser
// trocado (Leitura e Gravação, Somente Leitura, Somente Gravação, Sem Acesso), como no Mac.
#include "finder.h"
#include "../menu/context_menu.h"
#include <aclapi.h>
#include <sddl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "../comum/idioma.h"
#include "../comum/semaforo.h"

namespace {

const float PAD = 16, HDR_ROW = 24, ROW = 17;
// macOS 27 Golden Gate (captura Finder-Get-Info, 2x): janela de 301,5 de largura, barra de título de 31,5 (botões com
// o centro em 15,75), rótulos alinhados a 107 da borda
#define W (semaforo::Aqua() ? 301.5f : 265.f)
#define TITLE_H (semaforo::Aqua() ? 31.5f : 28.f)
#define LABEL_X (semaforo::Aqua() ? 107.f : 92.f)
const UINT WM_INFO_CALC = WM_APP + 1;

enum Sec { S_GENERAL, S_MORE, S_NAME, S_COMMENTS, S_OPENWITH, S_PREVIEW, S_SHARING, S_COUNT };
const wchar_t* SEC_NAMES[S_COUNT] = { Tr(L"Geral:"), Tr(L"Mais Informações:"), Tr(L"Nome e Extensão:"), Tr(L"Comentários:"), Tr(L"Abrir com:"), Tr(L"Pré-visualização:"), Tr(L"Compartilhamento e Permissões:") };

struct Calc {  // tamanho de pasta, calculado em segundo plano
    std::atomic<bool> cancel{ false }, done{ false };
    std::mutex m;
    unsigned long long bytes = 0, disk = 0, items = 0;
};

enum Priv { P_NONE, P_READ, P_WRITE, P_RW };
const wchar_t* PRIV_NAMES[4] = { Tr(L"Sem Acesso"), Tr(L"Somente Leitura"), Tr(L"Somente Gravação"), Tr(L"Leitura e Gravação") };

struct Perm {
    std::vector<BYTE> sid;
    std::wstring name;
    int priv = P_NONE;
    bool me = false, inherited = false;
};

struct Info {
    HWND hwnd = nullptr;
    bool admin = false;      // janela aberta como administrador: privilégios editáveis
    bool unlocked = false;   // cadeado aberto (só como administrador)
    std::vector<Perm> perms;
    std::vector<D2D1_RECT_F> permHit;
    D2D1_RECT_F lockHit{};
    ID2D1HwndRenderTarget* rt = nullptr;
    std::wstring path, name, kind, where, openWith, created, modified, accessed, headerDate, sizeText, headerSize;
    bool dir = false, readOnly = false, canWrite = true, isDrive = false;
    unsigned tagMask = 0;
    bool open[S_COUNT] = { true, false, false, false, false, true, false };
    std::shared_ptr<Calc> calc;
    std::vector<BYTE> iconPx, prevPx; UINT iw = 0, ih = 0, pw = 0, ph = 0;
    ID2D1Bitmap* icon = nullptr; ID2D1Bitmap* prev = nullptr;
    D2D1_RECT_F secHit[S_COUNT]{}, lockedHit{};
    int hoverLight = -1, pressLight = -1;
    bool active = true, hoverLights = false;
    float contentH = 0;
};

ID2D1Factory* g_f;
bool g_standalone;  // processo só desta janela (aberto como administrador)
std::wstring g_family = L"Segoe UI";
bool g_darkMode = true;
std::wstring g_packDir;

IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(gfx::dw->CreateTextFormat(g_family.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf))) return nullptr;
    tf->SetTextAlignment(a);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return tf;
}

// ---- textos

std::wstring Thousands(unsigned long long v) {
    std::wstring s = std::to_wstring(v), out;
    int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) { out.insert(out.begin(), s[i]); if (++n % 3 == 0 && i > 0) out.insert(out.begin(), L'.'); }
    return out;
}

// unidades decimais, como no Mac: 999 bytes, 16 KB, 1,2 MB, 12,5 GB
std::wstring Human(unsigned long long b) {
    if (b < 1000) return std::to_wstring(b) + (b == 1 ? Tr(L" byte") : Tr(L" bytes"));
    const wchar_t* u[] = { L"KB", L"MB", L"GB", L"TB" };
    double v = (double)b; int k = -1;
    while (v >= 1000 && k < 3) { v /= 1000; k++; }
    wchar_t buf[32];
    if (k == 0 || v >= 100) swprintf_s(buf, L"%.0f %ls", v, u[k]);
    else swprintf_s(buf, L"%.1f %ls", v, u[k]);
    for (wchar_t* p = buf; *p; p++) if (*p == L'.') *p = L',';
    return buf;
}

std::wstring FullDate(const FILETIME& ft) {
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return L"--";
    FILETIME lt; SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
    wchar_t d[96] = {}, t[32] = {};
    GetDateFormatEx(idioma::Locale(), 0, &st, idioma::DatePattern(idioma::D_LONG), d, 96, nullptr);
    GetTimeFormatEx(L"pt-BR", 0, &st, L"HH:mm", t, 32);
    return std::wstring(d) + Tr(L" às ") + t;
}

std::wstring ShortDate(const FILETIME& ft) {  // cabeçalho: "Hoje, 18:20", "Ontem, 09:02" ou a data
    FILETIME lt; SYSTEMTIME st, now;
    FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
    GetLocalTime(&now);
    wchar_t t[32] = {};
    GetTimeFormatEx(L"pt-BR", 0, &st, L"HH:mm", t, 32);
    FILETIME a, b; SYSTEMTIME d0 = st, d1 = now;
    d0.wHour = d0.wMinute = d0.wSecond = d0.wMilliseconds = 0; d1.wHour = d1.wMinute = d1.wSecond = d1.wMilliseconds = 0;
    SystemTimeToFileTime(&d0, &a); SystemTimeToFileTime(&d1, &b);
    long long days = ((((long long)b.dwHighDateTime << 32) | b.dwLowDateTime) - (((long long)a.dwHighDateTime << 32) | a.dwLowDateTime)) / 864000000000LL;
    if (days == 0) return std::wstring(Tr(L"Hoje, ")) + t;
    if (days == 1) return std::wstring(Tr(L"Ontem, ")) + t;
    return FullDate(ft);
}

unsigned long long Cluster(const std::wstring& path) {
    wchar_t root[MAX_PATH] = {};
    if (!GetVolumePathNameW(path.c_str(), root, MAX_PATH)) return 4096;
    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &fc, &tc)) return 4096;
    return (unsigned long long)spc * bps;
}

unsigned long long OnDisk(unsigned long long bytes, unsigned long long cl) { return cl ? (bytes + cl - 1) / cl * cl : bytes; }

void Walk(const std::wstring& dir, Calc& c, unsigned long long cl) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (c.cancel) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        {
            std::lock_guard<std::mutex> g(c.m);
            c.items++;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { c.bytes += sz; c.disk += OnDisk(sz, cl); }
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            Walk(dir + L"\\" + fd.cFileName, c, cl);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---- imagens

bool ShellPixels(const std::wstring& path, int px, bool iconOnly, std::vector<BYTE>& out, UINT& w, UINT& h) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    HRESULT hr = f->GetImage(SIZE{ px, px }, SIIGBF_RESIZETOFIT | (iconOnly ? SIIGBF_ICONONLY : 0), &hb);
    f->Release();
    if (FAILED(hr) || !hb) return false;
    BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
    w = bm.bmWidth; h = std::abs(bm.bmHeight);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB };
    out.resize((size_t)w * h * 4);
    HDC screen = GetDC(nullptr);
    GetDIBits(screen, hb, 0, h, out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(hb);
    bool anyAlpha = false;
    for (size_t i = 3; i < out.size(); i += 4) if (out[i]) { anyAlpha = true; break; }
    if (!anyAlpha) for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return true;
}

bool PngPixels(const std::wstring& file, std::vector<BYTE>& out, UINT& w, UINT& h) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        cv->GetSize(&w, &h);
        out.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)out.size(), out.data()));
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    wic->Release();
    return ok;
}

ID2D1Bitmap* ToBitmap(ID2D1RenderTarget* rt, const std::vector<BYTE>& px, UINT w, UINT h) {
    if (px.empty()) return nullptr;
    ID2D1Bitmap* b = nullptr;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, bp, &b);
    return b;
}

// ---- permissões (DACL)

std::wstring SidName(PSID sid) {
    wchar_t name[256], dom[256]; DWORD nn = 256, dn = 256; SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, sid, name, &nn, dom, &dn, &use)) {
        std::wstring n = name;
        if (n == L"Everyone" || n == L"Todos") return L"todos";
        if (n == L"Users" || n == L"Usuários") return L"usuários";
        if (n == L"Administrators" || n == L"Administradores") return L"administradores";
        if (n == L"SYSTEM" || n == L"SISTEMA") return L"sistema";
        if (n == L"Authenticated Users" || n == L"Usuários autenticados") return L"usuários autenticados";
        return n;
    }
    LPWSTR str = nullptr;
    std::wstring r = ConvertSidToStringSidW(sid, &str) ? str : Tr(L"(desconhecido)");
    if (str) LocalFree(str);
    return r;
}

std::vector<BYTE> CurrentUserSid() {
    std::vector<BYTE> out;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    DWORD n = 0; GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> buf(n);
    if (n && GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
        PSID s = ((TOKEN_USER*)buf.data())->User.Sid;
        out.assign((BYTE*)s, (BYTE*)s + GetLengthSid(s));
    }
    CloseHandle(tok);
    return out;
}

void GatherPerms(Info& in) {
    in.perms.clear();
    PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(in.path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) return;
    auto me = CurrentUserSid();
    auto add = [&](PSID sid) -> Perm& {
        for (auto& p : in.perms) if (EqualSid((PSID)p.sid.data(), sid)) return p;
        Perm p; p.sid.assign((BYTE*)sid, (BYTE*)sid + GetLengthSid(sid)); p.name = SidName(sid);
        p.me = !me.empty() && EqualSid(sid, (PSID)me.data());
        in.perms.push_back(p);
        return in.perms.back();
    };
    if (owner) add(owner);  // o dono primeiro, como no Mac
    std::vector<ACCESS_MASK> allow(64, 0);
    if (dacl) {
        for (DWORD i = 0; i < dacl->AceCount; i++) {
            ACE_HEADER* h = nullptr;
            if (!GetAce(dacl, i, (void**)&h)) continue;
            if (h->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
            if (h->AceFlags & INHERIT_ONLY_ACE) continue;  // só vale para o que fica dentro
            auto* a = (ACCESS_ALLOWED_ACE*)h;
            PSID sid = (PSID)&a->SidStart;
            wchar_t* str = nullptr; ConvertSidToStringSidW(sid, &str);
            bool skip = str && (!wcsncmp(str, L"S-1-15-", 7) || !wcscmp(str, L"S-1-5-18") || !wcsncmp(str, L"S-1-5-80-", 9));  // apps, sistema, serviços
            if (str) LocalFree(str);
            if (skip) continue;
            Perm& p = add(sid);
            ACCESS_MASK m = a->Mask;
            bool r = (m & (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL)) != 0;
            bool w = (m & (FILE_WRITE_DATA | GENERIC_WRITE | GENERIC_ALL)) != 0;
            int pv = r && w ? P_RW : r ? P_READ : w ? P_WRITE : P_NONE;
            if (pv == P_RW || (pv == P_READ && p.priv == P_WRITE) || (pv == P_WRITE && p.priv == P_READ)) p.priv = P_RW;
            else if (p.priv == P_NONE) p.priv = pv;
            if (h->AceFlags & INHERITED_ACE) p.inherited = true;
        }
    }
    if (sd) LocalFree(sd);
    if (in.perms.size() > 6) in.perms.resize(6);
}

// troca o privilégio de alguém (precisa de administrador); devolve false se o Windows recusar
bool SetPriv(const std::wstring& path, bool dir, const std::vector<BYTE>& sid, int priv) {
    PACL dacl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) return false;
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessMode = priv == P_NONE ? REVOKE_ACCESS : SET_ACCESS;
    ea.grfAccessPermissions = priv == P_RW ? (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE)
                            : priv == P_READ ? (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)
                            : priv == P_WRITE ? FILE_GENERIC_WRITE : 0;
    ea.grfInheritance = dir ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.ptstrName = (LPWSTR)sid.data();
    PACL nd = nullptr;
    DWORD r = SetEntriesInAclW(1, &ea, dacl, &nd);
    if (r == ERROR_SUCCESS) r = SetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nd, nullptr);
    if (nd) LocalFree(nd);
    if (sd) LocalFree(sd);
    Log(L"info: privilégio de %ls -> %d em %ls: %ls", L"(sid)", priv, path.c_str(), r == ERROR_SUCCESS ? L"ok" : L"recusado");
    return r == ERROR_SUCCESS;
}

// ---- dados do item

void Gather(Info& in) {
    std::wstring p = in.path;
    in.isDrive = p.size() <= 3 && p.size() >= 2 && p[1] == L':';
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME | SHGFI_TYPENAME)) { in.name = sfi.szDisplayName; in.kind = sfi.szTypeName; }
    if (!in.isDrive) in.name = PathFindFileNameW(p.c_str());
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa);
    in.dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    in.readOnly = !in.dir && (fa.dwFileAttributes & FILE_ATTRIBUTE_READONLY);
    if (in.dir && !in.isDrive) in.kind = Tr(L"Pasta");
    if (in.isDrive) in.kind = Tr(L"Volume");
    std::wstring parent = p;
    if (!in.isDrive) { size_t s = parent.find_last_of(L'\\'); parent = s == std::wstring::npos ? L"" : parent.substr(0, s); if (parent.size() == 2) parent += L"\\"; }
    in.where = in.isDrive ? Tr(L"Este Computador") : parent;
    in.created = FullDate(fa.ftCreationTime);
    in.modified = FullDate(fa.ftLastWriteTime);
    in.accessed = FullDate(fa.ftLastAccessTime);
    in.headerDate = Tr(L"Modificado: ") + ShortDate(fa.ftLastWriteTime);
    in.tagMask = tags::Read(p);
    if (in.isDrive) {
        ULARGE_INTEGER freeB{}, total{}, totalFree{};
        GetDiskFreeSpaceExW(p.c_str(), &freeB, &total, &totalFree);
        in.headerSize = Human(total.QuadPart);
        in.sizeText = Human(total.QuadPart) + L" (" + Human(totalFree.QuadPart) + Tr(L" disponíveis)");
    } else if (in.dir) {
        in.headerSize = L"--";
        in.sizeText = Tr(L"Calculando…");
    } else {
        unsigned long long sz = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        in.headerSize = Human(sz);
        in.sizeText = Thousands(sz) + Tr(L" bytes (") + Human(OnDisk(sz, Cluster(p))) + Tr(L" no disco)");
        wchar_t app[260] = {}; DWORD n = 260;
        const wchar_t* ext = PathFindExtensionW(p.c_str());
        if (*ext && SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME, ext, L"open", app, &n))) in.openWith = app;
        else in.openWith = Tr(L"Nenhum aplicativo");
    }
    // permissões: tenta abrir para escrita sem mexer em nada
    DWORD acc = in.dir ? FILE_ADD_FILE : FILE_WRITE_DATA;
    HANDLE hf = CreateFileW(p.c_str(), acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, in.dir ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
    in.canWrite = hf != INVALID_HANDLE_VALUE;
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    GatherPerms(in);
    // ícone e pré-visualização: pasta azul do pacote do Sequoia; arquivo: miniatura do Windows
    if (in.dir && !in.isDrive && PngPixels(g_packDir + L"\\folder default.png", in.iconPx, in.iw, in.ih)) {
        in.prevPx = in.iconPx; in.pw = in.iw; in.ph = in.ih;
    } else {
        ShellPixels(p, 64, false, in.iconPx, in.iw, in.ih);
        ShellPixels(p, 256, false, in.prevPx, in.pw, in.ph);
    }
}

// ---- layout e desenho

// valores longos quebram linha, como no Mac (tamanho com bytes, datas, caminho)
float ValueH(const std::wstring& v) {
    IDWriteTextFormat* tf = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL);
    if (!tf) return ROW;
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    IDWriteTextLayout* lay = nullptr; float h = ROW;
    if (SUCCEEDED(gfx::dw->CreateTextLayout(v.c_str(), (UINT32)v.size(), tf, W - PAD - LABEL_X, 400, &lay))) {
        DWRITE_TEXT_METRICS m{}; lay->GetMetrics(&m); h = std::max(ROW, m.height + 3); lay->Release();
    }
    tf->Release();
    return h;
}

float SectionBodyH(const Info& in, int s) {
    switch (s) {
    case S_GENERAL: return ValueH(in.kind) + ValueH(in.sizeText) + ValueH(in.where) + ValueH(in.created) + ValueH(in.modified) + 8 + (in.dir ? 0 : 22) + 6;
    case S_MORE: return ValueH(in.accessed) + 8;
    case S_NAME: return 26 + 22 + 6;
    case S_COMMENTS: return 50;
    case S_OPENWITH: return ROW + 8;
    case S_PREVIEW: return 150;
    case S_SHARING: return ROW + 6 + 20 + (float)std::max<size_t>(1, in.perms.size()) * 19 + 8 + 24 + 6;
    }
    return 0;
}

bool HasSection(const Info& in, int s) { return s != S_OPENWITH || !in.dir; }

float ContentHeight(const Info& in) {
    float y = TITLE_H + 8 + 44 + 34;
    for (int s = 0; s < S_COUNT; s++) {
        if (!HasSection(in, s)) continue;
        y += HDR_ROW;
        if (in.open[s]) y += SectionBodyH(in, s);
    }
    return y + 10;
}

void Text(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (!tf || s.empty()) return;
    br->SetColor(c);
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Chevron(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, float cx, float cy, bool open) {
    D2D1_POINT_2F a, b, c;
    if (open) { a = { cx - 3.5f, cy - 1.8f }; b = { cx, cy + 1.8f }; c = { cx + 3.5f, cy - 1.8f }; }
    else { a = { cx - 1.8f, cy - 3.5f }; b = { cx + 1.8f, cy }; c = { cx - 1.8f, cy + 3.5f }; }
    rt->DrawLine(a, b, br, 1.4f); rt->DrawLine(b, c, br, 1.4f);
}

void Checkbox(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, float x, float cy, bool on, bool enabled, bool dark) {
    D2D1_RECT_F r{ x, cy - 7, x + 14, cy + 7 };
    if (on) {
        br->SetColor(enabled ? D2D1::ColorF(0.04f, 0.47f, 1.0f) : D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.5f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 3.5f, 3.5f), br);
        br->SetColor(D2D1::ColorF(1, 1, 1));
        rt->DrawLine({ x + 3.5f, cy }, { x + 6, cy + 3 }, br, 1.6f);
        rt->DrawLine({ x + 6, cy + 3 }, { x + 10.5f, cy - 3.5f }, br, 1.6f);
    } else {
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, enabled ? 0.12f : 0.06f) : D2D1::ColorF(1, 1, 1));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 3.5f, 3.5f), br);
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.22f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 3, 3), br, 1);
    }
}

void Paint(Info& in) {
    if (!in.rt) return;
    ID2D1HwndRenderTarget* rt = in.rt;
    in.permHit.clear(); in.lockHit = {}; in.lockedHit = {};  // só vale o que for desenhado agora (seção fechada não tem alvo)
    bool dark = g_darkMode;
    rt->BeginDraw();
    rt->Clear(dark ? D2D1::ColorF(0.157f, 0.157f, 0.165f) : D2D1::ColorF(0.925f, 0.925f, 0.93f));
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.88f) : D2D1::ColorF(0, 0, 0, 0.85f);
    D2D1_COLOR_F text2 = dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.50f);
    D2D1_COLOR_F line = dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.10f);

    // bolinhas: fechar e minimizar ativas; ampliar desligada (a janela tem tamanho fixo)
    static const D2D1_COLOR_F fill_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.345f), D2D1::ColorF(0.996f, 0.737f, 0.180f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
    static const D2D1_COLOR_F ring_c[3] = { D2D1::ColorF(0.886f, 0.275f, 0.247f), D2D1::ColorF(0.882f, 0.631f, 0.086f), D2D1::ColorF(0.071f, 0.675f, 0.157f) }; const D2D1_COLOR_F* ring = semaforo::Pick(ring_c, semaforo::RING);
    for (int k = 0; k < 3; k++) {
        D2D1_POINT_2F c{ (semaforo::Aqua() ? 15.75f : 19.f) + k * 23, semaforo::Aqua() ? 15.75f : 14.f };  // padrão do projeto: 14 pt, 23 entre centros (macOS 27: centro em 15,75)
        bool lit = k < 2 && (in.active || in.hoverLights);
        br->SetColor(lit ? fill[k] : dark ? D2D1::ColorF(0.30f, 0.30f, 0.31f) : D2D1::ColorF(0.84f, 0.84f, 0.84f));
        rt->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
        semaforo::Gloss(rt, c, 7, br);
        br->SetColor(lit ? ring[k] : dark ? D2D1::ColorF(0, 0, 0, 0.3f) : D2D1::ColorF(0.73f, 0.73f, 0.74f));
        rt->DrawEllipse(D2D1::Ellipse(c, 6.75f, 6.75f), br, 0.5f);
        if (in.hoverLights && k < 2) {
            br->SetColor(semaforo::Grafite() ? D2D1::ColorF(0.20f, 0.20f, 0.21f) : k == 0 ? D2D1::ColorF(0.302f, 0, 0) : D2D1::ColorF(0.6f, 0.341f, 0));
            if (k == 0) { rt->DrawLine({ c.x - 2.5f, c.y - 2.5f }, { c.x + 2.5f, c.y + 2.5f }, br, 1.1f); rt->DrawLine({ c.x - 2.5f, c.y + 2.5f }, { c.x + 2.5f, c.y - 2.5f }, br, 1.1f); }
            else rt->DrawLine({ c.x - 3.3f, c.y }, { c.x + 3.3f, c.y }, br, 1.2f);
        }
    }
    IDWriteTextFormat* tTitle = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (tTitle) tTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (tTitle) { DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; IDWriteInlineObject* el = nullptr; gfx::dw->CreateEllipsisTrimmingSign(tTitle, &el); tTitle->SetTrimming(&tr, el); if (el) el->Release(); }
    {   // título centrado na janela inteira (as bolinhas ficam fora); nome longo é cortado no meio
        std::wstring nm = in.name, t = Tr(L"Informações de ") + nm;
        float maxW = W - 80 - 10 - 2;  // das bolinhas até a borda (2 de folga para o arredondamento)
        auto width = [&](const std::wstring& x) {
            IDWriteTextLayout* tl = nullptr; float w2 = 0;
            if (tTitle && SUCCEEDED(gfx::dw->CreateTextLayout(x.c_str(), (UINT32)x.size(), tTitle, 2000, 30, &tl))) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); w2 = m.width; tl->Release(); }
            return w2;
        };
        if (width(t) > maxW) {
            for (size_t keep = nm.size(); keep > 2; keep--) {
                size_t head = (keep + 1) / 2, tail = keep / 2;
                t = Tr(L"Informações de ") + nm.substr(0, head) + L"…" + nm.substr(nm.size() - tail);
                if (width(t) <= maxW) break;
            }
        }
        float tw = width(t);  // centrado na janela; se bater nas bolinhas, desliza para a direita (como no Mac)
        float x0 = std::max(80.f, (W - tw) / 2);
        IDWriteTextFormat* tl = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (tl) tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Text(rt, br, t, tl, D2D1::RectF(x0, 6, W - 8, 24), in.active ? text : text2);
        if (tl) tl->Release();
    }

    // cabeçalho: ícone 32, nome em negrito, tamanho à direita, data embaixo
    float y = TITLE_H + 8;
    if (!in.icon) in.icon = ToBitmap(rt, in.iconPx, in.iw, in.ih);
    if (in.icon) {
        float s = 32, aw = (float)in.iw, ah = (float)in.ih, k = std::min(s / aw, s / ah);
        float dw = aw * k, dh = ah * k;
        rt->DrawBitmap(in.icon, D2D1::RectF(PAD + (s - dw) / 2, y + (s - dh) / 2, PAD + (s + dw) / 2, y + (s + dh) / 2), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    IDWriteTextFormat* tName = Fmt(13, DWRITE_FONT_WEIGHT_BOLD);
    IDWriteTextFormat* tSizeR = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    IDWriteTextFormat* tSmall = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* tLabel = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    IDWriteTextFormat* tSec = Fmt(11, DWRITE_FONT_WEIGHT_BOLD);
    for (auto* f : { tName, tSmall }) if (f) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; IDWriteInlineObject* el = nullptr;
        gfx::dw->CreateEllipsisTrimmingSign(f, &el); f->SetTrimming(&tr, el); if (el) el->Release();
    }
    float sizeW = 70;
    Text(rt, br, in.headerSize, tSizeR, D2D1::RectF(W - PAD - sizeW, y, W - PAD, y + 18), text);
    Text(rt, br, in.name, tName, D2D1::RectF(PAD + 40, y, W - PAD - sizeW - 4, y + 18), text);
    Text(rt, br, in.headerDate, tSmall, D2D1::RectF(PAD + 40, y + 19, W - PAD, y + 34), text2);
    y += 44;

    // etiquetas: campo com as bolinhas atuais ou o texto de ajuda
    D2D1_RECT_F tagField{ PAD, y, W - PAD, y + 22 };
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(1, 1, 1, 1));
    rt->FillRoundedRectangle(D2D1::RoundedRect(tagField, 5, 5), br);
    br->SetColor(line);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tagField.left + 0.5f, tagField.top + 0.5f, tagField.right - 0.5f, tagField.bottom - 0.5f), 4.5f, 4.5f), br, 1);
    float tx = tagField.left + 8;
    bool anyTag = false;
    for (int t = 0; t < tags::COUNT; t++) {
        if (!(in.tagMask & (1u << t))) continue;
        anyTag = true;
        UINT32 c = tags::colors[t];
        br->SetColor(D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f));
        rt->FillEllipse(D2D1::Ellipse({ tx + 5, y + 11 }, 5, 5), br);
        IDWriteTextLayout* lay = nullptr; float lw = 30;
        std::wstring nm = Tr(tags::names[t]);
        if (tSmall && SUCCEEDED(gfx::dw->CreateTextLayout(nm.c_str(), (UINT32)nm.size(), tSmall, 200, 20, &lay))) { DWRITE_TEXT_METRICS m{}; lay->GetMetrics(&m); lw = m.width; lay->Release(); }
        Text(rt, br, nm, tSmall, D2D1::RectF(tx + 13, y + 4, tagField.right - 4, y + 20), text);
        tx += 13 + lw + 10;
    }
    if (!anyTag) Text(rt, br, Tr(L"Adicionar Etiquetas…"), tSmall, D2D1::RectF(tagField.left + 8, y + 4, tagField.right - 6, y + 20), text2);
    y += 34;

    // seções
    auto row = [&](const std::wstring& label, const std::wstring& value, float yy) {  // devolve a altura usada
        float vh = ValueH(value);
        Text(rt, br, label, tLabel, D2D1::RectF(PAD, yy, LABEL_X - 4, yy + ROW), text);
        IDWriteTextFormat* tv = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL);
        if (tv) tv->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        Text(rt, br, value, tv, D2D1::RectF(LABEL_X, yy, W - PAD, yy + vh), text);
        if (tv) tv->Release();
        return vh;
    };
    for (int s = 0; s < S_COUNT; s++) {
        if (!HasSection(in, s)) { in.secHit[s] = {}; continue; }
        br->SetColor(line);
        rt->DrawLine({ PAD - 4, y + 0.5f }, { W - PAD + 4, y + 0.5f }, br, 1);
        in.secHit[s] = D2D1::RectF(0, y, W, y + HDR_ROW);
        br->SetColor(text2);
        Chevron(rt, br, PAD + 1, y + HDR_ROW / 2, in.open[s]);
        Text(rt, br, SEC_NAMES[s], tSec, D2D1::RectF(PAD + 12, y + 5, W - PAD, y + HDR_ROW), text);
        y += HDR_ROW;
        if (!in.open[s]) continue;
        float yy = y + 2;
        switch (s) {
        case S_GENERAL:
            yy += row(Tr(L"Tipo:"), in.kind, yy);
            yy += row(Tr(L"Tamanho:"), in.sizeText, yy);
            yy += row(Tr(L"Onde:"), in.where, yy);
            yy += row(Tr(L"Criado:"), in.created, yy);
            yy += row(Tr(L"Modificado:"), in.modified, yy) + 6;
            if (!in.dir) {
                Checkbox(rt, br, LABEL_X, yy + 8, in.readOnly, true, dark);
                in.lockedHit = D2D1::RectF(LABEL_X - 2, yy, LABEL_X + 90, yy + 16);
                Text(rt, br, Tr(L"Bloqueado"), tSmall, D2D1::RectF(LABEL_X + 20, yy + 1, W - PAD, yy + 17), text);
            } else in.lockedHit = {};
            break;
        case S_MORE: row(Tr(L"Última abertura:"), in.accessed, yy); break;
        case S_NAME: {
            D2D1_RECT_F f{ PAD, yy, W - PAD, yy + 22 };
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(1, 1, 1));
            rt->FillRoundedRectangle(D2D1::RoundedRect(f, 5, 5), br);
            Text(rt, br, in.name, tSmall, D2D1::RectF(f.left + 7, yy + 4, f.right - 6, yy + 20), text);
            Checkbox(rt, br, PAD, yy + 37, false, false, dark);
            Text(rt, br, Tr(L"Ocultar extensão"), tSmall, D2D1::RectF(PAD + 20, yy + 29, W - PAD, yy + 45), text2);
            break;
        }
        case S_COMMENTS: {
            D2D1_RECT_F f{ PAD, yy, W - PAD, yy + 40 };
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(1, 1, 1));
            rt->FillRoundedRectangle(D2D1::RoundedRect(f, 5, 5), br);
            break;
        }
        case S_OPENWITH: row(Tr(L"Aplicativo:"), in.openWith, yy); break;
        case S_PREVIEW: {
            if (!in.prev) in.prev = ToBitmap(rt, in.prevPx, in.pw, in.ph);
            if (in.prev) {
                float box = 128, aw = (float)in.pw, ah = (float)in.ph, k = std::min(box / aw, box / ah);
                float dw = aw * k, dh = ah * k, cx = W / 2, cy = yy + 70;
                rt->DrawBitmap(in.prev, D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            break;
        }
        case S_SHARING: {
            Text(rt, br, in.canWrite ? Tr(L"Você pode ler e gravar") : Tr(L"Você só pode ler"), tSmall, D2D1::RectF(PAD, yy, W - PAD, yy + ROW), text);
            float ty = yy + ROW + 6;
            D2D1_RECT_F box{ PAD, ty, W - PAD, ty + 20 + std::max<size_t>(1, in.perms.size()) * 19 };
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.05f) : D2D1::ColorF(1, 1, 1));
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 4, 4), br);
            br->SetColor(line);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), 3.5f, 3.5f), br, 1);
            float col = PAD + (W - 2 * PAD) * 0.44f;  // coluna do privilégio cabe "Leitura e Gravação"
            IDWriteTextFormat* th = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            Text(rt, br, Tr(L"Nome"), th, D2D1::RectF(PAD + 8, ty + 3, col - 4, ty + 18), text2);
            Text(rt, br, Tr(L"Privilégio"), th, D2D1::RectF(col + 6, ty + 3, W - PAD - 4, ty + 18), text2);
            if (th) th->Release();
            br->SetColor(line);
            rt->DrawLine({ PAD, ty + 20.5f }, { W - PAD, ty + 20.5f }, br, 1);
            rt->DrawLine({ col + 0.5f, ty + 3 }, { col + 0.5f, box.bottom - 3 }, br, 1);
            in.permHit.assign(in.perms.size(), {});
            bool editable = in.admin && in.unlocked;
            for (size_t k = 0; k < in.perms.size(); k++) {
                float ry = ty + 21 + k * 19;
                const Perm& pm = in.perms[k];
                // ícone de pessoa (cabeça + ombros), um para cada linha
                br->SetColor(text2);
                rt->FillEllipse(D2D1::Ellipse({ PAD + 14, ry + 7 }, 2.6f, 2.6f), br);
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(PAD + 9.5f, ry + 10.5f, PAD + 18.5f, ry + 15), 2.5f, 2.5f), br);
                Text(rt, br, pm.name + (pm.me ? Tr(L" (Eu)") : L""), tSmall, D2D1::RectF(PAD + 24, ry + 2, col - 4, ry + 18), text);
                D2D1_RECT_F pr{ col + 4, ry + 1, W - PAD - 3, ry + 18 };
                in.permHit[k] = pr;
                Text(rt, br, PRIV_NAMES[pm.priv], tSmall, D2D1::RectF(pr.left + 4, ry + 2, pr.right - 12, ry + 18), editable ? text : text2);
                if (editable) {  // setinhas do menu de privilégio
                    br->SetColor(text2);
                    float ax = pr.right - 6, ay = ry + 9.5f;
                    rt->DrawLine({ ax - 2.5f, ay - 1 }, { ax, ay - 3.5f }, br, 1.1f); rt->DrawLine({ ax, ay - 3.5f }, { ax + 2.5f, ay - 1 }, br, 1.1f);
                    rt->DrawLine({ ax - 2.5f, ay + 1 }, { ax, ay + 3.5f }, br, 1.1f); rt->DrawLine({ ax, ay + 3.5f }, { ax + 2.5f, ay + 1 }, br, 1.1f);
                }
            }
            // cadeado embaixo à direita: fechado = pede a senha; aberto = pode alterar
            float ly = box.bottom + 8, lx = W - PAD - 14;
            in.lockHit = D2D1::RectF(lx - 6, ly - 2, lx + 18, ly + 20);
            br->SetColor(text2);
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(lx, ly + 7, lx + 12, ly + 16), 1.5f, 1.5f), br);
            ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g_f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                float sx = editable ? lx + 8 : lx + 2;  // aberto: a alça sobe para a direita
                sk->BeginFigure({ sx + 1.5f, editable ? ly + 3 : ly + 7 }, D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddLine({ sx + 1.5f, ly + 3.5f });
                sk->AddArc(D2D1::ArcSegment({ sx + 6.5f, ly + 3.5f }, D2D1::SizeF(2.5f, 2.5f), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->AddLine({ sx + 6.5f, ly + 7 });
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close(); sk->Release();
                rt->DrawGeometry(g, br, 1.5f);
            }
            if (g) g->Release();
            IDWriteTextFormat* tHint = Fmt(10, DWRITE_FONT_WEIGHT_NORMAL);
            if (tHint) tHint->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            Text(rt, br, editable ? Tr(L"Clique no cadeado para impedir alterações.") : Tr(L"Clique no cadeado para fazer alterações."), tHint,
                 D2D1::RectF(PAD, ly + 4, lx - 4, ly + 19), text2);
            if (tHint) tHint->Release();
            break;
        }
        }
        y += SectionBodyH(in, s);
    }
    for (auto* f : { tTitle, tName, tSizeR, tSmall, tLabel, tSec }) if (f) f->Release();
    br->Release();
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        if (in.icon) { in.icon->Release(); in.icon = nullptr; }
        if (in.prev) { in.prev->Release(); in.prev = nullptr; }
        in.rt->Release(); in.rt = nullptr;
    }
}

float Scale(HWND h) { return GetDpiForWindow(h) / 96.f; }

void FitHeight(Info& in) {
    float sc = Scale(in.hwnd);
    RECT r; GetWindowRect(in.hwnd, &r);
    int hpx = (int)std::lround(ContentHeight(in) * sc);
    if (r.bottom - r.top != hpx) SetWindowPos(in.hwnd, nullptr, 0, 0, r.right - r.left, hpx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EnsureTarget(Info& in) {
    if (in.rt) return;
    RECT rc; GetClientRect(in.hwnd, &rc);
    float dpi = (float)GetDpiForWindow(in.hwnd);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    g_f->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(in.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &in.rt);
    if (in.rt) in.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

void StartCalc(Info& in) {
    if (!in.dir || in.isDrive) return;
    in.calc = std::make_shared<Calc>();
    auto c = in.calc; std::wstring p = in.path; HWND h = in.hwnd;
    std::thread([c, p, h] {
        unsigned long long cl = Cluster(p);
        DWORD last = GetTickCount();
        std::thread ticker;  // atualizações parciais a cada 300 ms enquanto conta
        std::atomic<bool> stop{ false };
        ticker = std::thread([&] { while (!stop) { Sleep(300); if (!stop && !c->cancel) PostMessageW(h, WM_INFO_CALC, 0, 0); } });
        Walk(p, *c, cl);
        stop = true; ticker.join();
        (void)last;
        c->done = true;
        if (!c->cancel) PostMessageW(h, WM_INFO_CALC, 0, 0);
    }).detach();
}

void UpdateCalcText(Info& in) {
    if (!in.calc) return;
    unsigned long long b, d, n;
    { std::lock_guard<std::mutex> g(in.calc->m); b = in.calc->bytes; d = in.calc->disk; n = in.calc->items; }
    std::wstring items = Thousands(n) + (n == 1 ? Tr(L" item") : Tr(L" itens"));
    if (in.calc->done) {
        in.sizeText = Thousands(b) + Tr(L" bytes (") + Human(d) + Tr(L" no disco) para ") + items;
        in.headerSize = Human(b);
    } else in.sizeText = Tr(L"Calculando… ") + Human(b) + L", " + items;
}

int HitLight(D2D1_POINT_2F p) {
    for (int k = 0; k < 2; k++) if (std::fabs(p.x - ((semaforo::Aqua() ? 15.75f : 19.f) + k * 23)) <= 9 && std::fabs(p.y - (semaforo::Aqua() ? 15.75f : 14.f)) <= 9) return k;
    return -1;
}

bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* in = (Info*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_NCHITTEST: {
        LRESULT r = DefWindowProcW(h, m, w, l);
        if (r != HTCLIENT || !in) return r == HTCAPTION ? HTCLIENT : r;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
        float sc = Scale(h); D2D1_POINT_2F p{ pt.x / sc, pt.y / sc };
        if (HitLight(p) >= 0 || (p.x < 80 && p.y < TITLE_H)) return HTCLIENT;
        for (auto& r2 : in->secHit) if (In(r2, p)) return HTCLIENT;
        if (In(in->lockHit, p) || In(in->lockedHit, p)) return HTCLIENT;
        for (auto& r2 : in->permHit) if (In(r2, p)) return HTCLIENT;
        return HTCAPTION;  // o resto arrasta a janela, como no Mac
    }
    case WM_ACTIVATE: if (in) { in->active = LOWORD(w) != WA_INACTIVE; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_SIZE: if (in && in->rt) in->rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l))); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        if (in && in->rt) { in->rt->Release(); in->rt = nullptr; if (in->icon) { in->icon->Release(); in->icon = nullptr; } if (in->prev) { in->prev->Release(); in->prev = nullptr; } }
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (in) FitHeight(*in);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        if (in) { EnsureTarget(*in); Paint(*in); }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_INFO_CALC: if (in) { UpdateCalcText(*in); FitHeight(*in); InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_MOUSEMOVE: {
        if (!in) break;
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        float sc = Scale(h); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        bool over = p.x < 80 && p.y < TITLE_H;
        if (over != in->hoverLights) { in->hoverLights = over; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE: if (in && in->hoverLights) { in->hoverLights = false; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
        if (!in) break;
        float sc = Scale(h); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        in->pressLight = HitLight(p);
        if (in->pressLight >= 0) { SetCapture(h); return 0; }
        for (int s = 0; s < S_COUNT; s++)
            if (In(in->secHit[s], p)) { in->open[s] = !in->open[s]; FitHeight(*in); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (In(in->lockHit, p) && in->open[S_SHARING] && in->lockHit.bottom > in->lockHit.top && p.y > in->secHit[S_SHARING].top) {
            if (in->admin) { in->unlocked = !in->unlocked; InvalidateRect(h, nullptr, FALSE); return 0; }
            // pede a senha de administrador (UAC) e reabre esta janela como administrador, no mesmo lugar
            wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH);
            RECT wr; GetWindowRect(h, &wr);
            std::wstring args = L"--info-admin \"" + in->path + L"\" " + std::to_wstring(wr.left) + L" " + std::to_wstring(wr.top);
            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_NOASYNC; sei.hwnd = h; sei.lpVerb = L"runas"; sei.lpFile = exe; sei.lpParameters = args.c_str(); sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei)) DestroyWindow(h);  // a de administrador ocupa o lugar desta
            return 0;
        }
        if (in->admin && in->unlocked)
            for (size_t k = 0; k < in->permHit.size() && k < in->perms.size(); k++)
                if (In(in->permHit[k], p)) {
                    std::vector<ctxmenu::Item> items;
                    for (int v = P_RW; v >= P_NONE; v--) { ctxmenu::Item it; it.label = PRIV_NAMES[v]; it.id = v + 1; it.checked = in->perms[k].priv == v; items.push_back(it); }
                    float sc2 = Scale(h);
                    POINT sp{ (LONG)(in->permHit[k].left * sc2), (LONG)(in->permHit[k].bottom * sc2) };
                    ClientToScreen(h, &sp);
                    int id = ctxmenu::ShowItems(h, sp, items);
                    if (id > 0 && id - 1 != in->perms[k].priv) {
                        if (!SetPriv(in->path, in->dir, in->perms[k].sid, id - 1)) MessageBeep(MB_ICONWARNING);
                        GatherPerms(*in);
                        FitHeight(*in);
                    }
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
        if (In(in->lockedHit, p)) {  // Bloqueado = somente leitura
            DWORD a = GetFileAttributesW(in->path.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(in->path.c_str(), in->readOnly ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY)))
                in->readOnly = !in->readOnly;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!in || in->pressLight < 0) break;
        ReleaseCapture();
        float sc = Scale(h); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        int k = HitLight(p), was = in->pressLight; in->pressLight = -1;
        if (k == was && k == 0) DestroyWindow(h);
        else if (k == was && k == 1) ShowWindow(h, SW_MINIMIZE);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || (w == 'W' && GetKeyState(VK_CONTROL) < 0)) { DestroyWindow(h); return 0; }
        break;
    case WM_DESTROY:
        if (in) {
            if (in->calc) in->calc->cancel = true;
            if (in->icon) in->icon->Release();
            if (in->prev) in->prev->Release();
            if (in->rt) in->rt->Release();
            delete in;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        if (g_standalone) {  // era a última (fora esta, que ainda existe aqui): fecha o processo de administrador
            bool other = false;
            for (HWND w2 = FindWindowW(L"ArquivosInfo", nullptr); w2; w2 = FindWindowExW(nullptr, w2, L"ArquivosInfo", nullptr)) {
                DWORD pid = 0; GetWindowThreadProcessId(w2, &pid);
                if (w2 != h && pid == GetCurrentProcessId()) other = true;
            }
            if (!other) PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace



void info::Show(const std::wstring& path, HWND anchor, const std::wstring& family, bool dark, bool admin, int px, int py) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ArquivosInfo";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_f);
        wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH);
        PathRemoveFileSpecW(exe); PathRemoveFileSpecW(exe);  // bin\.. = pasta do projeto
        g_packDir = std::wstring(exe) + L"\\icones\\sequoia";
        registered = true;
        if (!gfx::dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&gfx::dw);
    }
    g_family = family; g_darkMode = dark;
    // já tem uma janela deste item: traz para a frente
    for (HWND w2 = FindWindowW(L"ArquivosInfo", nullptr); w2; w2 = FindWindowExW(nullptr, w2, L"ArquivosInfo", nullptr)) {
        auto* other = (Info*)GetWindowLongPtrW(w2, GWLP_USERDATA);
        if (other && !_wcsicmp(other->path.c_str(), path.c_str())) { ShowWindow(w2, SW_RESTORE); SetForegroundWindow(w2); return; }
    }
    auto* in = new Info;
    in->path = path;
    in->admin = admin; in->unlocked = admin;
    if (admin) { in->open[S_SHARING] = true; g_standalone = true; }
    Gather(*in);
    // posição: ao lado direito da janela do Finder, em cascata se já houver outras
    RECT nr{ 200, 200, 800, 600 }; if (anchor) GetWindowRect(anchor, &nr);
    float sc = anchor ? GetDpiForWindow(anchor) / 96.f : 1.f;
    int count = 0;
    for (HWND w2 = FindWindowW(L"ArquivosInfo", nullptr); w2; w2 = FindWindowExW(nullptr, w2, L"ArquivosInfo", nullptr)) count++;
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(ContentHeight(*in) * sc);
    int x = nr.right + (int)(12 * sc) + count * (int)(22 * sc), y = nr.top + count * (int)(22 * sc);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromRect(&nr, MONITOR_DEFAULTTONEAREST), &mi);
    if (x + wpx > mi.rcWork.right) x = std::max<LONG>(mi.rcWork.left, nr.left + (int)(40 * sc) + count * (int)(22 * sc));
    if (y + hpx > mi.rcWork.bottom) y = std::max<LONG>(mi.rcWork.top, mi.rcWork.bottom - hpx);
    if (px != (-2147483647 - 1)) { x = px; y = py; }
    HWND h = CreateWindowExW(0, L"ArquivosInfo", (Tr(L"Informações de ") + in->name).c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             x, y, wpx, hpx, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) { delete in; return; }
    in->hwnd = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)in);
    BOOL dm = dark; DwmSetWindowAttribute(h, 20, &dm, sizeof(dm));
    DWORD corner = 2; DwmSetWindowAttribute(h, 33, &corner, sizeof(corner));
    COLORREF border = dark ? RGB(88, 88, 92) : RGB(196, 196, 200); DwmSetWindowAttribute(h, 34, &border, sizeof(border));
    MARGINS mg{ 0, 0, 1, 0 }; DwmExtendFrameIntoClientArea(h, &mg);  // mantém a sombra do sistema
    SetWindowPos(h, nullptr, x, y, wpx, hpx, SWP_FRAMECHANGED | SWP_NOZORDER);
    FitHeight(*in);
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
    StartCalc(*in);
}
