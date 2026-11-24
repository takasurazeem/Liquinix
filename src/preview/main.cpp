// Pré-Visualização (Preview do Mac): abre imagens e PDFs. Barra de ferramentas no alto (nome e página/tamanho à
// esquerda; girar, reduzir, ampliar, ajustar e imprimir à direita), fundo cinza e as páginas no meio.
//  - Imagens (WIC: PNG, JPEG, GIF, BMP, TIFF, WebP, HEIC com o codec instalado): ← / → passam para a imagem
//    anterior/próxima da mesma pasta; Ctrl+R / Ctrl+L giram (só a visualização).
//  - PDF (Windows.Data.Pdf): páginas uma embaixo da outra, desenhadas numa thread conforme aparecem;
//    PageUp/PageDown/Home/End.
//  - Zoom: Ctrl+roda, Ctrl+= / Ctrl+-, Ctrl+0 tamanho real, Ctrl+9 ajustar à janela. Arrastar move a página.
//  - Ctrl+O abrir, Ctrl+P imprimir (o "imprimir" do Windows), Ctrl+W fechar; soltar um arquivo na janela abre.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shcore.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../comum/semaforo.h"  // semaforo::Aqua(): tema macOS Golden Gate
#include "../comum/barra_mac.h"  // os três botões do Mac (WacOSWindow.dll), com ou sem o Shell
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include "../comum/idioma.h"
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")

namespace wdp = winrt::Windows::Data::Pdf;
namespace wst = winrt::Windows::Storage;
namespace wss = winrt::Windows::Storage::Streams;

namespace {

const UINT WM_PV_INFO = WM_APP + 1, WM_PV_PAGE = WM_APP + 2, WM_PV_ASK = WM_APP + 3;
const float TOOLH = 52, MB = 42, GAP = 18, MARGIN = 24;
float TB = TOOLH;  // altura de cima: a barra de ferramentas e, marcando, a barra de marcação

HWND g_wnd;
ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
IWICImagingFactory* g_wic;
ID2D1RenderTarget* g_rt;        // a janela (ou, no teste --foto, um bitmap)
ID2D1HwndRenderTarget* g_hrt;
IWICBitmap* g_fotoBmp;          // --foto <arquivo.png> (teste): desenha num bitmap e salva, sem passar pela tela
std::wstring g_fotoPath;
ID2D1SolidColorBrush* g_br;
IDWriteTextFormat *g_tfTitle, *g_tfSub, *g_tfIcon, *g_tfEmpty;

struct Page {
    float w = 0, h = 0;          // tamanho de origem (pontos do PDF / pixels da imagem)
    ID2D1Bitmap* bmp = nullptr;
    std::vector<BYTE> px; UINT pw = 0, ph = 0;  // PDF: os pixels da última renderização (para exportar)
    int renderedW = 0;           // largura (px) com que foi desenhada
    bool pending = false;
};
std::vector<Page> g_pages;
std::wstring g_path, g_error;
bool g_pdf = false;
int g_gen = 0;                   // documento aberto (as respostas de documentos antigos são descartadas)
bool g_fit = true;               // ajustar à janela (senão g_zoom vale)
float g_zoom = 1;                // pixels de tela (DIP) por unidade de origem
int g_rot = 0;                   // imagem: 0..3 (x90 graus)
float g_sx = 0, g_sy = 0;        // rolagem (DIP)
bool g_drag = false; POINT g_dragAt{}; float g_dragSx = 0, g_dragSy = 0;
int g_hoverBtn = -1;
// macOS 27 (captura Preview do 512 Pixels): barra unificada de 52 (sem a barra do Windows; a WacOSWindow.dll põe os
// três botões a 26/49/72), o nome em negrito e "Página 1 de 81" à esquerda depois deles, e os botões em grupos de
// vidro redondos à direita (−/1:1/+ juntos numa cápsula)
struct Btn { wchar_t glyph; const wchar_t* tip; int id; D2D1_RECT_F r; };
std::vector<Btn> g_btns;
enum { B_ROT, B_OUT, B_IN, B_FIT, B_PRINT, B_MARK, B_EXPORT, B_SIDE };
// macOS 27 (captura Preview, 2x): PDF com mais de uma página abre com a coluna das miniaturas à esquerda (206 pt,
// por baixo dos três botões da janela); a página atual fica num fundo azul-acinzentado arredondado, o número embaixo.
// O botão da lateral (com ⌄) fica na barra, no fim da coluna.
bool g_side = true;
float g_thumbTotal = 0;
IDWriteTextFormat* g_tfThumbNum;
float g_thumbScroll = 0;
std::vector<std::pair<int, D2D1_RECT_F>> g_thumbHits;

// ---- marcação (Marcar do Mac): os traços ficam nas unidades da página (pixels da imagem / pontos do PDF)
enum Tool { T_PEN, T_HILITE, T_RECT, T_OVAL, T_ARROW, T_TEXT, T_SIGN, T_CROP, T_COUNT };
struct Ann { int page; int tool; D2D1_COLOR_F col; std::vector<D2D1_POINT_2F> pts; std::wstring text; };
std::vector<Ann> g_anns;
bool g_markup = false, g_drawing = false, g_dirty = false;
int g_tool = T_PEN, g_color = 1;
Ann g_cur;
std::vector<BYTE> g_imgPx; UINT g_imgW = 0, g_imgH = 0;   // a imagem aberta (recortar / exportar)
std::vector<std::vector<D2D1_POINT_2F>> g_sign;          // assinatura guardada (0..1 na largura)
bool g_signCapture = false; size_t g_signFrom = 0;       // desenhando a assinatura (os traços desde g_signFrom)
HWND g_text; WNDPROC g_textOld; D2D1_POINT_2F g_textAt; int g_textPage = -1;
struct MBtn { int kind, arg; D2D1_RECT_F r; };           // kind: 0 ferramenta, 1 cor, 2 desfazer, 3 concluir
std::vector<MBtn> g_mbtns;
const D2D1_COLOR_F COLORS[] = { { 0, 0, 0, 1 }, { 1, 0.23f, 0.19f, 1 }, { 1, 0.58f, 0, 1 }, { 1, 0.8f, 0, 1 }, { 0.2f, 0.78f, 0.35f, 1 }, { 0, 0.48f, 1, 1 }, { 1, 1, 1, 1 } };

// ---- thread do PDF: abre o documento e desenha as páginas pedidas
struct Job { int gen; int page; int width; };
std::mutex g_mx; std::condition_variable g_cv; std::deque<Job> g_jobs;
std::wstring g_jobPath; int g_jobGen = -1; bool g_quit = false;
struct PageResult { int gen, page, width; UINT w, h; std::vector<BYTE> px; };
struct InfoResult { int gen; std::vector<std::pair<float, float>> sizes; std::wstring error; };

bool Dark() {
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}
float Sc() { return GetDpiForWindow(g_wnd) / 96.f; }

bool DecodeStream(IStream* st, UINT& w, UINT& h, std::vector<BYTE>& px) {
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(g_wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h));
    if (ok) { px.resize((size_t)w * h * 4); ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())); }
    if (cv) cv->Release(); if (fr) fr->Release(); dec->Release();
    return ok;
}

void PdfThread() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    wdp::PdfDocument doc{ nullptr };
    int docGen = -1;
    for (;;) {
        Job job{};
        std::wstring path; int gen;
        {
            std::unique_lock<std::mutex> l(g_mx);
            g_cv.wait(l, [&] { return g_quit || !g_jobs.empty() || g_jobGen != docGen; });
            if (g_quit) return;
            path = g_jobPath; gen = g_jobGen;
            if (gen == docGen && !g_jobs.empty()) { job = g_jobs.front(); g_jobs.pop_front(); }
        }
        if (gen != docGen) {  // documento novo: abre e manda os tamanhos das páginas
            docGen = gen; doc = nullptr;
            auto* info = new InfoResult{ gen };
            try {
                auto file = wst::StorageFile::GetFileFromPathAsync(path).get();
                doc = wdp::PdfDocument::LoadFromFileAsync(file).get();
                for (uint32_t i = 0; i < doc.PageCount(); i++) { auto s = doc.GetPage(i).Size(); info->sizes.push_back({ s.Width, s.Height }); }
            } catch (winrt::hresult_error const& e) {
                info->error = e.code() == HRESULT_FROM_WIN32(ERROR_INVALID_PASSWORD) || e.code() == (HRESULT)0x8007052B ? Tr(L"O PDF tem senha.") : Tr(L"Não foi possível abrir o PDF.");
            } catch (...) { info->error = Tr(L"Não foi possível abrir o PDF."); }
            if (!PostMessageW(g_wnd, WM_PV_INFO, 0, (LPARAM)info)) delete info;
            continue;
        }
        if (!doc || job.gen != docGen || job.page < 0 || job.page >= (int)doc.PageCount()) continue;
        auto* res = new PageResult{ job.gen, job.page, job.width };
        try {
            auto page = doc.GetPage(job.page);
            wss::InMemoryRandomAccessStream ms;
            wdp::PdfPageRenderOptions opt;
            opt.DestinationWidth(job.width);
            opt.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
            page.RenderToStreamAsync(ms, opt).get();
            ms.Seek(0);
            IStream* st = nullptr;
            if (SUCCEEDED(CreateStreamOverRandomAccessStream(winrt::get_unknown(ms), IID_PPV_ARGS(&st)))) {
                DecodeStream(st, res->w, res->h, res->px);
                st->Release();
            }
        } catch (...) {}
        if (!PostMessageW(g_wnd, WM_PV_PAGE, 0, (LPARAM)res)) delete res;
    }
}

void Request(int page, int width) {
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& j : g_jobs) if (j.page == page && j.gen == g_gen) { j.width = width; return; }
    g_jobs.push_back({ g_gen, page, width });
    g_cv.notify_one();
}

// ---- documento
void FreePages() { for (auto& p : g_pages) if (p.bmp) p.bmp->Release(); g_pages.clear(); }

bool IsPdf(const std::wstring& p) { return !_wcsicmp(PathFindExtensionW(p.c_str()), L".pdf"); }
bool IsImage(const std::wstring& p) {
    std::wstring x = PathFindExtensionW(p.c_str()); for (auto& c : x) c = towlower(c);
    for (auto e : { L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".tif", L".tiff", L".webp", L".heic", L".heif", L".ico", L".jfif", L".avif", L".dds", L".jxr" }) if (x == e) return true;
    return false;
}

void SetTitle() {
    std::wstring n = g_path.empty() ? Tr(L"Pré-Visualização") : g_path.substr(g_path.find_last_of(L"\\/") + 1);
    SetWindowTextW(g_wnd, n.c_str());
}

void Open(const std::wstring& path) {
    FreePages();
    g_path = path; g_error.clear(); g_gen++;
    g_fit = true; g_rot = 0; g_sx = g_sy = 0;
    g_pdf = IsPdf(path);
    SetTitle();
    if (g_pdf) {
        g_anns.clear(); g_dirty = false; g_imgPx.clear();
        std::lock_guard<std::mutex> l(g_mx);
        g_jobs.clear(); g_jobPath = path; g_jobGen = g_gen; g_cv.notify_one();
    } else {
        IWICBitmapDecoder* dec = nullptr;
        UINT w = 0, h = 0; std::vector<BYTE> px;
        IStream* st = nullptr;
        bool ok = SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &st)) && DecodeStream(st, w, h, px);
        if (st) st->Release(); (void)dec;
        g_anns.clear(); g_dirty = false;
        if (ok) { g_imgPx = px; g_imgW = w; g_imgH = h; }
        if (ok && g_rt) {
            Page p; p.w = (float)w; p.h = (float)h;
            g_rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &p.bmp);
            g_pages.push_back(p);
        } else g_error = Tr(L"Não foi possível abrir este arquivo.");
    }
    SHAddToRecentDocs(SHARD_PATHW, path.c_str());
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// imagens da mesma pasta, em ordem de nome (← / →)
void Neighbor(int dir) {
    if (g_pdf || g_path.empty()) return;
    std::wstring folder = g_path.substr(0, g_path.find_last_of(L"\\/"));
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImage(fd.cFileName)) files.push_back(folder + L"\\" + fd.cFileName); } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    auto it = std::find_if(files.begin(), files.end(), [](auto& x) { return !_wcsicmp(x.c_str(), g_path.c_str()); });
    if (it == files.end() || files.size() < 2) return;
    int i = (int)(it - files.begin()) + dir;
    i = (i + (int)files.size()) % (int)files.size();
    Open(files[i]);
}

// ---- geometria: tamanho de cada página na tela (DIP), com o zoom atual
bool g_unified;
float SideW() { return g_unified && g_side && g_pdf && g_pages.size() > 1 ? 206.f : 0.f; }
D2D1_SIZE_F ViewSize() {
    RECT r; GetClientRect(g_wnd, &r);
    float sc = Sc();
    return { r.right / sc - SideW(), r.bottom / sc - TB };
}
float FitZoom() {
    if (g_pages.empty()) return 1;
    D2D1_SIZE_F v = ViewSize();
    float maxW = 0, firstH = 0;
    for (auto& p : g_pages) { bool sw = !g_pdf && (g_rot & 1); maxW = std::max(maxW, sw ? p.h : p.w); }
    auto& p0 = g_pages[0];
    bool sw = !g_pdf && (g_rot & 1);
    firstH = sw ? p0.w : p0.h;
    float zw = (v.width - 2 * MARGIN) / maxW;
    if (g_pdf) return std::max(0.05f, zw);  // PDF: a largura da janela
    float zh = (v.height - 2 * MARGIN) / firstH;
    return std::max(0.01f, std::min(zw, zh));  // imagem: inteira na janela
}
float Zoom() { return g_fit ? (g_pdf ? FitZoom() : std::min(FitZoom(), 1.f)) : g_zoom; }

struct Placed { D2D1_RECT_F r; int idx; };
std::vector<Placed> Layout(float& totalW, float& totalH) {
    std::vector<Placed> out;
    float z = Zoom(), y = MARGIN;
    totalW = 0;
    for (int i = 0; i < (int)g_pages.size(); i++) {
        auto& p = g_pages[i];
        bool sw = !g_pdf && (g_rot & 1);
        float w = (sw ? p.h : p.w) * z, h = (sw ? p.w : p.h) * z;
        out.push_back({ D2D1::RectF(0, y, w, y + h), i });
        totalW = std::max(totalW, w);
        y += h + GAP;
    }
    totalH = y - GAP + MARGIN;
    totalW += 2 * MARGIN;
    return out;
}

void ClampScroll() {
    float tw, th; Layout(tw, th);
    D2D1_SIZE_F v = ViewSize();
    g_sx = std::clamp(g_sx, 0.f, std::max(0.f, tw - v.width));
    g_sy = std::clamp(g_sy, 0.f, std::max(0.f, th - v.height));
}

int CurrentPage() {
    float tw, th; auto pl = Layout(tw, th);
    D2D1_SIZE_F v = ViewSize();
    float mid = g_sy + v.height / 3;
    for (auto& p : pl) if (p.r.bottom + GAP >= mid) return p.idx;
    return pl.empty() ? 0 : pl.back().idx;
}

void SetZoom(float z, POINT anchorClient = { -1, -1 }) {
    float old = Zoom();
    z = std::clamp(z, 0.05f, 16.f);
    D2D1_SIZE_F v = ViewSize();
    float sc = Sc();
    float ax = anchorClient.x >= 0 ? anchorClient.x / sc - SideW() : v.width / 2, ay = anchorClient.y >= 0 ? anchorClient.y / sc - TB : v.height / 2;
    float cx = (g_sx + ax) / old, cy = (g_sy + ay) / old;  // ponto do documento sob o mouse fica parado
    g_fit = false; g_zoom = z;
    g_sx = cx * z - ax; g_sy = cy * z - ay;
    ClampScroll();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// ---- desenho
void CreateRes() {
    if (g_rt) return;
    RECT r; GetClientRect(g_wnd, &r);
    float dpi = (float)GetDpiForWindow(g_wnd);
    if (!g_fotoPath.empty()) {
        IWICImagingFactory* wic = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (wic && r.right > 0 && r.bottom > 0 && SUCCEEDED(wic->CreateBitmap(r.right, r.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &g_fotoBmp)))
            g_d2d->CreateWicBitmapRenderTarget(g_fotoBmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi), &g_rt);
        if (wic) wic->Release();
    } else {
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi);
        g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(r.right, r.bottom)), &g_hrt);
        g_rt = g_hrt;
    }
    if (g_rt) g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_br);
}
void DropRes() {
    for (auto& p : g_pages) if (p.bmp) { p.bmp->Release(); p.bmp = nullptr; p.renderedW = 0; }
    if (g_br) { g_br->Release(); g_br = nullptr; }
    if (g_rt) { g_rt->Release(); g_rt = nullptr; g_hrt = nullptr; }
    if (!g_pdf && !g_path.empty()) { std::wstring p = g_path; CreateRes(); float z = g_zoom; bool f = g_fit; int ro = g_rot; float sx = g_sx, sy = g_sy; Open(p); g_zoom = z; g_fit = f; g_rot = ro; g_sx = sx; g_sy = sy; }
}

// ---- marcação
bool PageRect(int idx, D2D1_RECT_F& out) {
    D2D1_SIZE_F v = ViewSize();
    float tw, th; auto pl = Layout(tw, th);
    float offX = (tw < v.width ? (v.width - tw) / 2 : -g_sx) + SideW(), offY = TB - g_sy;
    for (auto& p : pl) if (p.idx == idx) {
        out = D2D1::RectF(offX + MARGIN + (tw - 2 * MARGIN - (p.r.right - p.r.left)) / 2, offY + p.r.top, 0, offY + p.r.bottom);
        out.right = out.left + (p.r.right - p.r.left);
        return true;
    }
    return false;
}
int PageAt(float x, float y, D2D1_POINT_2F& pt) {
    float z = Zoom();
    for (int i = 0; i < (int)g_pages.size(); i++) {
        D2D1_RECT_F r;
        if (PageRect(i, r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom) { pt = { (x - r.left) / z, (y - r.top) / z }; return i; }
    }
    return -1;
}

void DrawAnn(ID2D1RenderTarget* rt, const Ann& a, D2D1_RECT_F r, float z) {
    if (a.pts.empty()) return;
    ID2D1SolidColorBrush* b = nullptr;
    D2D1_COLOR_F c = a.col;
    if (a.tool == T_HILITE) c.a = 0.35f;
    rt->CreateSolidColorBrush(c, &b);
    if (!b) return;
    auto P = [&](D2D1_POINT_2F p) { return D2D1::Point2F(r.left + p.x * z, r.top + p.y * z); };
    ID2D1StrokeStyle* round = nullptr;
    g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
    float w = (a.tool == T_HILITE ? 16.f : a.tool == T_SIGN ? 2.2f : 3.f) * z;
    if (a.tool == T_PEN || a.tool == T_HILITE || a.tool == T_SIGN) {
        ID2D1PathGeometry* g = nullptr;
        g_d2d->CreatePathGeometry(&g);
        if (g) {
            ID2D1GeometrySink* s = nullptr; g->Open(&s);
            s->BeginFigure(P(a.pts[0]), D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < a.pts.size(); i++) s->AddLine(P(a.pts[i]));
            if (a.pts.size() == 1) s->AddLine(P({ a.pts[0].x + 0.1f, a.pts[0].y }));
            s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
            rt->DrawGeometry(g, b, w, round);
            g->Release();
        }
    } else if (a.pts.size() >= 2) {
        D2D1_POINT_2F p0 = P(a.pts[0]), p1 = P(a.pts.back());
        D2D1_RECT_F rr = D2D1::RectF(std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y));
        if (a.tool == T_RECT) rt->DrawRectangle(rr, b, w);
        else if (a.tool == T_OVAL) rt->DrawEllipse(D2D1::Ellipse({ (rr.left + rr.right) / 2, (rr.top + rr.bottom) / 2 }, (rr.right - rr.left) / 2, (rr.bottom - rr.top) / 2), b, w);
        else if (a.tool == T_ARROW) {
            rt->DrawLine(p0, p1, b, w, round);
            float ang = std::atan2(p1.y - p0.y, p1.x - p0.x), L = 14 * z;
            for (float s : { 0.5f, -0.5f }) rt->DrawLine(p1, { p1.x - L * std::cos(ang + s), p1.y - L * std::sin(ang + s) }, b, w, round);
        } else if (a.tool == T_CROP) {
            ID2D1SolidColorBrush* dim = nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.45f), &dim);
            if (dim) {  // escurece o que vai sair
                rt->FillRectangle(D2D1::RectF(r.left, r.top, r.right, rr.top), dim); rt->FillRectangle(D2D1::RectF(r.left, rr.bottom, r.right, r.bottom), dim);
                rt->FillRectangle(D2D1::RectF(r.left, rr.top, rr.left, rr.bottom), dim); rt->FillRectangle(D2D1::RectF(rr.right, rr.top, r.right, rr.bottom), dim);
                dim->Release();
            }
            b->SetColor(D2D1::ColorF(1, 1, 1, 1)); rt->DrawRectangle(rr, b, 1.5f);
        }
    }
    if (a.tool == T_TEXT && !a.text.empty()) {
        IDWriteTextFormat* f = nullptr;
        g_dw->CreateTextFormat(L"Helvetica", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, std::max(1.f, 18 * z), L"", &f);
        if (!f) g_dw->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, std::max(1.f, 18 * z), L"", &f);
        if (f) { D2D1_POINT_2F p = P(a.pts[0]); rt->DrawTextW(a.text.c_str(), (UINT32)a.text.size(), f, D2D1::RectF(p.x, p.y, p.x + 4000, p.y + 400), b); f->Release(); }
    }
    if (round) round->Release();
    b->Release();
}

// gira os pixels da imagem (marcar sempre trabalha na imagem já girada)
void BakeRotation() {
    if (g_pdf || !g_rot || g_imgPx.empty()) return;
    for (int k = 0; k < g_rot; k++) {  // 90° no sentido horário, g_rot vezes
        std::vector<BYTE> o(g_imgPx.size());
        UINT W = g_imgW, H = g_imgH;
        for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) memcpy(&o[((size_t)x * H + (H - 1 - y)) * 4], &g_imgPx[((size_t)y * W + x) * 4], 4);
        g_imgPx.swap(o); g_imgW = H; g_imgH = W;
    }
    g_rot = 0;
    Page& p = g_pages[0];
    if (p.bmp) { p.bmp->Release(); p.bmp = nullptr; }
    p.w = (float)g_imgW; p.h = (float)g_imgH;
    g_rt->CreateBitmap(D2D1::SizeU(g_imgW, g_imgH), g_imgPx.data(), g_imgW * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &p.bmp);
}

void ApplyCrop(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    if (g_pdf || g_imgPx.empty()) return;
    int x0 = std::clamp((int)std::min(a.x, b.x), 0, (int)g_imgW), y0 = std::clamp((int)std::min(a.y, b.y), 0, (int)g_imgH);
    int x1 = std::clamp((int)std::max(a.x, b.x), 0, (int)g_imgW), y1 = std::clamp((int)std::max(a.y, b.y), 0, (int)g_imgH);
    if (x1 - x0 < 8 || y1 - y0 < 8) return;
    UINT W = x1 - x0, H = y1 - y0;
    std::vector<BYTE> o((size_t)W * H * 4);
    for (UINT y = 0; y < H; y++) memcpy(&o[(size_t)y * W * 4], &g_imgPx[(((size_t)y0 + y) * g_imgW + x0) * 4], (size_t)W * 4);
    g_imgPx.swap(o); g_imgW = W; g_imgH = H;
    for (auto& an : g_anns) for (auto& p : an.pts) { p.x -= x0; p.y -= y0; }
    Page& p = g_pages[0];
    if (p.bmp) { p.bmp->Release(); p.bmp = nullptr; }
    p.w = (float)W; p.h = (float)H;
    g_rt->CreateBitmap(D2D1::SizeU(W, H), g_imgPx.data(), W * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &p.bmp);
    g_dirty = true;
    g_fit = true;
}

// assinatura guardada: traços normalizados (0..1 na largura do conjunto)
std::wstring SignFile() { wchar_t ad[MAX_PATH] = {}; GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH); return std::wstring(ad) + L"\\Shell\\assinatura.txt"; }
void LoadSign() {
    g_sign.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, SignFile().c_str(), L"r") || !f) return;
    wchar_t line[65536];
    while (fgetws(line, 65536, f)) {
        std::vector<D2D1_POINT_2F> s; wchar_t* ctx = nullptr;
        for (wchar_t* tok = wcstok_s(line, L" \n", &ctx); tok; tok = wcstok_s(nullptr, L" \n", &ctx)) { float x, y; if (swscanf_s(tok, L"%f,%f", &x, &y) == 2) s.push_back({ x, y }); }
        if (!s.empty()) g_sign.push_back(s);
    }
    fclose(f);
}
void SaveSign(const std::vector<std::vector<D2D1_POINT_2F>>& strokes) {
    float x0 = 1e9, y0 = 1e9, x1 = -1e9;
    for (auto& s : strokes) for (auto& p : s) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); }
    float w = std::max(1.f, x1 - x0);
    wchar_t ad[MAX_PATH] = {}; GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH); CreateDirectoryW((std::wstring(ad) + L"\\Shell").c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, SignFile().c_str(), L"w") || !f) return;
    g_sign.clear();
    for (auto& s : strokes) {
        std::vector<D2D1_POINT_2F> n;
        for (auto& p : s) { D2D1_POINT_2F q{ (p.x - x0) / w, (p.y - y0) / w }; n.push_back(q); fwprintf(f, L"%.4f,%.4f ", q.x, q.y); }
        fwprintf(f, L"\n"); g_sign.push_back(n);
    }
    fclose(f);
}
void EndSignCapture() {
    if (!g_signCapture) return;
    g_signCapture = false;
    std::vector<std::vector<D2D1_POINT_2F>> strokes;
    for (size_t i = g_signFrom; i < g_anns.size(); i++) if (g_anns[i].tool == T_SIGN) strokes.push_back(g_anns[i].pts);
    if (!strokes.empty()) SaveSign(strokes);
}

void CommitText();
LRESULT CALLBACK TextProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) { if (w == VK_ESCAPE) SetWindowTextW(h, L""); CommitText(); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == 27)) return 0;
    if (m == WM_KILLFOCUS) PostMessageW(g_wnd, WM_APP + 9, 0, 0);
    return CallWindowProcW(g_textOld, h, m, w, l);
}
void CommitText() {
    if (!g_text) return;
    int n = GetWindowTextLengthW(g_text);
    std::wstring t(n, L'\0'); GetWindowTextW(g_text, t.data(), n + 1);
    HWND h = g_text; g_text = nullptr; DestroyWindow(h);
    if (!t.empty() && g_textPage >= 0) { g_anns.push_back({ g_textPage, T_TEXT, COLORS[g_color], { g_textAt }, t }); g_dirty = true; }
    InvalidateRect(g_wnd, nullptr, FALSE);
}
void StartText(int page, D2D1_POINT_2F at, float sx, float sy) {
    CommitText();
    g_textPage = page; g_textAt = at;
    float s = Sc(), z = Zoom();
    static HFONT font; if (font) DeleteObject(font);
    font = CreateFontW(-(int)std::lround(18 * z * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Arial");
    g_text = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, (int)(sx * s), (int)(sy * s), (int)(320 * s), (int)(18 * z * s * 1.5f + 6), g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_text, WM_SETFONT, (WPARAM)font, TRUE);
    g_textOld = (WNDPROC)SetWindowLongPtrW(g_text, GWLP_WNDPROC, (LONG_PTR)TextProc);
    SetFocus(g_text);
}

// exporta (imagem: a imagem toda; PDF: a página atual) com a marcação, em PNG ou JPEG
void Export() {
    if (g_pages.empty()) return;
    int page = g_pdf ? CurrentPage() : 0;
    Page& pg = g_pages[page];
    const std::vector<BYTE>& src = g_pdf ? pg.px : g_imgPx;
    UINT W = g_pdf ? pg.pw : g_imgW, H = g_pdf ? pg.ph : g_imgH;
    if (src.empty() || !W || !H) { MessageBoxW(g_wnd, Tr(L"A página ainda está carregando."), Tr(L"Pré-Visualização"), MB_OK); return; }
    if (!g_pdf && g_rot) BakeRotation();
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"PNG", L"*.png" }, { L"JPEG", L"*.jpg" } };
    d->SetFileTypes(2, f); d->SetDefaultExtension(L"png");
    std::wstring base = g_path.substr(g_path.find_last_of(L"\\/") + 1); base = base.substr(0, base.find_last_of(L'.'));
    if (g_pdf) { wchar_t b[32]; swprintf_s(b, L" - %d", page + 1); base += b; }
    d->SetFileName((base + Tr(L" (editado)")).c_str());
    std::wstring out;
    if (SUCCEEDED(d->Show(g_wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); } it->Release(); } }
    d->Release();
    if (out.empty()) return;
    bool jpg = !_wcsicmp(PathFindExtensionW(out.c_str()), L".jpg") || !_wcsicmp(PathFindExtensionW(out.c_str()), L".jpeg");
    IWICBitmap* wb = nullptr; ID2D1RenderTarget* rt = nullptr; IWICBitmap* srcBmp = nullptr; ID2D1Bitmap* img = nullptr;
    bool ok = SUCCEEDED(g_wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wb)) &&
              SUCCEEDED(g_d2d->CreateWicBitmapRenderTarget(wb, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &rt)) &&
              SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(W, H), src.data(), W * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &img));
    if (ok) {
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(1, 1, 1, jpg ? 1.f : 0.f));
        rt->DrawBitmap(img, D2D1::RectF(0, 0, (float)W, (float)H));
        float z = W / pg.w;
        for (auto& a : g_anns) if (a.page == page) DrawAnn(rt, a, D2D1::RectF(0, 0, (float)W, (float)H), z);
        ok = SUCCEEDED(rt->EndDraw());
    }
    if (img) img->Release(); if (rt) rt->Release(); (void)srcBmp;
    if (ok) {
        IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fe = nullptr;
        ok = SUCCEEDED(g_wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(g_wic->CreateEncoder(jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(enc->CreateNewFrame(&fe, nullptr)) && SUCCEEDED(fe->Initialize(nullptr));
        if (ok) {
            WICPixelFormatGUID fmt = jpg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
            fe->SetSize(W, H); fe->SetPixelFormat(&fmt);
            IWICFormatConverter* cv = nullptr;
            ok = SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(wb, fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                 SUCCEEDED(fe->WriteSource(cv, nullptr)) && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
            if (cv) cv->Release();
        }
        if (fe) fe->Release(); if (enc) enc->Release(); if (st) st->Release();
    }
    if (wb) wb->Release();
    if (ok) g_dirty = false;
    else MessageBoxW(g_wnd, Tr(L"Não foi possível exportar."), Tr(L"Pré-Visualização"), MB_OK | MB_ICONERROR);
}

void SetMarkup(bool on) {
    if (on == g_markup) return;
    CommitText(); EndSignCapture();
    g_markup = on;
    TB = on ? TOOLH + MB : TOOLH;
    if (on) { BakeRotation(); LoadSign(); }
    ClampScroll();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void PaintMarkupBar(bool dark, float W) {
    g_mbtns.clear();
    if (!g_markup) return;
    g_br->SetColor(dark ? D2D1::ColorF(0.21f, 0.21f, 0.22f) : D2D1::ColorF(0.93f, 0.93f, 0.94f));
    g_rt->FillRectangle(D2D1::RectF(0, TOOLH, W, TOOLH + MB), g_br);
    g_br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0, 0, 0, 0.12f));
    g_rt->DrawLine({ 0, TOOLH + MB - 0.5f }, { W, TOOLH + MB - 0.5f }, g_br, 1);
    struct T { int tool; wchar_t g; } tools[] = { { T_PEN, 0xE76D }, { T_HILITE, 0xE7E6 }, { T_RECT, 0xE739 }, { T_OVAL, 0xEA3A }, { T_ARROW, 0xE72A }, { T_TEXT, 0xE8D2 }, { T_SIGN, 0xEE56 }, { T_CROP, 0xE7A8 } };
    float x = 14, cy = TOOLH + MB / 2;
    for (auto& t : tools) {
        if (t.tool == T_CROP && g_pdf) continue;
        D2D1_RECT_F r = D2D1::RectF(x, cy - 14, x + 32, cy + 14);
        if (g_tool == t.tool) { g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.10f)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), g_br); }
        g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.85f) : D2D1::ColorF(0, 0, 0, 0.75f));
        wchar_t g[2] = { t.g, 0 };
        g_rt->DrawTextW(g, 1, g_tfIcon, r, g_br);
        g_mbtns.push_back({ 0, t.tool, r });
        x += 36;
    }
    x += 14;
    for (int i = 0; i < 7; i++) {
        D2D1_POINT_2F c{ x + 11, cy };
        g_br->SetColor(COLORS[i]); g_rt->FillEllipse(D2D1::Ellipse(c, 9, 9), g_br);
        g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.3f) : D2D1::ColorF(0, 0, 0, 0.25f)); g_rt->DrawEllipse(D2D1::Ellipse(c, 9, 9), g_br, 1);
        if (i == g_color) { g_br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1, 1)); g_rt->DrawEllipse(D2D1::Ellipse(c, 12.5f, 12.5f), g_br, 2); }
        g_mbtns.push_back({ 1, i, D2D1::RectF(x, cy - 12, x + 22, cy + 12) });
        x += 28;
    }
    x += 14;
    D2D1_RECT_F u = D2D1::RectF(x, cy - 14, x + 32, cy + 14);
    g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, g_anns.empty() ? 0.3f : 0.85f) : D2D1::ColorF(0, 0, 0, g_anns.empty() ? 0.25f : 0.75f));
    wchar_t ug[2] = { 0xE7A7, 0 }; g_rt->DrawTextW(ug, 1, g_tfIcon, u, g_br);
    g_mbtns.push_back({ 2, 0, u });
    if (g_tool == T_SIGN) {
        const wchar_t* tip = g_sign.empty() || g_signCapture ? Tr(L"Desenhe a assinatura na página; ela fica guardada.") : Tr(L"Clique na página para pôr a assinatura. Botão direito: desenhar outra.");
        g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.55f));
        g_rt->DrawTextW(tip, (UINT32)wcslen(tip), g_tfSub, D2D1::RectF(u.right + 16, TOOLH, W - 100, TOOLH + MB), g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    D2D1_RECT_F ok = D2D1::RectF(W - 84, cy - 13, W - 14, cy + 13);
    g_br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1, 1)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(ok, 6, 6), g_br);
    g_br->SetColor(D2D1::ColorF(1, 1, 1, 1)); g_rt->DrawTextW(L"OK", 2, g_tfEmpty, ok, g_br);
    g_mbtns.push_back({ 3, 0, ok });
}

void Paint() {
    CreateRes();
    if (!g_rt) return;
    bool dark = Dark();
    D2D1_SIZE_F v = ViewSize();
    float W = v.width;
    g_rt->BeginDraw();
    g_rt->Clear(dark ? D2D1::ColorF(0.118f, 0.118f, 0.122f) : D2D1::ColorF(0.89f, 0.89f, 0.90f));
    // páginas
    float tw, th; auto pl = Layout(tw, th);
    const float XS = SideW();
    float offX = (tw < W ? (W - tw) / 2 : -g_sx) + XS, offY = TB - g_sy;
    float z = Zoom(), sc = Sc();
    g_rt->PushAxisAlignedClip(D2D1::RectF(XS, TB, XS + W, TB + v.height), D2D1_ANTIALIAS_MODE_ALIASED);
    for (auto& p : pl) {
        D2D1_RECT_F r = D2D1::RectF(offX + MARGIN + (tw - 2 * MARGIN - (p.r.right - p.r.left)) / 2, offY + p.r.top, 0, offY + p.r.bottom);
        r.right = r.left + (p.r.right - p.r.left);
        if (r.bottom < TB - 200 || r.top > TB + v.height + 200) continue;
        Page& pg = g_pages[p.idx];
        if (g_pdf) {  // pede a página na resolução da tela (de novo se o zoom mudou bastante)
            int want = std::min(6000, (int)std::lround((r.right - r.left) * sc));
            if (!pg.pending && (pg.renderedW == 0 || std::abs(pg.renderedW - want) > want / 6)) { pg.pending = true; Request(p.idx, want); }
            g_br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
            g_rt->FillRectangle(D2D1::RectF(r.left + 1, r.top + 2, r.right + 1, r.bottom + 3), g_br);  // sombra leve
            g_br->SetColor(D2D1::ColorF(1, 1, 1));
            g_rt->FillRectangle(r, g_br);
        }
        if (pg.bmp) {
            D2D1_POINT_2F c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            D2D1_MATRIX_3X2_F old; g_rt->GetTransform(&old);
            D2D1_RECT_F dst = r;
            if (!g_pdf && g_rot) {
                g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(90.f * g_rot, c) * old);
                if (g_rot & 1) { float hw = (r.bottom - r.top) / 2, hh = (r.right - r.left) / 2; dst = D2D1::RectF(c.x - hw, c.y - hh, c.x + hw, c.y + hh); }
            }
            g_rt->DrawBitmap(pg.bmp, dst, 1, z * sc < 1 ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            g_rt->SetTransform(old);
        }
        for (auto& a : g_anns) if (a.page == p.idx) DrawAnn(g_rt, a, r, z);  // marcação
        if (g_drawing && g_cur.page == p.idx) DrawAnn(g_rt, g_cur, r, z);
    }
    g_rt->PopAxisAlignedClip();
    g_thumbHits.clear();
    if (XS > 0) {  // coluna das miniaturas
        g_br->SetColor(dark ? D2D1::ColorF(0.17f, 0.165f, 0.16f) : D2D1::ColorF(0.93f, 0.93f, 0.925f));
        g_rt->FillRectangle(D2D1::RectF(0, 0, XS, TB + v.height), g_br);
        g_br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.5f) : D2D1::ColorF(0, 0, 0, 0.10f));
        g_rt->FillRectangle(D2D1::RectF(XS - 0.5f, 0, XS, TB + v.height), g_br);
        g_rt->PushAxisAlignedClip(D2D1::RectF(0, TOOLH, XS, TB + v.height), D2D1_ANTIALIAS_MODE_ALIASED);
        int cur = CurrentPage();
        float y = TOOLH + 8 - g_thumbScroll, tw2 = XS - 62;
        for (int i = 0; i < (int)g_pages.size(); i++) {
            auto& pg = g_pages[i];
            float th2 = pg.w > 0 ? tw2 * pg.h / pg.w : tw2 * 1.3f;
            D2D1_RECT_F pr = D2D1::RectF((XS - tw2) / 2, y + 8, (XS + tw2) / 2, y + 8 + th2);
            D2D1_RECT_F cell = D2D1::RectF(14, y, XS - 14, pr.bottom + 26);
            if (cell.bottom > TOOLH && cell.top < TB + v.height) {
                if (i == cur) { g_br->SetColor(dark ? D2D1::ColorF(0.33f, 0.42f, 0.58f, 0.85f) : D2D1::ColorF(0.55f, 0.64f, 0.78f, 0.85f)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.left, cell.top, cell.right, pr.bottom + 8), 8, 8), g_br); }
                g_br->SetColor(D2D1::ColorF(0, 0, 0, 0.15f)); g_rt->FillRectangle(D2D1::RectF(pr.left + 0.5f, pr.top + 1, pr.right + 0.5f, pr.bottom + 1.5f), g_br);
                g_br->SetColor(D2D1::ColorF(1, 1, 1)); g_rt->FillRectangle(pr, g_br);
                if (pg.bmp) g_rt->DrawBitmap(pg.bmp, pr, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                else if (!pg.pending && pg.renderedW == 0) { pg.pending = true; Request(i, (int)std::lround(tw2 * 2 * sc)); }  // a miniatura também serve de primeira imagem
                wchar_t nb[16]; swprintf_s(nb, L"%d", i + 1);
                g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.75f) : D2D1::ColorF(0, 0, 0, 0.7f));
                g_rt->DrawTextW(nb, (UINT32)wcslen(nb), g_tfThumbNum ? g_tfThumbNum : g_tfSub, D2D1::RectF(cell.left, pr.bottom + 8, cell.right, pr.bottom + 24), g_br);
            }
            g_thumbHits.push_back({ i, cell });
            y = pr.bottom + 34;
        }
        g_thumbTotal = y + g_thumbScroll - TOOLH;
        g_rt->PopAxisAlignedClip();
    }
    if (g_pages.empty()) {
        const wchar_t* t = !g_error.empty() ? g_error.c_str() : g_path.empty() ? Tr(L"Abra uma imagem ou um PDF (Ctrl+O) ou solte um arquivo aqui.") : Tr(L"Abrindo…");
        g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.5f) : D2D1::ColorF(0, 0, 0, 0.5f));
        g_rt->DrawTextW(t, (UINT32)wcslen(t), g_tfEmpty, D2D1::RectF(20, TB, W - 20, TB + v.height), g_br);
    }
    // barra de ferramentas (a janela toda; com a coluna, ela fica atrás só do conteúdo)
    W = v.width + XS;
    g_br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f) : D2D1::ColorF(0.965f, 0.965f, 0.965f));
    g_rt->FillRectangle(D2D1::RectF(XS, 0, W, TOOLH), g_br);
    if (XS > 0) { g_br->SetColor(dark ? D2D1::ColorF(0.17f, 0.165f, 0.16f) : D2D1::ColorF(0.93f, 0.93f, 0.925f)); g_rt->FillRectangle(D2D1::RectF(0, 0, XS - 0.5f, TOOLH), g_br); }
    g_br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0, 0, 0, 0.14f));
    g_rt->DrawLine({ 0, TOOLH - 0.5f }, { W, TOOLH - 0.5f }, g_br, 1);
    std::wstring name = g_path.empty() ? Tr(L"Pré-Visualização") : g_path.substr(g_path.find_last_of(L"\\/") + 1);
    std::wstring sub;
    wchar_t b[96];
    if (g_pdf && !g_pages.empty()) { swprintf_s(b, Tr(L"Página %d de %d"), CurrentPage() + 1, (int)g_pages.size()); sub = b; }
    else if (!g_pages.empty()) { swprintf_s(b, L"%d × %d", (int)g_pages[0].w, (int)g_pages[0].h); sub = b; }
    if (!g_fit || !g_pages.empty()) { swprintf_s(b, L"   %d%%", (int)std::lround(Zoom() * 100)); if (!g_pages.empty()) sub += b; }
    // botões à direita
    g_btns = { { 0xE70F, L"Marcar", B_MARK }, { 0xE78C, L"Exportar", B_EXPORT }, { 0xE7AD, L"Girar", B_ROT }, { 0xE71F, L"Reduzir", B_OUT }, { 0xE8A3, L"Ampliar", B_IN }, { 0xE9A6, L"Ajustar", B_FIT }, { 0xE749, L"Imprimir", B_PRINT } };
    float bx = W - 14;
    if (g_unified) {  // macOS 27: grupos de vidro
        auto glass = [&](D2D1_RECT_F r) {
            float rad = (r.bottom - r.top) / 2;
            for (int k = 3; k >= 1; k--) { g_br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.10f : 0.035f)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k * 0.5f, r.top + k * 0.6f, r.right + k * 0.5f, r.bottom + k * 1.1f), rad + k, rad + k), g_br); }
            g_br->SetColor(dark ? D2D1::ColorF(0.24f, 0.24f, 0.245f, 1) : D2D1::ColorF(0.985f, 0.985f, 0.985f, 1));
            g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_br);
            g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.13f) : D2D1::ColorF(0, 0, 0, 0.09f));
            g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.25f, r.top + 0.25f, r.right - 0.25f, r.bottom - 0.25f), rad - 0.25f, rad - 0.25f), g_br, 0.5f);
        };
        // da direita para a esquerda: imprimir, exportar, girar, marcar (redondos) e a cápsula −/ajustar/+
        int order[4] = { 6, 1, 2, 0 };
        bx = W - 8;
        for (int k = 0; k < 4; k++) { auto& bt = g_btns[order[k]]; bt.r = D2D1::RectF(bx - 36, 8, bx, 44); glass(bt.r); bx -= 36 + 8; }
        D2D1_RECT_F cap = D2D1::RectF(bx - 108, 8, bx, 44);
        glass(cap);
        g_btns[3].r = D2D1::RectF(cap.left + 2, 8, cap.left + 36, 44);
        g_btns[5].r = D2D1::RectF(cap.left + 37, 8, cap.left + 71, 44);
        g_btns[4].r = D2D1::RectF(cap.left + 72, 8, cap.left + 106, 44);
        bx = cap.left - 8;
        if (g_pdf && g_pages.size() > 1) {  // lateral (com ⌄): no fim da coluna, ou depois dos três botões sem ela
            float sx = XS > 0 ? XS - 58 : 84;
            g_btns.push_back({ 0xE8A0, L"Barra Lateral", B_SIDE, D2D1::RectF(sx, 8, sx + 50, 44) });
            glass(g_btns.back().r);
        }
        for (int i = 0; i < (int)g_btns.size(); i++) {
            auto& bt = g_btns[i];
            if (i == g_hoverBtn || (bt.id == B_MARK && g_markup)) { g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.06f)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(bt.r, 18, 18), g_br); }
            bool off = (bt.id == B_ROT && g_pdf) || g_pages.empty();
            g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, off ? 0.3f : 0.88f) : D2D1::ColorF(0, 0, 0, off ? 0.25f : 0.8f));
            if (bt.id == B_SIDE) {  // o ícone da lateral e o ⌄
                float cx = bt.r.left + 19, cy = 26;
                g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8, cy - 6.5f, cx + 8, cy + 6.5f), 2.5f, 2.5f), g_br, 1.3f);
                g_rt->DrawLine({ cx - 3, cy - 6 }, { cx - 3, cy + 6 }, g_br, 1.3f);
                g_rt->DrawLine({ bt.r.right - 17, cy - 1.5f }, { bt.r.right - 14, cy + 1.5f }, g_br, 1.3f); g_rt->DrawLine({ bt.r.right - 14, cy + 1.5f }, { bt.r.right - 11, cy - 1.5f }, g_br, 1.3f);
                continue;
            }
            wchar_t g[2] = { bt.glyph, 0 };
            g_rt->DrawTextW(g, 1, g_tfIcon, bt.r, g_br);
        }
    } else
    for (int i = (int)g_btns.size() - 1; i >= 0; i--) {
        auto& bt = g_btns[i];
        bt.r = D2D1::RectF(bx - 34, TOOLH / 2 - 15, bx, TOOLH / 2 + 15);
        bx -= 38;
        if (i == g_hoverBtn || (bt.id == B_MARK && g_markup)) {
            g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.07f));
            g_rt->FillRoundedRectangle(D2D1::RoundedRect(bt.r, 7, 7), g_br);
        }
        bool off = (bt.id == B_ROT && g_pdf) || g_pages.empty();
        g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, off ? 0.3f : 0.82f) : D2D1::ColorF(0, 0, 0, off ? 0.25f : 0.72f));
        wchar_t g[2] = { bt.glyph, 0 };
        g_rt->DrawTextW(g, 1, g_tfIcon, bt.r, g_br);
    }
    g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.88f));
    float tx = g_unified ? (XS > 0 ? XS + 12.f : g_pdf && g_pages.size() > 1 ? 146.f : 96.f) : 16.f;  // macOS 27: depois dos três botões (ou da coluna)
    g_rt->DrawTextW(name.c_str(), (UINT32)name.size(), g_tfTitle, D2D1::RectF(tx, 8, bx - 10, 28), g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    g_br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.5f) : D2D1::ColorF(0, 0, 0, 0.5f));
    g_rt->DrawTextW(sub.c_str(), (UINT32)sub.size(), g_tfSub, D2D1::RectF(tx, 28, bx - 10, 46), g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    PaintMarkupBar(dark, W);
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) DropRes();
}

void Scroll(float dx, float dy) { g_sx += dx; g_sy += dy; ClampScroll(); InvalidateRect(g_wnd, nullptr, FALSE); }

void GoPage(int i) {
    float tw, th; auto pl = Layout(tw, th);
    if (pl.empty()) return;
    i = std::clamp(i, 0, (int)pl.size() - 1);
    g_sy = pl[i].r.top - MARGIN / 2;
    ClampScroll();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void OpenDialog() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"Imagens e PDF", L"*.pdf;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.heic;*.ico" }, { L"Todos", L"*.*" } };
    d->SetFileTypes(2, f);
    if (SUCCEEDED(d->Show(g_wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { Open(p); CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
}

void Action(int id) {
    if (id == B_SIDE) { g_side = !g_side; ClampScroll(); InvalidateRect(g_wnd, nullptr, FALSE); return; }
    if (g_pages.empty()) return;
    switch (id) {
    case B_ROT: if (!g_pdf) { g_rot = (g_rot + 3) % 4; ClampScroll(); InvalidateRect(g_wnd, nullptr, FALSE); } break;
    case B_OUT: SetZoom(Zoom() / 1.25f); break;
    case B_IN: SetZoom(Zoom() * 1.25f); break;
    case B_FIT: g_fit = true; g_sx = g_sy = 0; ClampScroll(); InvalidateRect(g_wnd, nullptr, FALSE); break;
    case B_PRINT: ShellExecuteW(g_wnd, L"print", g_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case B_MARK: SetMarkup(!g_markup); break;
    case B_EXPORT: Export(); break;
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_unified && m == WM_NCCALCSIZE && w) {  // macOS 27: sem a barra de cima do Windows (as bordas ficam)
        auto* p = (NCCALCSIZE_PARAMS*)l;
        LONG top = p->rgrc[0].top;
        LRESULT r = DefWindowProcW(h, m, w, l);
        p->rgrc[0].top = top;
        if (IsZoomed(h)) { UINT dpi = GetDpiForWindow(h); p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi); }
        return r;
    }
    if (g_unified && m == WM_NCHITTEST) {  // arrasta pela barra, menos pelos botões
        LRESULT r = DefWindowProcW(h, m, w, l);
        if (r != HTCLIENT) return r;
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        float s = Sc(), x = p.x / s, y = p.y / s;
        if (!IsZoomed(h) && p.y < (int)(5 * s)) return HTTOP;
        if (y >= TOOLH || x < 84) return HTCLIENT;
        for (auto& b : g_btns) if (x >= b.r.left && x < b.r.right && y >= b.r.top && y < b.r.bottom) return HTCLIENT;
        return HTCAPTION;
    }
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); Paint(); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if (g_hrt) g_hrt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l))); ClampScroll(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hrt) g_hrt->SetDpi((float)HIWORD(w), (float)HIWORD(w));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PV_ASK: OpenDialog(); return 0;
    case WM_PV_INFO: {
        auto* info = (InfoResult*)l;
        if (info->gen == g_gen) {
            g_error = info->error;
            for (auto& s : info->sizes) { Page p; p.w = s.first; p.h = s.second; g_pages.push_back(p); }
            InvalidateRect(h, nullptr, FALSE);
        }
        delete info;
        return 0;
    }
    case WM_PV_PAGE: {
        auto* r = (PageResult*)l;
        if (r->gen == g_gen && r->page < (int)g_pages.size() && g_rt) {
            Page& p = g_pages[r->page];
            p.pending = false;
            if (!r->px.empty()) {
                ID2D1Bitmap* b = nullptr;
                g_rt->CreateBitmap(D2D1::SizeU(r->w, r->h), r->px.data(), r->w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
                if (b) { if (p.bmp) p.bmp->Release(); p.bmp = b; p.renderedW = r->width; p.px = std::move(r->px); p.pw = r->w; p.ph = r->h; }
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        delete r;
        return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        float d = GET_WHEEL_DELTA_WPARAM(w) / 120.f;
        if (m == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)) {
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
            SetZoom(Zoom() * std::pow(1.15f, d), p);
        } else if (POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p), p.x / Sc() < SideW()) {  // a coluna rola sozinha
            g_thumbScroll = std::clamp(g_thumbScroll - d * 80, 0.f, std::max(0.f, g_thumbTotal - ViewSize().height));
            InvalidateRect(h, nullptr, FALSE);
        } else if (m == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(w) & MK_SHIFT)) Scroll(m == WM_MOUSEHWHEEL ? d * 60 : -d * 60, 0);
        else Scroll(0, -d * 80);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float sc = Sc(), x = GET_X_LPARAM(l) / sc, y = GET_Y_LPARAM(l) / sc;
        for (auto& b : g_btns) if (x >= b.r.left && x < b.r.right && y >= b.r.top && y < b.r.bottom) { Action(b.id); return 0; }
        if (x < SideW() && y > TOOLH) { for (auto& th : g_thumbHits) if (y >= th.second.top && y < th.second.bottom) { GoPage(th.first); return 0; } return 0; }
        for (auto& b : g_mbtns) if (x >= b.r.left && x < b.r.right && y >= b.r.top && y < b.r.bottom) {
            CommitText();
            if (b.kind == 0) { if (g_tool == T_SIGN && b.arg != T_SIGN) EndSignCapture(); g_tool = b.arg; }
            else if (b.kind == 1) g_color = b.arg;
            else if (b.kind == 2) { if (!g_anns.empty()) { g_anns.pop_back(); g_dirty = true; } }
            else if (b.kind == 3) SetMarkup(false);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g_markup && y > TB) {
            CommitText();
            D2D1_POINT_2F pt; int pg = PageAt(x, y, pt);
            if (pg < 0) return 0;
            if (g_tool == T_TEXT) { StartText(pg, pt, x, y - 10); return 0; }
            if (g_tool == T_SIGN && !g_sign.empty() && !g_signCapture) {  // põe a assinatura guardada (180 unidades de largura)
                float sw = 180 * (g_pdf ? 1.f : std::max(1.f, g_pages[pg].w / 900.f));
                for (auto& s : g_sign) { Ann a{ pg, T_SIGN, COLORS[0] }; for (auto& q : s) a.pts.push_back({ pt.x + q.x * sw, pt.y + q.y * sw - sw * 0.15f }); g_anns.push_back(a); }
                g_dirty = true; InvalidateRect(h, nullptr, FALSE); return 0;
            }
            if (g_tool == T_SIGN && !g_signCapture) { g_signCapture = true; g_signFrom = g_anns.size(); }
            g_cur = Ann{ pg, g_tool, g_tool == T_SIGN ? COLORS[0] : g_tool == T_HILITE && g_color == 0 ? COLORS[3] : COLORS[g_color], { pt } };
            if (g_tool != T_PEN && g_tool != T_HILITE && g_tool != T_SIGN) g_cur.pts.push_back(pt);
            g_drawing = true; SetCapture(h);
            return 0;
        }
        if (y > TB) { g_drag = true; g_dragAt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; g_dragSx = g_sx; g_dragSy = g_sy; SetCapture(h); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        float sc = Sc(), x = GET_X_LPARAM(l) / sc, y = GET_Y_LPARAM(l) / sc;
        if (g_drawing) {
            D2D1_RECT_F r; float z = Zoom();
            if (PageRect(g_cur.page, r)) {
                D2D1_POINT_2F pt{ std::clamp((x - r.left) / z, 0.f, g_pages[g_cur.page].w), std::clamp((y - r.top) / z, 0.f, g_pages[g_cur.page].h) };
                if (g_cur.tool == T_PEN || g_cur.tool == T_HILITE || g_cur.tool == T_SIGN) g_cur.pts.push_back(pt); else g_cur.pts.back() = pt;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g_drag) { g_sx = g_dragSx - (GET_X_LPARAM(l) - g_dragAt.x) / sc; g_sy = g_dragSy - (GET_Y_LPARAM(l) - g_dragAt.y) / sc; ClampScroll(); InvalidateRect(h, nullptr, FALSE); return 0; }
        int hv = -1;
        for (int i = 0; i < (int)g_btns.size(); i++) { auto& b = g_btns[i]; if (x >= b.r.left && x < b.r.right && y >= b.r.top && y < b.r.bottom) hv = i; }
        if (hv != g_hoverBtn) { g_hoverBtn = hv; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hoverBtn >= 0) { g_hoverBtn = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP:
        if (g_drawing) {
            g_drawing = false; ReleaseCapture();
            if (g_cur.tool == T_CROP) { if (g_cur.pts.size() >= 2) ApplyCrop(g_cur.pts[0], g_cur.pts.back()); }
            else { g_anns.push_back(g_cur); g_dirty = true; }
            g_cur = Ann{};
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g_drag) { g_drag = false; ReleaseCapture(); }
        return 0;
    case WM_RBUTTONUP:  // assinatura: botão direito desenha outra
        if (g_markup && g_tool == T_SIGN) { g_sign.clear(); g_signCapture = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_APP + 9: if (g_text && GetFocus() != g_text) CommitText(); return 0;
    case WM_LBUTTONDBLCLK: {  // duplo clique na página: alterna entre ajustar e tamanho real
        if (GET_Y_LPARAM(l) / Sc() <= TB || g_markup) return 0;
        if (g_fit) SetZoom(1, { GET_X_LPARAM(l), GET_Y_LPARAM(l) }); else Action(B_FIT);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            SetCursor(LoadCursorW(nullptr, p.y / Sc() > TB && !g_pages.empty() ? (g_markup ? (g_tool == T_TEXT ? IDC_IBEAM : IDC_CROSS) : IDC_HAND) : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        D2D1_SIZE_F v = ViewSize();
        if (ctrl) switch (w) {
            case 'O': OpenDialog(); return 0;
            case 'S': Export(); return 0;
            case 'Z': if (g_markup && !g_anns.empty()) { g_anns.pop_back(); g_dirty = true; InvalidateRect(h, nullptr, FALSE); } return 0;
            case 'A': if (GetKeyState(VK_SHIFT) < 0) { SetMarkup(!g_markup); return 0; } break;
            case 'W': PostMessageW(h, WM_CLOSE, 0, 0); return 0;
            case 'P': Action(B_PRINT); return 0;
            case 'R': if (!g_pdf) { g_rot = (g_rot + 1) % 4; ClampScroll(); InvalidateRect(h, nullptr, FALSE); } return 0;
            case 'L': Action(B_ROT); return 0;
            case VK_OEM_PLUS: case VK_ADD: Action(B_IN); return 0;
            case VK_OEM_MINUS: case VK_SUBTRACT: Action(B_OUT); return 0;
            case '0': case VK_NUMPAD0: SetZoom(1); return 0;
            case '9': Action(B_FIT); return 0;
        }
        switch (w) {
        case VK_LEFT: if (g_pdf) Scroll(-60, 0); else Neighbor(-1); return 0;
        case VK_RIGHT: if (g_pdf) Scroll(60, 0); else Neighbor(1); return 0;
        case VK_UP: Scroll(0, -60); return 0;
        case VK_DOWN: Scroll(0, 60); return 0;
        case VK_SPACE: Scroll(0, GetKeyState(VK_SHIFT) < 0 ? -v.height * 0.9f : v.height * 0.9f); return 0;
        case VK_PRIOR: if (g_pdf) GoPage(CurrentPage() - 1); else Scroll(0, -v.height * 0.9f); return 0;
        case VK_NEXT: if (g_pdf) GoPage(CurrentPage() + 1); else Scroll(0, v.height * 0.9f); return 0;
        case VK_HOME: GoPage(0); return 0;
        case VK_END: GoPage((int)g_pages.size() - 1); return 0;
        case VK_ESCAPE: if (!g_fit) Action(B_FIT); return 0;
        }
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t p[MAX_PATH * 2] = {};
        if (DragQueryFileW((HDROP)w, 0, p, MAX_PATH * 2)) Open(p);
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_CLOSE:
        if (g_dirty && !g_anns.empty()) {
            int r = MessageBoxW(h, Tr(L"Exportar a marcação antes de fechar?"), Tr(L"Pré-Visualização"), MB_YESNOCANCEL | MB_ICONWARNING);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) { Export(); if (g_dirty) return 0; }
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: {
        { std::lock_guard<std::mutex> lk(g_mx); g_quit = true; g_cv.notify_all(); }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

IDWriteTextFormat* Fmt(const wchar_t* fam, float size, DWRITE_FONT_WEIGHT wt, DWRITE_TEXT_ALIGNMENT a, DWRITE_PARAGRAPH_ALIGNMENT pa) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(fam, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (f) { f->SetTextAlignment(a); f->SetParagraphAlignment(pa); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    return f;
}

bool HasFont(const wchar_t* name) {
    IDWriteFontCollection* c = nullptr; g_dw->GetSystemFontCollection(&c);
    UINT32 i; BOOL ex = FALSE;
    if (c) { c->FindFamilyName(name, &i, &ex); c->Release(); }
    return ex != FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (!g_d2d || !g_dw || !g_wic) return 1;
    const wchar_t* fam = HasFont(L"SF Pro Text") ? L"SF Pro Text" : HasFont(L"Inter") ? L"Inter" : L"Segoe UI";
    g_tfTitle = Fmt(fam, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_tfSub = Fmt(fam, 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_tfThumbNum = Fmt(fam, 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_tfEmpty = Fmt(fam, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_tfEmpty) g_tfEmpty->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (g_tfTitle) { DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; IDWriteInlineObject* e = nullptr; g_dw->CreateEllipsisTrimmingSign(g_tfTitle, &e); g_tfTitle->SetTrimming(&t, e); if (e) e->Release(); }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"PreVisualizacaoJanela";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    // tamanho: 70% x 80% da área de trabalho, no meio
    MONITORINFO mi{ sizeof(mi) }; POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    int W = std::min(1400L, (mi.rcWork.right - mi.rcWork.left) * 7 / 10), H = (mi.rcWork.bottom - mi.rcWork.top) * 8 / 10;
    bool mac = barramac::Ligar();
    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, Tr(L"Pré-Visualização"), WS_OVERLAPPEDWINDOW,
                            mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2, W, H,
                            nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return 1;
    BOOL dark = Dark(); DwmSetWindowAttribute(g_wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (mac && semaforo::Aqua()) {  // macOS 27: barra unificada (a DLL só desenha os três botões)
        g_unified = true;
        SetPropW(g_wnd, L"WacOSBarraUnificada", (HANDLE)(INT_PTR)52);
        SetWindowPos(g_wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    std::thread(PdfThread).detach();
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; i++) if (!wcscmp(argv[i], L"--foto")) g_fotoPath = argv[i + 1];
    if (!g_fotoPath.empty()) {  // teste: salva a foto da janela daqui a pouco e fecha
        if (g_rt) { g_rt->Release(); g_rt = nullptr; g_hrt = nullptr; }
        if (g_br) { g_br->Release(); g_br = nullptr; }
        CreateRes();
        SetTimer(nullptr, 0, 2500, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            Paint();
            IWICImagingFactory* wic = nullptr;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
            IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
            UINT w = 0, hh = 0; if (g_fotoBmp) g_fotoBmp->GetSize(&w, &hh);
            if (g_fotoBmp && wic && SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(g_fotoPath.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr))) {
                fr->SetSize(w, hh); WICPixelFormatGUID f = GUID_WICPixelFormat32bppPBGRA; fr->SetPixelFormat(&f);
                fr->WriteSource(g_fotoBmp, nullptr); fr->Commit(); enc->Commit();
            }
            if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wic) wic->Release();
            DestroyWindow(g_wnd);
        });
    }
    if (argc > 1 && argv[1][0] && wcscmp(argv[1], L"--foto")) Open(argv[1]); else if (g_fotoPath.empty()) PostMessageW(g_wnd, WM_PV_ASK, 0, 0);  // sem arquivo: pergunta qual, como o Mac
    if (argv) LocalFree(argv);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
