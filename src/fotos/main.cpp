// Fotos (Photos do Mac): a biblioteca são as imagens e vídeos da pasta Imagens (com as subpastas) e as capturas
// de tela da mesa. À esquerda: Biblioteca, Favoritos, Recentes, Capturas de Tela, Vídeos. À direita a grade em
// quadrados, agrupada por mês ("novembro de 2026"), com miniaturas lidas numa thread só para o que está à vista.
// Ctrl+roda ou Ctrl+= / Ctrl+- mudam o tamanho. Duplo clique abre (imagem na Pré-Visualização); espaço também.
// "." (ou botão direito) favorita; Delete manda para a Lixeira. Favoritos em %APPDATA%\Shell\fotos-favoritos.txt.
#include "../comum/app2d.h"
#include "../comum/nomes.h"
#include "../comum/abrircom.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#pragma comment(lib, "shlwapi.lib")

using namespace app2d;

namespace {

float SB = 210, TB = 52, GAP = 3, HEADH = 40;  // macOS 27: lateral 216 (0 escondida), 17,5 entre as fotos
const UINT WM_SCAN = WM_APP + 3, WM_THUMB = WM_APP + 4;

struct Item { std::wstring path; FILETIME time; bool video, shot; };
std::vector<Item> g_all;
std::mutex g_mx;
std::set<std::wstring> g_fav;
std::wstring g_favFile;
int g_view = 0;  // 0 biblioteca, 1 favoritos, 2 recentes, 3 capturas, 4 vídeos
float g_cell = 150, g_scroll = 0;
int g_sel = -1;  // índice em g_shown
std::vector<int> g_shown;
bool g_scanning = true;
IDWriteTextFormat *g_tfSide2, *g_tfSmallC, *g_tfSide, *g_tfHead, *g_tfTitle, *g_tfSmall, *g_tfIcon, *g_tfBadge;
IDWriteTextFormat *g_tfTitleG, *g_tfSideB, *g_tfSec, *g_tfSeg, *g_tfFootB;  // macOS 27

// ---- miniaturas
struct Thumb { ID2D1Bitmap* bmp = nullptr; bool asked = false; ULONGLONG used = 0; };
std::map<std::wstring, Thumb> g_thumbs;
std::mutex g_qmx; std::condition_variable g_qcv; std::deque<std::wstring> g_queue; std::atomic<int> g_px{ 256 };
struct ThumbData { std::wstring path; UINT w, h; std::vector<BYTE> px; };

bool ShellThumb(const std::wstring& path, int px, UINT& w, UINT& h, std::vector<BYTE>& out) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    HRESULT hr = f->GetImage(SIZE{ px, px }, SIIGBF_RESIZETOFIT, &hb);
    f->Release();
    if (FAILED(hr) || !hb) return false;
    BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
    w = bm.bmWidth; h = std::abs(bm.bmHeight);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB };
    out.resize((size_t)w * h * 4);
    HDC dc = GetDC(nullptr); GetDIBits(dc, hb, 0, h, out.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, dc);
    DeleteObject(hb);
    bool any = false; for (size_t i = 3; i < out.size(); i += 4) if (out[i]) { any = true; break; }
    if (!any) for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return true;
}
void ThumbWorker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        std::wstring p;
        { std::unique_lock<std::mutex> l(g_qmx); g_qcv.wait(l, [] { return !g_queue.empty(); }); p = g_queue.back(); g_queue.pop_back(); }  // o pedido mais novo primeiro
        auto* d = new ThumbData{ p };
        if (!ShellThumb(p, g_px, d->w, d->h, d->px)) d->px.clear();
        if (!PostMessageW(wnd, WM_THUMB, 0, (LPARAM)d)) delete d;
    }
}

// ---- varredura
bool IsImg(const std::wstring& x) { for (auto e : { L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", L".heif", L".tif", L".tiff", L".avif", L".jfif" }) if (x == e) return true; return false; }
bool IsVid(const std::wstring& x) { for (auto e : { L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".wmv", L".webm" }) if (x == e) return true; return false; }
void ScanDir(const std::wstring& dir, int depth, std::vector<Item>& out, bool shotsOnly) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 6 && !shotsOnly) ScanDir(p, depth + 1, out, false); continue; }
        std::wstring x = PathFindExtensionW(fd.cFileName); for (auto& c : x) c = towlower(c);
        bool img = IsImg(x), vid = IsVid(x);
        if (!img && !vid) continue;
        std::wstring low = fd.cFileName; for (auto& c : low) c = towlower(c);
        bool shot = low.rfind(L"captura de tela", 0) == 0 || low.rfind(L"screenshot", 0) == 0 || dir.find(L"Screenshots") != std::wstring::npos || dir.find(L"Capturas") != std::wstring::npos;
        if (shotsOnly && !shot) continue;
        out.push_back({ p, fd.ftLastWriteTime, vid, shot });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
void Scan() {
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<Item> v;
        auto known = [](REFKNOWNFOLDERID id) { PWSTR p = nullptr; std::wstring d; if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) d = p; CoTaskMemFree(p); return d; };
        std::wstring pics = known(FOLDERID_Pictures), vids = known(FOLDERID_Videos), desk = known(FOLDERID_Desktop);
        if (!pics.empty()) ScanDir(pics, 0, v, false);
        if (!vids.empty() && vids.find(pics) != 0) ScanDir(vids, 0, v, false);
        if (!desk.empty()) ScanDir(desk, 6, v, true);  // só as capturas soltas na mesa
        std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return CompareFileTime(&a.time, &b.time) < 0; });  // mais antigas em cima, como no Mac
        { std::lock_guard<std::mutex> l(g_mx); g_all = std::move(v); }
        PostMessageW(wnd, WM_SCAN, 0, 0);
    }).detach();
}

void LoadFav() { g_fav.clear(); for (auto& l : ReadLines(g_favFile)) if (!l.empty() && l[0] != L'#') g_fav.insert(l); }
void SaveFav() { std::vector<std::wstring> v{ L"# Fotos favoritas" }; for (auto& f : g_fav) v.push_back(f); WriteLines(g_favFile, v); }

std::wstring g_search;  // macOS 27: a lupa filtra pelo nome do arquivo
int g_showMode = 2;  // 0 anos, 1 meses, 2 todas: a grade ganha um cabeçalho por ano ou por mês
void Filter() {
    g_shown.clear();
    std::wstring q = g_search; for (auto& c : q) c = towlower(c);
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime, month = 30ULL * 24 * 3600 * 10000000ULL;
    std::lock_guard<std::mutex> l(g_mx);
    for (int i = 0; i < (int)g_all.size(); i++) {
        auto& it = g_all[i];
        ULONGLONG t = ((ULONGLONG)it.time.dwHighDateTime << 32) | it.time.dwLowDateTime;
        bool ok = g_view == 0 || (g_view == 1 && g_fav.count(it.path)) || (g_view == 2 && n - t < month) || (g_view == 3 && it.shot) || (g_view == 4 && it.video);
        if (ok && !q.empty()) { std::wstring nm = it.path.substr(it.path.find_last_of(L"\\/") + 1); for (auto& c : nm) c = towlower(c); ok = nm.find(q) != std::wstring::npos; }
        if (ok) g_shown.push_back(i);
    }
    if (g_sel >= (int)g_shown.size()) g_sel = -1;
}

// ---- grade: cada mês começa numa linha nova, com o cabeçalho
struct Cell { int idx; D2D1_RECT_F r; };
struct Head { std::wstring text; float y; };
std::vector<Cell> g_cells; std::vector<Head> g_heads; float g_total = 0;
void Layout() {
    g_cells.clear(); g_heads.clear();
    D2D1_SIZE_F sz = Size();
    const bool gg = BAR > 0;  // macOS 27: margens de 17, sem cabeçalhos de mês
    float M = gg ? 17.f : 16.f;
    float W = sz.width - SB - 2 * M;
    int cols = std::max(1, (int)((W + GAP) / (g_cell + GAP)));
    float cw = (W - (cols - 1) * GAP) / cols;
    float y = gg ? 6.f : 8.f; int col = 0, lastKey = -1;
    std::lock_guard<std::mutex> l(g_mx);
    for (int k = 0; k < (int)g_shown.size(); k++) {
        auto& it = g_all[g_shown[k]];
        SYSTEMTIME st; FILETIME loc; FileTimeToLocalFileTime(&it.time, &loc); FileTimeToSystemTime(&loc, &st);
        bool years = gg && g_showMode == 0;
        int key = years ? st.wYear : st.wYear * 12 + st.wMonth;
        if (key != lastKey && (!gg || g_showMode < 2)) {  // macOS 27: Anos e Meses separam a grade; Todas as Fotos não
            if (col) { y += cw + GAP; col = 0; }
            lastKey = key;
            wchar_t b[64];
            if (years) swprintf_s(b, L"%d", st.wYear);
            else wcsncpy_s(b, idioma::Date(st, idioma::D_MONTH_YEAR).c_str(), _TRUNCATE);  // "novembro de 2026", "November 2026"
            g_heads.push_back({ b, y });
            y += HEADH;
        }
        float x = SB + M + col * (cw + GAP);
        g_cells.push_back({ k, D2D1::RectF(x, TB + y, x + cw, TB + y + cw) });
        if (++col >= cols) { col = 0; y += cw + GAP; }
    }
    if (col) y += cw + GAP;
    g_total = y + (gg ? 70.f : 20.f);
}

bool EdOn();
void PaintEditor(D2D1_SIZE_F sz, bool dark);
std::wstring SelPath();
// ---- macOS 27 (captura Photos do 512 Pixels, 2x, medida em pt): barra lateral de 216 com o botão redondo de
// "barra lateral" (centro 190, 26); linhas de 32 com o ícone azul a 37 e o nome a 55, a escolhida numa pílula cinza
// (10–206); "Fixados" em cinza. Em cima, à esquerda do conteúdo, o nome em negrito e as datas em cinza; à direita
// grupos de vidro: −|+ (73), Anos/Meses/Todas as Fotos (253), três ícones (110), quatro ícones (146) e a lupa (36),
// a 8 da borda. Divisa a 52. Grade sem cabeçalhos: fotos inteiras (sem cortar) em quadrados de 176 com 17,5 entre
// eles e cantos 8; no fim, "N Fotos" em negrito.
struct GHit { int kind, arg; D2D1_RECT_F r; };  // 1 lado, 2 barra lateral, 3 −, 4 +, 5 favoritar, 6 compartilhar, 7 info, 8 editar, 10 mostrar
std::vector<GHit> g_gh;
bool g_sbHidden;
// g_showMode: lá em cima, antes do Layout

void Glyph(wchar_t g, D2D1_RECT_F r, D2D1_COLOR_F c) { wchar_t s[2] = { g, 0 }; Text(s, g_tfIcon, r, c); }

void PaintGolden2(D2D1_SIZE_F sz, bool dark) {
    g_gh.clear();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    D2D1_COLOR_F accent = Rgb(0, 136, 255), sep = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.09f);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    Layout();
    g_scroll = std::clamp(g_scroll, 0.f, std::max(0.f, g_total - (sz.height - TB)));
    float X0 = SB;
    // grade
    rt->PushAxisAlignedClip(D2D1::RectF(X0, TB, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    ULONGLONG now = GetTickCount64();
    float lastBottom = TB;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& c : g_cells) {
            D2D1_RECT_F r = c.r; r.top -= g_scroll; r.bottom -= g_scroll;
            lastBottom = std::max(lastBottom, r.bottom);
            if (r.bottom < TB - 200 || r.top > sz.height + 200) continue;
            auto& it = g_all[g_shown[c.idx]];
            Thumb& t = g_thumbs[it.path];
            t.used = now;
            if (!t.bmp && !t.asked) { t.asked = true; std::lock_guard<std::mutex> q(g_qmx); g_queue.push_back(it.path); g_qcv.notify_one(); }
            D2D1_RECT_F ir = r;
            if (t.bmp) {  // a foto inteira dentro do quadrado (como no Mac 27)
                D2D1_SIZE_F bs = t.bmp->GetSize();
                float k = std::min((r.right - r.left) / bs.width, (r.bottom - r.top) / bs.height);
                float w = bs.width * k, hh = bs.height * k, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                ir = D2D1::RectF(cx - w / 2, cy - hh / 2, cx + w / 2, cy + hh / 2);
                ID2D1BitmapBrush* bb = nullptr;
                rt->CreateBitmapBrush(t.bmp, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &bb);
                if (bb) {
                    bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(ir.left, ir.top));
                    rt->FillRoundedRectangle(D2D1::RoundedRect(ir, 8, 8), bb);
                    bb->Release();
                }
            } else {
                float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, w = (r.right - r.left), hh = w * 0.66f;
                ir = D2D1::RectF(r.left, cy - hh / 2, r.right, cy + hh / 2);
                Fill(ir, dark ? Rgb(48, 48, 50) : Rgb(238, 238, 240), 8);
                (void)cx;
            }
            if (it.video) Glyph(0xE714, D2D1::RectF(ir.left + 4, ir.bottom - 22, ir.left + 24, ir.bottom - 2), Rgb(255, 255, 255));
            if (g_fav.count(it.path)) Text(L"♥", g_tfBadge, D2D1::RectF(ir.left + 4, ir.bottom - 22, ir.left + 24, ir.bottom - 2), Rgb(255, 255, 255));
            if (c.idx == g_sel) { br->SetColor(accent); rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ir.left - 2, ir.top - 2, ir.right + 2, ir.bottom + 2), 10, 10), br, 3); }
        }
    }
    for (auto& hd : g_heads) {  // Anos / Meses
        float y = TB + hd.y - g_scroll;
        if (y < TB - HEADH || y > sz.height) continue;
        Text(hd.text, g_tfTitleG, D2D1::RectF(X0 + 17, y + 8, sz.width - 17, y + HEADH - 6), text);
    }
    if (g_shown.empty()) Text(g_scanning ? Tr(L"Lendo a biblioteca…") : !g_search.empty() ? Tr(L"Nenhum Resultado") : Tr(L"Nenhuma Foto"), g_tfTitle, D2D1::RectF(X0, TB, sz.width, sz.height), gray);
    else {  // rodapé: "N Fotos"
        int nv = 0; { std::lock_guard<std::mutex> l(g_mx); for (int k : g_shown) if (g_all[k].video) nv++; }
        int np = (int)g_shown.size() - nv;
        wchar_t b[96];
        if (nv && np) swprintf_s(b, Tr(L"%d Fotos, %d Vídeos"), np, nv); else if (nv) swprintf_s(b, Tr(L"%d Vídeos"), nv); else swprintf_s(b, Tr(L"%d Fotos"), np);
        Text(b, g_tfFootB, D2D1::RectF(X0, lastBottom + 22, sz.width, lastBottom + 40), text);
    }
    rt->PopAxisAlignedClip();
    // barra de cima
    Fill(D2D1::RectF(X0, 0, sz.width, TB), dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    Fill(D2D1::RectF(X0, TB - 0.5f, sz.width, TB), sep);
    const wchar_t* names[] = { L"Biblioteca", L"Favoritos", L"Recentes", L"Capturas de Tela", L"Vídeos" };
    float tx = X0 + (g_sbHidden ? 140.f : 20.f);
    Text(Tr(names[g_view]), g_tfTitleG, D2D1::RectF(tx, 10, tx + 300, 29), text);
    {
        std::wstring range;
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_shown.empty()) {
            auto fmt = [](const FILETIME& f) {
                SYSTEMTIME st; FILETIME loc; FileTimeToLocalFileTime(&f, &loc); FileTimeToSystemTime(&loc, &st);
                return idioma::Date(st, idioma::D_SHORT);  // "20 de nov. de 2026", "Nov 20, 2026"
            };
            range = fmt(g_all[g_shown.front()].time) + L" – " + fmt(g_all[g_shown.back()].time);
        }
        Text(range, g_tfSmall, D2D1::RectF(tx, 27, tx + 320, 42), gray);
    }
    // botões da direita, da borda para dentro
    float R = sz.width - 8, cy = 26;
    auto group = [&](float w) { D2D1_RECT_F r = D2D1::RectF(R - w, cy - 18, R, cy + 18); GlassButton(r, dark); BarButton(r); R = r.left; return r; };
    {   // lupa (com busca: o campo aberto com o texto)
        D2D1_RECT_F r = group(g_search.empty() ? 36.f : 200.f);
        IconSearch({ g_search.empty() ? (r.left + r.right) / 2 - 0.5f : r.left + 18, cy - 0.5f }, text, 1.05f);
        if (!g_search.empty()) Text(g_search, g_tfSeg, D2D1::RectF(r.left + 32, r.top, r.right - 10, r.bottom), text);
        g_gh.push_back({ 11, 0, r });
        R -= 8;
    }
    {   // info, compartilhar, favoritar, girar/editar
        D2D1_RECT_F r = group(146);
        wchar_t gl[4] = { 0xE946, 0xE72D, (wchar_t)(g_sel >= 0 && g_fav.count(SelPath()) ? 0xEB52 : 0xEB51), 0xE7AD };
        int kinds[4] = { 7, 6, 5, 8 };
        for (int i = 0; i < 4; i++) {
            D2D1_RECT_F c = D2D1::RectF(r.left + 6 + i * 33.5f, r.top, r.left + 6 + (i + 1) * 33.5f, r.bottom);
            Glyph(gl[i], c, i == 0 || g_sel >= 0 ? text : gray);
            g_gh.push_back({ kinds[i], 0, c });
        }
        R -= 7;
    }
    {   // tamanho, ordem, mais
        D2D1_RECT_F r = group(110);
        wchar_t gl[3] = { 0xE9A6, 0xE8CB, 0xE712 };
        for (int i = 0; i < 3; i++) Glyph(gl[i], D2D1::RectF(r.left + 6 + i * 32.6f, r.top, r.left + 6 + (i + 1) * 32.6f, r.bottom), text);
        R -= 12;
    }
    if (R - 253 - 16 - 73 > tx + 160) {
        D2D1_RECT_F r = group(253);
        const wchar_t* seg[3] = { L"Anos", L"Meses", L"Todas as Fotos" };
        float ws[3] = { 70, 76, 107 }, x = r.left;
        for (int i = 0; i < 3; i++) {
            D2D1_RECT_F c = D2D1::RectF(x, r.top, x + ws[i], r.bottom);
            if (i == g_showMode) Fill(D2D1::RectF(c.left + 4, c.top + 4, c.right - 4, c.bottom - 4), dark ? Rgb(255, 255, 255, 0.14f) : Rgb(0, 0, 0, 0.08f), 14);
            Text(Tr(seg[i]), g_tfSeg, c, text);
            g_gh.push_back({ 10, i, c });
            x += ws[i];
        }
        R -= 16;
        D2D1_RECT_F z = group(73);
        D2D1_RECT_F a = D2D1::RectF(z.left, z.top, z.left + 36.5f, z.bottom), b = D2D1::RectF(z.left + 36.5f, z.top, z.right, z.bottom);
        br->SetColor(text);
        rt->DrawLine({ a.left + 11, cy }, { a.right - 11, cy }, br, 1.6f);
        IconPlus({ (b.left + b.right) / 2, cy }, text, 7);
        Fill(D2D1::RectF(z.left + 36.25f, cy - 9, z.left + 36.75f, cy + 9), sep);
        g_gh.push_back({ 3, 0, a }); g_gh.push_back({ 4, 0, b });
    }
    // barra lateral
    if (!g_sbHidden) {
        Fill(D2D1::RectF(0, 0, SB, sz.height), SidebarColor());
        Fill(D2D1::RectF(SB - 0.5f, 0, SB, sz.height), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.08f));
        D2D1_RECT_F tb = D2D1::RectF(190 - 18, cy - 18, 190 + 18, cy + 18);
        GlassButton(tb, dark); IconSidebar({ 190, cy }, text);
        g_gh.push_back({ 2, 0, tb }); BarButton(tb);
        wchar_t icons[] = { 0xE8B9, 0xEB51, 0xE823, 0xE714, 0xE722 };
        int order[5] = { 0, 1, 2, 4, 3 };  // Biblioteca; Fixados: Favoritos, Recentes, Vídeos, Capturas de Tela
        float ys[5] = { 68, 132, 164, 196, 228 };
        Text(Tr(L"Fixados"), g_tfSec, D2D1::RectF(16, 98, SB, 114), gray);
        for (int k = 0; k < 5; k++) {
            int v = order[k];
            D2D1_RECT_F r = D2D1::RectF(10, ys[k] - 16, SB - 10, ys[k] + 16);
            bool on = v == g_view;
            if (on) Fill(r, dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.07f), 9);
            Glyph(icons[k], D2D1::RectF(26, r.top, 48, r.bottom), accent);
            Text(Tr(names[v]), on ? g_tfSideB : g_tfSide, D2D1::RectF(55, r.top, r.right - 6, r.bottom), text);
            g_gh.push_back({ 1, v, r });
        }
    } else {
        D2D1_RECT_F tb = D2D1::RectF(108 - 18, cy - 18, 108 + 18, cy + 18);
        GlassButton(tb, dark); IconSidebar({ 108, cy }, text);
        g_gh.push_back({ 2, 0, tb }); BarButton(tb);
    }
}

void Paint() {
    if (!BeginPaint()) return;
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    if (EdOn()) { PaintEditor(sz, dark); EndPaint(); return; }  // editando uma foto
    if (BAR > 0) { PaintGolden2(sz, dark); EndPaint(); return; }
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    Layout();
    g_scroll = std::clamp(g_scroll, 0.f, std::max(0.f, g_total - (sz.height - TB)));
    // grade
    rt->PushAxisAlignedClip(D2D1::RectF(SB, TB, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& c : g_cells) {
            D2D1_RECT_F r = c.r; r.top -= g_scroll; r.bottom -= g_scroll;
            if (r.bottom < TB - 200 || r.top > sz.height + 200) continue;
            auto& it = g_all[g_shown[c.idx]];
            Thumb& t = g_thumbs[it.path];
            t.used = now;
            if (!t.bmp && !t.asked) { t.asked = true; std::lock_guard<std::mutex> q(g_qmx); g_queue.push_back(it.path); g_qcv.notify_one(); }
            Fill(r, dark ? Rgb(48, 48, 50) : Rgb(236, 236, 238));
            if (t.bmp) {  // preenche o quadrado (corta o que sobra, como no Mac)
                D2D1_SIZE_F bs = t.bmp->GetSize();
                float k = std::max((r.right - r.left) / bs.width, (r.bottom - r.top) / bs.height);
                float sw = (r.right - r.left) / k, sh = (r.bottom - r.top) / k;
                D2D1_RECT_F src = D2D1::RectF((bs.width - sw) / 2, (bs.height - sh) / 2, (bs.width + sw) / 2, (bs.height + sh) / 2);
                rt->DrawBitmap(t.bmp, r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
            }
            if (it.video) { wchar_t v[2] = { 0xE714, 0 }; Text(v, g_tfBadge, D2D1::RectF(r.left + 4, r.bottom - 22, r.left + 24, r.bottom - 2), Rgb(255, 255, 255)); }
            if (g_fav.count(it.path)) Text(L"♥", g_tfBadge, D2D1::RectF(r.left + 4, r.bottom - 22, r.left + 24, r.bottom - 2), Rgb(255, 255, 255));
            if (c.idx == g_sel) { br->SetColor(Rgb(10, 132, 255)); rt->DrawRectangle(D2D1::RectF(r.left + 1.5f, r.top + 1.5f, r.right - 1.5f, r.bottom - 1.5f), br, 3); }
        }
    }
    for (auto& h : g_heads) {
        float y = TB + h.y - g_scroll;
        if (y < TB - HEADH || y > sz.height) continue;
        Text(h.text, g_tfHead, D2D1::RectF(SB + 16, y + 6, sz.width - 16, y + HEADH - 4), text);
    }
    if (g_shown.empty()) Text(g_scanning ? Tr(L"Lendo a biblioteca…") : Tr(L"Nenhuma Foto"), g_tfTitle, D2D1::RectF(SB, TB, sz.width, sz.height), gray);
    rt->PopAxisAlignedClip();
    // topo
    Fill(D2D1::RectF(SB, 0, sz.width, TB), dark ? Rgb(40, 40, 42, 0.96f) : Rgb(250, 250, 252, 0.96f));
    const wchar_t* names[] = { L"Biblioteca", L"Favoritos", L"Recentes", L"Capturas de Tela", L"Vídeos" };
    Text(Tr(names[g_view]), g_tfTitle, D2D1::RectF(SB + 16, 10, sz.width - 200, 32), text);
    wchar_t b[64]; swprintf_s(b, Tr(L"%d itens"), (int)g_shown.size());
    Text(b, g_tfSmall, D2D1::RectF(SB + 16, 30, sz.width - 200, 48), gray);
    // lateral
    Fill(D2D1::RectF(0, 0, SB, sz.height), SidebarColor());
    Text(Tr(L"Fotos"), g_tfHead, D2D1::RectF(16, 16, SB, 40), gray);
    wchar_t icons[] = { 0xE8B9, 0xEB51, 0xE823, 0xE722, 0xE714 };
    for (int i = 0; i < 5; i++) {
        D2D1_RECT_F r = D2D1::RectF(8, 48 + i * 34, SB - 8, 48 + i * 34 + 30);
        if (i == g_view) Fill(r, dark ? Rgb(255, 255, 255, 0.12f) : Rgb(0, 0, 0, 0.08f), 7);
        wchar_t g[2] = { icons[i], 0 };
        Text(g, g_tfIcon, D2D1::RectF(r.left + 6, r.top, r.left + 30, r.bottom), Rgb(10, 132, 255));
        Text(Tr(names[i]), g_tfSide, D2D1::RectF(r.left + 36, r.top, r.right, r.bottom), text);
    }
    EndPaint();
    // libera miniaturas que não aparecem há tempo (memória da GPU)
    if (g_thumbs.size() > 900) for (auto it = g_thumbs.begin(); it != g_thumbs.end();) {
        if (it->second.bmp && now - it->second.used > 20000) { it->second.bmp->Release(); it = g_thumbs.erase(it); } else ++it;
    }
}

// ======================= editar (Editar do Fotos): ajustes, filtros, girar e recortar; salva uma cópia
struct Edit {
    bool on = false;
    std::wstring path;
    UINT fw = 0, fh = 0; std::vector<BYTE> full;   // original (BGRA pré-multiplicado, opaco)
    UINT pw = 0, ph = 0; std::vector<BYTE> prev;   // reduzida, para mostrar
    ID2D1Bitmap* bmp = nullptr;
    float light = 0, contrast = 0, sat = 0, warm = 0;  // -1..1
    int filter = 0;
    bool cropMode = false, cropping = false; D2D1_POINT_2F c0{}, c1{};  // recorte em coordenadas da prévia
    D2D1_RECT_F crop{ 0, 0, 0, 0 };                // aplicado (fração 0..1 da imagem), vazio = inteira
    int drag = -1;                                  // slider sendo arrastado
    D2D1_RECT_F imgRect{};
} g_ed;
bool EdOn() { return g_ed.on; }
const wchar_t* FILTERS[] = { L"Original", L"Vívido", L"Dramático", L"Quente", L"Frio", L"Mono", L"Prata", L"Noir" };
struct Hit2 { int kind, arg; D2D1_RECT_F r; };  // 1 slider, 2 filtro, 3 girar, 4 recortar, 5 restaurar, 6 cancelar, 7 salvar
std::vector<Hit2> g_ehits;
IWICImagingFactory* Wic() { static IWICImagingFactory* f; if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)); return f; }

bool LoadFull(const std::wstring& p) {
    IWICImagingFactory* f = Wic(); if (!f) return false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr; IWICBitmapFlipRotator* rot = nullptr;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr));
    IWICBitmapSource* src = fr;
    if (ok) {  // orientação da câmera (EXIF): a foto aparece de pé
        IWICMetadataQueryReader* q = nullptr; PROPVARIANT v; PropVariantInit(&v); USHORT o = 1;
        if (SUCCEEDED(fr->GetMetadataQueryReader(&q)) && (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=274}", &v)) || SUCCEEDED(q->GetMetadataByName(L"/ifd/{ushort=274}", &v))) && v.vt == VT_UI2) o = v.uiVal;
        PropVariantClear(&v); if (q) q->Release();
        WICBitmapTransformOptions t = o == 3 ? WICBitmapTransformRotate180 : o == 6 ? WICBitmapTransformRotate90 : o == 8 ? WICBitmapTransformRotate270 : WICBitmapTransformRotate0;
        if (t != WICBitmapTransformRotate0 && SUCCEEDED(f->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(fr, t))) src = rot;
    }
    ok = ok && SUCCEEDED(f->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) && SUCCEEDED(cv->GetSize(&g_ed.fw, &g_ed.fh));
    if (ok) { g_ed.full.resize((size_t)g_ed.fw * g_ed.fh * 4); ok = SUCCEEDED(cv->CopyPixels(nullptr, g_ed.fw * 4, (UINT)g_ed.full.size(), g_ed.full.data())); }
    if (cv) cv->Release(); if (rot) rot->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    return ok;
}
void MakePreview() {  // reduz para no máximo 1600 px (os ajustes ficam rápidos)
    float k = std::min(1.f, 1600.f / std::max(g_ed.fw, g_ed.fh));
    g_ed.pw = std::max(1u, (UINT)(g_ed.fw * k)); g_ed.ph = std::max(1u, (UINT)(g_ed.fh * k));
    g_ed.prev.resize((size_t)g_ed.pw * g_ed.ph * 4);
    for (UINT y = 0; y < g_ed.ph; y++) { UINT sy = std::min(g_ed.fh - 1, (UINT)(y / k)); for (UINT x = 0; x < g_ed.pw; x++) { UINT sx = std::min(g_ed.fw - 1, (UINT)(x / k)); memcpy(&g_ed.prev[((size_t)y * g_ed.pw + x) * 4], &g_ed.full[((size_t)sy * g_ed.fw + sx) * 4], 4); } }
}
// parâmetros finais (os do filtro somados aos sliders)
void Params(float& e, float& c, float& s, float& w) {
    e = g_ed.light; c = g_ed.contrast; s = g_ed.sat; w = g_ed.warm;
    switch (g_ed.filter) {
    case 1: s += 0.35f; c += 0.15f; break;
    case 2: c += 0.4f; e -= 0.1f; s -= 0.1f; break;
    case 3: w += 0.6f; break;
    case 4: w -= 0.6f; break;
    case 5: s = -1; break;
    case 6: s = -1; c += 0.2f; e += 0.05f; break;
    case 7: s = -1; c += 0.6f; e -= 0.15f; break;
    }
}
void Adjust(std::vector<BYTE>& px) {
    float e, c, s, w; Params(e, c, s, w);
    float ex = std::pow(2.f, e), ck = 1 + std::clamp(c, -0.6f, 1.2f), sk = 1 + std::clamp(s, -1.f, 1.f);
    BYTE lut[256]; for (int i = 0; i < 256; i++) { float v = i / 255.f * ex; v = (v - 0.5f) * ck + 0.5f; lut[i] = (BYTE)std::lround(std::clamp(v, 0.f, 1.f) * 255); }
    for (size_t i = 0; i < px.size(); i += 4) {
        float b = lut[px[i]] / 255.f, g = lut[px[i + 1]] / 255.f, r = lut[px[i + 2]] / 255.f;
        float l = 0.299f * r + 0.587f * g + 0.114f * b;
        r = l + (r - l) * sk; g = l + (g - l) * sk; b = l + (b - l) * sk;
        r += w * 0.08f; b -= w * 0.08f;
        px[i] = (BYTE)std::lround(std::clamp(b, 0.f, 1.f) * 255); px[i + 1] = (BYTE)std::lround(std::clamp(g, 0.f, 1.f) * 255); px[i + 2] = (BYTE)std::lround(std::clamp(r, 0.f, 1.f) * 255); px[i + 3] = 255;
    }
}
void Rot90(std::vector<BYTE>& px, UINT& w, UINT& h) {  // anti-horário, como o botão do Fotos
    std::vector<BYTE> o(px.size());
    for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) memcpy(&o[((size_t)(w - 1 - x) * h + y) * 4], &px[((size_t)y * w + x) * 4], 4);
    px.swap(o); std::swap(w, h);
}
void Refresh() {
    if (g_ed.bmp) { g_ed.bmp->Release(); g_ed.bmp = nullptr; }
    std::vector<BYTE> p = g_ed.prev; Adjust(p);
    if (rt) rt->CreateBitmap(D2D1::SizeU(g_ed.pw, g_ed.ph), p.data(), g_ed.pw * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &g_ed.bmp);
    InvalidateRect(wnd, nullptr, FALSE);
}
void EditOpen(const std::wstring& p) {
    std::wstring x = PathFindExtensionW(p.c_str()); for (auto& c : x) c = towlower(c);
    if (!IsImg(x)) return;
    g_ed = Edit{}; g_ed.path = p;
    if (!LoadFull(p)) { MessageBoxW(wnd, Tr(L"Não foi possível abrir a foto para editar."), Tr(L"Fotos"), MB_OK); return; }
    MakePreview();
    g_ed.on = true;
    Refresh();
}
void EditClose() { if (g_ed.bmp) g_ed.bmp->Release(); g_ed = Edit{}; InvalidateRect(wnd, nullptr, FALSE); }
void EditSave() {
    // aplica no original: recorte, ajustes e grava "nome (editada).jpg" ao lado (o original fica)
    std::vector<BYTE> px = g_ed.full; UINT w = g_ed.fw, h = g_ed.fh;
    if (g_ed.crop.right > g_ed.crop.left) {
        UINT x0 = (UINT)(g_ed.crop.left * w), y0 = (UINT)(g_ed.crop.top * h), x1 = (UINT)(g_ed.crop.right * w), y1 = (UINT)(g_ed.crop.bottom * h);
        UINT cw = std::max(1u, x1 - x0), ch = std::max(1u, y1 - y0);
        std::vector<BYTE> o((size_t)cw * ch * 4);
        for (UINT y = 0; y < ch; y++) memcpy(&o[(size_t)y * cw * 4], &px[(((size_t)y0 + y) * w + x0) * 4], (size_t)cw * 4);
        px.swap(o); w = cw; h = ch;
    }
    Adjust(px);
    std::wstring stem = g_ed.path.substr(0, g_ed.path.find_last_of(L'.')), out = stem + Tr(L" (editada)") + L".jpg";
    for (int k = 2; GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES && k < 100; k++) out = stem + Tr(L" (editada)") + L" " + std::to_wstring(k) + L".jpg";
    IWICImagingFactory* f = Wic();
    IWICBitmap* wb = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fe = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = f && SUCCEEDED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, (UINT)px.size(), px.data(), &wb)) &&
              SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fe, nullptr)) && SUCCEEDED(fe->Initialize(nullptr)) && SUCCEEDED(fe->SetSize(w, h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(fe->SetPixelFormat(&fmt)) && SUCCEEDED(f->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(wb, fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
         SUCCEEDED(fe->WriteSource(cv, nullptr)) && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
    if (cv) cv->Release(); if (fe) fe->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wb) wb->Release();
    if (!ok) { MessageBoxW(wnd, Tr(L"Não foi possível salvar a foto."), Tr(L"Fotos"), MB_OK | MB_ICONERROR); return; }
    WIN32_FILE_ATTRIBUTE_DATA fa{}; GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa);
    { std::lock_guard<std::mutex> l(g_mx); g_all.push_back({ out, fa.ftLastWriteTime, false, false }); }
    Filter();
    EditClose();
}

void PaintEditor(D2D1_SIZE_F sz, bool dark) {
    g_ehits.clear();
    D2D1_COLOR_F text = Rgb(245, 245, 247), gray = Rgb(152, 152, 157), yellow = Rgb(255, 204, 0);
    rt->Clear(Rgb(18, 18, 18));
    const float PANEL = 270;
    // a foto
    float aw = sz.width - PANEL - 40, ah = sz.height - 90;
    if (g_ed.bmp) {
        float k = std::min(aw / g_ed.pw, ah / g_ed.ph);
        float w = g_ed.pw * k, h = g_ed.ph * k, x = 20 + (aw - w) / 2, y = 60 + (ah - h) / 2;
        g_ed.imgRect = D2D1::RectF(x, y, x + w, y + h);
        rt->DrawBitmap(g_ed.bmp, g_ed.imgRect);
        auto toScr = [&](D2D1_RECT_F f) { return D2D1::RectF(x + f.left * w, y + f.top * h, x + f.right * w, y + f.bottom * h); };
        if (g_ed.crop.right > g_ed.crop.left || g_ed.cropping) {
            D2D1_RECT_F c = g_ed.cropping ? D2D1::RectF(std::min(g_ed.c0.x, g_ed.c1.x), std::min(g_ed.c0.y, g_ed.c1.y), std::max(g_ed.c0.x, g_ed.c1.x), std::max(g_ed.c0.y, g_ed.c1.y)) : toScr(g_ed.crop);
            D2D1_COLOR_F dim = Rgb(0, 0, 0, 0.55f); D2D1_RECT_F ir = g_ed.imgRect;
            Fill(D2D1::RectF(ir.left, ir.top, ir.right, c.top), dim); Fill(D2D1::RectF(ir.left, c.bottom, ir.right, ir.bottom), dim);
            Fill(D2D1::RectF(ir.left, c.top, c.left, c.bottom), dim); Fill(D2D1::RectF(c.right, c.top, ir.right, c.bottom), dim);
            br->SetColor(yellow); rt->DrawRectangle(c, br, 2);
        }
    }
    // topo: Cancelar / nome / Salvar Cópia
    D2D1_RECT_F cancel = D2D1::RectF(20, 14, 120, 42), save = D2D1::RectF(sz.width - 160, 14, sz.width - 20, 42);
    Fill(cancel, Rgb(58, 58, 60), 7); Text(Tr(L"Cancelar"), g_tfSide2, cancel, text); g_ehits.push_back({ 6, 0, cancel });
    Fill(save, yellow, 7); Text(Tr(L"Salvar Cópia"), g_tfSide2, save, Rgb(0, 0, 0)); g_ehits.push_back({ 7, 0, save });
    Text(g_ed.path.substr(g_ed.path.find_last_of(L'\\') + 1), g_tfSmallC, D2D1::RectF(140, 14, sz.width - 180, 42), gray);
    // painel
    float px = sz.width - PANEL, y = 60;
    Text(Tr(L"Ajustar"), g_tfTitle, D2D1::RectF(px, y, sz.width - 20, y + 24), text); y += 34;
    const wchar_t* names[] = { L"Luz", L"Contraste", L"Saturação", L"Calor" };
    float* vals[] = { &g_ed.light, &g_ed.contrast, &g_ed.sat, &g_ed.warm };
    for (int i = 0; i < 4; i++) {
        Text(Tr(names[i]), g_tfSide, D2D1::RectF(px, y, sz.width - 20, y + 20), text);
        D2D1_RECT_F tr = D2D1::RectF(px, y + 26, sz.width - 30, y + 30);
        Fill(tr, Rgb(72, 72, 74), 2);
        float mid = (tr.left + tr.right) / 2, kx = mid + *vals[i] * (tr.right - tr.left) / 2;
        Fill(D2D1::RectF(std::min(mid, kx), tr.top, std::max(mid, kx), tr.bottom), yellow, 2);
        Circle({ kx, (tr.top + tr.bottom) / 2 }, 7, Rgb(255, 255, 255), true);
        g_ehits.push_back({ 1, i, D2D1::RectF(tr.left - 8, tr.top - 12, tr.right + 8, tr.bottom + 12) });
        y += 50;
    }
    y += 6;
    Text(Tr(L"Filtros"), g_tfTitle, D2D1::RectF(px, y, sz.width - 20, y + 24), text); y += 32;
    for (int i = 0; i < 8; i++) {
        D2D1_RECT_F r = D2D1::RectF(px + (i % 2) * 120, y + (i / 2) * 36, px + (i % 2) * 120 + 112, y + (i / 2) * 36 + 30);
        Fill(r, g_ed.filter == i ? yellow : Rgb(58, 58, 60), 7);
        Text(Tr(FILTERS[i]), g_tfSide2, r, g_ed.filter == i ? Rgb(0, 0, 0) : text);
        g_ehits.push_back({ 2, i, r });
    }
    y += 4 * 36 + 14;
    D2D1_RECT_F rot = D2D1::RectF(px, y, px + 112, y + 32), crop = D2D1::RectF(px + 120, y, px + 232, y + 32), reset = D2D1::RectF(px, y + 40, px + 232, y + 72);
    Fill(rot, Rgb(58, 58, 60), 7); Text(Tr(L"Girar"), g_tfSide2, rot, text); g_ehits.push_back({ 3, 0, rot });
    Fill(crop, g_ed.cropMode ? yellow : Rgb(58, 58, 60), 7); Text(Tr(L"Recortar"), g_tfSide2, crop, g_ed.cropMode ? Rgb(0, 0, 0) : text); g_ehits.push_back({ 4, 0, crop });
    Fill(reset, Rgb(58, 58, 60), 7); Text(Tr(L"Restaurar Original"), g_tfSide2, reset, text); g_ehits.push_back({ 5, 0, reset });
    if (g_ed.cropMode) Text(Tr(L"Arraste sobre a foto para escolher o recorte."), g_tfSmall, D2D1::RectF(px, y + 80, sz.width - 10, y + 100), gray);
    (void)dark;
}

void SliderTo(int i, float x) {
    for (auto& h : g_ehits) if (h.kind == 1 && h.arg == i) {
        float l = h.r.left + 8, r = h.r.right - 8;
        float v = std::clamp(((x - l) / (r - l)) * 2 - 1, -1.f, 1.f);
        if (std::fabs(v) < 0.04f) v = 0;  // encaixa no meio
        float* vals[] = { &g_ed.light, &g_ed.contrast, &g_ed.sat, &g_ed.warm };
        *vals[i] = v;
        Refresh();
    }
}

bool EditMouse(UINT m, float x, float y) {
    if (!g_ed.on) return false;
    if (m == WM_LBUTTONDOWN) {
        for (auto& h : g_ehits) if (In(h.r, x, y)) {
            switch (h.kind) {
            case 1: g_ed.drag = h.arg; SetCapture(wnd); SliderTo(h.arg, x); break;
            case 2: g_ed.filter = h.arg; Refresh(); break;
            case 3: Rot90(g_ed.full, g_ed.fw, g_ed.fh); Rot90(g_ed.prev, g_ed.pw, g_ed.ph); g_ed.crop = {}; Refresh(); break;
            case 4: g_ed.cropMode = !g_ed.cropMode; InvalidateRect(wnd, nullptr, FALSE); break;
            case 5: { std::wstring p = g_ed.path; EditOpen(p); break; }
            case 6: EditClose(); break;
            case 7: EditSave(); break;
            }
            return true;
        }
        if (g_ed.cropMode && In(g_ed.imgRect, x, y)) { g_ed.cropping = true; g_ed.c0 = g_ed.c1 = { x, y }; SetCapture(wnd); }
        return true;
    }
    if (m == WM_MOUSEMOVE) {
        if (g_ed.drag >= 0) SliderTo(g_ed.drag, x);
        if (g_ed.cropping) { g_ed.c1 = { std::clamp(x, g_ed.imgRect.left, g_ed.imgRect.right), std::clamp(y, g_ed.imgRect.top, g_ed.imgRect.bottom) }; InvalidateRect(wnd, nullptr, FALSE); }
        return true;
    }
    if (m == WM_LBUTTONUP) {
        if (g_ed.drag >= 0) { g_ed.drag = -1; ReleaseCapture(); }
        if (g_ed.cropping) {
            g_ed.cropping = false; ReleaseCapture();
            D2D1_RECT_F ir = g_ed.imgRect; float w = ir.right - ir.left, h = ir.bottom - ir.top;
            D2D1_RECT_F c = D2D1::RectF((std::min(g_ed.c0.x, g_ed.c1.x) - ir.left) / w, (std::min(g_ed.c0.y, g_ed.c1.y) - ir.top) / h, (std::max(g_ed.c0.x, g_ed.c1.x) - ir.left) / w, (std::max(g_ed.c0.y, g_ed.c1.y) - ir.top) / h);
            if (c.right - c.left > 0.03f && c.bottom - c.top > 0.03f) g_ed.crop = c;
            InvalidateRect(wnd, nullptr, FALSE);
        }
        return true;
    }
    return true;
}

int HitCell(float x, float y) { for (auto& c : g_cells) if (In(c.r, x, y + g_scroll)) return c.idx; return -1; }
std::wstring SelPath() { std::lock_guard<std::mutex> l(g_mx); return g_sel >= 0 && g_sel < (int)g_shown.size() ? g_all[g_shown[g_sel]].path : L""; }
void OpenSel() {
    std::wstring p = SelPath();
    if (p.empty()) return;
    std::wstring x = PathFindExtensionW(p.c_str()); for (auto& c : x) c = towlower(c);
    wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); PathRemoveFileSpecW(me);
    std::wstring pv = std::wstring(me) + L"\\" EXE_PREVIEW;
    if (IsImg(x) && GetFileAttributesW(pv.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(nullptr, L"open", pv.c_str(), (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else if (!AbrirNoAppMac(p)) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);  // vídeo: QuickTime
}
void ToggleFav() { std::wstring p = SelPath(); if (p.empty()) return; if (!g_fav.erase(p)) g_fav.insert(p); SaveFav(); if (g_view == 1) Filter(); InvalidateRect(wnd, nullptr, FALSE); }
void Trash() {
    std::wstring p = SelPath(); if (p.empty()) return;
    std::wstring dbl = p + L'\0';
    SHFILEOPSTRUCTW op{ wnd, FO_DELETE, dbl.c_str(), nullptr, FOF_ALLOWUNDO | FOF_NOCONFIRMATION };
    if (SHFileOperationW(&op) == 0) { std::lock_guard<std::mutex> l(g_mx); g_all.erase(std::remove_if(g_all.begin(), g_all.end(), [&](const Item& i) { return i.path == p; }), g_all.end()); }
    Filter(); InvalidateRect(wnd, nullptr, FALSE);
}
void Zoom(float f) { g_cell = std::clamp(g_cell * f, 60.f, 420.f); InvalidateRect(wnd, nullptr, FALSE); }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); return 0;
    case WM_DPICHANGED: OnDpi(w, l); return 0;
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SCAN: g_scanning = false; Filter(); g_scroll = 1e9f; InvalidateRect(h, nullptr, FALSE); return 0;  // abre no fim (as mais novas), como no Mac
    case WM_THUMB: {
        auto* d = (ThumbData*)l;
        auto it = g_thumbs.find(d->path);
        if (it != g_thumbs.end() && !d->px.empty() && rt) {
            ID2D1Bitmap* b = nullptr;
            rt->CreateBitmap(D2D1::SizeU(d->w, d->h), d->px.data(), d->w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
            if (b) { if (it->second.bmp) it->second.bmp->Release(); it->second.bmp = b; }
            InvalidateRect(h, nullptr, FALSE);
        }
        delete d;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        float d = GET_WHEEL_DELTA_WPARAM(w) / 120.f;
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) Zoom(d > 0 ? 1.12f : 1 / 1.12f);
        else { g_scroll -= d * 120; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEMOVE: case WM_LBUTTONUP: { float s = Sc(); if (g_ed.on) EditMouse(m, GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s); return 0; }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONUP: {
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        if (g_ed.on) { if (m != WM_RBUTTONUP) EditMouse(WM_LBUTTONDOWN, x, y); return 0; }
        if (BAR > 0) {  // macOS 27: botões da barra e da lateral
            for (auto& gh : g_gh) if (In(gh.r, x, y)) {
                if (m == WM_RBUTTONUP) return 0;
                switch (gh.kind) {
                case 1: g_view = gh.arg; g_sel = -1; Filter(); g_scroll = 1e9f; break;
                case 2: g_sbHidden = !g_sbHidden; SB = g_sbHidden ? 0.f : 216.f; break;
                case 3: Zoom(1 / 1.2f); break;
                case 4: Zoom(1.2f); break;
                case 5: ToggleFav(); break;
                case 6: { std::wstring p = SelPath(); if (!p.empty()) { SHELLEXECUTEINFOW se{ sizeof(se) }; se.fMask = SEE_MASK_INVOKEIDLIST; se.lpVerb = L"windows.modernshare"; se.lpFile = p.c_str(); se.nShow = SW_SHOWNORMAL; ShellExecuteExW(&se); } break; }
                case 7: { std::wstring p = SelPath(); if (!p.empty()) { SHELLEXECUTEINFOW se{ sizeof(se) }; se.fMask = SEE_MASK_INVOKEIDLIST; se.lpVerb = L"properties"; se.lpFile = p.c_str(); se.nShow = SW_SHOWNORMAL; ShellExecuteExW(&se); } break; }
                case 8: EditOpen(SelPath()); break;
                case 10: g_showMode = gh.arg; g_scroll = 0; break;
                case 11: {  // busca: o campo abre na barra e filtra pelo nome enquanto digita
                    D2D1_RECT_F fr = D2D1::RectF(gh.r.right - 200 + 32, gh.r.top + 9, gh.r.right - 10, gh.r.bottom - 9);
                    FieldOpen(fr, g_search, 13, false, [](const std::wstring& q, int key) {
                        g_search = key == VK_ESCAPE ? L"" : q; g_sel = -1; Filter(); g_scroll = 0; InvalidateRect(wnd, nullptr, FALSE);
                    });
                    return 0;
                }
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (x < SB || y < TB) return 0;
        } else if (x < SB) {
            int i = (int)((y - 48) / 34);
            if (y >= 48 && i >= 0 && i < 5) { g_view = i; g_sel = -1; Filter(); g_scroll = 1e9f; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        int c = HitCell(x, y);
        g_sel = c;
        InvalidateRect(h, nullptr, FALSE);
        if (c < 0) return 0;
        if (m == WM_LBUTTONDBLCLK) OpenSel();
        else if (m == WM_RBUTTONUP) {
            POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
            bool fav = g_fav.count(SelPath()) > 0;
            HMENU mm = CreatePopupMenu();
            AppendMenuW(mm, MF_STRING, 1, Tr(L"Abrir"));
            AppendMenuW(mm, MF_STRING, 2, fav ? Tr(L"Remover dos Favoritos") : Tr(L"Favoritar"));
            AppendMenuW(mm, MF_STRING, 3, Tr(L"Mostrar no Finder"));
            AppendMenuW(mm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(mm, MF_STRING, 4, Tr(L"Mover para o Lixo"));
            int k = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr);
            DestroyMenu(mm);
            if (k == 1) OpenSel();
            else if (k == 2) ToggleFav();
            else if (k == 3) { std::wstring p = SelPath(); PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(p.c_str()); if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); } }
            else if (k == 4) Trash();
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (g_ed.on) { if (w == VK_ESCAPE) EditClose(); else if (ctrl && w == 'S') EditSave(); return 0; }
        if (w == 'E' && !ctrl) { EditOpen(SelPath()); return 0; }
        int cols = 1; if (g_cells.size() > 1) { float top = g_cells[0].r.top; for (size_t i = 1; i < g_cells.size() && g_cells[i].r.top == top; i++) cols++; }
        if (ctrl && (w == VK_OEM_PLUS || w == VK_ADD)) Zoom(1.2f);
        else if (ctrl && (w == VK_OEM_MINUS || w == VK_SUBTRACT)) Zoom(1 / 1.2f);
        else if (ctrl && w == 'W') DestroyWindow(h);
        else if (w == VK_RETURN || w == VK_SPACE) OpenSel();
        else if (w == VK_OEM_PERIOD) ToggleFav();
        else if (w == VK_DELETE) Trash();
        else if (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN) {
            int d = w == VK_LEFT ? -1 : w == VK_RIGHT ? 1 : w == VK_UP ? -cols : cols;
            g_sel = std::clamp(g_sel < 0 ? (int)g_shown.size() - 1 : g_sel + d, 0, (int)g_shown.size() - 1);
            for (auto& c : g_cells) if (c.idx == g_sel) {  // rola até a escolhida
                float vh = Size().height;
                if (c.r.top - g_scroll < TB) g_scroll = c.r.top - TB - 8;
                if (c.r.bottom - g_scroll > vh) g_scroll = c.r.bottom - vh + 8;
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_favFile = DataPath(L"fotos-favoritos.txt");
    LoadFav();
    g_tfSide = Fmt(13);
    g_tfHead = Fmt(17, DWRITE_FONT_WEIGHT_BOLD);
    g_tfTitle = Fmt(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfSmall = Fmt(12);
    g_tfIcon = Fmt(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfBadge = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfTitleG = Fmt(13, DWRITE_FONT_WEIGHT_BOLD);
    g_tfSideB = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfSec = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfSeg = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfFootB = Fmt(12, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    barWanted = 52;  // macOS 27: barra unificada
    if (!CreateMain(hi, L"FotosJanela", Tr(L"Fotos"), Proc, semaforo::Aqua() ? 1222 : 1200, semaforo::Aqua() ? 720 : 800)) return 1;
    if (BAR > 0) { SB = 216; GAP = 17.5f; g_cell = 176; }
    g_px = (int)std::lround(256 * Sc());
    for (int i = 0; i < 2; i++) std::thread(ThumbWorker).detach();
    Scan();
    ShowWindow(wnd, show);
    return Run();
}
