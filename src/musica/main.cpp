// Música (Music do Mac) para os arquivos da pasta Música: à esquerda Recentes, Artistas, Álbuns e Músicas; à
// direita a lista (tabela de músicas, grade de álbuns com a capa, artistas com as músicas deles). No alto o
// "visor" do Mac: ⏮ ⏯ ⏭, aleatório e repetir, a capa, nome e artista, a barra de progresso (clique leva até lá)
// e o volume. Quem toca é o MediaPlayer do Windows: a música aparece no "Tocando Agora" da Central de Controle e
// nas teclas de mídia. Duplo clique toca a partir dali; espaço pausa; Ctrl+→ / Ctrl+← próxima/anterior.
#include "../comum/app2d.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <map>
#include <mutex>
#include <thread>
#include <random>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace app2d;
namespace wmp = winrt::Windows::Media::Playback;
namespace wmc = winrt::Windows::Media::Core;

namespace {

float SB = 200, TB = 70, ROWH = 28;  // macOS 27: barra de 52 (o player vai para baixo)
const UINT WM_SCAN = WM_APP + 3, WM_ART = WM_APP + 4, WM_ENDED = WM_APP + 6;

struct Song { std::wstring path, title, artist, album; int track = 0; double secs = 0; FILETIME added{}; };
std::vector<Song> g_songs;
std::mutex g_mx;
bool g_scanning = true;
int g_view = 3;  // 0 recentes, 1 artistas, 2 álbuns, 3 músicas
std::wstring g_artistSel, g_albumSel;
float g_scroll = 0;
int g_selRow = -1;
std::vector<int> g_rows;         // músicas à vista (índices em g_songs)
std::vector<int> g_queue; int g_pos = -1;
bool g_shuffle = false; int g_repeat = 0;  // 0 não, 1 tudo, 2 uma
wmp::MediaPlayer g_player{ nullptr };
std::map<std::wstring, ID2D1Bitmap*> g_art;   // por álbum
std::map<std::wstring, bool> g_artAsked;
IDWriteTextFormat *g_tfSide, *g_tfHead, *g_tfRow, *g_tfRowR, *g_tfSmall, *g_tfLcd, *g_tfLcdSub, *g_tfIcon, *g_tfIconBig, *g_tfAlbum;
IDWriteTextFormat *g_tfSec, *g_tfSideB, *g_tfTitleG, *g_tfIconS, *g_tfLcdL, *g_tfLcdSubL;  // macOS 27
float g_volume = 0.8f;

std::wstring Prop(IPropertyStore* ps, const PROPERTYKEY& k) {
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(k, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        else if (v.vt == (VT_VECTOR | VT_LPWSTR) && v.calpwstr.cElems) out = v.calpwstr.pElems[0];
    }
    PropVariantClear(&v);
    return out;
}
void Scan() {
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        PWSTR p = nullptr; std::wstring root;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &p))) root = p;
        CoTaskMemFree(p);
        std::vector<Song> v;
        std::vector<std::wstring> dirs{ root };
        while (!dirs.empty() && !root.empty()) {
            std::wstring d = dirs.back(); dirs.pop_back();
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                std::wstring path = d + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs.push_back(path); continue; }
                std::wstring x = PathFindExtensionW(fd.cFileName); for (auto& c : x) c = towlower(c);
                if (x != L".mp3" && x != L".m4a" && x != L".flac" && x != L".wav" && x != L".aac" && x != L".ogg" && x != L".wma" && x != L".aif" && x != L".aiff" && x != L".opus") continue;
                Song s; s.path = path; s.added = fd.ftCreationTime;
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps)))) {
                    s.title = Prop(ps, PKEY_Title); s.artist = Prop(ps, PKEY_Music_Artist); if (s.artist.empty()) s.artist = Prop(ps, PKEY_Music_AlbumArtist);
                    s.album = Prop(ps, PKEY_Music_AlbumTitle);
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Media_Duration, &pv)) && pv.vt == VT_UI8) s.secs = pv.uhVal.QuadPart / 1e7;
                    PropVariantClear(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Music_TrackNumber, &pv)) && pv.vt == VT_UI4) s.track = pv.ulVal;
                    PropVariantClear(&pv);
                    ps->Release();
                }
                if (s.title.empty()) { s.title = fd.cFileName; s.title = s.title.substr(0, s.title.find_last_of(L'.')); }
                if (s.artist.empty()) s.artist = Tr(L"Artista Desconhecido");
                if (s.album.empty()) s.album = Tr(L"Álbum Desconhecido");
                v.push_back(s);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        auto lo = [](std::wstring s) { for (auto& c : s) c = towlower(c); return s; };
        std::sort(v.begin(), v.end(), [&](const Song& a, const Song& b) { return lo(a.title) < lo(b.title); });
        auto* nv = new std::vector<Song>(std::move(v));
        if (!PostMessageW(wnd, WM_SCAN, 0, (LPARAM)nv)) delete nv;
    }).detach();
}

// capa do álbum: a miniatura do primeiro arquivo dele (a capa embutida)
struct ArtData { std::wstring album; UINT w, h; std::vector<BYTE> px; };
void AskArt(const std::wstring& album, const std::wstring& file) {
    if (g_artAsked[album]) return;
    g_artAsked[album] = true;
    HWND hw = wnd;
    std::thread([album, file, hw] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* d = new ArtData{ album };
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            HBITMAP hb = nullptr;
            if (SUCCEEDED(f->GetImage(SIZE{ 300, 300 }, SIIGBF_THUMBNAILONLY, &hb)) && hb) {
                BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
                d->w = bm.bmWidth; d->h = std::abs(bm.bmHeight);
                BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)d->w, -(LONG)d->h, 1, 32, BI_RGB };
                d->px.resize((size_t)d->w * d->h * 4);
                HDC dc = GetDC(nullptr); GetDIBits(dc, hb, 0, d->h, d->px.data(), &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, dc);
                for (size_t i = 3; i < d->px.size(); i += 4) d->px[i] = 255;
                DeleteObject(hb);
            }
            f->Release();
        }
        CoUninitialize();
        if (!PostMessageW(hw, WM_ART, 0, (LPARAM)d)) delete d;
    }).detach();
}
ID2D1Bitmap* Art(const Song& s) { auto it = g_art.find(s.album); if (it != g_art.end()) return it->second; AskArt(s.album, s.path); return nullptr; }

// ---- reprodução
void PlayIndex(int pos) {
    if (pos < 0 || pos >= (int)g_queue.size()) return;
    g_pos = pos;
    const Song& s = g_songs[g_queue[pos]];
    try {
        g_player.Source(wmc::MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri(L"file:///" + s.path)));
        g_player.Play();
    } catch (...) {}
    SetWindowTextW(wnd, (s.title + L" — " + s.artist).c_str());
    InvalidateRect(wnd, nullptr, FALSE);
}
void PlayFrom(const std::vector<int>& list, int start) {
    g_queue = list;
    if (g_shuffle) { int first = list[start]; std::shuffle(g_queue.begin(), g_queue.end(), std::mt19937{ std::random_device{}() }); auto it = std::find(g_queue.begin(), g_queue.end(), first); std::iter_swap(g_queue.begin(), it); start = 0; }
    PlayIndex(start);
}
bool Playing() { try { return g_player && g_player.PlaybackSession().PlaybackState() == wmp::MediaPlaybackState::Playing; } catch (...) { return false; } }
void Toggle() {
    if (g_pos < 0) { if (!g_rows.empty()) PlayFrom(g_rows, std::max(0, g_selRow)); return; }
    if (Playing()) g_player.Pause(); else g_player.Play();
    InvalidateRect(wnd, nullptr, FALSE);
}
void Next(int d) {
    if (g_queue.empty()) return;
    int p = g_pos + d;
    if (p >= (int)g_queue.size()) { if (g_repeat == 1) p = 0; else { g_player.Pause(); return; } }
    if (p < 0) p = 0;
    PlayIndex(p);
}
// um arquivo aberto de fora (Finder, mesa, Spotlight): toca já, e a fila vira a pasta dele
Song ReadSong(const std::wstring& path) {
    Song s; s.path = path;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps)))) {
        s.title = Prop(ps, PKEY_Title); s.artist = Prop(ps, PKEY_Music_Artist); s.album = Prop(ps, PKEY_Music_AlbumTitle);
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Media_Duration, &pv)) && pv.vt == VT_UI8) s.secs = pv.uhVal.QuadPart / 1e7;
        PropVariantClear(&pv); ps->Release();
    }
    if (s.title.empty()) { s.title = path.substr(path.find_last_of(L'\\') + 1); s.title = s.title.substr(0, s.title.find_last_of(L'.')); }
    if (s.artist.empty()) s.artist = Tr(L"Artista Desconhecido");
    if (s.album.empty()) s.album = Tr(L"Álbum Desconhecido");
    return s;
}
void PlayFile(const std::wstring& path) {
    if (path.empty()) return;
    int idx = -1;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (int i = 0; i < (int)g_songs.size(); i++) if (!_wcsicmp(g_songs[i].path.c_str(), path.c_str())) idx = i;
        if (idx < 0) { g_songs.push_back(ReadSong(path)); idx = (int)g_songs.size() - 1; }  // fora da pasta Música: entra só nesta sessão
    }
    // fila: as músicas da mesma pasta, a partir desta
    std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
    std::vector<int> q;
    for (int i = 0; i < (int)g_songs.size(); i++) if (!_wcsicmp(g_songs[i].path.substr(0, g_songs[i].path.find_last_of(L'\\')).c_str(), dir.c_str())) q.push_back(i);
    int start = (int)(std::find(q.begin(), q.end(), idx) - q.begin());
    if (start >= (int)q.size()) { q = { idx }; start = 0; }
    g_queue = q;
    PlayIndex(start);
}

double PosSecs() { try { return g_player.PlaybackSession().Position().count() / 1e7; } catch (...) { return 0; } }
double LenSecs() { try { return g_player.PlaybackSession().NaturalDuration().count() / 1e7; } catch (...) { return 0; } }
std::wstring Mmss(double s) { int t = (int)s; wchar_t b[16]; swprintf_s(b, L"%d:%02d", t / 60, t % 60); return b; }

// ---- listas
// macOS 27: a busca da barra filtra (título, artista ou álbum) e o botão de filtro escolhe a ordem das músicas
std::wstring g_search;
int g_sortBy;  // 0 padrão, 1 título, 2 artista, 3 álbum
bool Matches(const std::wstring& a) {
    if (g_search.empty()) return true;
    std::wstring x = a, q = g_search;
    for (auto& c : x) c = towlower(c);
    for (auto& c : q) c = towlower(c);
    return x.find(q) != std::wstring::npos;
}
std::vector<std::wstring> Albums() {
    std::vector<std::wstring> v;
    std::map<std::wstring, bool> seen;
    for (auto& s : g_songs) if (!seen[s.album] && (g_artistSel.empty() || s.artist == g_artistSel) && (Matches(s.album) || Matches(s.artist))) { seen[s.album] = true; v.push_back(s.album); }
    std::sort(v.begin(), v.end());
    return v;
}
std::vector<std::wstring> Artists() {
    std::vector<std::wstring> v; std::map<std::wstring, bool> seen;
    for (auto& s : g_songs) if (!seen[s.artist] && Matches(s.artist)) { seen[s.artist] = true; v.push_back(s.artist); }
    std::sort(v.begin(), v.end());
    return v;
}
void BuildRows() {
    g_rows.clear();
    for (int i = 0; i < (int)g_songs.size(); i++) {
        auto& s = g_songs[i];
        if (g_view == 1 && !g_artistSel.empty() && s.artist != g_artistSel) continue;
        if (g_view == 2 && !g_albumSel.empty() && s.album != g_albumSel) continue;
        if (!Matches(s.title) && !Matches(s.artist) && !Matches(s.album)) continue;
        g_rows.push_back(i);
    }
    if (g_sortBy && (g_view == 3 || g_view == 0)) std::stable_sort(g_rows.begin(), g_rows.end(), [](int a, int b) {
        auto& x = g_songs[a]; auto& y = g_songs[b];
        const std::wstring& p = g_sortBy == 1 ? x.title : g_sortBy == 2 ? x.artist : x.album;
        const std::wstring& q = g_sortBy == 1 ? y.title : g_sortBy == 2 ? y.artist : y.album;
        return _wcsicmp(p.c_str(), q.c_str()) < 0;
    });
    if (g_view == 0) std::sort(g_rows.begin(), g_rows.end(), [](int a, int b) { return CompareFileTime(&g_songs[a].added, &g_songs[b].added) > 0; });
    if (g_view == 2 && !g_albumSel.empty()) std::sort(g_rows.begin(), g_rows.end(), [](int a, int b) { return g_songs[a].track < g_songs[b].track; });
    if (g_view == 0 && g_rows.size() > 100) g_rows.resize(100);
}

struct Hit { int kind, arg; D2D1_RECT_F r; };  // 1 vista, 2 linha, 3 álbum, 4 artista, 5 botões do visor, 6 progresso, 7 volume, 8 voltar
std::vector<Hit> g_hits;

// ---- macOS 27 (captura Music do 512 Pixels, 2x, medida em pt): lateral de 200, cheia, com os três botões em cima;
// seções em cinza ("Biblioteca") e linhas de 32 com o ícone vermelho a 37 e o nome a 55 (a escolhida numa pílula
// cinza de 10 a 190). O título da vista fica na barra, em negrito, a 20 do conteúdo, com a divisa a 52; à direita o
// botão redondo de filtro e a busca em cápsula. O player é uma cápsula de vidro de 56 flutuando a 18 do fundo, no
// meio do conteúdo: aleatório, anterior, tocar, próxima e repetir à esquerda, o que toca no meio, fila e volume à
// direita.
void PaintSideGolden(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray, D2D1_COLOR_F pink) {
    Fill(D2D1::RectF(0, 0, SB, sz.height), SidebarColor());
    Fill(D2D1::RectF(SB - 0.5f, 0, SB, sz.height), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.08f));
    Text(Tr(L"Biblioteca"), g_tfSec, D2D1::RectF(16, 58, SB, 74), gray);
    const wchar_t* titles[] = { L"Recentes", L"Artistas", L"Álbuns", L"Músicas" };
    wchar_t icons[] = { 0xE823, 0xE77B, 0xE93C, 0xEC4F };
    for (int i = 0; i < 4; i++) {
        float cy = 92 + i * 32.f;
        D2D1_RECT_F r = D2D1::RectF(10, cy - 16, SB - 10, cy + 16);
        bool on = i == g_view;
        if (on) Fill(r, dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.07f), 9);
        wchar_t g[2] = { icons[i], 0 };
        Text(g, g_tfIcon, D2D1::RectF(26, r.top, 48, r.bottom), pink);
        Text(i == 3 ? TrPick(L"Músicas", L"Songs") : Tr(titles[i]), on ? g_tfSideB : g_tfSide, D2D1::RectF(55, r.top, r.right - 6, r.bottom), text);
        g_hits.push_back({ 1, i, r });
    }
}

void PaintBarGolden(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray, const std::wstring& head) {
    Fill(D2D1::RectF(SB, 0, sz.width, TB), dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    Fill(D2D1::RectF(SB, TB - 0.5f, sz.width, TB), dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.09f));
    Text(head, g_tfTitleG, D2D1::RectF(SB + 20, 0, sz.width - 240, TB), dark ? Rgb(255, 255, 255, 0.85f) : Rgb(0, 0, 0, 0.75f));
    // busca em cápsula e o filtro
    D2D1_RECT_F f = D2D1::RectF(sz.width - 8 - 141, 8, sz.width - 8, 44);
    GlassField(f, dark);
    IconSearch({ f.left + 18, 26 }, gray, 0.85f);
    std::wstring ph = g_view == 2 ? Tr(L"Buscar em Álbuns") : g_view == 1 ? Tr(L"Buscar em Artistas") : Tr(L"Buscar em Músicas");
    Text(g_search.empty() ? ph : g_search, g_tfSmall, D2D1::RectF(f.left + 30, f.top, f.right - 10, f.bottom), g_search.empty() ? gray : text);
    g_hits.push_back({ 20, 0, f });
    D2D1_RECT_F b = D2D1::RectF(f.left - 8 - 36, 8, f.left - 8, 44);
    GlassButton(b, dark);
    br->SetColor(text);
    for (int i = 0; i < 3; i++) { float w = 7.f - i * 2; rt->DrawLine({ (b.left + b.right) / 2 - w, 21.f + i * 5 }, { (b.left + b.right) / 2 + w, 21.f + i * 5 }, br, 1.4f); }
    BarButton(b); BarButton(f);
    g_hits.push_back({ 21, 0, b });
}

void PaintPlayerGolden(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray, D2D1_COLOR_F pink) {
    float cx = (SB + sz.width) / 2, w = std::min(702.f, sz.width - SB - 40);
    D2D1_RECT_F cap = D2D1::RectF(cx - w / 2, sz.height - 18 - 56, cx + w / 2, sz.height - 18);
    for (int k = 4; k >= 1; k--) {  // sombra
        br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.12f : 0.035f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cap.left - k, cap.top + k * 0.8f, cap.right + k, cap.bottom + k * 1.6f), 28.f + k, 28.f + k), br);
    }
    Fill(cap, dark ? Rgb(44, 44, 46, 0.96f) : Rgb(252, 252, 252, 0.97f), 28);
    br->SetColor(dark ? Rgb(255, 255, 255, 0.12f) : Rgb(0, 0, 0, 0.07f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cap.left + 0.25f, cap.top + 0.25f, cap.right - 0.25f, cap.bottom - 0.25f), 27.75f, 27.75f), br, 0.5f);
    float cy = (cap.top + cap.bottom) / 2;
    bool has = g_pos >= 0 && g_pos < (int)g_queue.size();
    D2D1_COLOR_F off = dark ? Rgb(255, 255, 255, 0.35f) : Rgb(0, 0, 0, 0.28f);
    struct B { int id; wchar_t g; float x; bool big; bool on; } btns[] = {
        { 0, 0xE8B1, 24, false, g_shuffle }, { 1, 0xE892, 56, false, false }, { 2, Playing() ? (wchar_t)0xE769 : (wchar_t)0xE768, 92, true, false },
        { 3, 0xE893, 128, false, false }, { 4, g_repeat == 2 ? (wchar_t)0xE8ED : (wchar_t)0xE8EE, 160, false, g_repeat != 0 } };
    for (auto& b : btns) {
        D2D1_RECT_F r = D2D1::RectF(cap.left + b.x - 15, cy - 15, cap.left + b.x + 15, cy + 15);
        wchar_t g[2] = { b.g, 0 };
        Text(g, b.big ? g_tfIconBig : g_tfIconS, r, b.on ? pink : has || b.id == 2 ? text : off);
        g_hits.push_back({ 5, b.id, r });
    }
    // o que toca: capa, nome e artista, e a barra de progresso
    D2D1_RECT_F mid = D2D1::RectF(cap.left + 190, cap.top + 6, cap.right - 150, cap.bottom - 6);
    if (has) {
        const Song& s = g_songs[g_queue[g_pos]];
        D2D1_RECT_F art = D2D1::RectF(mid.left, cy - 20, mid.left + 40, cy + 20);
        if (ID2D1Bitmap* b = Art(s)) rt->DrawBitmap(b, art); else Fill(art, dark ? Rgb(72, 72, 74) : Rgb(220, 220, 225), 6);
        float tx = art.right + 10;
        Text(s.title, g_tfLcdL, D2D1::RectF(tx, cy - 19, mid.right, cy - 3), text);
        Text(s.artist, g_tfLcdSubL, D2D1::RectF(tx, cy - 3, mid.right, cy + 11), gray);
        double pos = PosSecs(), len = std::max(LenSecs(), s.secs);
        D2D1_RECT_F bar = D2D1::RectF(tx, cy + 15, mid.right, cy + 17.5f);
        Fill(bar, dark ? Rgb(255, 255, 255, 0.15f) : Rgb(0, 0, 0, 0.10f), 1.25f);
        if (len > 0) Fill(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * (float)std::clamp(pos / len, 0.0, 1.0), bar.bottom), gray, 1.25f);
        g_hits.push_back({ 6, 0, D2D1::RectF(bar.left, bar.top - 6, bar.right, bar.bottom + 6) });
    } else {
        wchar_t g[2] = { 0xEC4F, 0 };
        Text(g, g_tfIconBig, mid, off);
    }
    // direita: fila e volume
    wchar_t q[2] = { 0xE8FD, 0 };
    Text(q, g_tfIconS, D2D1::RectF(cap.right - 140, cy - 15, cap.right - 112, cy + 15), text);
    D2D1_RECT_F vol = D2D1::RectF(cap.right - 84, cy - 1.5f, cap.right - 24, cy + 1.5f);
    wchar_t vg[2] = { 0xE767, 0 }; Text(vg, g_tfIconS, D2D1::RectF(vol.left - 26, cy - 12, vol.left - 4, cy + 12), text);
    Fill(vol, dark ? Rgb(255, 255, 255, 0.15f) : Rgb(0, 0, 0, 0.12f), 1.5f);
    Fill(D2D1::RectF(vol.left, vol.top, vol.left + (vol.right - vol.left) * g_volume, vol.bottom), gray, 1.5f);
    g_hits.push_back({ 7, 0, D2D1::RectF(vol.left - 4, cy - 10, vol.right + 4, cy + 10) });
}

void Paint() {
    if (!BeginPaint()) return;
    g_hits.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139), pink = Rgb(250, 45, 72), line = dark ? Rgb(255, 255, 255, 0.07f) : Rgb(0, 0, 0, 0.06f);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    std::lock_guard<std::mutex> lk(g_mx);
    const bool gg = BAR > 0;
    float X = SB + 20, W = sz.width - 20;
    // ---- conteúdo
    rt->PushAxisAlignedClip(D2D1::RectF(SB, TB, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = TB + (gg ? 30.f : 14.f) - g_scroll;
    const wchar_t* titles[] = { L"Recentes", L"Artistas", L"Álbuns", L"Músicas" };
    std::wstring head = g_view == 3 ? TrPick(L"Músicas", L"Songs") : Tr(titles[g_view]);
    if (g_view == 1 && !g_artistSel.empty()) head = g_artistSel;
    if (g_view == 2 && !g_albumSel.empty()) head = g_albumSel;
    if (gg) {  // macOS 27: o título vai para a barra; aqui só o "voltar"
        if ((g_view == 1 && !g_artistSel.empty()) || (g_view == 2 && !g_albumSel.empty())) {
            D2D1_RECT_F back = D2D1::RectF(X, y - 20, X + 160, y + 4);
            Text(L"‹ " + std::wstring(g_view == 1 ? Tr(L"Artistas") : Tr(L"Álbuns")), g_tfRow, back, pink);
            g_hits.push_back({ 8, 0, back });
            y += 14;
        }
    } else if ((g_view == 1 && !g_artistSel.empty()) || (g_view == 2 && !g_albumSel.empty())) {
        D2D1_RECT_F back = D2D1::RectF(X, y, X + 24, y + 34);
        Text(L"‹", g_tfHead, back, pink);
        g_hits.push_back({ 8, 0, back });
        Text(head, g_tfHead, D2D1::RectF(X + 26, y, W, y + 34), text);
    } else Text(head, g_tfHead, D2D1::RectF(X, y, W, y + 34), text);
    if (!gg) y += 46;
    if (g_view == 2 && g_albumSel.empty()) {  // grade de álbuns
        float cell = gg ? 200.f : 170.f, gap = 22;
        if (gg) X = SB + 33;
        int cols = std::max(1, (int)((W - X + gap) / (cell + gap)));
        auto al = Albums();
        for (int i = 0; i < (int)al.size(); i++) {
            float cx = X + (i % cols) * (cell + gap), cy = y + (i / cols) * (cell + 52);
            D2D1_RECT_F r = D2D1::RectF(cx, cy, cx + cell, cy + cell);
            if (r.bottom > TB - 200 && r.top < sz.height + 100) {
                const Song* first = nullptr; for (auto& s : g_songs) if (s.album == al[i]) { first = &s; break; }
                ID2D1Bitmap* b = first ? Art(*first) : nullptr;
                if (b) rt->DrawBitmap(b, r); else { Fill(r, dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), 8); wchar_t n[2] = { 0xEC4F, 0 }; Text(n, g_tfIconBig, r, gray); }
                Text(al[i], g_tfRow, D2D1::RectF(cx, cy + cell + 6, cx + cell, cy + cell + 24), text);
                Text(first ? first->artist : L"", g_tfSmall, D2D1::RectF(cx, cy + cell + 24, cx + cell, cy + cell + 42), gray);
            }
            g_hits.push_back({ 3, i, r });
        }
        y += ((int)al.size() + cols - 1) / cols * (cell + 52);
        if (gg) X = SB + 20;
    } else if (g_view == 1 && g_artistSel.empty()) {  // artistas
        auto ar = Artists();
        for (int i = 0; i < (int)ar.size(); i++) {
            D2D1_RECT_F r = D2D1::RectF(X, y, W, y + 44);
            Circle({ X + 20, y + 22 }, 18, dark ? Rgb(72, 72, 74) : Rgb(209, 209, 214), true);
            wchar_t ini[2] = { ar[i].empty() ? L'?' : (wchar_t)towupper(ar[i][0]), 0 };
            Text(ini, g_tfRowR, D2D1::RectF(X + 2, y, X + 38, y + 44), text);
            Text(ar[i], g_tfRow, D2D1::RectF(X + 52, y, W, y + 44), text);
            Fill(D2D1::RectF(X + 52, y + 44, W, y + 45), line);
            g_hits.push_back({ 4, i, r });
            y += 46;
        }
    } else {  // tabela de músicas
        float cT = X + 30, cA = X + (W - X) * 0.45f, cB = X + (W - X) * 0.68f, cD = W - 50;
        Text(Tr(L"Título"), g_tfSmall, D2D1::RectF(cT, y, cA, y + 20), gray);
        Text(Tr(L"Artista"), g_tfSmall, D2D1::RectF(cA, y, cB, y + 20), gray);
        Text(Tr(L"Álbum"), g_tfSmall, D2D1::RectF(cB, y, cD, y + 20), gray);
        y += 24;
        int playing = g_pos >= 0 && g_pos < (int)g_queue.size() ? g_queue[g_pos] : -1;
        for (int k = 0; k < (int)g_rows.size(); k++) {
            D2D1_RECT_F r = D2D1::RectF(X, y, W, y + ROWH);
            if (r.bottom > TB && r.top < sz.height) {
                auto& s = g_songs[g_rows[k]];
                if (k == g_selRow) Fill(r, pink, 6); else if (k % 2) Fill(r, line, 6);
                D2D1_COLOR_F tc = k == g_selRow ? Rgb(255, 255, 255) : text, sc = k == g_selRow ? Rgb(255, 255, 255, 0.85f) : gray;
                if (g_rows[k] == playing) { wchar_t sp[2] = { 0xE767, 0 }; Text(sp, g_tfIcon, D2D1::RectF(X + 4, y, X + 26, y + ROWH), k == g_selRow ? tc : pink); }
                Text(s.title, g_tfRow, D2D1::RectF(cT, y, cA - 10, y + ROWH), tc);
                Text(s.artist, g_tfRow, D2D1::RectF(cA, y, cB - 10, y + ROWH), sc);
                Text(s.album, g_tfRow, D2D1::RectF(cB, y, cD - 10, y + ROWH), sc);
                if (s.secs > 0) Text(Mmss(s.secs), g_tfRowR, D2D1::RectF(cD, y, W - 8, y + ROWH), sc);
            }
            g_hits.push_back({ 2, k, r });
            y += ROWH;
        }
        if (g_rows.empty()) Text(g_scanning ? Tr(L"Lendo a biblioteca…") : Tr(L"Nenhuma música na pasta Música."), g_tfRow, D2D1::RectF(SB, TB + 80, sz.width, TB + 160), gray);
    }
    rt->PopAxisAlignedClip();
    if (gg) {  // macOS 27: lateral cheia, título na barra e o player numa cápsula embaixo
        PaintSideGolden(sz, dark, text, gray, pink);
        PaintBarGolden(sz, dark, text, gray, head);
        PaintPlayerGolden(sz, dark, text, gray, pink);
        EndPaint();
        return;
    }
    // ---- barra lateral
    Fill(D2D1::RectF(0, TB, SB, sz.height), SidebarColor());
    Text(Tr(L"Biblioteca"), g_tfSmall, D2D1::RectF(16, TB + 12, SB, TB + 30), gray);
    wchar_t icons[] = { 0xE823, 0xE77B, 0xE93C, 0xEC4F };
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = D2D1::RectF(8, TB + 36 + i * 32, SB - 8, TB + 64 + i * 32);
        if (i == g_view) Fill(r, dark ? Rgb(255, 255, 255, 0.12f) : Rgb(0, 0, 0, 0.08f), 7);
        wchar_t g[2] = { icons[i], 0 };
        Text(g, g_tfIcon, D2D1::RectF(r.left + 4, r.top, r.left + 28, r.bottom), pink);
        Text(i == 3 ? TrPick(L"Músicas", L"Songs") : Tr(titles[i]), g_tfSide, D2D1::RectF(r.left + 34, r.top, r.right, r.bottom), text);
        g_hits.push_back({ 1, i, r });
    }
    // ---- visor (topo)
    Fill(D2D1::RectF(0, 0, sz.width, TB), dark ? Rgb(44, 44, 46) : Rgb(246, 246, 248));
    Fill(D2D1::RectF(0, TB - 1, sz.width, TB), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.1f));
    float cy = TB / 2;
    struct B { int id; wchar_t g; float x; bool big; bool on; } btns[] = {
        { 0, 0xE8B1, 24, false, g_shuffle }, { 1, 0xE892, 60, false, false }, { 2, Playing() ? (wchar_t)0xE769 : (wchar_t)0xE768, 100, true, false },
        { 3, 0xE893, 140, false, false }, { 4, g_repeat == 2 ? (wchar_t)0xE8ED : (wchar_t)0xE8EE, 176, false, g_repeat != 0 } };
    for (auto& b : btns) {
        D2D1_RECT_F r = D2D1::RectF(b.x - 16, cy - 16, b.x + 16, cy + 16);
        wchar_t g[2] = { b.g, 0 };
        Text(g, b.big ? g_tfIconBig : g_tfIcon, r, b.on ? pink : text);
        g_hits.push_back({ 5, b.id, r });
    }
    // o "LCD"
    float lx = std::max(220.f, sz.width / 2 - 260), lw = std::min(520.f, sz.width - lx - 180);
    D2D1_RECT_F lcd = D2D1::RectF(lx, 10, lx + lw, TB - 10);
    Fill(lcd, dark ? Rgb(58, 58, 60) : Rgb(232, 232, 236), 6);
    if (g_pos >= 0 && g_pos < (int)g_queue.size()) {
        const Song& s = g_songs[g_queue[g_pos]];
        D2D1_RECT_F art = D2D1::RectF(lcd.left, lcd.top, lcd.left + (lcd.bottom - lcd.top), lcd.bottom);
        if (ID2D1Bitmap* b = Art(s)) rt->DrawBitmap(b, art); else Fill(art, dark ? Rgb(72, 72, 74) : Rgb(209, 209, 214), 6);
        float tx = art.right + 10;
        Text(s.title, g_tfLcd, D2D1::RectF(tx, lcd.top + 2, lcd.right - 10, lcd.top + 20), text);
        Text(s.artist + L" — " + s.album, g_tfLcdSub, D2D1::RectF(tx, lcd.top + 18, lcd.right - 10, lcd.top + 34), gray);
        double pos = PosSecs(), len = std::max(LenSecs(), s.secs);
        D2D1_RECT_F bar = D2D1::RectF(tx + 34, lcd.bottom - 9, lcd.right - 44, lcd.bottom - 6);
        Fill(bar, dark ? Rgb(255, 255, 255, 0.15f) : Rgb(0, 0, 0, 0.12f), 1.5f);
        if (len > 0) Fill(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * (float)std::clamp(pos / len, 0.0, 1.0), bar.bottom), gray, 1.5f);
        Text(Mmss(pos), g_tfLcdSub, D2D1::RectF(tx, lcd.bottom - 16, tx + 32, lcd.bottom), gray);
        Text(Mmss(len), g_tfLcdSub, D2D1::RectF(lcd.right - 40, lcd.bottom - 16, lcd.right - 4, lcd.bottom), gray);
        g_hits.push_back({ 6, 0, D2D1::RectF(bar.left, bar.top - 6, bar.right, bar.bottom + 6) });
    } else {
        wchar_t g[2] = { 0xEC4F, 0 };
        Text(g, g_tfIconBig, lcd, gray);
    }
    // volume
    D2D1_RECT_F vol = D2D1::RectF(sz.width - 150, cy - 2, sz.width - 30, cy + 2);
    wchar_t vg[2] = { 0xE767, 0 }; Text(vg, g_tfIcon, D2D1::RectF(vol.left - 28, cy - 12, vol.left - 4, cy + 12), gray);
    Fill(vol, dark ? Rgb(255, 255, 255, 0.15f) : Rgb(0, 0, 0, 0.12f), 2);
    Fill(D2D1::RectF(vol.left, vol.top, vol.left + (vol.right - vol.left) * g_volume, vol.bottom), gray, 2);
    Circle({ vol.left + (vol.right - vol.left) * g_volume, cy }, 6, Rgb(255, 255, 255), true);
    g_hits.push_back({ 7, 0, D2D1::RectF(vol.left - 4, cy - 10, vol.right + 4, cy + 10) });
    EndPaint();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); return 0;
    case WM_DPICHANGED: OnDpi(w, l); return 0;
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_TIMER: if (g_pos >= 0) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SCAN: {  // a biblioteca chegou: junta com o que já estava (arquivo aberto de fora) e refaz a fila pelos caminhos
        auto* nv = (std::vector<Song>*)l;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            std::vector<std::wstring> qp; for (int i : g_queue) qp.push_back(g_songs[i].path);
            for (auto& s : g_songs) { bool in = false; for (auto& n : *nv) if (!_wcsicmp(n.path.c_str(), s.path.c_str())) { in = true; break; } if (!in) nv->push_back(s); }
            g_songs = std::move(*nv);
            g_queue.clear();
            for (auto& p : qp) for (int i = 0; i < (int)g_songs.size(); i++) if (!_wcsicmp(g_songs[i].path.c_str(), p.c_str())) { g_queue.push_back(i); break; }
        }
        delete nv;
        g_scanning = false; BuildRows(); InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_ART: {
        auto* d = (ArtData*)l;
        if (!d->px.empty() && rt) { ID2D1Bitmap* b = nullptr; rt->CreateBitmap(D2D1::SizeU(d->w, d->h), d->px.data(), d->w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b); g_art[d->album] = b; }
        else g_art[d->album] = nullptr;
        delete d; InvalidateRect(h, nullptr, FALSE); return 0;
    }
    case WM_ENDED: if (g_repeat == 2) PlayIndex(g_pos); else Next(1); return 0;
    case WM_COPYDATA: {  // outra abertura mandou um arquivo (o app tem uma janela só)
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->dwData == 0x4D555343 && cd->cbData >= 2) { std::wstring p((const wchar_t*)cd->lpData, cd->cbData / 2); while (!p.empty() && !p.back()) p.pop_back(); PlayFile(p); }
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return TRUE;
    }
    case WM_MOUSEWHEEL: g_scroll = std::max(0.f, g_scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 80); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        bool dbl = m == WM_LBUTTONDBLCLK;
        for (auto hit = g_hits.rbegin(); hit != g_hits.rend(); ++hit) {  // o desenhado por cima (o player) primeiro
            auto& ht = *hit;
            if (!In(ht.r, x, y)) continue;
            switch (ht.kind) {
            case 1: g_view = ht.arg; g_artistSel.clear(); g_albumSel.clear(); g_scroll = 0; g_selRow = -1; BuildRows(); break;
            case 8: if (g_view == 1) g_artistSel.clear(); else g_albumSel.clear(); g_scroll = 0; g_selRow = -1; BuildRows(); break;
            case 20: {  // busca: filtra enquanto digita
                FieldOpen(D2D1::RectF(ht.r.left + 30, ht.r.top + 9, ht.r.right - 10, ht.r.bottom - 9), g_search, 12, false, [](const std::wstring& q, int key) {
                    g_search = key == VK_ESCAPE ? L"" : q; g_scroll = 0; g_selRow = -1; BuildRows(); InvalidateRect(wnd, nullptr, FALSE);
                });
                return 0;
            }
            case 21: {  // filtro: a ordem das músicas
                HMENU mm = CreatePopupMenu();
                const wchar_t* nm[4] = { L"Ordem Padrão", L"Título", L"Artista", L"Álbum" };
                for (int k = 0; k < 4; k++) AppendMenuW(mm, MF_STRING | (g_sortBy == k ? MF_CHECKED : 0), 100 + k, Tr(nm[k]));
                POINT scr{ (LONG)(ht.r.left * s), (LONG)(ht.r.bottom * s) }; ClientToScreen(h, &scr);
                int c = TrackPopupMenu(mm, TPM_RETURNCMD, scr.x, scr.y, 0, h, nullptr);
                DestroyMenu(mm);
                if (c >= 100) { g_sortBy = c - 100; BuildRows(); }
                break;
            }
            case 2: g_selRow = ht.arg; if (dbl) PlayFrom(g_rows, ht.arg); break;
            case 3: { auto al = Albums(); g_albumSel = al[ht.arg]; g_scroll = 0; g_selRow = -1; BuildRows(); if (dbl && !g_rows.empty()) PlayFrom(g_rows, 0); break; }
            case 4: { auto ar = Artists(); g_artistSel = ar[ht.arg]; g_scroll = 0; g_selRow = -1; BuildRows(); break; }
            case 5:
                if (ht.arg == 0) g_shuffle = !g_shuffle;
                else if (ht.arg == 1) { if (PosSecs() > 3) { try { g_player.PlaybackSession().Position(std::chrono::seconds(0)); } catch (...) {} } else Next(-1); }
                else if (ht.arg == 2) Toggle();
                else if (ht.arg == 3) Next(1);
                else g_repeat = (g_repeat + 1) % 3;
                break;
            case 6: { double len = LenSecs(); if (len > 0) { double f = (x - ht.r.left) / (ht.r.right - ht.r.left); try { g_player.PlaybackSession().Position(winrt::Windows::Foundation::TimeSpan((long long)(std::clamp(f, 0.0, 1.0) * len * 1e7))); } catch (...) {} } break; }
            case 7: g_volume = std::clamp((x - ht.r.left - 4) / (ht.r.right - ht.r.left - 8), 0.f, 1.f); try { g_player.Volume(g_volume); } catch (...) {} break;
            default: continue;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (w == VK_SPACE) Toggle();
        else if (ctrl && w == VK_RIGHT) Next(1);
        else if (ctrl && w == VK_LEFT) Next(-1);
        else if (ctrl && w == VK_UP) { g_volume = std::min(1.f, g_volume + 0.1f); try { g_player.Volume(g_volume); } catch (...) {} }
        else if (ctrl && w == VK_DOWN) { g_volume = std::max(0.f, g_volume - 0.1f); try { g_player.Volume(g_volume); } catch (...) {} }
        else if (ctrl && w == 'W') DestroyWindow(h);
        else if (w == VK_RETURN && g_selRow >= 0) PlayFrom(g_rows, g_selRow);
        else if ((w == VK_UP || w == VK_DOWN) && !g_rows.empty()) { g_selRow = std::clamp(g_selRow + (w == VK_UP ? -1 : 1), 0, (int)g_rows.size() - 1); }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY: try { if (g_player) g_player.Close(); } catch (...) {} PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    std::wstring arg;
    { int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc); if (argc > 1 && wcscmp(argv[1], L"--foto")) arg = argv[1]; if (argv) LocalFree(argv); }
    // já aberto: manda o arquivo para a janela que existe (uma janela só, como no Mac)
    if (HWND other = FindWindowW(L"MusicaJanela", nullptr)) {
        if (!arg.empty()) { COPYDATASTRUCT cd{ 0x4D555343, (DWORD)((arg.size() + 1) * 2), (PVOID)arg.c_str() }; DWORD_PTR r; SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r); }
        else { if (IsIconic(other)) ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    if (!Init()) return 1;
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfSide = Fmt(13);
    g_tfHead = Fmt(26, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfRow = Fmt(13);
    g_tfRowR = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfSmall = Fmt(11.5f);
    g_tfLcd = Fmt(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfLcdSub = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfIconBig = Fmt(24, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfSec = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfSideB = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfTitleG = Fmt(13, DWRITE_FONT_WEIGHT_BOLD);
    g_tfIconS = Fmt(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfLcdL = Fmt(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfLcdSubL = Fmt(11);
    barWanted = 52;  // macOS 27: barra unificada
    if (!CreateMain(hi, L"MusicaJanela", Tr(L"Música"), Proc, semaforo::Aqua() ? 1145 : 1150, semaforo::Aqua() ? 735 : 760)) return 1;
    if (BAR > 0) TB = 52;
    try {
        g_player = wmp::MediaPlayer();
        g_player.Volume(g_volume);
        g_player.MediaEnded([](auto&&, auto&&) { PostMessageW(wnd, WM_ENDED, 0, 0); });
    } catch (...) {}
    Scan();
    SetTimer(wnd, 1, 500, nullptr);
    ShowWindow(wnd, show);
    if (!arg.empty()) PlayFile(arg);  // aberto com um arquivo: toca já
    return Run();
}
