// Ilha Dinâmica (Dynamic Island) no topo da tela, em três estilos (ajuste `ilha_estilo`):
//  - notch (padrão): um entalhe preto grudado na borda de cima, centrado, da altura da barra de menus, com as
//    "orelhas" côncavas do entalhe do MacBook. Fica sempre lá; cresce para os lados (asas) com o que está
//    acontecendo e para baixo quando abre.
//  - ilha: uma pílula preta sempre visível dentro da barra de menus, com contorno fino, brilho em cima e sombra.
//  - notificacao: a mesma pílula, mas só aparece quando acontece alguma coisa (música, aviso, volume...).
//
// O que mostra fechada (asas): música (capa à esquerda, barrinhas na cor da capa à direita), timer correndo,
// volume, Caps Lock, bateria ao ligar/desligar da tomada, ponto verde/laranja com câmera/microfone em uso.
// "Espiada" (cresce para baixo uns segundos): notificações (no lugar do banner do canto), download concluído,
// timer concluído, texto copiado. Aberta (passar o ponteiro ou clicar), com abas no cabeçalho:
// Início (o player: capa, barrinhas, tempo com o restante na cor da capa, aleatório/anterior/tocar/próxima/saída),
// Timer, Transferência (histórico do que foi copiado, só na memória), Estante (arquivos arrastados para a ilha;
// arrastar de volta para fora) e Sistema (CPU, memória e rede com gráfico).
//
// Referência: o projeto Atoll (github.com/Ebullioscopic/Atoll, GPL-3.0, derivado do boring.notch). Nada do código
// dele foi copiado (é Swift/SwiftUI); vieram só as medidas e o comportamento observados: entalhe com raio de cima
// 6 / de baixo 14 fechado e 19 / 24 aberto, pílula com raio 24 aberta, mola de abrir (resposta 0,42) e de fechar
// (0,45, sem quicar), troca de atividade fechada (0,34 / 0,88), capa pequena nas asas, barrinhas e tempo restante
// na cor da capa, abas Início/Estante/Timer/Sistema/Transferência, espiadas, abrir ao passar o ponteiro.
//
// Janela de tamanho fixo com SetWindowRgn (retângulo) acompanhando a forma: redimensionar a swap chain a cada quadro
// da mola deixava a borda um quadro atrás do desenho. Fora da forma o clique passa para quem está embaixo.
// Ocioso tem que ser ocioso: mídia, microfone/câmera, downloads e área de transferência chegam por eventos; o relógio
// de quadros só roda enquanto a mola mexe; barrinhas (30 q/s), timer (4/s) e Sistema (1/s) só enquanto aparecem.
#include "shell.h"
#include "../comum/simbolos.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wincodec.h>
#include <shcore.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace wmc = winrt::Windows::Media::Control;

namespace {

const UINT WM_IL_FRAME = WM_APP + 1, WM_IL_MEDIA = WM_APP + 2, WM_IL_CAPS = WM_APP + 3, WM_IL_PRIV = WM_APP + 4, WM_IL_DOWN = WM_APP + 5;
const UINT_PTR TM_HOVER = 1, TM_LEAVE = 2, TM_PEEK = 3, TM_HUD = 4, TM_EQ = 5, TM_PAUSE = 6, TM_GONE = 7, TM_TICK = 8, TM_STATS = 9, TM_CAPS = 10;

enum Style { S_NOTCH, S_ILHA, S_AVISO };
enum Mode { M_HIDDEN, M_IDLE, M_MEDIA, M_TIMER, M_HUD, M_PEEK, M_OPEN };
enum Tab { TAB_HOME, TAB_TIMER, TAB_CLIP, TAB_SHELF, TAB_STATS, TAB_N };
enum HudKind { HUD_VOL, HUD_CAPS, HUD_BAT };

// medidas em pt
const float WMAX = 470, HMAX = 210;   // a janela: o maior tamanho (aberta) com folga para a mola e a sombra
const float PILL_H = 22;              // pílula fechada (estilos ilha/notificação): cabe na barra de menus
const float OPEN_W = 300, BODY_H = 106;  // aberta: corpo de 300 pt (o player do Atoll tem ~280) e conteúdo abaixo do cabeçalho
const float NOTCH_W = 184;            // entalhe parado
const float SHADOW = 10;              // margem da sombra (estilo ilha)

// ---- mola do SwiftUI (resposta, amortecimento), integrada por quadro: pode trocar de alvo no meio
struct Spr {
    float x = 0, v = 0, t = 0;
    bool Step(float dtMs, float response, float damping, float eps) {
        float wn = 6.2831853f / response, k = wn * wn, c = 4 * 3.1415927f * damping / response;
        float left = dtMs / 1000.f;
        while (left > 0) {
            float h = std::min(left, 0.002f);
            v += (-k * (x - t) - c * v) * h;
            x += v * h;
            left -= h;
        }
        if (std::fabs(x - t) < eps && std::fabs(v) < eps * 10) { x = t; v = 0; return false; }
        return true;
    }
};

HWND g_wnd;
ui::GpuSurface g_s;
bool g_on, g_notesOn, g_volOn, g_hoverOpen;
Style g_style = S_NOTCH;
float g_barPt = 24;          // altura da barra de menus (pt)
bool g_full;                 // app em tela cheia no monitor principal (ou tela travada): some
bool g_shown;
Mode g_mode = M_HIDDEN, g_prev = M_HIDDEN;
double g_modeAt, g_frameAt;
int g_springKind;            // 0 troca fechada, 1 abrindo, 2 fechando
Spr g_w, g_h, g_rt, g_rb, g_a;  // largura, altura, raio de cima (orelha no notch), raio de baixo, opacidade
bool g_running;
RECT g_rgn{};
IDWriteTextFormat *g_tfTitle, *g_tfSub, *g_tfSmall, *g_tfSmallR, *g_tfSmallC, *g_tfWing, *g_tfWingR, *g_tfBig, *g_tfVal,
    *g_tfIcon, *g_tfIconS, *g_tfIconL, *g_tfChip;

// ---- estados
bool g_openOn, g_hoverIn, g_pauseHold, g_dragIn;
Tab g_tab = TAB_HOME;
double g_tabAt; int g_tabDir = 1;
struct Peek {
    std::wstring title, sub;
    wchar_t glyph = 0; D2D1_COLOR_F color{ 1, 1, 1, 1 };
    std::function<void(ID2D1DeviceContext*, ID2D1SolidColorBrush*, D2D1_RECT_F)> icon;
    std::function<void()> click;
};
bool g_peekOn; Peek g_peek;
bool g_hudOn; HudKind g_hud; float g_vol; bool g_mute; bool g_caps; int g_batPct; bool g_batAC;
int g_priv;                  // 1 microfone em uso, 2 câmera em uso
bool g_hasBattery;

// ---- timer
bool g_tRun, g_tPaused; double g_tEnd, g_tLeft, g_tTotal;  // ms (anim::Now); g_tLeft quando pausado

// ---- mídia (escrita pela thread da mídia; a janela copia ao receber WM_IL_MEDIA)
struct Media {
    bool has = false, playing = false, shuffle = false, canShuffle = false;
    std::wstring title, artist, aumid;
    std::vector<BYTE> art; UINT aw = 0, ah = 0; unsigned artGen = 0;
    D2D1_COLOR_F tint{ 1, 1, 1, 1 };
    double pos = 0, len = 0, start = 0, at = 0;
};
std::mutex g_mmx;
std::condition_variable g_mcv;
Media g_md;
bool g_mWake, g_mRebind;
struct Cmd { int c; double v; };
std::vector<Cmd> g_cmds;   // 1 tocar/pausar, 2 próxima, 3 anterior, 4 aleatório (v), 5 posição (v s)
bool g_mThread;
Media g_m;
unsigned g_artGenUi = ~0u;
ID2D1Bitmap* g_art;

// ---- barrinhas
IAudioMeterInformation* g_meter;
double g_meterAt, g_eqFrame;
float g_bars[4];

// ---- Transferência (só na memória) e Estante (%APPDATA%\Shell\estante.txt)
std::deque<std::wstring> g_clips;
bool g_selfCopy;
std::vector<std::wstring> g_shelf;
std::map<std::wstring, ID2D1Bitmap*> g_shelfIcons;

// ---- Sistema
struct Stats { std::deque<float> cpu, mem, net; float cpuNow = 0, memNow = 0; double down = 0, up = 0; } g_st;
ULONGLONG g_lastIdle, g_lastKern, g_lastUser; double g_lastNetAt;

// ---- interação
enum HitId { H_NONE, H_TAB, H_GEAR, H_PREV, H_PLAY, H_NEXT, H_SHUF, H_OUT, H_SEEK, H_ART, H_PRESET, H_TPAUSE, H_TSTOP, H_TPLUS,
             H_CLIP, H_CLIPCLR, H_SHELF, H_SHELFCLR };
struct Hit { HitId id; D2D1_RECT_F r; int arg = 0; };
std::vector<Hit> g_hits;
HitId g_hoverHit = H_NONE; int g_hoverArg = -1;
HitId g_pressHit = H_NONE; int g_pressArg = -1; POINT g_pressPt;
double g_flashAt; int g_flashClip = -1;  // "Copiado" no item clicado

float Sc() { return ui::Scale(g_wnd); }
const D2D1_COLOR_F WHITE{ 1, 1, 1, 1 }, GRAY{ 1, 1, 1, 0.55f }, GRAY2{ 1, 1, 1, 0.35f };
const D2D1_COLOR_F ORANGE{ 1.f, 0.62f, 0.04f, 1 }, GREEN{ 0.2f, 0.84f, 0.29f, 1 }, RED{ 1.f, 0.27f, 0.23f, 1 };

void Update();
void Paint();
void UpdateTimers();

// ================= mídia (thread própria, por eventos)
void MediaWake(bool rebind) {
    { std::lock_guard<std::mutex> l(g_mmx); g_mWake = true; g_mRebind |= rebind; }
    g_mcv.notify_all();
}

bool DecodeThumb(const wmc::GlobalSystemMediaTransportControlsSessionMediaProperties& p, std::vector<BYTE>& px, UINT& w, UINT& h) {
    if (!p.Thumbnail()) return false;
    winrt::Windows::Storage::Streams::IRandomAccessStreamWithContentType stream{ nullptr };
    try { stream = p.Thumbnail().OpenReadAsync().get(); } catch (...) { return false; }  // capa ilegível: segue sem ela
    if (!stream) return false;
    IStream* is = nullptr;
    bool ok = false;
    if (SUCCEEDED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(&is)))) {
        IWICImagingFactory* f = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
            IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
            IWICBitmapScaler* sc = nullptr;
            if (SUCCEEDED(f->CreateDecoderFromStream(is, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
                UINT fw = 0, fh = 0; fr->GetSize(&fw, &fh);
                IWICBitmapSource* src = fr;
                if (fw > 256 || fh > 256) {  // capa no máximo 256 px: menos memória e cópia
                    float k = 256.f / std::max(fw, fh);
                    if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, std::max(1u, (UINT)(fw * k)), std::max(1u, (UINT)(fh * k)), WICBitmapInterpolationModeFant)))
                        src = sc;
                }
                if (SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                    SUCCEEDED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                    cv->GetSize(&w, &h);
                    px.resize((size_t)w * h * 4);
                    ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
                    if (!ok) px.clear();
                }
            }
            if (cv) cv->Release(); if (sc) sc->Release(); if (fr) fr->Release(); if (dec) dec->Release();
            f->Release();
        }
        is->Release();
    }
    return ok;
}

// cor das barrinhas e do tempo restante: a média da capa, viva e clara o bastante para aparecer no preto
D2D1_COLOR_F ArtTint(const std::vector<BYTE>& px) {
    double r = 0, g = 0, b = 0, n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4 * 7) {
        double a = px[i + 3]; if (a < 8) continue;
        double pb = px[i] / a, pg = px[i + 1] / a, pr = px[i + 2] / a;
        double mx = std::max({ pr, pg, pb }), mn = std::min({ pr, pg, pb });
        double wgt = 0.25 + (mx - mn);  // cores vivas pesam mais que cinzas
        r += pr * wgt; g += pg * wgt; b += pb * wgt; n += wgt;
    }
    if (n <= 0) return { 1, 1, 1, 1 };
    r /= n; g /= n; b /= n;
    double mx = std::max({ r, g, b, 0.001 });
    r /= mx; g /= mx; b /= mx;
    const double k = 0.25;
    return D2D1::ColorF((float)(r + (1 - r) * k), (float)(g + (1 - g) * k), (float)(b + (1 - b) * k), 1);
}

void MediaThread() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    wmc::GlobalSystemMediaTransportControlsSessionManager mgr{ nullptr };
    wmc::GlobalSystemMediaTransportControlsSession ses{ nullptr };
    winrt::event_token tProp{}, tPlay{}, tTime{};
    try {
        mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        mgr.CurrentSessionChanged([](auto&&, auto&&) { MediaWake(true); });
    } catch (...) { Log(L"ilha: sem acesso aos controles de mídia do Windows"); return; }
    std::wstring lastKey;
    for (;;) {
        bool rebind; std::vector<Cmd> cmds;
        {
            std::unique_lock<std::mutex> l(g_mmx);
            g_mcv.wait(l, [] { return g_mWake; });
            g_mWake = false; rebind = g_mRebind; g_mRebind = false; cmds.swap(g_cmds);
        }
        Media out;
        try {
            if (rebind) {
                if (ses) {
                    try { ses.MediaPropertiesChanged(tProp); ses.PlaybackInfoChanged(tPlay); ses.TimelinePropertiesChanged(tTime); } catch (...) {}
                }
                ses = mgr.GetCurrentSession();
                if (ses) {
                    tProp = ses.MediaPropertiesChanged([](auto&&, auto&&) { MediaWake(false); });
                    tPlay = ses.PlaybackInfoChanged([](auto&&, auto&&) { MediaWake(false); });
                    tTime = ses.TimelinePropertiesChanged([](auto&&, auto&&) { MediaWake(false); });
                }
            }
            if (ses) {
                double startNow;
                { std::lock_guard<std::mutex> l(g_mmx); startNow = g_md.start; }
                for (auto& c : cmds) {
                    try {
                        if (c.c == 1) ses.TryTogglePlayPauseAsync().get();
                        else if (c.c == 2) ses.TrySkipNextAsync().get();
                        else if (c.c == 3) ses.TrySkipPreviousAsync().get();
                        else if (c.c == 4) ses.TryChangeShuffleActiveAsync(c.v != 0).get();
                        else if (c.c == 5) ses.TryChangePlaybackPositionAsync((int64_t)((c.v + startNow) * 1e7)).get();
                    } catch (...) {}
                }
                // sessão fechando: o Windows devolve objetos nulos (e um nulo do C++/WinRT derruba o Shell, não lança)
                auto pi = ses.GetPlaybackInfo();
                if (!pi) throw winrt::hresult_error(E_POINTER);
                auto st = pi.PlaybackStatus();
                using PS = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
                out.has = st == PS::Playing || st == PS::Paused;
                out.playing = st == PS::Playing;
                if (auto ctl = pi.Controls()) out.canShuffle = ctl.IsShuffleEnabled();
                if (auto sh = pi.IsShuffleActive()) out.shuffle = sh.Value();
                out.aumid = ses.SourceAppUserModelId().c_str();
                auto p = ses.TryGetMediaPropertiesAsync().get();
                if (!p) throw winrt::hresult_error(E_POINTER);
                out.title = p.Title().c_str();
                out.artist = p.Artist().c_str();
                if (out.title.empty()) out.has = false;  // sessão sem nada (aba do navegador parada)
                auto tl = ses.GetTimelineProperties();
                if (!tl) throw winrt::hresult_error(E_POINTER);
                double start = tl.StartTime().count() / 1e7, end = tl.EndTime().count() / 1e7, pos = tl.Position().count() / 1e7;
                out.start = start;
                out.len = std::max(0.0, end - start);
                pos -= start;
                if (out.playing && out.len > 0) {  // o app avisa a posição de vez em quando: soma o tempo desde o aviso
                    double age = std::chrono::duration<double>(winrt::clock::now() - tl.LastUpdatedTime()).count();
                    if (age > 0 && age < 6 * 3600) pos += age;
                }
                out.pos = std::clamp(pos, 0.0, std::max(out.len, 0.0));
                out.at = anim::Now();
                std::wstring key = out.title + L"\x1f" + out.artist + L"\x1f" + out.aumid;
                bool needArt;
                { std::lock_guard<std::mutex> l(g_mmx); needArt = key != lastKey || g_md.art.empty(); }
                if (out.has && needArt) {
                    std::vector<BYTE> px; UINT w = 0, h = 0;
                    bool thumb = (bool)p.Thumbnail();
                    if (DecodeThumb(p, px, w, h)) { out.tint = ArtTint(px); out.art = std::move(px); out.aw = w; out.ah = h; }
                    else if (key != lastKey) Log(L"ilha: '%ls' sem capa (%ls)", out.title.c_str(), thumb ? L"não decodificou" : L"o app não mandou");
                }
                std::lock_guard<std::mutex> l(g_mmx);
                if (out.has && needArt && (!out.art.empty() || key != lastKey)) {  // faixa nova (ou a capa chegou depois)
                    g_md.art = std::move(out.art); g_md.aw = out.aw; g_md.ah = out.ah; g_md.tint = out.tint; g_md.artGen++;
                }
                if (out.has) lastKey = key;
                g_md.has = out.has; g_md.playing = out.playing; g_md.shuffle = out.shuffle; g_md.canShuffle = out.canShuffle;
                g_md.title = out.title; g_md.artist = out.artist; g_md.aumid = out.aumid;
                g_md.pos = out.pos; g_md.len = out.len; g_md.start = out.start; g_md.at = out.at;
            } else {
                std::lock_guard<std::mutex> l(g_mmx);
                g_md.has = g_md.playing = false;
            }
        } catch (...) {
            std::lock_guard<std::mutex> l(g_mmx);
            g_md.has = g_md.playing = false;
        }
        if (g_wnd) PostMessageW(g_wnd, WM_IL_MEDIA, 0, 0);
    }
}

void MediaStart() {
    if (g_mThread) return;
    g_mThread = true;
    { std::lock_guard<std::mutex> l(g_mmx); g_mWake = g_mRebind = true; }
    std::thread(MediaThread).detach();
}

void MediaCmd(int c, double v = 0) {
    { std::lock_guard<std::mutex> l(g_mmx); g_cmds.push_back({ c, v }); g_mWake = true; }
    g_mcv.notify_all();
}

// ================= microfone e câmera em uso (o mesmo registro que o Windows usa para o ícone da bandeja)
// HKCU\...\CapabilityAccessManager\ConsentStore\{microphone,webcam}\<app> (e NonPackaged\<exe>): em uso enquanto
// LastUsedTimeStart > 0 e LastUsedTimeStop = 0. Avisado por RegNotifyChangeKeyValue, sem reler em laço.
bool InUse(HKEY k) {
    bool use = false;
    wchar_t name[512];
    for (DWORD i = 0;; i++) {
        DWORD n = 512;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY s;
        if (RegOpenKeyExW(k, name, 0, KEY_READ, &s) != ERROR_SUCCESS) continue;
        if (!_wcsicmp(name, L"NonPackaged")) use |= InUse(s);
        else {
            ULONGLONG a = 0, b = 1; DWORD cb = sizeof(a);
            RegQueryValueExW(s, L"LastUsedTimeStart", nullptr, nullptr, (BYTE*)&a, &cb); cb = sizeof(b);
            if (RegQueryValueExW(s, L"LastUsedTimeStop", nullptr, nullptr, (BYTE*)&b, &cb) == ERROR_SUCCESS && a && !b) use = true;
        }
        RegCloseKey(s);
        if (use) break;
    }
    return use;
}

void PrivacyThread() {
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    HKEY k[2]{};
    HANDLE ev[2]{};
    for (int i = 0; i < 2; i++) {
        RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(base) + (i ? L"webcam" : L"microphone")).c_str(), 0, KEY_READ | KEY_NOTIFY, &k[i]);
        ev[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (!k[0] && !k[1]) return;
    int last = -1;
    for (;;) {
        int bits = 0;
        for (int i = 0; i < 2; i++) {
            if (!k[i]) continue;
            RegNotifyChangeKeyValue(k[i], TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, ev[i], TRUE);
            if (InUse(k[i])) bits |= i ? 2 : 1;
        }
        if (bits != last) { last = bits; if (g_wnd) PostMessageW(g_wnd, WM_IL_PRIV, bits, 0); }
        WaitForMultipleObjects(2, ev, FALSE, INFINITE);
        Sleep(150);  // o app grava início/fim em seguida: lê uma vez só
    }
}

// ================= downloads concluídos (pasta Downloads, ReadDirectoryChangesW)
void DownloadsThread() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) return;
    std::wstring dir = p; CoTaskMemFree(p);
    HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    alignas(DWORD) BYTE buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadDirectoryChangesW(h, buf, sizeof(buf), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &got, nullptr, nullptr)) break;
        if (!got) continue;
        for (auto* fi = (FILE_NOTIFY_INFORMATION*)buf;; fi = (FILE_NOTIFY_INFORMATION*)((BYTE*)fi + fi->NextEntryOffset)) {
            if (fi->Action == FILE_ACTION_ADDED || fi->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                std::wstring name(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
                std::wstring low = name; for (auto& c : low) c = towlower(c);
                auto ends = [&](const wchar_t* e) { size_t n = wcslen(e); return low.size() >= n && low.compare(low.size() - n, n, e) == 0; };
                // arquivos parciais dos navegadores e temporários: espera virar o arquivo final
                bool partial = ends(L".crdownload") || ends(L".part") || ends(L".tmp") || ends(L".partial") || ends(L".download") || ends(L".opdownload") ||
                               low.rfind(L"unconfirmed ", 0) == 0 || low[0] == L'~' || low[0] == L'.' || name.find(L'\\') != std::wstring::npos;
                if (!partial) {
                    std::wstring full = dir + L"\\" + name;
                    std::thread([full] {
                        Sleep(500);
                        WIN32_FILE_ATTRIBUTE_DATA fa{};
                        if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
                        if (!fa.nFileSizeHigh && !fa.nFileSizeLow) return;
                        if (g_wnd) PostMessageW(g_wnd, WM_IL_DOWN, 0, (LPARAM)new std::wstring(full));
                    }).detach();
                }
            }
            if (!fi->NextEntryOffset) break;
        }
    }
    CloseHandle(h);
}

// ================= barrinhas (nível de pico da saída padrão)
float Peak() {
    double now = anim::Now();
    if (!g_meter && now - g_meterAt > 3000) {
        g_meterAt = now;
        IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&g_meter);
        if (dev) dev->Release(); if (en) en->Release();
    }
    float p = 0;
    if (g_meter && FAILED(g_meter->GetPeakValue(&p))) { g_meter->Release(); g_meter = nullptr; p = 0; }
    return p;
}

void StepBars() {
    double now = anim::Now();
    float dt = (float)std::clamp(now - g_eqFrame, 1.0, 100.0);
    g_eqFrame = now;
    float t = (float)(now / 1000.0);
    static const float freq[4] = { 7.3f, 9.1f, 5.7f, 8.3f }, ph[4] = { 0.3f, 1.9f, 3.1f, 4.4f };
    float lvl = 0.18f;
    if (g_m.playing) {
        float p = Peak();
        lvl = p > 0.002f ? std::clamp(0.45f + std::sqrt(p) * 0.8f, 0.f, 1.f) : 0.8f;  // sem medida: balanço calmo
    }
    for (int i = 0; i < 4; i++) {
        float target = g_m.playing ? lvl * (0.4f + 0.6f * std::fabs(std::sin(t * freq[i] + ph[i]))) : 0.18f;
        g_bars[i] += (target - g_bars[i]) * (1 - std::exp(-dt / 70.f));
    }
}

// ================= Sistema
void SampleStats() {
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        auto q = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
        ULONGLONG i = q(fi), k = q(fk), u = q(fu);
        if (g_lastKern) {
            ULONGLONG di = i - g_lastIdle, dt = (k - g_lastKern) + (u - g_lastUser);
            g_st.cpuNow = dt ? std::clamp(1.f - (float)di / dt, 0.f, 1.f) : 0;
            g_st.cpu.push_back(g_st.cpuNow);
        }
        g_lastIdle = i; g_lastKern = k; g_lastUser = u;
    }
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) { g_st.memNow = ms.dwMemoryLoad / 100.f; g_st.mem.push_back(g_st.memNow); }
    // GetIfTable2 pede o winsock2 (briga com o windows.h do Shell): o GetIfTable clássico, contadores de 32 bits
    // (a diferença em DWORD já trata a virada)
    ULONG sz = 0;
    GetIfTable(nullptr, &sz, FALSE);
    std::vector<BYTE> buf(sz ? sz : 1);
    auto* t = (MIB_IFTABLE*)buf.data();
    if (sz && GetIfTable(t, &sz, FALSE) == NO_ERROR) {
        static std::map<DWORD, std::pair<DWORD, DWORD>> last;  // por interface: in/out da última leitura
        ULONGLONG din = 0, dout = 0;
        for (DWORD j = 0; j < t->dwNumEntries; j++) {
            auto& r = t->table[j];
            if (r.dwType == MIB_IF_TYPE_LOOPBACK || r.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
            if (r.dwType != MIB_IF_TYPE_ETHERNET && r.dwType != IF_TYPE_IEEE80211) continue;
            auto it = last.find(r.dwIndex);
            if (it != last.end()) { din += (DWORD)(r.dwInOctets - it->second.first); dout += (DWORD)(r.dwOutOctets - it->second.second); }
            last[r.dwIndex] = { r.dwInOctets, r.dwOutOctets };
        }
        double now = anim::Now();
        if (g_lastNetAt > 0 && now > g_lastNetAt) {
            double s = (now - g_lastNetAt) / 1000.0;
            g_st.down = din / s; g_st.up = dout / s;
            g_st.net.push_back((float)(g_st.down + g_st.up));
        }
        g_lastNetAt = now;
    }
    for (auto* d : { &g_st.cpu, &g_st.mem, &g_st.net }) while (d->size() > 40) d->pop_front();
}

std::wstring Rate(double bps) {
    wchar_t b[32];
    if (bps >= 1e6) swprintf_s(b, L"%.1f MB/s", bps / 1e6);
    else swprintf_s(b, L"%.0f KB/s", bps / 1e3);
    return b;
}

// ================= Estante
std::wstring ShelfFile() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { r = std::wstring(p) + L"\\Shell\\estante.txt"; CoTaskMemFree(p); }
    return r;
}

void ShelfSave() {
    std::wstring s;
    for (auto& p : g_shelf) s += p + L"\r\n";
    FILE* f = nullptr;
    if (_wfopen_s(&f, ShelfFile().c_str(), L"w, ccs=UTF-8") == 0 && f) { fputws(s.c_str(), f); fclose(f); }
}

void ShelfLoad() {
    g_shelf.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, ShelfFile().c_str(), L"r, ccs=UTF-8") || !f) return;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring p = line;
        while (!p.empty() && (p.back() == L'\n' || p.back() == L'\r')) p.pop_back();
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) g_shelf.push_back(p);
    }
    fclose(f);
}

void ShelfAdd(const std::wstring& p) {
    if (std::find(g_shelf.begin(), g_shelf.end(), p) != g_shelf.end()) return;
    g_shelf.push_back(p);
    if (g_shelf.size() > 24) g_shelf.erase(g_shelf.begin());
    ShelfSave();
}

ID2D1Bitmap* ShelfIcon(ID2D1DeviceContext* dc, const std::wstring& p) {
    auto it = g_shelfIcons.find(p);
    if (it != g_shelfIcons.end()) return it->second;
    UINT w = 0, h = 0; std::vector<BYTE> px;
    ID2D1Bitmap* b = ui::FileImage(p, 64, w, h, px) ? ui::Bitmap(dc, w, h, px) : nullptr;
    g_shelfIcons[p] = b;
    return b;
}

// arrastar de volta para fora: o objeto de dados do próprio Windows para o arquivo
struct DropSource : IDropSource {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDropSource) { *o = this; return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
} g_dropSrc;

void ShelfDragOut(int i) {
    if (i < 0 || i >= (int)g_shelf.size()) return;
    std::wstring p = g_shelf[i];
    IShellItem* it = nullptr; IDataObject* d = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&it))) &&
        SUCCEEDED(it->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&d)))) {
        DWORD eff = 0;
        HRESULT hr = DoDragDrop(d, &g_dropSrc, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
        if (hr == DRAGDROP_S_DROP && (eff & DROPEFFECT_MOVE || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)) {
            g_shelf.erase(std::remove(g_shelf.begin(), g_shelf.end(), p), g_shelf.end());
            ShelfSave();
        }
    }
    if (d) d->Release();
    if (it) it->Release();
}

// arrastar arquivos para a ilha: abre na Estante
struct DropTarget : IDropTarget {
    bool files = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDropTarget) { *o = this; return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        files = d && d->QueryGetData(&fe) == S_OK;
        *eff = files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (files) {
            g_dragIn = true;
            KillTimer(g_wnd, TM_LEAVE);
            if (g_tab != TAB_SHELF) { g_tabDir = TAB_SHELF > g_tab ? 1 : -1; g_tab = TAB_SHELF; g_tabAt = anim::Now(); }
            g_openOn = true;
            Update();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override { *eff = files ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        g_dragIn = false;
        SetTimer(g_wnd, TM_LEAVE, 400, nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        g_dragIn = false;
        *eff = DROPEFFECT_NONE;
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm{};
        if (d && SUCCEEDED(d->GetData(&fe, &sm))) {
            if (HDROP hd = (HDROP)GlobalLock(sm.hGlobal)) {
                UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; i++) {
                    wchar_t p[MAX_PATH * 2];
                    if (DragQueryFileW(hd, i, p, MAX_PATH * 2)) ShelfAdd(p);
                }
                GlobalUnlock(sm.hGlobal);
                Log(L"ilha: %u arquivo(s) na Estante", n);
            }
            ReleaseStgMedium(&sm);
            *eff = DROPEFFECT_COPY;
        }
        SetTimer(g_wnd, TM_LEAVE, 1500, nullptr);
        if (!g_running) Paint();
        return S_OK;
    }
} g_dropTgt;

// ================= Transferência
void ReadClipboard() {
    static UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    // gerenciadores de senha marcam o que não deve ir para históricos: respeita
    if (IsClipboardFormatAvailable(exclude)) return;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    for (int i = 0; i < 5 && !OpenClipboard(g_wnd); i++) Sleep(20);
    std::wstring s;
    bool skip = false;
    if (HANDLE hh = GetClipboardData(history)) { if (DWORD* v = (DWORD*)GlobalLock(hh)) { skip = *v == 0; GlobalUnlock(hh); } }
    if (!skip)
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* t = (const wchar_t*)GlobalLock(h)) { s.assign(t, wcsnlen(t, 20000)); GlobalUnlock(h); }
        }
    CloseClipboard();
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    if (skip || a == std::wstring::npos) return;
    s = s.substr(a, b - a + 1);
    g_clips.erase(std::remove(g_clips.begin(), g_clips.end(), s), g_clips.end());
    g_clips.push_front(s);
    while (g_clips.size() > 12) g_clips.pop_back();
}

void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    g_selfCopy = true;
}

// ================= o que a ilha mostra
bool ShowsMedia() { return g_m.has && (g_m.playing || g_pauseHold); }
bool Notch() { return g_style == S_NOTCH; }
float HeadH() { return Notch() ? g_barPt : 28.f; }   // cabeçalho das abas (no notch, a própria altura da barra)
float TopOff() { return Notch() ? 0.f : std::max(0.f, (g_barPt - PILL_H) / 2); }

Mode Want() {
    if (!g_on || g_full) return M_HIDDEN;
    if (g_peekOn) return M_PEEK;
    if (g_hudOn) return M_HUD;
    if (g_openOn) return M_OPEN;
    if (ShowsMedia()) return M_MEDIA;
    if (g_tRun) return M_TIMER;
    if (g_style == S_AVISO && !g_priv) return M_HIDDEN;
    return M_IDLE;
}

struct Size { float w, h, rt, rb; };
Size SizeOf(Mode m) {
    float priv = g_priv ? 24.f : 0.f;  // ponto verde/laranja na asa direita
    if (Notch()) {
        const float H = g_barPt;
        if (liquid::Leopard()) {  // o HUD do 10.5 pendurado reto da barra (sem as orelhas do entalhe moderno), cantos de painel 6/10
            switch (m) {
            case M_IDLE:  return { NOTCH_W + priv, H, 0, 6 };
            case M_MEDIA: case M_TIMER: return { NOTCH_W + 2 * 40 + priv, H, 0, 6 };
            case M_HUD:   return { NOTCH_W + 2 * 66, H, 0, 6 };
            case M_PEEK:  return { 320, H + 50, 0, 10 };
            case M_OPEN:  return { OPEN_W, H + BODY_H, 0, 10 };
            default:      return { NOTCH_W * 0.7f, H * 0.5f, 0, 4 };
            }
        }
        switch (m) {
        case M_IDLE:  return { NOTCH_W + priv, H, 6, 9 };
        case M_MEDIA: case M_TIMER: return { NOTCH_W + 2 * 40 + priv, H, 6, 9 };
        case M_HUD:   return { NOTCH_W + 2 * 66, H, 6, 9 };
        case M_PEEK:  return { 320 + 2 * 12, H + 50, 12, 20 };
        case M_OPEN:  return { OPEN_W + 2 * 19, H + BODY_H, 19, 24 };  // largura com as orelhas (Atoll: 19 em cima, 24 embaixo)
        default:      return { NOTCH_W * 0.7f, H * 0.5f, 4, 6 };
        }
    }
    switch (m) {
    case M_IDLE:  return { 112 + priv, PILL_H, 11, 11 };
    case M_MEDIA: case M_TIMER: return { 232 + priv, PILL_H, 11, 11 };
    case M_HUD:   return { 244, PILL_H, 11, 11 };
    case M_PEEK:  return { 320, 54, 20, 20 };
    case M_OPEN: {  // cantos do cartão por tema: Golden Gate maiores (macOS 27), Leopard o HUD do 10.5
        float r = liquid::Golden() ? 30.f : liquid::Leopard() ? 12.f : 24.f;
        return { OPEN_W, 28 + BODY_H, r, r };
    }
    default:      return { 90, 14, 7, 7 };
    }
}

void UpdateTimers() {
    bool eq = g_shown && g_m.has && (g_mode == M_MEDIA || (g_mode == M_OPEN && g_tab == TAB_HOME)) &&
              (g_m.playing || std::any_of(std::begin(g_bars), std::end(g_bars), [](float b) { return std::fabs(b - 0.18f) > 0.01f; }));
    if (eq) SetTimer(g_wnd, TM_EQ, 33, nullptr); else KillTimer(g_wnd, TM_EQ);
    bool tick = g_shown && g_tRun && (g_mode == M_TIMER || g_mode == M_MEDIA || (g_mode == M_OPEN && g_tab == TAB_TIMER));
    if (g_tRun && !g_tPaused) SetTimer(g_wnd, TM_TICK, 250, nullptr);  // corre mesmo escondido: precisa avisar ao acabar
    else KillTimer(g_wnd, TM_TICK);
    (void)tick;
    if (g_shown && g_mode == M_OPEN && g_tab == TAB_STATS) {
        if (g_st.cpu.empty() && !g_lastKern) SampleStats();
        SetTimer(g_wnd, TM_STATS, 1000, nullptr);
    } else KillTimer(g_wnd, TM_STATS);
}

void Place() {
    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    if (HWND mb = menubar::Hwnd()) dpi = GetDpiForWindow(mb);
    float sc = dpi / 96.f;
    g_barPt = std::max(18.f, menubar::HeightOn(mon) / sc);
    int w = (int)std::lround(WMAX * sc), h = (int)std::lround(HMAX * sc);
    int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, mi.rcMonitor.top, w, h, SWP_NOACTIVATE | (g_shown ? 0 : SWP_NOREDRAW));
}

void StartFrames() {
    if (g_running) return;
    g_running = true;
    g_frameAt = anim::Now();
    ui::FrameClock(g_wnd, WM_IL_FRAME, true);
}

bool FullOnPrimary();
void Update() {
    if (!g_wnd) return;
    Mode m = Want();
    // ia aparecer do nada: confere antes se não há algo em tela cheia (vídeo que entrou em tela cheia sem trocar de janela)
    if (m != M_HIDDEN && !g_shown && FullOnPrimary()) { g_full = true; Log(L"ilha: tela cheia, some"); m = M_HIDDEN; }
    double now = anim::Now();
    if (m != g_mode) {
        bool big = m == M_OPEN || m == M_PEEK, wasBig = g_mode == M_OPEN || g_mode == M_PEEK;
        g_springKind = big ? 1 : wasBig ? 2 : 0;
        g_prev = now - g_modeAt < 90 ? g_prev : g_mode;
        g_mode = m;
        g_modeAt = now;
        g_hits.clear();
        if (m != M_OPEN) { g_hoverHit = g_pressHit = H_NONE; }
        Log(L"ilha: %d -> %d", (int)g_prev, (int)m);
    }
    Size s = SizeOf(m);
    float a = m == M_HIDDEN ? 0.f : 1.f;
    bool retarget = s.w != g_w.t || s.h != g_h.t || s.rt != g_rt.t || s.rb != g_rb.t || a != g_a.t;
    g_w.t = s.w; g_h.t = s.h; g_rt.t = s.rt; g_rb.t = s.rb; g_a.t = a;
    if (m != M_HIDDEN && !g_shown) {
        // vem do nada: nasce menor e transparente e cresce com a mola
        Size s0 = SizeOf(M_HIDDEN);
        g_w.x = s0.w; g_h.x = s0.h; g_rt.x = s0.rt; g_rb.x = s0.rb; g_a.x = 0;
        g_w.v = g_h.v = g_rt.v = g_rb.v = g_a.v = 0;
        g_prev = M_HIDDEN;
        Place();
        g_shown = true;
        Paint();
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    }
    if (g_shown) SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // acima da barra de menus
    if (retarget || now - g_modeAt < 1) StartFrames();
    UpdateTimers();
}

void ShowPeek(Peek p, UINT ms) {
    g_peek = std::move(p);
    bool again = g_peekOn && g_mode == M_PEEK;
    g_peekOn = true;
    g_hudOn = false; KillTimer(g_wnd, TM_HUD);
    SetTimer(g_wnd, TM_PEEK, ms, nullptr);
    if (again) { g_modeAt = anim::Now() - 90; g_prev = M_PEEK; StartFrames(); }  // outra espiada: só o conteúdo troca
    Update();
}

void ShowHud(HudKind k, UINT ms) {
    g_hud = k;
    g_hudOn = true;
    SetTimer(g_wnd, TM_HUD, ms, nullptr);
    if (g_mode == M_HUD) { if (!g_running) Paint(); }
    else Update();
}

// ---- timer
double TimerLeft() { return !g_tRun ? 0 : g_tPaused ? g_tLeft : std::max(0.0, g_tEnd - anim::Now()); }
void TimerStart(double minutes) {
    g_tTotal = minutes * 60000; g_tEnd = anim::Now() + g_tTotal; g_tRun = true; g_tPaused = false;
    Log(L"ilha: timer de %.0f min", minutes);
    Update();
}
void TimerStop() { g_tRun = g_tPaused = false; Update(); }

std::wstring Clock(double s) {
    int t = (int)std::max(0.0, std::ceil(s - 0.001));
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else swprintf_s(b, L"%d:%02d", t / 60, t % 60);
    return b;
}
std::wstring ClockFloor(double s) {
    int t = (int)std::max(0.0, s);
    wchar_t b[32];
    if (t >= 3600) swprintf_s(b, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else swprintf_s(b, L"%d:%02d", t / 60, t % 60);
    return b;
}

// ================= desenho
void Text(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (!tf || s.empty()) return;
    br->SetColor(c);
    dc->DrawText(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Glyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, wchar_t g, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F c) {
    wchar_t s[2] = { g, 0 };
    br->SetColor(c);
    dc->DrawText(s, 1, tf, r, br);
}
void Sym(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const wchar_t* name, D2D1_RECT_F r, D2D1_COLOR_F c) {
    br->SetColor(c);
    simbolo::Draw(dc, name, r, br);
}
D2D1_RECT_F Around(float cx, float cy, float w, float h) { return D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2); }
void AddHit(HitId id, D2D1_RECT_F r, int arg = 0) { g_hits.push_back({ id, r, arg }); }
bool Hover(HitId id, int arg = 0) { return g_hoverHit == id && (g_hoverArg == arg || id != H_CLIP && id != H_SHELF && id != H_TAB && id != H_PRESET); }

void DrawArt(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float rad) {
    if (g_art) {
        D2D1_SIZE_F bs = g_art->GetSize();
        float side = std::min(bs.width, bs.height);
        D2D1_RECT_F src = D2D1::RectF((bs.width - side) / 2, (bs.height - side) / 2, (bs.width + side) / 2, (bs.height + side) / 2);
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);  // geometria da fábrica do contexto (D2DERR_WRONG_FACTORY)
        ID2D1RoundedRectangleGeometry* gg = nullptr;
        if (f) { f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, rad, rad), &gg); f->Release(); }
        if (gg) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), gg, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
        dc->DrawBitmap(g_art, r, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, src);
        if (gg) { dc->PopLayer(); gg->Release(); }
        return;
    }
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.14f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    float s = r.right - r.left;
    Sym(dc, br, L"music.note", D2D1::RectF(r.left + s * 0.28f, r.top + s * 0.25f, r.right - s * 0.28f, r.bottom - s * 0.25f), D2D1::ColorF(1, 1, 1, 0.6f));
}

void DrawBars(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r) {  // 4 barrinhas encostadas à direita de r
    const float bw = 3, gap = 2.2f;
    float total = 4 * bw + 3 * gap, x = r.right - total, cy = (r.top + r.bottom) / 2, hmax = r.bottom - r.top;
    br->SetColor(g_m.tint);
    for (int i = 0; i < 4; i++) {
        float h = std::max(bw, g_bars[i] * hmax);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy - h / 2, x + bw, cy + h / 2), bw / 2, bw / 2), br);
        x += bw + gap;
    }
}

void DrawPrivDot(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F b) {
    if (!g_priv) return;
    float cy = (b.top + b.bottom) / 2, x = b.right - 13;
    br->SetColor(g_priv & 2 ? GREEN : ORANGE);  // câmera ganha (verde); só o microfone, laranja
    dc->FillEllipse(D2D1::Ellipse({ x, cy }, 3.2f, 3.2f), br);
}

// caixa do conteúdo: tamanho final do modo, centrada no topo da janela (com o deslocamento da pílula)
D2D1_RECT_F BoxOf(Mode m) {
    Size s = SizeOf(m);
    float top = TopOff();
    return D2D1::RectF((WMAX - s.w) / 2, top, (WMAX + s.w) / 2, top + s.h);
}

void DrawProgress(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F tr, float f, D2D1_COLOR_F c) {
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.2f));
    float r = (tr.bottom - tr.top) / 2;
    dc->FillRoundedRectangle(D2D1::RoundedRect(tr, r, r), br);
    if (f > 0.001f) {
        br->SetColor(c);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, tr.top, tr.left + std::max(2 * r, f * (tr.right - tr.left)), tr.bottom), r, r), br);
    }
}

void CircleBtn(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, HitId id, bool always = false, int arg = 0) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, rad = std::min(r.right - r.left, r.bottom - r.top) / 2;
    bool hv = Hover(id, arg);
    if (hv || always) {
        br->SetColor(D2D1::ColorF(1, 1, 1, (g_pressHit == id ? 0.22f : hv ? 0.13f : 0.08f)));
        dc->FillEllipse(D2D1::Ellipse({ cx, cy }, rad, rad), br);
    }
}

// ---- acabamento por tema (Sequoia, Tahoe, Golden Gate, Leopard)
// O notch é sempre preto (como o entalhe de verdade); o que muda por tema é o desenho dos botões, das fichas e da
// barra de tempo, e no estilo "ilha" o próprio corpo: Sequoia = preto com contorno fino e brilho; Tahoe = vidro
// escuro com a borda especular do Liquid Glass; Golden Gate = o mesmo com cantos maiores e brilho mais forte;
// Leopard = o painel HUD do Mac OS X 10.5 (cinza-escuro translúcido, verniz em cima, contorno claro, Lucida Grande,
// barra de progresso em gel azul).
enum Theme { TH_SEQUOIA, TH_TAHOE, TH_GOLDEN, TH_LEOPARD };
Theme g_theme = TH_SEQUOIA;
Theme ThemeNow() { return liquid::Leopard() ? TH_LEOPARD : liquid::Golden() ? TH_GOLDEN : liquid::On() ? TH_TAHOE : TH_SEQUOIA; }
bool Glassy() { return g_theme == TH_TAHOE || g_theme == TH_GOLDEN; }
float ChipR(float h) { return g_theme == TH_LEOPARD ? 5.f : g_theme == TH_GOLDEN ? h / 2 : g_theme == TH_TAHOE ? h / 2 : 9.f; }
D2D1_COLOR_F Accent() {
    if (g_theme == TH_LEOPARD) return D2D1::ColorF(0.33f, 0.6f, 1.f, 1);
    if (Glassy()) { D2D1_COLOR_F a = ui::Accent(); return D2D1::ColorF(a.r * 0.7f + 0.3f, a.g * 0.7f + 0.3f, a.b * 0.7f + 0.3f, 1); }
    return WHITE;
}

// ficha/botão de fundo: preenchimento do tema (+ a borda de vidro no Tahoe/Golden Gate, o verniz no Leopard)
void Chip(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float rad, float level) {  // level 0 repouso, 1 ponteiro, 2 apertado
    float a = level >= 2 ? 0.22f : level >= 1 ? 0.15f : 0.09f;
    br->SetColor(D2D1::ColorF(1, 1, 1, a));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    if (Glassy()) liquid::Rim(dc, r, rad, true, g_theme == TH_GOLDEN ? 0.9f : 0.6f, level >= 1 ? 1.f : 0.f);
    else if (g_theme == TH_LEOPARD) {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.10f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, (r.top + r.bottom) / 2), std::max(0.f, rad - 1), std::max(0.f, rad - 1)), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, rad), br, 1);
    }
}

void DrawProgressT(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F tr, float f) {
    if (g_theme != TH_LEOPARD) { DrawProgress(dc, br, tr, f, D2D1::ColorF(1, 1, 1, 0.92f)); return; }
    // Leopard: trilho escuro afundado e o gel azul com o verniz
    float r = (tr.bottom - tr.top) / 2;
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(tr, r, r), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.22f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(tr, r, r), br, 0.75f);
    if (f > 0.001f) {
        D2D1_RECT_F fr = D2D1::RectF(tr.left, tr.top, tr.left + std::max(2 * r, f * (tr.right - tr.left)), tr.bottom);
        br->SetColor(D2D1::ColorF(0.2f, 0.48f, 0.95f, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(fr, r, r), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fr.left, fr.top, fr.right, (fr.top + fr.bottom) / 2), r / 2, r / 2), br);
    }
}

// ---- abas
const wchar_t TAB_GLYPH[TAB_N] = { 0xE80F, 0xE916, 0xE77F, 0xE7B8, 0xE9D9 };
const wchar_t* TAB_NAME[TAB_N] = { L"Início", L"Timer", L"Transferência", L"Estante", L"Sistema" };

// b = o corpo (no notch, sem as orelhas); o cabeçalho fica na faixa da barra de menus, como as abas do Atoll
void DrawHeader(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F b) {
    float H = HeadH(), cy = b.top + H / 2 + (Notch() ? 0 : 2);
    float x = b.left + 12;
    for (int i = 0; i < TAB_N; i++) {
        D2D1_RECT_F r = D2D1::RectF(x, cy - 9, x + 24, cy + 9);
        bool on = g_tab == i;
        if (on && g_theme == TH_LEOPARD) {  // o segmento escolhido em gel azul Aqua
            float rr = 5;
            br->SetColor(D2D1::ColorF(0.16f, 0.42f, 0.92f, 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rr, rr), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.30f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, (r.top + r.bottom) / 2), rr - 1, rr - 1), br);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, rr, rr), br, 0.75f);
        } else if (on || Hover(H_TAB, i)) Chip(dc, br, r, ChipR(18), on ? 1.f : 0.f);
        Glyph(dc, br, TAB_GLYPH[i], g_tfIconS, r, on ? WHITE : GRAY);
        AddHit(H_TAB, r, i);
        x += 27;
    }
    D2D1_RECT_F gr = D2D1::RectF(b.right - 12 - 22, cy - 9, b.right - 12, cy + 9);
    if (Hover(H_GEAR)) Chip(dc, br, gr, ChipR(18), 1);
    Glyph(dc, br, 0xE713, g_tfIconS, gr, GRAY);
    AddHit(H_GEAR, gr);
}

// o player do Atoll (modo minimalista), nas proporções da captura do projeto: capa 40 pt a 14 pt da borda,
// título 12,5 / artista 11, barrinhas à direita na altura do título; linha do tempo 3 pt com o restante na cor da
// capa; 5 botões a 50 pt um do outro
void DrawHome(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F b, float y0) {
    const float L = b.left + 14, R = b.right - 16;
    D2D1_RECT_F art = D2D1::RectF(L, y0, L + 40, y0 + 40);
    DrawArt(dc, br, art, g_theme == TH_LEOPARD ? 4.f : g_theme == TH_GOLDEN ? 11.f : 9.f);
    if (g_theme == TH_LEOPARD) {
        // o reflexo do Cover Flow embaixo da capa: espelhada, sumindo em 10 pt
        ID2D1GradientStopCollection* gs = nullptr;
        D2D1_GRADIENT_STOP st[] = { { 0, D2D1::ColorF(0, 0, 0, 0.35f) }, { 1, D2D1::ColorF(0, 0, 0, 0) } };
        dc->CreateGradientStopCollection(st, 2, &gs);
        ID2D1LinearGradientBrush* mask = nullptr;
        if (gs) dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, art.bottom + 1 }, { 0, art.bottom + 11 }), gs, &mask);
        if (mask) {
            D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(art.left, art.bottom + 1, art.right, art.bottom + 11), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1, mask), nullptr);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(1, -1, { 0, art.bottom + 0.5f }) * old);
            DrawArt(dc, br, art, 4);
            dc->SetTransform(old);
            dc->PopLayer();
            mask->Release();
        }
        if (gs) gs->Release();
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.25f));  // moldura fina da capa, como os ícones do 10.5
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(art.left + 0.5f, art.top + 0.5f, art.right - 0.5f, art.bottom - 0.5f), 3.5f, 3.5f), br, 1);
    }
    AddHit(H_ART, art);
    bool has = g_m.has;
    Text(dc, br, has ? g_m.title : Tr(L"Nada tocando"), g_tfTitle, D2D1::RectF(art.right + 9, y0 + 3, R - 24, y0 + 20), WHITE);
    if (has) Text(dc, br, g_m.artist, g_tfSub, D2D1::RectF(art.right + 9, y0 + 20, R - 24, y0 + 35), GRAY);
    if (has) DrawBars(dc, br, D2D1::RectF(R - 16, y0 + 6, R, y0 + 18));
    float ty = y0 + 54;
    double pos = 0, len = g_m.len;
    bool tl = has && len > 1;
    if (tl) pos = std::clamp(g_m.pos + (g_m.playing ? (anim::Now() - g_m.at) / 1000.0 : 0), 0.0, len);
    D2D1_RECT_F tr = D2D1::RectF(L + 36, ty - 1.5f, R - 38, ty + 1.5f);
    Text(dc, br, tl ? ClockFloor(pos) : L"", g_tfSmall, D2D1::RectF(L, ty - 7, tr.left - 4, ty + 7), WHITE);
    Text(dc, br, tl ? L"-" + ClockFloor(len - pos) : L"", g_tfSmallR, D2D1::RectF(tr.right + 4, ty - 7, R, ty + 7), g_theme == TH_LEOPARD ? Accent() : g_m.tint);
    DrawProgressT(dc, br, Hover(H_SEEK) ? D2D1::RectF(tr.left, ty - 2.5f, tr.right, ty + 2.5f) : tr, tl ? (float)(pos / len) : 0);
    if (tl) AddHit(H_SEEK, D2D1::RectF(tr.left, ty - 7, tr.right, ty + 7));
    float cy = y0 + 82, cx = (b.left + b.right) / 2;
    D2D1_COLOR_F ink = has ? WHITE : GRAY2;
    struct B { HitId id; float x; } bs[] = { { H_SHUF, cx - 100 }, { H_PREV, cx - 50 }, { H_PLAY, cx }, { H_NEXT, cx + 50 }, { H_OUT, cx + 100 } };
    for (auto& bt : bs) {
        float s = bt.id == H_PLAY ? 32.f : 28.f;
        D2D1_RECT_F r = Around(bt.x, cy, s, s);
        bool live = has || bt.id == H_OUT;
        if (live && (Hover(bt.id) || g_pressHit == bt.id)) Chip(dc, br, r, s / 2, g_pressHit == bt.id ? 2.f : 1.f);
        switch (bt.id) {
        case H_SHUF: Glyph(dc, br, 0xE8B1, g_tfIconS, r, has && g_m.shuffle ? (g_theme == TH_SEQUOIA ? g_m.tint : Accent()) : has && g_m.canShuffle ? WHITE : GRAY2); break;
        case H_OUT:  Glyph(dc, br, 0xE7F4, g_tfIconS, r, WHITE); break;  // a saída de som (o menu Som)
        case H_PLAY:
            if (g_m.playing) Sym(dc, br, L"pause.fill", Around(bt.x, cy, 13, 17), ink);
            else Sym(dc, br, L"play.fill", Around(bt.x + 1.5f, cy, 15, 17), ink);
            break;
        default: {
            D2D1_RECT_F sr = Around(bt.x, cy, 21, 12);
            if (bt.id == H_PREV) {  // não há "backward.fill" no pacote: o "forward.fill" espelhado
                D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
                dc->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, { bt.x, cy }) * old);
                Sym(dc, br, L"forward.fill", sr, ink);
                dc->SetTransform(old);
            } else Sym(dc, br, L"forward.fill", sr, ink);
        }
        }
        if (live) AddHit(bt.id, r);
    }
}

void Ring(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_POINT_2F c, float rad, float f, float stroke) {
    br->SetColor(D2D1::ColorF(1.f, 0.62f, 0.04f, 0.22f));
    dc->DrawEllipse(D2D1::Ellipse(c, rad, rad), br, stroke);
    if (f <= 0.001f) return;
    ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
    ID2D1PathGeometry* g = nullptr;
    if (fac && SUCCEEDED(fac->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
        float a0 = -3.1415927f / 2, a1 = a0 + 6.2831853f * std::min(f, 0.9999f);
        s->BeginFigure({ c.x + rad * std::cos(a0), c.y + rad * std::sin(a0) }, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc(D2D1::ArcSegment({ c.x + rad * std::cos(a1), c.y + rad * std::sin(a1) }, D2D1::SizeF(rad, rad), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                   f > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        ID2D1StrokeStyle* ss = nullptr;
        fac->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &ss);
        br->SetColor(ORANGE);
        dc->DrawGeometry(g, br, stroke, ss);
        if (ss) ss->Release();
        g->Release();
    }
    if (fac) fac->Release();
}

void DrawTimer(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F b, float y0) {
    const float L = b.left + 14, R = b.right - 14;
    if (!g_tRun) {
        static const int mins[6] = { 1, 3, 5, 10, 15, 30 };
        float gap = 8, cw = (R - L - 2 * gap) / 3, ch = 32;
        for (int i = 0; i < 6; i++) {
            float x = L + (i % 3) * (cw + gap), y = y0 + 6 + (i / 3) * (ch + gap);
            D2D1_RECT_F r = D2D1::RectF(x, y, x + cw, y + ch);
            Chip(dc, br, r, ChipR(ch), Hover(H_PRESET, mins[i]) ? (g_pressHit == H_PRESET ? 2.f : 1.f) : 0.f);
            Text(dc, br, std::to_wstring(mins[i]) + L" min", g_tfChip, r, WHITE);
            AddHit(H_PRESET, r, mins[i]);
        }
        return;
    }
    // correndo: anel com o que falta, tempo grande, pausar/parar/+1 min
    double left = TimerLeft();
    D2D1_POINT_2F c = { L + 30, y0 + 38 };
    Ring(dc, br, c, 28, g_tTotal > 0 ? (float)(left / g_tTotal) : 0, 5);
    Sym(dc, br, L"timer", Around(c.x, c.y, 18, 18), ORANGE);
    float x = c.x + 44;
    Text(dc, br, Clock(left / 1000), g_tfBig, D2D1::RectF(x, y0 + 4, R, y0 + 42), g_tPaused ? GRAY : WHITE);
    float by = y0 + 62;
    D2D1_RECT_F rp = D2D1::RectF(x, by - 14, x + 28, by + 14), rs = D2D1::RectF(x + 36, by - 14, x + 64, by + 14), r1 = D2D1::RectF(x + 72, by - 13, x + 130, by + 13);
    Chip(dc, br, rp, 14, Hover(H_TPAUSE) ? 1.f : 0.f);
    if (g_tPaused) Sym(dc, br, L"play.fill", Around(x + 15, by, 10, 11), ORANGE);
    else Sym(dc, br, L"pause.fill", Around(x + 14, by, 9, 11), ORANGE);
    Chip(dc, br, rs, 14, Hover(H_TSTOP) ? 1.f : 0.f);
    Glyph(dc, br, 0xE711, g_tfIconS, rs, WHITE);
    Chip(dc, br, r1, ChipR(26), Hover(H_TPLUS) ? 1.f : 0.f);
    Text(dc, br, L"+1 min", g_tfChip, r1, WHITE);
    AddHit(H_TPAUSE, rp); AddHit(H_TSTOP, rs); AddHit(H_TPLUS, r1);
}

void DrawClip(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F b, float y0) {
    const float L = b.left + 10, R = b.right - 10;
    if (g_clips.empty()) {
        Glyph(dc, br, 0xE77F, g_tfIconL, D2D1::RectF(L, y0 + 14, R, y0 + 42), GRAY2);
        Text(dc, br, Tr(L"O que você copiar aparece aqui"), g_tfSmallC, D2D1::RectF(L, y0 + 46, R, y0 + 62), GRAY);
        return;
    }
    float rowH = 23, y = y0;
    for (int i = 0; i < (int)g_clips.size() && i < 4; i++, y += rowH) {
        D2D1_RECT_F r = D2D1::RectF(L, y, R - (i == 0 ? 44 : 0), y + rowH - 2);
        if (Hover(H_CLIP, i)) Chip(dc, br, r, ChipR(rowH - 2) * 0.8f, 1);
        std::wstring s = g_clips[i];
        for (auto& c : s) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        bool flash = g_flashClip == i && anim::Now() - g_flashAt < 1200;
        Glyph(dc, br, flash ? 0xE73E : 0xE8C8, g_tfIconS, D2D1::RectF(r.left + 4, r.top, r.left + 22, r.bottom), flash ? GREEN : GRAY);
        Text(dc, br, flash ? Tr(L"Copiado") : s, g_tfSub, D2D1::RectF(r.left + 27, r.top, r.right - 6, r.bottom), flash ? GREEN : WHITE);
        AddHit(H_CLIP, r, i);
    }
    D2D1_RECT_F cl = D2D1::RectF(R - 42, y0, R, y0 + 21);
    Text(dc, br, Tr(L"Limpar"), g_tfSmallC, cl, Hover(H_CLIPCLR) ? WHITE : GRAY);
    AddHit(H_CLIPCLR, cl);
}

void DrawShelf(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F b, float y0) {
    const float L = b.left + 12, R = b.right - 12;
    D2D1_RECT_F box = D2D1::RectF(L, y0 + 2, R, y0 + BODY_H - 14);
    if (g_shelf.empty() || g_dragIn) {
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        ID2D1StrokeStyle* ss = nullptr;
        float dash[] = { 4, 3 };
        if (f) { f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM), dash, 2, &ss); f->Release(); }
        float rr = g_theme == TH_LEOPARD ? 6.f : 14.f;
        if (g_dragIn) { br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(box, rr, rr), br); }
        br->SetColor(D2D1::ColorF(1, 1, 1, g_dragIn ? 0.6f : 0.25f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(box, rr, rr), br, 1.5f, ss);
        if (ss) ss->Release();
        float cy = (box.top + box.bottom) / 2;
        Glyph(dc, br, 0xE7B8, g_tfIconL, D2D1::RectF(L, cy - 24, R, cy), g_dragIn ? WHITE : GRAY);
        Text(dc, br, Tr(L"Arraste arquivos para cá"), g_tfSmallC, D2D1::RectF(L, cy + 2, R, cy + 18), g_dragIn ? WHITE : GRAY);
        return;
    }
    float tw = 56, x = L;
    int n = (int)g_shelf.size(), shown = std::min(n, (int)((R - L) / tw));
    for (int i = n - shown; i < n; i++, x += tw) {  // os mais novos
        D2D1_RECT_F r = D2D1::RectF(x, y0 + 2, x + tw - 4, y0 + 70);
        if (Hover(H_SHELF, i)) Chip(dc, br, r, g_theme == TH_LEOPARD ? 5.f : 10.f, 1);
        if (ID2D1Bitmap* bmp = ShelfIcon(dc, g_shelf[i])) {
            D2D1_SIZE_F s = bmp->GetSize();
            float k = std::min(34 / s.width, 34 / s.height);
            dc->DrawBitmap(bmp, Around((r.left + r.right) / 2, r.top + 22, s.width * k, s.height * k), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else Glyph(dc, br, 0xE8A5, g_tfIconL, Around((r.left + r.right) / 2, r.top + 22, 34, 34), GRAY);
        std::wstring name = g_shelf[i].substr(g_shelf[i].find_last_of(L'\\') + 1);
        Text(dc, br, name, g_tfSmallC, D2D1::RectF(r.left + 2, r.top + 44, r.right - 2, r.top + 60), WHITE);
        AddHit(H_SHELF, r, i);
    }
    D2D1_RECT_F cl = D2D1::RectF(R - 50, y0 + BODY_H - 30, R, y0 + BODY_H - 14);
    Text(dc, br, Tr(L"Limpar"), g_tfSmallR, cl, Hover(H_SHELFCLR) ? WHITE : GRAY);
    AddHit(H_SHELFCLR, cl);
}

void Spark(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::deque<float>& d, D2D1_RECT_F r, float mx, D2D1_COLOR_F c) {
    if (d.size() < 2) return;
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1PathGeometry* g = nullptr;
    if (f && SUCCEEDED(f->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr; g->Open(&s);
        float step = (r.right - r.left) / 39;
        float x0 = r.right - step * (d.size() - 1);
        auto Y = [&](float v) { return r.bottom - std::clamp(v / mx, 0.f, 1.f) * (r.bottom - r.top); };
        s->BeginFigure({ x0, r.bottom }, D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 0; i < d.size(); i++) s->AddLine({ x0 + i * step, Y(d[i]) });
        s->AddLine({ r.right, r.bottom });
        s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
        br->SetColor(D2D1::ColorF(c.r, c.g, c.b, 0.25f));
        dc->FillGeometry(g, br);
        g->Release();
        br->SetColor(c);
        for (size_t i = 1; i < d.size(); i++) dc->DrawLine({ x0 + (i - 1) * step, Y(d[i - 1]) }, { x0 + i * step, Y(d[i]) }, br, 1.3f);
    }
    if (f) f->Release();
}

void DrawStats(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F b, float y0) {
    const float L = b.left + 10, R = b.right - 10, gap = 6;
    float cw = (R - L - 2 * gap) / 3;
    struct C { const wchar_t* name; std::wstring val; const std::deque<float>* d; float mx; D2D1_COLOR_F c; } cs[3] = {
        { L"CPU", std::to_wstring((int)std::lround(g_st.cpuNow * 100)) + L"%", &g_st.cpu, 1, D2D1::ColorF(0.35f, 0.65f, 1.f, 1) },
        { L"Memória", std::to_wstring((int)std::lround(g_st.memNow * 100)) + L"%", &g_st.mem, 1, GREEN },
        { L"Rede", L"↓ " + Rate(g_st.down), &g_st.net, 1, ORANGE },
    };
    float netMax = 50e3f;
    for (float v : g_st.net) netMax = std::max(netMax, v);
    cs[2].mx = netMax * 1.1f;
    for (int i = 0; i < 3; i++) {
        float x = L + i * (cw + gap);
        D2D1_RECT_F r = D2D1::RectF(x, y0 + 2, x + cw, y0 + BODY_H - 12);
        Chip(dc, br, r, g_theme == TH_LEOPARD ? 6.f : g_theme == TH_GOLDEN ? 16.f : 12.f, 0);
        Text(dc, br, Tr(cs[i].name), g_tfSmall, D2D1::RectF(r.left + 9, r.top + 6, r.right - 6, r.top + 20), GRAY);
        Text(dc, br, cs[i].val, i == 2 ? g_tfSub : g_tfVal, D2D1::RectF(r.left + 9, r.top + 20, r.right - 6, r.top + 42), WHITE);
        if (i == 2) Text(dc, br, L"↑ " + Rate(g_st.up), g_tfSmall, D2D1::RectF(r.left + 9, r.top + 36, r.right - 6, r.top + 50), GRAY);
        Spark(dc, br, *cs[i].d, D2D1::RectF(r.left + 8, r.top + 54, r.right - 8, r.bottom - 8), cs[i].mx, cs[i].c);
    }
}

// t = ms desde a troca de modo (aparecer: o cabeçalho primeiro, o corpo logo depois, descendo um tiquinho)
void DrawContent(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, Mode m, double t) {
    D2D1_RECT_F b = BoxOf(m);
    float inset = Notch() ? SizeOf(m).rt : 0;  // no notch, as orelhas ficam fora do corpo
    b.left += inset; b.right -= inset;
    float closedH = Notch() ? g_barPt : PILL_H, cy = b.top + closedH / 2;
    float wingL = b.left + (Notch() ? 6 : 4), wingR = b.right - (Notch() ? 8 : 7) - (g_priv ? 18 : 0);
    D2D1_RECT_F closedBox = D2D1::RectF(b.left, b.top, b.right - (Notch() ? 2 : 0), b.top + closedH);
    switch (m) {
    case M_IDLE:
        DrawPrivDot(dc, br, closedBox);
        break;
    case M_MEDIA:
    case M_TIMER: {
        if (m == M_MEDIA) DrawArt(dc, br, D2D1::RectF(wingL, cy - 8, wingL + 16, cy + 8), g_theme == TH_LEOPARD ? 2.5f : 4.f);
        else Sym(dc, br, L"timer", D2D1::RectF(wingL, cy - 7.5f, wingL + 15, cy + 7.5f), ORANGE);
        if (g_tRun) Text(dc, br, Clock(TimerLeft() / 1000), g_tfWingR, D2D1::RectF(wingR - 60, cy - 8, wingR, cy + 8), ORANGE);
        else DrawBars(dc, br, D2D1::RectF(wingR - 18, cy - 6, wingR, cy + 6));
        DrawPrivDot(dc, br, closedBox);
        break;
    }
    case M_HUD: {
        float rr = b.right - (Notch() ? 10 : 11);
        if (g_hud == HUD_VOL) {
            const float v = g_mute ? 0 : g_vol;
            const wchar_t* s = g_mute || v < 0.001f ? L"speaker.slash.fill" : v < 0.34f ? L"speaker.wave.1.fill" : v < 0.67f ? L"speaker.wave.2.fill" : L"speaker.wave.3.fill";
            Sym(dc, br, s, D2D1::RectF(wingL + 2, cy - 5.5f, wingL + 19, cy + 5.5f), WHITE);
            DrawProgressT(dc, br, D2D1::RectF(rr - 84, cy - 2, rr, cy + 2), v);
        } else if (g_hud == HUD_CAPS) {
            Glyph(dc, br, 0xE765, g_tfIconS, D2D1::RectF(wingL, cy - 9, wingL + 22, cy + 9), g_caps ? GREEN : GRAY);
            Text(dc, br, g_caps ? L"Caps Lock" : Tr(L"Caps Lock desligado"), g_tfWingR, D2D1::RectF(rr - 130, cy - 8, rr, cy + 8), g_caps ? GREEN : GRAY);
        } else {
            Sym(dc, br, g_batAC ? L"bolt.fill" : L"battery.50percent", D2D1::RectF(wingL + 2, cy - 6.5f, wingL + (g_batAC ? 12 : 24), cy + 6.5f), g_batAC ? GREEN : g_batPct <= 20 ? RED : WHITE);
            Text(dc, br, std::to_wstring(g_batPct) + L"%", g_tfWingR, D2D1::RectF(rr - 60, cy - 8, rr, cy + 8), g_batAC ? GREEN : g_batPct <= 20 ? RED : WHITE);
        }
        break;
    }
    case M_PEEK: {
        float top = b.top + (Notch() ? g_barPt : 0), h = b.bottom - top;
        float icy = top + h / 2 - (Notch() ? 3 : 0);
        D2D1_RECT_F ic = Around(b.left + 14 + 16, icy, 32, 32);
        if (g_peek.icon) g_peek.icon(dc, br, ic);
        else {
            br->SetColor(D2D1::ColorF(g_peek.color.r, g_peek.color.g, g_peek.color.b, 0.22f));
            dc->FillEllipse(D2D1::Ellipse({ (ic.left + ic.right) / 2, icy }, 16, 16), br);
            Glyph(dc, br, g_peek.glyph, g_tfIconS, ic, g_peek.color);
        }
        float x = ic.right + 10, rr = b.right - 16;
        bool sub = !g_peek.sub.empty();
        float ty = sub ? icy - 16 : icy - 8;
        Text(dc, br, g_peek.title, g_tfTitle, D2D1::RectF(x, ty, rr, ty + 17), WHITE);
        if (sub) Text(dc, br, g_peek.sub, g_tfSub, D2D1::RectF(x, ty + 17, rr, ty + 32), GRAY);
        break;
    }
    case M_OPEN: {
        D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
        float eh = anim::EaseOut(std::clamp((float)((t - 30) / 170), 0.f, 1.f));
        if (eh < 1) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), eh), nullptr);
        DrawHeader(dc, br, b);
        if (eh < 1) dc->PopLayer();
        float y0 = b.top + HeadH() + (Notch() ? 2 : 4);
        // o corpo entra descendo 8 pt e crescendo de 96% (o "blurReplace" do Atoll sem o desfoque) e, ao trocar de aba,
        // deslizando para o lado
        float eb = anim::EaseOut(std::clamp((float)((t - 70) / 230), 0.f, 1.f));
        float et = anim::EaseOut(std::clamp((float)((anim::Now() - g_tabAt) / 220), 0.f, 1.f));
        float e = std::min(eb, et);
        if (e < 1) {
            float s = 0.96f + 0.04f * eb;
            D2D1_POINT_2F c = { (b.left + b.right) / 2, y0 };
            dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * D2D1::Matrix3x2F::Translation(g_tabDir * (1 - et) * 22, -(1 - eb) * 8) * old);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), e), nullptr);
        }
        switch (g_tab) {
        case TAB_HOME: DrawHome(dc, br, b, y0); break;
        case TAB_TIMER: DrawTimer(dc, br, b, y0); break;
        case TAB_CLIP: DrawClip(dc, br, b, y0); break;
        case TAB_SHELF: DrawShelf(dc, br, b, y0); break;
        case TAB_STATS: DrawStats(dc, br, b, y0); break;
        default: break;
        }
        if (e < 1) { dc->PopLayer(); dc->SetTransform(old); }
        break;
    }
    default: break;
    }
}

// a forma: no notch, o entalhe com as orelhas côncavas em cima e cantos redondos embaixo; senão, a pílula/cartão
ID2D1Geometry* Shape(ID2D1Factory* f, D2D1_RECT_F p, float rt, float rb) {
    if (!Notch()) {
        ID2D1RoundedRectangleGeometry* g = nullptr;
        float r = std::min({ rb, (p.bottom - p.top) / 2, (p.right - p.left) / 2 });
        f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(p, r, r), &g);
        return g;
    }
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(f->CreatePathGeometry(&g))) return nullptr;
    float L = p.left, R = p.right, B = p.bottom;
    rt = std::clamp(rt, 0.f, (R - L) / 4);
    rb = std::clamp(rb, 0.f, std::min((B - p.top - rt), (R - L - 2 * rt) / 2));
    ID2D1GeometrySink* s = nullptr; g->Open(&s);
    s->BeginFigure({ L, p.top }, D2D1_FIGURE_BEGIN_FILLED);
    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ L + rt, p.top }, { L + rt, p.top + rt }));
    s->AddLine({ L + rt, B - rb });
    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ L + rt, B }, { L + rt + rb, B }));
    s->AddLine({ R - rt - rb, B });
    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ R - rt, B }, { R - rt, B - rb }));
    s->AddLine({ R - rt, p.top + rt });
    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ R - rt, p.top }, { R, p.top }));
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close(); s->Release();
    return g;
}

// contorno do painel do Leopard: desce pela esquerda, contorna a base e sobe pela direita (sem a borda de cima,
// que encosta na barra de menus)
ID2D1PathGeometry* Outline(ID2D1Factory* f, D2D1_RECT_F p, float rb) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(f->CreatePathGeometry(&g))) return nullptr;
    rb = std::clamp(rb, 0.f, std::min(p.bottom - p.top, (p.right - p.left) / 2));
    ID2D1GeometrySink* s = nullptr; g->Open(&s);
    s->BeginFigure({ p.left, p.top }, D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddLine({ p.left, p.bottom - rb });
    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ p.left, p.bottom }, { p.left + rb, p.bottom }));
    s->AddLine({ p.right - rb, p.bottom });
    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ p.right, p.bottom }, { p.right, p.bottom - rb }));
    s->AddLine({ p.right, p.top });
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close(); s->Release();
    return g;
}

D2D1_RECT_F ShapeRect() {
    float w = std::max(1.f, g_w.x), h = std::max(1.f, g_h.x), top = TopOff();
    return D2D1::RectF((WMAX - w) / 2, top, (WMAX + w) / 2, top + h);
}

void SetRegion() {
    float sc = Sc();
    D2D1_RECT_F p = ShapeRect();
    float m = Notch() && g_theme != TH_LEOPARD ? 1.f : 4.f;
    RECT r{ (LONG)std::floor((p.left - m) * sc), (LONG)std::floor(std::max(0.f, p.top - m) * sc), (LONG)std::ceil((p.right + m) * sc), (LONG)std::ceil((p.bottom + m) * sc) };
    if (g_a.x < 0.05f && g_a.t == 0) r = { 0, 0, 0, 0 };  // sumindo: o clique já passa
    if (EqualRect(&r, &g_rgn)) return;
    g_rgn = r;
    SetWindowRgn(g_wnd, CreateRectRgn(r.left, r.top, r.right, r.bottom), FALSE);
}

void Paint() {
    if (!g_wnd || !g_shown) return;
    if (g_m.artGen != g_artGenUi) {
        g_artGenUi = g_m.artGen;
        if (g_art) { g_art->Release(); g_art = nullptr; }
        if (ID2D1DeviceContext* gdc = ui::Gpu()) g_art = ui::Bitmap(gdc, g_m.aw, g_m.ah, g_m.art);
    }
    float sc = Sc();
    auto* dc = ui::GpuBegin(g_s, (int)std::lround(WMAX * sc), (int)std::lround(HMAX * sc), sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &br);
    if (!br) { ui::GpuEnd(g_s); return; }
    D2D1_RECT_F p = ShapeRect();
    float alpha = std::clamp(g_a.x, 0.f, 1.f);
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1Geometry* geo = f ? Shape(f, p, std::max(0.f, g_rt.x), std::max(0.f, g_rb.x)) : nullptr;
    float rad = std::min({ std::max(0.f, g_rb.x), (p.bottom - p.top) / 2, (p.right - p.left) / 2 });
    const bool pill = !Notch();
    const bool leoNotch = Notch() && g_theme == TH_LEOPARD;
    if (leoNotch && alpha > 0.01f && geo) {
        // sombra do Leopard embaixo do painel (os menus e HUDs do 10.5 têm sombra larga e macia)
        for (int i = 5; i >= 1; i--) {
            float e = i * 1.4f;
            D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0, e) * old);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.06f * alpha * (6 - i) / 5));
            dc->DrawGeometry(geo, br, e * 1.6f);
            dc->FillGeometry(geo, br);
            dc->SetTransform(old);
        }
    }
    if (pill && alpha > 0.01f) {
        // sombra suave em volta da pílula (anéis cada vez mais largos e mais fracos); no Leopard, a do HUD, mais forte
        float k = g_theme == TH_LEOPARD ? 1.6f : 1.f;
        for (int i = 4; i >= 1; i--) {
            float e = i * 1.6f;
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.07f * k * alpha * (5 - i) / 4));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p.left - e, p.top - e * 0.5f, p.right + e, p.bottom + e * 1.3f), rad + e, rad + e), br);
        }
    }
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), nullptr);
    // o corpo: o notch é sempre preto; a pílula muda com o tema
    D2D1_COLOR_F fill = D2D1::ColorF(0, 0, 0, 1);
    if (pill && Glassy()) fill = D2D1::ColorF(0.055f, 0.055f, 0.065f, 0.95f);
    if (pill && g_theme == TH_LEOPARD) fill = D2D1::ColorF(0.10f, 0.10f, 0.11f, 0.90f);
    br->SetColor(fill);
    if (leoNotch) {
        // grafite do HUD: degradê de cima (mais claro) para baixo, e o verniz Aqua na faixa da barra de menus
        ID2D1GradientStopCollection* gs = nullptr;
        D2D1_GRADIENT_STOP st[] = { { 0, D2D1::ColorF(0.24f, 0.24f, 0.26f, 0.96f) }, { 0.22f, D2D1::ColorF(0.13f, 0.13f, 0.14f, 0.95f) }, { 1, D2D1::ColorF(0.07f, 0.07f, 0.08f, 0.95f) } };
        dc->CreateGradientStopCollection(st, 3, &gs);
        ID2D1LinearGradientBrush* lg = nullptr;
        if (gs) dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, p.top }, { 0, std::max(p.top + g_barPt * 1.4f, p.bottom) }), gs, &lg);
        if (lg) { dc->FillRectangle(D2D1::RectF(p.left, p.top - 1, p.right, p.bottom), lg); lg->Release(); }
        else dc->FillRectangle(D2D1::RectF(p.left, p.top - 1, p.right, p.bottom), br);
        if (gs) gs->Release();
        float band = std::min(g_barPt, p.bottom - p.top);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.11f));
        dc->FillRectangle(D2D1::RectF(p.left, p.top, p.right, p.top + band / 2), br);  // verniz: metade de cima da faixa, corte nítido
        if (p.bottom - p.top > g_barPt + 6) {  // aberto: a faixa vira a barra de título do HUD, com o filete embaixo
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
            dc->FillRectangle(D2D1::RectF(p.left, p.top + band, p.right, p.top + band + 1), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.09f));
            dc->FillRectangle(D2D1::RectF(p.left, p.top + band + 1, p.right, p.top + band + 2), br);
        }
    } else dc->FillRectangle(D2D1::RectF(p.left, p.top - 1, p.right, p.bottom), br);
    if (pill) {
        if (g_theme == TH_LEOPARD) {
            // o verniz do HUD do 10.5: metade de cima mais clara, com corte nítido
            float mid = p.top + std::min(11.f, (p.bottom - p.top) / 2);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.13f));
            dc->FillRectangle(D2D1::RectF(p.left, p.top, p.right, mid), br);
        } else {
            float strength = g_theme == TH_GOLDEN ? 0.18f : g_theme == TH_TAHOE ? 0.14f : 0.10f;
            ID2D1GradientStopCollection* gs = nullptr;
            D2D1_GRADIENT_STOP st[] = { { 0, D2D1::ColorF(1, 1, 1, strength) }, { 1, D2D1::ColorF(1, 1, 1, 0) } };
            dc->CreateGradientStopCollection(st, 2, &gs);
            ID2D1LinearGradientBrush* lg = nullptr;
            if (gs) dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, p.top }, { 0, p.top + std::min(26.f, (p.bottom - p.top) * 0.6f) }), gs, &lg);
            if (lg) { dc->FillRectangle(p, lg); lg->Release(); }
            if (gs) gs->Release();
        }
    }
    // conteúdo: o anterior some em 70 ms enquanto o novo já começa a aparecer (a forma está mudando por baixo)
    double t = anim::Now() - g_modeAt;
    auto layer = [&](Mode m, float a, double tm) {
        if (a <= 0.01f || m == M_HIDDEN) return;
        bool open = m == M_OPEN;
        float s = open ? 1.f : 0.92f + 0.08f * a;
        D2D1_RECT_F b = BoxOf(m);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, { (b.left + b.right) / 2, b.top + (Notch() ? g_barPt / 2 : PILL_H / 2) }));
        if (!open) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
        DrawContent(dc, br, m, tm);
        if (!open) dc->PopLayer();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    };
    if (t < 70 && g_prev != M_OPEN) layer(g_prev, 1 - (float)(t / 70), 1e9);
    else if (t < 70) layer(g_prev, 1 - (float)(t / 70), 1e9);
    g_hits.clear();
    float delay = g_mode == M_OPEN ? 0.f : (g_prev == M_OPEN || g_prev == M_PEEK) ? 110.f : 40.f;  // fechando: as asas voltam quando a forma pousa
    layer(g_mode, anim::EaseOut(std::clamp((float)((t - delay) / 200), 0.f, 1.f)), t);
    ID2D1PathGeometry* line = leoNotch && f ? Outline(f, p, std::max(0.f, g_rb.x)) : nullptr;
    if (line) {  // filete claro por dentro (metade do traço fica dentro do recorte)
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.22f));
        dc->DrawGeometry(line, br, 2);
    }
    dc->PopLayer();
    if (line) {  // e o contorno escuro por fora, para o painel não sumir na barra clara
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.40f * alpha));
        dc->DrawGeometry(line, br, 1);
        line->Release();
    }
    if (pill && alpha > 0.01f) {
        if (Glassy()) liquid::Rim(dc, p, rad, true, g_theme == TH_GOLDEN ? 1.f : 0.8f, 0.35f);
        else {
            br->SetColor(D2D1::ColorF(1, 1, 1, (g_theme == TH_LEOPARD ? 0.32f : 0.13f) * alpha));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p.left + 0.5f, p.top + 0.5f, p.right - 0.5f, p.bottom - 0.5f), std::max(0.f, rad - 0.5f), std::max(0.f, rad - 0.5f)), br, 1);
        }
    }
    if (geo) geo->Release();
    if (f) f->Release();
    br->Release();
    ui::GpuEnd(g_s);
    SetRegion();
}

void Frame() {
    double now = anim::Now();
    float dt = (float)std::clamp(now - g_frameAt, 1.0, 50.0);
    g_frameAt = now;
    // abrindo: a mola do Atoll com um pulinho; fechando: sem quicar; troca de atividade fechada: firme
    float resp = 0.34f, damp = 0.88f;
    if (g_springKind == 1) { resp = 0.42f; damp = 0.78f; }
    else if (g_springKind == 2) { resp = 0.45f; damp = 1.0f; }
    if (g_theme == TH_LEOPARD) { resp = g_springKind == 1 ? 0.36f : 0.32f; damp = 1.0f; }
    if (anim::reduceMotion) { resp = 0.2f; damp = 1; }
    bool moving = false;
    moving |= g_w.Step(dt, resp, damp, 0.05f);
    moving |= g_h.Step(dt, resp, damp, 0.05f);
    moving |= g_rt.Step(dt, resp, damp, 0.05f);
    moving |= g_rb.Step(dt, resp, damp, 0.05f);
    moving |= g_a.Step(dt, 0.25f, 1.0f, 0.004f);
    moving |= now - g_modeAt < 320 || now - g_tabAt < 230;
    if (g_mode == M_OPEN || g_mode == M_MEDIA) StepBars();
    Paint();
    if (!moving) {
        g_running = false;
        ui::FrameClock(g_wnd, WM_IL_FRAME, false);
        if (g_mode == M_HIDDEN) {
            ShowWindow(g_wnd, SW_HIDE);
            g_shown = false;
            g_rgn = {};
        }
        UpdateTimers();
    }
}

// ================= interação
D2D1_POINT_2F Pt(LPARAM l) { float sc = Sc(); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }
bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
const Hit* HitAt(D2D1_POINT_2F p) {
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) if (In(it->r, p)) return &*it;
    return nullptr;
}

void Activate(const std::wstring& aumid) {  // traz o app que está tocando
    if (aumid.empty()) return;
    for (auto& a : apps::list)
        if (!a.wins.empty() && (!_wcsicmp(a.id.c_str(), (L"aumid:" + aumid).c_str()) ||
                                (aumid.size() > 4 && !_wcsicmp(aumid.c_str(), (a.id.substr(a.id.find_last_of(L'\\') + 1)).c_str())))) {
            apps::Activate(a); return;
        }
    if (aumid.find(L'!') != std::wstring::npos && !apps::ActivatePackaged(L"shell:AppsFolder\\" + aumid))
        ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Close() { g_openOn = false; KillTimer(g_wnd, TM_LEAVE); Update(); }
void SetTab(int t) {
    if (t == g_tab) return;
    g_tabDir = t > g_tab ? 1 : -1;
    g_tab = (Tab)t;
    g_tabAt = anim::Now();
    StartFrames();
    UpdateTimers();
}

void ClickHit(const Hit& h, D2D1_POINT_2F p) {
    switch (h.id) {
    case H_TAB: SetTab(h.arg); break;
    case H_GEAR: Close(); shell::OpenSettings(L"central"); break;
    case H_PLAY: g_m.playing = !g_m.playing; MediaCmd(1); break;  // já mostra o novo estado; o evento confirma
    case H_NEXT: MediaCmd(2); break;
    case H_PREV: MediaCmd(3); break;
    case H_SHUF: if (g_m.canShuffle) { g_m.shuffle = !g_m.shuffle; MediaCmd(4, g_m.shuffle); } break;
    case H_OUT: {
        Close();
        RECT r;
        if (menubar::SoundIcon(&r)) som::Toggle(r); else shell::OpenSettings(L"som");
        break;
    }
    case H_SEEK: {
        float f = std::clamp((p.x - h.r.left) / (h.r.right - h.r.left), 0.f, 1.f);
        g_m.pos = f * g_m.len; g_m.at = anim::Now();
        MediaCmd(5, g_m.pos);
        break;
    }
    case H_ART: Close(); Activate(g_m.aumid); break;
    case H_PRESET: TimerStart(h.arg); break;
    case H_TPAUSE:
        if (g_tPaused) { g_tEnd = anim::Now() + g_tLeft; g_tPaused = false; }
        else { g_tLeft = TimerLeft(); g_tPaused = true; }
        UpdateTimers();
        break;
    case H_TSTOP: TimerStop(); break;
    case H_TPLUS:
        if (g_tPaused) g_tLeft += 60000; else g_tEnd += 60000;
        g_tTotal = std::max(g_tTotal, TimerLeft());
        break;
    case H_CLIP:
        if (h.arg < (int)g_clips.size()) { CopyText(g_clips[h.arg]); g_flashClip = h.arg; g_flashAt = anim::Now(); SetTimer(g_wnd, TM_TICK + 100, 1250, nullptr); }
        break;
    case H_CLIPCLR: g_clips.clear(); break;
    case H_SHELF:
        if (h.arg < (int)g_shelf.size()) ShellExecuteW(nullptr, L"open", g_shelf[h.arg].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case H_SHELFCLR: g_shelf.clear(); ShelfSave(); break;
    default: break;
    }
    if (!g_running) Paint();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_IL_FRAME: ui::FrameAck(h, WM_IL_FRAME); if (g_running) Frame(); return 0;
    case WM_IL_MEDIA: {
        bool was = g_m.playing;
        {
            std::lock_guard<std::mutex> k(g_mmx);
            // a mídia "sumiu": muitos apps passam um instante por "sem sessão" ao trocar de faixa (ou ao repetir);
            // só vale se continuar assim por 1,5 s, senão a ilha fechava e abria de novo
            if (!g_md.has && g_m.has && w != 1) { SetTimer(h, TM_GONE, 1500, nullptr); return 0; }
            KillTimer(h, TM_GONE);
            g_m = g_md;
        }
        if (was && !g_m.playing && g_m.has) { g_pauseHold = true; SetTimer(h, TM_PAUSE, 5000, nullptr); }
        if (g_m.playing) { g_pauseHold = false; KillTimer(h, TM_PAUSE); }
        if (!g_m.has) g_pauseHold = false;
        Update();
        if (!g_running) Paint();
        return 0;
    }
    case WM_IL_PRIV:
        if ((int)w != g_priv) { g_priv = (int)w; Log(L"ilha: microfone %d câmera %d", g_priv & 1, (g_priv >> 1) & 1); Update(); if (!g_running) Paint(); }
        return 0;
    case WM_IL_DOWN: {
        std::wstring* p = (std::wstring*)l;
        std::wstring full = *p; delete p;
        if (g_on && !g_full) {
            Peek pk;
            pk.title = Tr(L"Download concluído");
            pk.sub = full.substr(full.find_last_of(L'\\') + 1);
            pk.glyph = 0xE896; pk.color = D2D1::ColorF(0.35f, 0.65f, 1.f, 1);
            pk.click = [full] { ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + full + L"\"").c_str(), nullptr, SW_SHOWNORMAL); };
            ShowPeek(pk, 4000);
        }
        return 0;
    }
    case WM_IL_CAPS: SetTimer(h, TM_CAPS, 40, nullptr); return 0;  // o Windows ainda não trocou o estado da tecla
    case WM_CLIPBOARDUPDATE:
        if (g_selfCopy) { g_selfCopy = false; return 0; }
        if (g_on) { ReadClipboard(); if (g_mode == M_OPEN && g_tab == TAB_CLIP && !g_running) Paint(); }
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMPOWERSTATUSCHANGE && g_on) {
            SYSTEM_POWER_STATUS ps{};
            if (GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryLifePercent <= 100) {
                bool ac = ps.ACLineStatus == 1;
                int pct = ps.BatteryLifePercent;
                bool low = !ac && (pct == 20 || pct == 10 || pct == 5) && pct != g_batPct;
                if (ac != g_batAC || low) { g_batAC = ac; g_batPct = pct; ShowHud(HUD_BAT, 2500); }
                g_batPct = pct;
            }
        }
        return TRUE;
    case WM_TIMER:
        switch (w) {
        case TM_EQ: StepBars(); if (!g_running) Paint(); UpdateTimers(); break;
        case TM_HOVER: KillTimer(h, TM_HOVER); if (g_hoverIn && g_mode != M_OPEN && g_mode != M_PEEK) { g_openOn = true; Update(); } break;
        case TM_LEAVE: KillTimer(h, TM_LEAVE); if (!g_hoverIn && !g_dragIn) { Log(L"ilha: ponteiro saiu, fecha"); Close(); if (g_peekOn) SetTimer(h, TM_PEEK, 1500, nullptr); } break;
        case TM_PEEK: KillTimer(h, TM_PEEK); if (g_hoverIn && g_mode == M_PEEK) break; g_peekOn = false; g_peek = {}; Update(); break;
        case TM_HUD: KillTimer(h, TM_HUD); g_hudOn = false; Update(); break;
        case TM_PAUSE: KillTimer(h, TM_PAUSE); g_pauseHold = false; Update(); break;
        case TM_GONE: KillTimer(h, TM_GONE); PostMessageW(h, WM_IL_MEDIA, 1, 0); break;  // w = 1: aplica mesmo sem mídia
        case TM_STATS: SampleStats(); if (!g_running) Paint(); break;
        case TM_CAPS: {
            KillTimer(h, TM_CAPS);
            g_caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
            if (g_on && !g_full) ShowHud(HUD_CAPS, 1300);
            break;
        }
        case TM_TICK + 100: KillTimer(h, w); g_flashClip = -1; if (!g_running) Paint(); break;
        case TM_TICK:
            if (g_tRun && !g_tPaused && TimerLeft() <= 0) {
                g_tRun = false;
                KillTimer(h, TM_TICK);
                shell::Sound(L"Glass");
                Log(L"ilha: timer concluído");
                if (g_on) {
                    Peek pk; pk.title = Tr(L"Timer concluído"); pk.sub = Clock(g_tTotal / 1000);
                    pk.glyph = 0xE916; pk.color = ORANGE;
                    ShowPeek(pk, 6000);
                } else Update();
            } else if (g_shown && !g_running) Paint();
            break;
        }
        return 0;
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = Pt(l);
        if (!g_hoverIn) {
            g_hoverIn = true;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            if (g_hoverOpen && g_mode != M_OPEN && g_mode != M_PEEK && g_mode != M_HUD) SetTimer(h, TM_HOVER, 200, nullptr);  // Atoll: passar o ponteiro abre
        }
        KillTimer(h, TM_LEAVE);
        // arrastar um arquivo da Estante para fora
        if (g_pressHit == H_SHELF && (w & MK_LBUTTON) && (std::abs(GET_X_LPARAM(l) - g_pressPt.x) > 5 || std::abs(GET_Y_LPARAM(l) - g_pressPt.y) > 5)) {
            int i = g_pressArg;
            g_pressHit = H_NONE;
            if (GetCapture() == h) ReleaseCapture();
            ShelfDragOut(i);
            if (!g_running) Paint();
            return 0;
        }
        const Hit* ht = HitAt(p);
        HitId id = ht ? ht->id : H_NONE; int arg = ht ? ht->arg : -1;
        if (id == H_ART) id = H_NONE;
        if (id != g_hoverHit || arg != g_hoverArg) { g_hoverHit = id; g_hoverArg = arg; if (!g_running) Paint(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverIn = false;
        KillTimer(h, TM_HOVER);
        if (g_hoverHit != H_NONE) { g_hoverHit = H_NONE; g_hoverArg = -1; if (!g_running) Paint(); }
        if (g_openOn) SetTimer(h, TM_LEAVE, 250, nullptr);
        if (g_peekOn) SetTimer(h, TM_PEEK, 1500, nullptr);
        return 0;
    case WM_LBUTTONDOWN: {
        const Hit* ht = g_mode == M_OPEN ? HitAt(Pt(l)) : nullptr;
        g_pressHit = ht ? ht->id : H_NONE; g_pressArg = ht ? ht->arg : -1;
        g_pressPt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (g_pressHit != H_NONE) { SetCapture(h); if (!g_running) Paint(); }
        return 0;
    }
    case WM_LBUTTONUP: {
        D2D1_POINT_2F p = Pt(l);
        HitId pressed = g_pressHit; int parg = g_pressArg;
        g_pressHit = H_NONE;
        if (GetCapture() == h) ReleaseCapture();
        if (pressed != H_NONE) {
            const Hit* ht = HitAt(p);
            if (ht && ht->id == pressed && ht->arg == parg) { Hit copy = *ht; ClickHit(copy, p); }
            else if (!g_running) Paint();
            return 0;
        }
        if (g_mode == M_PEEK) {
            auto click = g_peek.click;
            g_peekOn = false; g_peek = {}; KillTimer(h, TM_PEEK);
            Update();
            if (click) click();
        } else if (g_mode != M_OPEN) { KillTimer(h, TM_HOVER); g_openOn = true; Update(); }
        return 0;
    }
    case WM_RBUTTONUP: {
        const Hit* ht = g_mode == M_OPEN ? HitAt(Pt(l)) : nullptr;
        if (ht && ht->id == H_SHELF && ht->arg < (int)g_shelf.size()) {  // tira da Estante (o arquivo fica onde está)
            g_shelf.erase(g_shelf.begin() + ht->arg); ShelfSave();
            if (!g_running) Paint();
        } else if (g_mode == M_OPEN) Close();
        return 0;
    }
    case WM_DISPLAYCHANGE: case WM_DPICHANGED:
        Place();
        if (g_shown) { Update(); Paint(); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT wt, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING, bool icons = false) {
    IDWriteTextFormat* tf = nullptr;
    if (!icons && g_theme == TH_LEOPARD) {  // Leopard: a Lucida da barra de menus do tema (Lucida Sans Unicode, o desenho da Lucida Grande)
        tf = ui::FormatFace(L"Lucida Sans Unicode", size - 0.5f, wt >= DWRITE_FONT_WEIGHT_SEMI_BOLD ? DWRITE_FONT_WEIGHT_BOLD : wt);
        if (tf) tf->SetTextAlignment(a);
    }
    if (!tf) tf = ui::Format(size, wt, icons, a);
    if (!tf) return nullptr;
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!icons) {
        IDWriteInlineObject* sign = nullptr;
        ui::dw->CreateEllipsisTrimmingSign(tf, &sign);
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        tf->SetTrimming(&tr, sign);
        if (sign) sign->Release();
    }
    return tf;
}

// fontes nas proporções do Atoll (título 12,5, artista 11, tempo 10,5); recriadas ao trocar de tema (Leopard: Lucida)
void MakeFonts() {
    for (auto** tf : { &g_tfTitle, &g_tfSub, &g_tfSmall, &g_tfSmallR, &g_tfSmallC, &g_tfWing, &g_tfWingR, &g_tfBig, &g_tfVal, &g_tfChip, &g_tfIcon, &g_tfIconS, &g_tfIconL })
        if (*tf) { (*tf)->Release(); *tf = nullptr; }
    g_tfTitle = Fmt(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfSub = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfSmall = Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfSmallR = Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfSmallC = Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfWing = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfWingR = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfBig = Fmt(28, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfVal = Fmt(17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfChip = Fmt(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_tfIconS = Fmt(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_tfIconL = Fmt(20, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
}

}  // namespace

bool ilha::Create(HINSTANCE hi) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ShellIlha";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Ilha Dinâmica",
                            WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    g_s.hwnd = g_wnd;
    g_theme = ThemeNow();
    MakeFonts();
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128)) { g_hasBattery = true; g_batAC = ps.ACLineStatus == 1; g_batPct = ps.BatteryLifePercent; }
    ShelfLoad();
    RegisterDragDrop(g_wnd, &g_dropTgt);
    Place();
    ilha::Reload();
    return true;
}

void ilha::Reload() {
    if (!g_wnd) return;
    bool was = g_on;
    g_on = shell::Setting(L"ilha", L"0") == L"1";
    std::wstring st = shell::Setting(L"ilha_estilo", L"notch");
    Style ns = st == L"ilha" ? S_ILHA : st == L"notificacao" ? S_AVISO : S_NOTCH;
    g_notesOn = shell::Setting(L"ilha_notificacoes", L"1") != L"0";
    g_volOn = shell::Setting(L"ilha_volume", L"1") != L"0";
    g_hoverOpen = shell::Setting(L"ilha_passar", L"1") != L"0";
    if (Theme th = ThemeNow(); th != g_theme) { g_theme = th; MakeFonts(); Log(L"ilha: tema %d", (int)th); }
    if (g_on) {
        MediaStart();
        static bool threads;
        if (!threads) {  // microfone/câmera, downloads e a área de transferência: só depois de ligar a ilha
            threads = true;
            std::thread(PrivacyThread).detach();
            std::thread(DownloadsThread).detach();
            AddClipboardFormatListener(g_wnd);
        }
    }
    if (g_on != was || ns != g_style) Log(L"ilha: %ls (%ls)", g_on ? L"ligada" : L"desligada", st.c_str());
    if (ns != g_style && g_shown) {  // trocou de estilo: some e volta no outro formato
        g_style = ns;
        ShowWindow(g_wnd, SW_HIDE); g_shown = false; g_rgn = {}; g_mode = M_HIDDEN;
    }
    g_style = ns;
    if (!g_volOn && g_hud == HUD_VOL) g_hudOn = false;
    if (!g_notesOn) g_peekOn = false;
    Place();
    Update();
}

namespace {
// Tela cheia no monitor principal: jogo em Direct3D exclusivo ou apresentação (estado do Windows), ou a janela mais de
// cima daquele monitor cobrindo a tela inteira (app em tela cheia, vídeo em tela cheia do navegador ou do player), mesmo
// que ela não seja a janela da frente. Pula as nossas janelas, as escondidas/"cloaked", as transparentes ao clique e os
// avisos pequenos (dicas, OSD de volume) que passam por cima do vídeo.
bool FullOnPrimary() {
    QUERY_USER_NOTIFICATION_STATE q{};
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE)) return true;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) return false;
    const RECT& m = mi.rcMonitor;
    const LONG monArea = (m.right - m.left) * (m.bottom - m.top);
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h) || shell::Nossa(h)) continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE), st = GetWindowLongW(h, GWL_STYLE);
        if (ex & WS_EX_TRANSPARENT) continue;  // sobreposições que não pegam clique (Discord, NVIDIA...)
        // sobreposições que nunca ganham o foco: a "NVIDIA GeForce Overlay" (CEF-OSC-WIDGET) fica do tamanho da tela,
        // invisível, por cima da mesa o tempo todo; um app/vídeo em tela cheia de verdade é uma janela que se ativa
        if ((ex & WS_EX_NOACTIVATE) || ((ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_LAYERED))) continue;
        RECT r, x;
        if (!GetWindowRect(h, &r) || !IntersectRect(&x, &r, &m)) continue;
        wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) continue;
        if (mesa::IsDeskClass(cls)) return false;  // chegou na mesa: nada em tela cheia
        bool covers = r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
        if (covers) return !(IsZoomed(h) && (st & WS_CAPTION) == WS_CAPTION);  // maximizada com barra de título não é tela cheia
        LONG area = (x.right - x.left) * (x.bottom - x.top);
        if (area < monArea / 4) continue;  // aviso pequeno por cima (dica, OSD, notificação)
        return false;  // uma janela normal na frente: quem está em tela cheia (se houver) ficou atrás
    }
    return false;
}

void ApplyFull(bool on) {
    if (on == g_full) return;
    g_full = on;
    if (!g_wnd) return;
    Log(L"ilha: %ls", on ? L"tela cheia, some" : L"saiu da tela cheia");
    if (on) {
        // some na hora, sem animar: uma janela "sempre no topo" por cima do jogo/vídeo, mesmo esmaecendo, pode tirar o
        // jogo da tela cheia (o Shell já viu isso com o Fortnite e os banners)
        KillTimer(g_wnd, TM_EQ); KillTimer(g_wnd, TM_HOVER); KillTimer(g_wnd, TM_LEAVE);
        if (g_running) { g_running = false; ui::FrameClock(g_wnd, WM_IL_FRAME, false); }
        ShowWindow(g_wnd, SW_HIDE);
        g_shown = false; g_rgn = {};
        g_mode = g_prev = M_HIDDEN;
        g_openOn = g_hoverIn = false;
        Size s0 = SizeOf(M_HIDDEN);
        g_w.x = g_w.t = s0.w; g_h.x = g_h.t = s0.h; g_rt.x = g_rt.t = s0.rt; g_rb.x = g_rb.t = s0.rb; g_a.x = g_a.t = 0;
        g_w.v = g_h.v = g_rt.v = g_rb.v = g_a.v = 0;
    }
    Update();
}

}  // namespace

void ilha::SetFullscreen(bool locked) {
    ApplyFull(locked || FullOnPrimary());
}

bool ilha::Notificacao(const std::wstring& app, const std::wstring& title, const std::wstring& body,
                       std::function<void(ID2D1DeviceContext*, ID2D1SolidColorBrush*, D2D1_RECT_F)> icon, std::function<void()> click) {
    if (!g_wnd || !g_on || !g_notesOn || g_full) return false;
    Peek p;
    p.title = title.empty() ? app : title;
    p.sub = body.empty() ? (title.empty() ? L"" : app) : body;
    p.icon = std::move(icon);
    p.click = std::move(click);
    ShowPeek(std::move(p), 4500);
    return true;
}

bool ilha::Volume(float level, bool mute) {
    if (!g_wnd || !g_on || !g_volOn || g_full || g_peekOn) return false;
    g_vol = level; g_mute = mute;
    ShowHud(HUD_VOL, 1500);
    return true;
}

void ilha::CapsLock() { if (g_wnd && g_on) PostMessageW(g_wnd, WM_IL_CAPS, 0, 0); }

bool ilha::On() { return g_on; }
