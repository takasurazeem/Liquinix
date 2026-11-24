// Tela de bloqueio do macOS Sequoia (Win+L, menu da Apple → Bloquear Tela, tela apagada).
// Medidas tiradas do vídeo de referência (1920 × 1080, 1 px do vídeo ≈ 1 pt do Mac ≈ 1 DIP aqui):
//  - papel de parede nítido (como no vídeo; `bloqueio_desfoque=1` desfoca), cobrindo todos os monitores;
//  - data "sábado, 7 de novembro" em SF Pro Display Semibold 28 (linha de base em y = 125) e o relógio
//    "9:20" em SF Pro Display Semibold 133 (algarismos de y = 154 a 248), com vibração: o texto é pintado
//    com o próprio papel de parede desfocado e clareado, como o material do Mac;
//  - embaixo: foto redonda de 53 pt (centro 164 pt acima da borda), nome em 13 pt negrito, campo em pílula
//    de 120 × 20,5 ("Digite a Senha"), dica em 10 pt; no canto de cima à direita, teclado e botão de energia.
// Senha: conferida com LogonUserW (nunca guardada; a cópia é zerada). Senha errada: o campo chacoalha.
// Segurança sem trancar o dono para fora (ver docs/problemas-conhecidos.md, "Tela de bloqueio"):
//  - desbloquear o Windows de verdade (Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear, PIN/Windows Hello) também tira esta tela;
//  - se o Shell sair ou cair com a tela travada, o Windows é bloqueado (LockWorkStation), também por um
//    vigia em outro processo (Shell.exe --vigia), então matar o Shell não abre a sessão;
//  - erros do LogonUserW que não são "senha errada" aceitam e ficam no shell.log;
//  - depois de 3 erros a tela espera antes de tentar de novo, para não disparar o bloqueio de conta do Windows.
#include "shell.h"
#include <d2d1effects.h>
#include <wincodec.h>
#include <sddl.h>
#include <lm.h>
#include <powrprof.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>
#include <UserConsentVerifierInterop.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "powrprof.lib")
#include <wtsapi32.h>
#include <bcrypt.h>
#include <shlobj.h>
#include "../comum/idioma.h"
#pragma comment(lib, "bcrypt.lib")

namespace {

const UINT WM_BQ_FRAME = WM_APP + 50, WM_BQ_RESULT = WM_APP + 51, WM_BQ_HELLO = WM_APP + 52;
const double IN_MS = 520, OUT_MS = 480, SHAKE_MS = 520;
enum { HIDDEN, LOCKING, LOCKED, UNLOCKING };

struct Mon {
    HWND h = nullptr;
    ui::GpuSurface s;
    RECT r{};
    bool primary = false;
    ID2D1Bitmap1* bg = nullptr;   // papel de parede no tamanho do monitor (px)
    ID2D1Bitmap1* vib = nullptr;  // desfocado e clareado: tinta do relógio (só o principal)
};

HINSTANCE g_hi;
HWND g_ctl;                       // janela escondida: sessão do Windows, energia, mensagem "ShellBloquearTela"
std::vector<Mon> g_mons;
std::atomic<int> g_phase{ HIDDEN };
double g_phaseAt;
std::wstring g_user, g_display, g_avatarPath;
UINT g_aw, g_ah; std::vector<BYTE> g_apx;
ID2D1Bitmap* g_avatar;
std::wstring g_pw;                // só enquanto digita; zerado ao conferir/sair
bool g_checking, g_menuOpen, g_demo, g_preview;  // preview: Shell.exe --testar-bloqueio (sem senha, sai sozinho)
int g_fails, g_menuHover = -1;
double g_shakeAt = -1e9, g_caretAt;
ULONGLONG g_waitUntil;
std::wstring g_msg;               // texto embaixo do campo (erro / espera)
std::atomic<bool> g_helloOk{ false }, g_helloBusy{ false };
HANDLE g_endEvent, g_lockedEvent;  // para o vigia: o Shell saiu direito / a tela está travada
bool g_policySet;                  // fomos nós que ligamos o DisableLockWorkstation (desligar ao sair)

// O Windows trata o Win+L antes de qualquer gancho de teclado. Para o Win+L chegar ao Shell, a política do
// usuário DisableLockWorkstation fica ligada SÓ enquanto o Shell roda (com a permissão do usuário); ao sair,
// cair ou ser encerrado (vigia), ela é apagada antes de bloquear o Windows de verdade.
const wchar_t* POL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
void PolicyOff() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, POL_KEY, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, L"DisableLockWorkstation");
    DWORD vals = 1, subs = 1;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, &vals, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    if (!vals && !subs) RegDeleteKeyW(HKEY_CURRENT_USER, POL_KEY);  // a chave não existia antes: não deixa rastro
}
void LockWindows() {  // bloqueio de verdade: a política precisa estar desligada, senão o LockWorkStation falha
    PolicyOff();
    g_policySet = false;
    // com a política ligada pelo administrador (o Shell não consegue apagar), o LockWorkStation falha:
    // desconectar a sessão leva à tela de login do Windows do mesmo jeito (volta com o PIN, tudo aberto)
    if (!LockWorkStation()) WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, FALSE);
}
D2D1_RECT_F g_powerRc{}, g_menuRc{};

bool Enabled() { return shell::Setting(L"bloqueio", L"1") != L"0"; }

void Wipe(std::wstring& s) { if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t)); s.clear(); }

// ---- senha própria da tela (opcional): só o resumo PBKDF2-SHA256 com sal, em %APPDATA%\Shell\bloqueio-senha.txt.
// Sem o arquivo, vale a senha da conta do Windows. Apagar o arquivo volta para ela.
const int PBKDF2_ITER = 200000;
std::wstring PassFile() {
    PWSTR p = nullptr; std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { f = std::wstring(p) + L"\\Shell\\bloqueio-senha.txt"; CoTaskMemFree(p); }
    return f;
}
std::string Hex(const BYTE* b, size_t n) { static const char* d = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; } return s; }
bool Derive(const std::wstring& pw, const BYTE* salt, ULONG saltLen, ULONGLONG iter, BYTE out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, pw.c_str(), (int)pw.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, pw.c_str(), (int)pw.size(), u.data(), n, nullptr, nullptr);
    bool ok = BCryptDeriveKeyPBKDF2(alg, (PUCHAR)u.data(), (ULONG)u.size(), (PUCHAR)salt, saltLen, iter, out, 32, 0) >= 0;
    SecureZeroMemory(u.data(), u.size());
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
bool HasOwnPassword() { return GetFileAttributesW(PassFile().c_str()) != INVALID_FILE_ATTRIBUTES; }
bool SaveOwnPassword(const std::wstring& pw) {
    BYTE salt[16], h[32];
    if (BCryptGenRandom(nullptr, salt, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !Derive(pw, salt, 16, PBKDF2_ITER, h)) return false;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, PassFile().c_str(), L"wb") || !fp) return false;
    fprintf(fp, "pbkdf2-sha256$%d$%s$%s\n", PBKDF2_ITER, Hex(salt, 16).c_str(), Hex(h, 32).c_str());
    fclose(fp);
    return true;
}
// 1 = certa, 0 = errada, -1 = arquivo ilegível (cai na senha do Windows)
int CheckOwnPassword(const std::wstring& pw) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, PassFile().c_str(), L"rb") || !fp) return -1;
    char line[256] = {}; fgets(line, sizeof(line), fp); fclose(fp);
    unsigned long long iter = 0; char saltHex[64] = {}, hashHex[80] = {};
    if (sscanf_s(line, "pbkdf2-sha256$%llu$%32[0-9a-f]$%64[0-9a-f]", &iter, saltHex, (unsigned)sizeof(saltHex), hashHex, (unsigned)sizeof(hashHex)) != 3 ||
        strlen(saltHex) != 32 || strlen(hashHex) != 64 || iter < 1000) return -1;
    BYTE salt[16], h[32];
    for (int i = 0; i < 16; i++) sscanf_s(saltHex + 2 * i, "%2hhx", &salt[i]);
    if (!Derive(pw, salt, 16, iter, h)) return -1;
    std::string got = Hex(h, 32);
    unsigned diff = 0;
    for (int i = 0; i < 64; i++) diff |= (unsigned)(got[i] ^ hashHex[i]);
    return diff == 0 ? 1 : 0;
}

// ---- quem é o usuário: nome de exibição e foto da conta do Windows
void LoadUser() {
    wchar_t u[256] = {}; DWORD n = 256;
    GetUserNameW(u, &n);
    g_user = u;
    g_display = g_user;
    USER_INFO_2* ui2 = nullptr;
    if (NetUserGetInfo(nullptr, u, 2, (LPBYTE*)&ui2) == NERR_Success && ui2) {
        if (ui2->usri2_full_name && *ui2->usri2_full_name) g_display = ui2->usri2_full_name;
        NetApiBufferFree(ui2);
    }
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BYTE buf[256]; DWORD got = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &got)) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &sid)) {
                std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\" + std::wstring(sid);
                for (auto v : { L"Image448", L"Image424", L"Image240", L"Image192" }) {
                    wchar_t p[MAX_PATH] = {}; DWORD cb = sizeof(p);
                    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), v, RRF_RT_REG_SZ, nullptr, p, &cb) == ERROR_SUCCESS &&
                        GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { g_avatarPath = p; break; }
                }
                LocalFree(sid);
            }
        }
        CloseHandle(tok);
    }
    g_apx.clear();
    if (!g_avatarPath.empty() && !ui::LoadPng(g_avatarPath, g_aw, g_ah, g_apx)) g_apx.clear();
    if (g_avatar) { g_avatar->Release(); g_avatar = nullptr; }
}

// ---- texto
IDWriteTextLayout* Lay(const std::wstring& s, const wchar_t* fam, float size, DWRITE_FONT_WEIGHT wt, float boxW, bool wrap = false) {
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(ui::dw->CreateTextFormat(fam, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf)) &&
        FAILED(ui::dw->CreateTextFormat(ui::family.c_str(), nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf)))
        return nullptr;
    tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    tf->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteTextLayout* l = nullptr;
    ui::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, boxW, 2000, &l);
    tf->Release();
    return l;
}

// desenha centrado em cx com a primeira linha de base em `base`
void DrawAt(ID2D1DeviceContext* dc, IDWriteTextLayout* l, float cx, float base, ID2D1Brush* br) {
    if (!l) return;
    DWRITE_LINE_METRICS lm[4]; UINT32 n = 0;
    l->GetLineMetrics(lm, 4, &n);
    float b = n ? lm[0].baseline : 0;
    dc->DrawTextLayout({ cx - l->GetMaxWidth() / 2, base - b }, l, br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

bool TahoeLock();
std::wstring DateText() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t d[128] = {};
    if (TahoeLock()) {  // macOS 26: data abreviada ("Thu Jun 19" / "qua. 11 de nov.")
        GetDateFormatEx(idioma::Locale(), 0, &st, idioma::DatePattern(idioma::D_WEEKDAY_SHORT), d, 128, nullptr);
        return idioma::Cap(d);
    }
    GetDateFormatEx(idioma::Locale(), 0, &st, idioma::DatePattern(idioma::D_WEEKDAY_LONG), d, 128, nullptr);  // "sexta-feira, 20 de novembro"
    return d;
}
std::wstring TimeText() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t t[32]; swprintf_s(t, L"%d:%02d", st.wHour, st.wMinute);
    return t;
}

std::wstring InputName() {  // fonte de entrada, como o Mac mostra no canto
    HKL k = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    switch (PRIMARYLANGID(LOWORD((UINT_PTR)k))) {
    case LANG_PORTUGUESE: return Tr(L"Brasileiro");
    case LANG_ENGLISH: return Tr(L"EUA");
    case LANG_SPANISH: return Tr(L"Espanhol");
    }
    wchar_t n[16] = {};
    GetLocaleInfoW(MAKELCID(LOWORD((UINT_PTR)k), SORT_DEFAULT), LOCALE_SABBREVLANGNAME, n, 16);
    return n;
}

// ---- papel de parede: nítido (ou desfocado) cobrindo o monitor; e a tinta da vibração
ID2D1Bitmap1* MakeTarget(int w, int h, float sc) {
    ID2D1Bitmap1* b = nullptr;
    ui::Gpu()->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * sc, 96 * sc), &b);
    return b;
}

void Paint(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, ID2D1Bitmap* src, UINT sw, UINT sh, int wpx, int hpx, float blurPx, D2D1_COLOR_F over) {
    dc->SetTarget(target);
    dc->SetDpi(96, 96);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.10f, 0.12f, 0.45f));  // sem papel de parede: o azul do Sequoia
    if (src) {
        float s = std::max(wpx / (float)sw, hpx / (float)sh);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation((wpx - sw * s) / 2, (hpx - sh * s) / 2));
        if (blurPx > 0) {
            ID2D1Effect* blur = nullptr;
            dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
            blur->SetInput(0, src);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blurPx / s);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            dc->DrawImage(blur, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            blur->Release();
        } else dc->DrawBitmap(src, D2D1::RectF(0, 0, (float)sw, (float)sh), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    if (over.a > 0) {
        ID2D1SolidColorBrush* br = nullptr;
        dc->CreateSolidColorBrush(over, &br);
        dc->FillRectangle(D2D1::RectF(0, 0, (float)wpx, (float)hpx), br);
        br->Release();
    }
    dc->EndDraw();
    dc->SetTarget(nullptr);
}

void BuildBitmaps(Mon& m, float sc) {
    auto* dc = ui::Gpu();
    if (!dc) return;
    int w = m.r.right - m.r.left, h = m.r.bottom - m.r.top;
    wchar_t path[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    {   // fundo escolhido no Ajustes do Sistema > Tela Bloqueada (imagem, ou o 1º quadro do vídeo)
        std::wstring own = shell::Setting(L"bloqueio_fundo", L"");
        if (!own.empty() && GetFileAttributesW(own.c_str()) != INVALID_FILE_ATTRIBUTES) wcsncpy_s(path, own.c_str(), _TRUNCATE);
    }
    UINT sw = 0, sh = 0; std::vector<BYTE> px;
    ID2D1Bitmap* src = path[0] && ui::LoadPng(path, sw, sh, px) ? ui::Bitmap(dc, sw, sh, px) : nullptr;
    bool blur = shell::Setting(L"bloqueio_desfoque", L"0") == L"1";
    if ((m.bg = MakeTarget(w, h, sc)))
        Paint(dc, m.bg, src, sw, sh, w, h, blur ? 40 * sc : 0, D2D1::ColorF(0, 0, 0, blur ? 0.18f : 0.04f));
    if (m.primary && (m.vib = MakeTarget(w, h, sc)))
        Paint(dc, m.vib, src, sw, sh, w, h, 28 * sc, D2D1::ColorF(1, 1, 1, 0.66f));
    if (src) src->Release();
}

void FreeMon(Mon& m) {
    if (m.bg) m.bg->Release();
    if (m.vib) m.vib->Release();
    ui::GpuFree(m.s);
    if (m.h) DestroyWindow(m.h);
    m = Mon{};
}

// ---- desenho
float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

void Glyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const wchar_t* g, float size, D2D1_POINT_2F c) {
    if (auto* l = Lay(g, ui::iconFamily.c_str(), size, DWRITE_FONT_WEIGHT_NORMAL, 60)) {
        DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm);
        dc->DrawTextLayout({ c.x - 30, c.y - tm.height / 2 }, l, br);
        l->Release();
    }
}

// Tahoe (vídeo do macOS 26): o relógio é de vidro: algarismos finos (Regular), translúcidos (o papel de
// parede aparece através deles, clareado), com um fio de luz na borda de cada algarismo e uma sombra suave
// embaixo; a data, menor, em cima. O texto vira uma "lista de comandos" (máscara) e os efeitos do D2D fazem a
// borda (máscara menos ela mesma encolhida 2 px) e a sombra.
bool TahoeLock() { return liquid::On() || shell::Setting(L"liquid_glass", L"0") == L"1"; }
void GlassText(ID2D1DeviceContext* dc, IDWriteTextLayout* l, float cx, float base, ID2D1BitmapBrush1* vib, float a) {
    if (!l) return;
    ID2D1SolidColorBrush* white = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    if (!white) return;
    D2D1_MATRIX_3X2_F tr; dc->GetTransform(&tr);
    // máscara: o texto em branco, nas coordenadas da tela (já com a transformação da animação)
    ID2D1CommandList* mask = nullptr;
    ID2D1Image* old = nullptr;
    dc->CreateCommandList(&mask);
    if (mask) {
        dc->GetTarget(&old);
        dc->SetTarget(mask);
        DrawAt(dc, l, cx, base, white);
        dc->SetTarget(old);
        mask->Close();
    }
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    ID2D1Effect *shadow = nullptr, *erode = nullptr, *ring = nullptr;
    if (mask) {
        dc->CreateEffect(CLSID_D2D1Shadow, &shadow);
        dc->CreateEffect(CLSID_D2D1Morphology, &erode);
        dc->CreateEffect(CLSID_D2D1Composite, &ring);
    }
    if (shadow) {  // sombra suave embaixo (o vidro "flutua" sobre o papel de parede)
        shadow->SetInput(0, mask);
        shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 7.f);
        shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.16f * a));
        dc->DrawImage(shadow, D2D1::Point2F(0, 3));
    }
    dc->SetTransform(tr);
    // corpo: o papel de parede (desfocado e clareado) através do algarismo, mais um véu branco leve
    if (vib) { vib->SetOpacity(0.55f * a); DrawAt(dc, l, cx, base, vib); }
    white->SetOpacity((vib ? 0.30f : 0.62f) * a);
    DrawAt(dc, l, cx, base, white);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    if (erode && ring) {  // o fio de luz: a máscara menos ela mesma encolhida
        erode->SetInput(0, mask);
        erode->SetValue(D2D1_MORPHOLOGY_PROP_MODE, D2D1_MORPHOLOGY_MODE_ERODE);
        erode->SetValue(D2D1_MORPHOLOGY_PROP_WIDTH, 3u);
        erode->SetValue(D2D1_MORPHOLOGY_PROP_HEIGHT, 3u);
        ring->SetInput(0, mask);
        ring->SetInputEffect(1, erode);
        ring->SetValue(D2D1_COMPOSITE_PROP_MODE, D2D1_COMPOSITE_MODE_DESTINATION_OUT);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.72f * a), nullptr);
        dc->DrawImage(ring);
        dc->PopLayer();
    }
    dc->SetTransform(tr);
    for (ID2D1Effect* e : { shadow, erode, ring }) if (e) e->Release();
    if (mask) mask->Release();
    if (old) old->Release();
    white->Release();
}

// geometria do bloco de baixo (DIPs)
struct Geo { float W, H, cx, avatarCy, nameBase, fieldTop; };
Geo GeoOf(float W, float H) { return { W, H, W / 2, H - 164, H - 115, H - 100.5f }; }
const float FIELD_W = 120, FIELD_H = 20.5f, AVATAR = 53;

void Draw(ID2D1DeviceContext* dc, Mon& m, float W, float H, double now) {
    int ph = g_phase;
    float t = (float)(now - g_phaseAt);
    // fundo: entra suave; sai suave revelando a mesa (o mesmo papel de parede)
    float bgA = 1, topA = 1, topDy = 0, topSc = 1, botA = 1, botDy = 0;
    if (ph == LOCKING) {
        bgA = anim::Ease(Clamp01(t / 300));
        float k = anim::Spring(t / 1000, 0.5f, 0.92f);
        topA = anim::Ease(Clamp01((t - 60) / 380)); topSc = 1.06f - 0.06f * k; topDy = -10 * (1 - k);
        botA = anim::Ease(Clamp01((t - 160) / 320)); botDy = 14 * (1 - anim::Ease(Clamp01((t - 160) / 360)));
    } else if (ph == UNLOCKING) {
        float u = Clamp01(t / (float)OUT_MS);
        botA = 1 - anim::Ease(Clamp01(t / 180));
        topA = 1 - anim::EaseInOut(Clamp01((t - 40) / 300)); topDy = -46 * anim::EaseInOut(Clamp01(t / 380));
        bgA = 1 - anim::EaseInOut(Clamp01((t - 120) / (float)(OUT_MS - 120)));
        (void)u;
    }
    if (m.bg) dc->DrawBitmap(m.bg, D2D1::RectF(0, 0, W, H), bgA);
    if (!m.primary) return;

    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    ID2D1BitmapBrush1* vib = nullptr;
    if (m.vib) dc->CreateBitmapBrush(m.vib, D2D1::BitmapBrushProperties1(), D2D1::BrushProperties(), &vib);

    // data e relógio, com a tinta do papel de parede
    D2D1::Matrix3x2F M = D2D1::Matrix3x2F::Scale(topSc, topSc, { W / 2, 200 }) * D2D1::Matrix3x2F::Translation(0, topDy);
    D2D1::Matrix3x2F inv = M; inv.Invert();
    dc->SetTransform(M);
    ID2D1Brush* ink = vib ? (ID2D1Brush*)vib : (ID2D1Brush*)br;
    if (vib) vib->SetTransform(inv);
    if (TahoeLock()) {  // macOS 26: data menor e branca; relógio fino de vidro (medidas do vídeo, 1080p)
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f * topA));
        if (auto* l = Lay(DateText(), L"SF Pro Display", 25, DWRITE_FONT_WEIGHT_SEMI_BOLD, W)) { DrawAt(dc, l, W / 2, 122, br); l->Release(); }
        if (auto* l = Lay(TimeText(), L"SF Pro Display", 124, DWRITE_FONT_WEIGHT_NORMAL, W)) { GlassText(dc, l, W / 2, 238, vib, topA); l->Release(); }
    } else {
    if (auto* l = Lay(DateText(), L"SF Pro Display", 28, DWRITE_FONT_WEIGHT_SEMI_BOLD, W)) {
        ink->SetOpacity(0.92f * topA); if (!vib) br->SetColor(D2D1::ColorF(1, 1, 1, 0.75f * topA));
        DrawAt(dc, l, W / 2, 125, ink); l->Release();
    }
    if (auto* l = Lay(TimeText(), L"SF Pro Display", 133, DWRITE_FONT_WEIGHT_SEMI_BOLD, W)) {
        ink->SetOpacity(0.97f * topA); if (!vib) br->SetColor(D2D1::ColorF(1, 1, 1, 0.80f * topA));
        DrawAt(dc, l, W / 2, 248, ink); l->Release();
    }
    }
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    br->SetOpacity(1);

    // canto de cima: fonte de entrada, bateria (se houver), energia
    {
        float y = 10, x = W - 25;
        br->SetColor(D2D1::ColorF(1, 1, 1, (g_menuOpen ? 1.f : 0.92f) * topA));
        Glyph(dc, br, L"\xE7E8", 13, { x, y + 1 });
        g_powerRc = D2D1::RectF(x - 11, 0, x + 11, 22);
        x -= 28;
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100) {
            D2D1_RECT_F b = D2D1::RectF(x - 11, y - 5, x + 9, y + 5);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f * topA));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(b, 2.5f, 2.5f), br, 1);
            dc->FillRectangle(D2D1::RectF(b.right + 1.2f, y - 1.8f, b.right + 2.4f, y + 1.8f), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * topA));
            float f = ps.BatteryLifePercent / 100.f;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 1.5f, b.top + 1.5f, b.left + 1.5f + (b.right - b.left - 3) * f, b.bottom - 1.5f), 1.2f, 1.2f), br);
            x -= 32;
        }
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f * topA));
        Glyph(dc, br, L"\xE765", 14, { x, y + 1 });
        if (auto* l = Lay(InputName(), L"SF Pro Text", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 200)) {
            DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm);
            DrawAt(dc, l, x - 14 - tm.width / 2, 14, br); l->Release();
        }
    }

    // bloco de baixo: foto, nome, campo, dica
    Geo g = GeoOf(W, H);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0, botDy));
    D2D1_ELLIPSE av = D2D1::Ellipse({ g.cx, g.avatarCy }, AVATAR / 2, AVATAR / 2);
    if (!g_avatar && !g_apx.empty()) g_avatar = ui::Bitmap(dc, g_aw, g_ah, g_apx);
    if (g_avatar) {
        ID2D1BitmapBrush* ab = nullptr;
        dc->CreateBitmapBrush(g_avatar, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &ab);
        float s = AVATAR / std::min(g_aw, g_ah);
        ab->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(g.cx - g_aw * s / 2, g.avatarCy - g_ah * s / 2));
        ab->SetOpacity(botA);
        dc->FillEllipse(av, ab);
        ab->Release();
    } else {  // sem foto: monograma no cinza do Mac
        br->SetColor(D2D1::ColorF(0.56f, 0.56f, 0.60f, botA));
        dc->FillEllipse(av, br);
        br->SetColor(D2D1::ColorF(1, 1, 1, botA));
        std::wstring ini(1, g_display.empty() ? L'?' : towupper(g_display[0]));
        size_t sp = g_display.find_last_of(L' ');
        if (sp != std::wstring::npos && sp + 1 < g_display.size()) ini += towupper(g_display[sp + 1]);
        if (auto* l = Lay(ini, L"SF Pro Rounded", 22, DWRITE_FONT_WEIGHT_MEDIUM, 100)) { DrawAt(dc, l, g.cx, g.avatarCy + 8, br); l->Release(); }
    }
    br->SetColor(D2D1::ColorF(1, 1, 1, botA));
    if (auto* l = Lay(g_display, L"SF Pro Text", 13, DWRITE_FONT_WEIGHT_BOLD, 400)) { DrawAt(dc, l, g.cx, g.nameBase, br); l->Release(); }

    // campo da senha (chacoalha quando erra)
    float st = (float)(now - g_shakeAt) / (float)SHAKE_MS, sx = 0;
    if (st >= 0 && st < 1) sx = 15 * std::sin(st * 2 * 3.14159265f * 3.5f) * std::pow(1 - st, 1.6f);
    D2D1_RECT_F f = D2D1::RectF(g.cx - FIELD_W / 2 + sx, g.fieldTop, g.cx + FIELD_W / 2 + sx, g.fieldTop + FIELD_H);
    bool waiting = GetTickCount64() < g_waitUntil;
    br->SetColor(D2D1::ColorF(1, 1, 1, (waiting ? 0.22f : 0.36f) * botA));
    dc->FillRoundedRectangle(D2D1::RoundedRect(f, FIELD_H / 2, FIELD_H / 2), br);
    float cy = (f.top + f.bottom) / 2;
    if (!g_pw.empty()) {  // bolinhas
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * botA));
        float x = f.left + 11;
        for (size_t i = 0; i < g_pw.size() && x < f.right - 26; i++, x += 7.2f) dc->FillEllipse(D2D1::Ellipse({ x, cy }, 2.6f, 2.6f), br);
        if (!g_checking && std::fmod(now - g_caretAt, 1060.0) < 530) dc->FillRectangle(D2D1::RectF(x - 1.5f, cy - 6, x - 0.5f, cy + 6), br);
    } else if (auto* l = Lay(waiting ? Tr(L"Aguarde…") : Tr(L"Digite a Senha"), L"SF Pro Text", 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, 300)) {
        DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f * botA));
        DrawAt(dc, l, f.left + 10 + tm.width / 2, cy + 4, br); l->Release();
        if (!g_checking && !waiting && std::fmod(now - g_caretAt, 1060.0) < 530) {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.9f * botA));
            dc->FillRectangle(D2D1::RectF(f.left + 9, cy - 6, f.left + 10, cy + 6), br);
        }
    }
    if (GetKeyState(VK_CAPITAL) & 1) {  // Caps Lock ligado, como no Mac
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.8f * botA));
        if (auto* l = Lay(L"⇪", L"SF Pro Text", 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, 30)) { DrawAt(dc, l, f.right - (g_pw.empty() ? 11 : 27), cy + 4, br); l->Release(); }
    }
    if (g_checking) {  // girando enquanto confere
        D2D1_POINT_2F c{ f.right - 11, cy };
        float a0 = (float)std::fmod(now / 1000.0 * 6.283, 6.283);  // uma volta por segundo
        for (int i = 0; i < 8; i++) {
            float a = a0 + i * 0.785f;
            br->SetColor(D2D1::ColorF(1, 1, 1, (0.2f + 0.8f * i / 7) * botA));
            dc->DrawLine({ c.x + 3 * std::cos(a), c.y + 3 * std::sin(a) }, { c.x + 6 * std::cos(a), c.y + 6 * std::sin(a) }, br, 1.3f);
        }
    } else if (!g_pw.empty()) {  // botão de entrar (seta no círculo)
        D2D1_POINT_2F c{ f.right - 11, cy };
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.38f * botA));
        dc->FillEllipse(D2D1::Ellipse(c, 7.5f, 7.5f), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * botA));
        dc->DrawLine({ c.x - 3.5f, c.y }, { c.x + 3.5f, c.y }, br, 1.4f);
        dc->DrawLine({ c.x + 0.5f, c.y - 3 }, { c.x + 3.6f, c.y }, br, 1.4f);
        dc->DrawLine({ c.x + 0.5f, c.y + 3 }, { c.x + 3.6f, c.y }, br, 1.4f);
    }
    std::wstring hint = g_msg;
    if (hint.empty() && g_helloOk && g_pw.empty()) hint = Tr(L"Pressione Enter para usar o Windows Hello");
    if (!hint.empty()) if (auto* l = Lay(hint, L"SF Pro Text", 10, DWRITE_FONT_WEIGHT_SEMI_BOLD, 190, true)) {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.80f * botA));
        DrawAt(dc, l, g.cx, f.bottom + 15, br); l->Release();
    }
    dc->SetTransform(D2D1::Matrix3x2F::Identity());

    // menu de energia
    if (g_menuOpen && ph == LOCKED) {
        const wchar_t* items[] = { Tr(L"Suspender"), Tr(L"Reiniciar…"), Tr(L"Desligar…") };
        float mw = 150, ih = 22, mx = std::min(W - mw - 6, g_powerRc.left), my = 26;
        g_menuRc = D2D1::RectF(mx, my, mx + mw, my + 10 + ih * 3);
        br->SetColor(D2D1::ColorF(0.14f, 0.14f, 0.18f, 0.82f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(g_menuRc, 8, 8), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.16f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(mx + 0.5f, my + 0.5f, mx + mw - 0.5f, g_menuRc.bottom - 0.5f), 7.5f, 7.5f), br, 1);
        for (int i = 0; i < 3; i++) {
            D2D1_RECT_F r = D2D1::RectF(mx + 5, my + 5 + i * ih, mx + mw - 5, my + 5 + (i + 1) * ih);
            if (i == g_menuHover) { br->SetColor(D2D1::ColorF(0.04f, 0.47f, 1.0f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), br); }
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
            if (auto* l = Lay(items[i], L"SF Pro Text", 13, DWRITE_FONT_WEIGHT_NORMAL, 300)) {
                DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm);
                DrawAt(dc, l, r.left + 9 + tm.width / 2, (r.top + r.bottom) / 2 + 4.5f, br); l->Release();
            }
        }
    }
    if (vib) vib->Release();
    br->Release();
}

void PaintAll() {
    double now = anim::Now();
    for (auto& m : g_mons) {
        if (!m.h) continue;
        float sc = ui::Scale(m.h);
        int w = m.r.right - m.r.left, h = m.r.bottom - m.r.top;
        auto* dc = ui::GpuBegin(m.s, w, h, sc);
        if (!dc) continue;
        Draw(dc, m, w / sc, h / sc, now);
        ui::GpuEnd(m.s);
    }
}

Mon* Primary() { for (auto& m : g_mons) if (m.primary) return &m; return g_mons.empty() ? nullptr : &g_mons[0]; }

// ---- foco: a tela de bloqueio fica por cima de tudo e com o teclado
void Assert(bool focus) {
    if (g_helloBusy) return;  // a janela do Windows Hello precisa ficar na frente
    for (auto& m : g_mons) if (m.h) SetWindowPos(m.h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Mon* p = Primary();
    if (focus && p && GetForegroundWindow() != p->h) {
        SetForegroundWindow(p->h);
        SetFocus(p->h);
    }
}

void Finish() {  // fim da animação de desbloquear
    ui::FrameClock(Primary() ? Primary()->h : nullptr, WM_BQ_FRAME, false);
    for (auto& m : g_mons) FreeMon(m);
    g_mons.clear();
    if (g_avatar) { g_avatar->Release(); g_avatar = nullptr; }
    g_phase = HIDDEN;
    if (g_lockedEvent) ResetEvent(g_lockedEvent);
    Log(L"bloqueio: tela liberada");
    if (g_preview) PostQuitMessage(0);
}

void Unlock(const wchar_t* why) {
    if (g_phase != LOCKED && g_phase != LOCKING) return;
    Log(L"bloqueio: desbloqueado (%ls)", why);
    Wipe(g_pw);
    g_menuOpen = false; g_msg.clear(); g_fails = 0; g_waitUntil = 0;
    g_phase = UNLOCKING; g_phaseAt = anim::Now();
    if (!g_preview) { dock::SetFullscreen(false); menubar::SetFullscreen(false); }
    if (Mon* p = Primary()) ui::FrameClock(p->h, WM_BQ_FRAME, true);
    g_demo = false;
}

void Fail() {
    g_fails++;
    g_shakeAt = anim::Now();
    MessageBeep(0xFFFFFFFF);
    ULONGLONG wait = g_fails == 3 ? 60000 : g_fails == 4 ? 5 * 60000 : g_fails >= 5 ? 15 * 60000 : 0;
    if (wait) {
        g_waitUntil = GetTickCount64() + wait;
        g_msg = wait == 60000 ? Tr(L"Tente novamente em 1 minuto. Ou Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear e use o Windows.")
                              : Tr(L"Tente novamente mais tarde. Ou Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear e use o PIN do Windows.");
    } else if (g_fails >= 2) g_msg = Tr(L"Senha incorreta. Esqueceu? Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear usa o PIN do Windows.");
}

void Submit(HWND h) {
    if (g_checking || g_phase != LOCKED || g_preview) return;
    if (GetTickCount64() < g_waitUntil) { g_shakeAt = anim::Now(); return; }
    if (g_pw.empty()) {
        if (g_helloOk && !g_helloBusy) {  // Windows Hello (o "Touch ID" daqui)
            g_helloBusy = true;
            std::thread([h] {
                bool ok = false;
                try {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    namespace cui = winrt::Windows::Security::Credentials::UI;
                    auto interop = winrt::get_activation_factory<cui::UserConsentVerifier, IUserConsentVerifierInterop>();
                    winrt::Windows::Foundation::IAsyncOperation<cui::UserConsentVerificationResult> op{ nullptr };
                    winrt::hstring text = Tr(L"Desbloquear a tela");
                    winrt::check_hresult(interop->RequestVerificationForWindowAsync(h, (HSTRING)winrt::get_abi(text),
                        winrt::guid_of<decltype(op)>(), winrt::put_abi(op)));
                    ok = op.get() == cui::UserConsentVerificationResult::Verified;
                } catch (...) {}
                PostMessageW(h, WM_BQ_HELLO, ok, 0);
            }).detach();
        }
        return;
    }
    g_checking = true;
    std::wstring pw = g_pw;  // cópia só para a thread, zerada logo depois
    Wipe(g_pw);
    std::wstring user = g_user;
    std::thread([h, pw, user]() mutable {
        int own = HasOwnPassword() ? CheckOwnPassword(pw) : -1;  // senha própria da tela, se definida
        BOOL ok = FALSE; DWORD err = 0;
        if (own >= 0) { ok = own == 1; err = ok ? 0 : ERROR_LOGON_FAILURE; }
        else {
            HANDLE tok = nullptr;
            ok = LogonUserW(user.c_str(), L".", pw.c_str(), LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &tok);
            err = ok ? 0 : GetLastError();
            if (tok) CloseHandle(tok);
        }
        bool hadText = !pw.empty();
        Wipe(pw);
        PostMessageW(h, WM_BQ_RESULT, ok ? 1 : 0, (LPARAM)err | (hadText ? 0x80000000 : 0));
    }).detach();
}

void Power(int i) {
    g_menuOpen = false;
    if (i == 0) { Log(L"bloqueio: suspender"); SetSuspendState(FALSE, FALSE, FALSE); return; }
    HANDLE tok;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{ 1 };
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    Log(L"bloqueio: %ls", i == 1 ? L"reiniciar" : L"desligar");
    ExitWindowsEx((i == 1 ? EWX_REBOOT : EWX_SHUTDOWN | EWX_POWEROFF), SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
}

int MenuHit(D2D1_POINT_2F p) {
    if (!g_menuOpen || p.x < g_menuRc.left || p.x > g_menuRc.right) return -1;
    int i = (int)((p.y - g_menuRc.top - 5) / 22);
    return p.y >= g_menuRc.top + 5 && i >= 0 && i < 3 ? i : -1;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Mon* me = nullptr;
    for (auto& x : g_mons) if (x.h == h) me = &x;
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); PaintAll(); return 0; }
    case WM_BQ_FRAME: {
        double t = anim::Now() - g_phaseAt;
        if (g_phase == UNLOCKING && t >= OUT_MS) { Finish(); return 0; }
        if (g_phase == LOCKING && t >= IN_MS + 200) { g_phase = LOCKED; g_phaseAt = anim::Now(); }
        PaintAll();
        return 0;
    }
    case WM_TIMER:
        if (w == 1) { if (g_phase == LOCKED || g_phase == LOCKING) Assert(true); }
        if (w == 2 && g_waitUntil && GetTickCount64() >= g_waitUntil) { g_waitUntil = 0; g_msg.clear(); }
        return 0;
    case WM_BQ_RESULT: {
        g_checking = false;
        DWORD err = (DWORD)l & 0x7FFFFFFF; bool hadText = (l & 0x80000000) != 0;
        if (w) { Unlock(L"senha"); return 0; }
        if (err == ERROR_LOGON_FAILURE || (err == ERROR_ACCOUNT_RESTRICTION && !hadText)) { Log(L"bloqueio: senha incorreta"); Fail(); return 0; }
        if (err == ERROR_ACCOUNT_LOCKED_OUT) {
            Log(L"bloqueio: conta travada pelo Windows (1909)");
            g_shakeAt = anim::Now();
            g_msg = Tr(L"O Windows travou a conta por tentativas. Use Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear e o PIN.");
            return 0;
        }
        Log(L"bloqueio: LogonUserW falhou com %lu (não é senha errada): aceito para não travar", err);
        Unlock(L"erro do LogonUserW aceito");
        return 0;
    }
    case WM_BQ_HELLO:
        g_helloBusy = false;
        if (w) Unlock(L"Windows Hello");
        else Assert(true);
        return 0;
    case WM_CLOSE: return 0;
    case WM_SYSCOMMAND: if ((w & 0xFFF0) == SC_CLOSE || (w & 0xFFF0) == SC_KEYMENU) return 0; break;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_MOUSEMOVE: if (me && me->primary && g_menuOpen) {
        float sc = ui::Scale(h); int hv = MenuHit({ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc });
        if (hv != g_menuHover) { g_menuHover = hv; PaintAll(); }
    } return 0;
    case WM_LBUTTONDOWN: {
        Mon* p = Primary();
        if (!me || !me->primary) { if (p) SetForegroundWindow(p->h); return 0; }
        float sc = ui::Scale(h);
        D2D1_POINT_2F pt{ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc };
        if (g_menuOpen) { int i = MenuHit(pt); g_menuOpen = false; if (i >= 0) Power(i); PaintAll(); return 0; }
        if (pt.x >= g_powerRc.left && pt.x <= g_powerRc.right && pt.y <= g_powerRc.bottom) { g_menuOpen = true; g_menuHover = -1; PaintAll(); return 0; }
        RECT rc; GetClientRect(h, &rc);
        Geo g = GeoOf(rc.right / sc, rc.bottom / sc);
        if (!g_pw.empty() && std::fabs(pt.x - (g.cx + FIELD_W / 2 - 11)) < 10 && std::fabs(pt.y - (g.fieldTop + FIELD_H / 2)) < 10) Submit(h);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (g_phase != LOCKED && g_phase != LOCKING) return 0;
        if (g_menuOpen && w == VK_ESCAPE) { g_menuOpen = false; PaintAll(); return 0; }
        if (w == VK_ESCAPE) { Wipe(g_pw); PaintAll(); }
        if (w == VK_RETURN) Submit(h);
        if (w == VK_CAPITAL) PaintAll();
        return 0;
    case WM_CHAR:
        if (g_phase != LOCKED && g_phase != LOCKING) return 0;
        if (g_checking || GetTickCount64() < g_waitUntil) return 0;
        if (w == 8) { if (!g_pw.empty()) { g_pw.back() = 0; g_pw.pop_back(); } }
        else if (w >= 32 && g_pw.size() < 256) g_pw += (wchar_t)w;
        else return 0;
        if (g_fails < 2 && !g_waitUntil) g_msg.clear();
        g_caretAt = anim::Now();
        PaintAll();
        return 0;
    case WM_SYSCHAR: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// janela escondida: desbloqueio do Windows tira a nossa tela; tela apagada trava; mensagem de fora trava
LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static UINT lockMsg = RegisterWindowMessageW(L"ShellBloquearTela");
    if (m == lockMsg && lockMsg) {
        if (w == 9) { if (g_phase == LOCKED) { g_demo = true; Unlock(L"teste"); } }      // fecha o teste
        else if (w == 8) { if (g_phase == LOCKED) { g_shakeAt = anim::Now(); g_fails++; g_msg = g_fails >= 2 ? Tr(L"Senha incorreta. Esqueceu? Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear usa o PIN do Windows.") : L""; } }  // teste do chacoalho, sem senha
        else bloqueio::Lock();
        return 0;
    }
    switch (m) {
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_UNLOCK && (g_phase == LOCKED || g_phase == LOCKING)) Unlock(L"Windows desbloqueado (PIN/senha do Windows)");
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            auto* ps = (POWERBROADCAST_SETTING*)l;
            if (ps && ps->PowerSetting == GUID_CONSOLE_DISPLAY_STATE && ps->DataLength >= 4 && *(DWORD*)ps->Data == 0 &&
                shell::Setting(L"bloqueio_ao_apagar", L"1") == L"1" && Enabled() && g_phase == HIDDEN) {
                Log(L"bloqueio: tela apagou");
                bloqueio::Lock();
            }
        } else if (w == PBT_APMSUSPEND && shell::Setting(L"bloqueio_ao_apagar", L"1") == L"1" && Enabled() && g_phase == HIDDEN) {
            Log(L"bloqueio: suspendendo");
            bloqueio::Lock();
        }
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

// vigia: outro Shell.exe que espera este sair; se sumir sem avisar, apaga a política e, se a tela estava
// travada, bloqueia o Windows
void StartWatchdog() {
    if (g_endEvent) return;
    wchar_t name[64];
    swprintf_s(name, L"ShellBloqueioFim-%lu", GetCurrentProcessId());
    g_endEvent = CreateEventW(nullptr, TRUE, FALSE, name);
    swprintf_s(name, L"ShellBloqueioAtivo-%lu", GetCurrentProcessId());
    g_lockedEvent = CreateEventW(nullptr, TRUE, FALSE, name);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t cmd[MAX_PATH + 64]; swprintf_s(cmd, L"\"%ls\" --vigia %lu %d", exe, GetCurrentProcessId(), g_policySet ? 1 : 0);
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

// ---- imagem em arquivo (testes sem cobrir a tela de ninguém)
bool SavePng(ID2D1Bitmap1* bmp, int w, int h, const std::wstring& path) {
    auto* dc = ui::Gpu();
    ID2D1Bitmap1* cpu = nullptr;
    dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &cpu);
    if (!cpu) return false;
    cpu->CopyFromBitmap(nullptr, bmp, nullptr);
    D2D1_MAPPED_RECT mr{};
    bool ok = false;
    if (SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mr))) {
        IWICImagingFactory* wic = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr))) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
            fr->SetSize(w, h); fr->SetPixelFormat(&fmt);
            ok = SUCCEEDED(fr->WritePixels(h, mr.pitch, mr.pitch * h, mr.bits)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        }
        if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wic) wic->Release();
        cpu->Unmap();
    }
    cpu->Release();
    return ok;
}

}  // namespace

bool bloqueio::Active() { int p = g_phase; return p == LOCKING || p == LOCKED; }

void bloqueio::Create(HINSTANCE hi) {
    g_hi = hi;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellBloqueio";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW cc{}; cc.lpfnWndProc = CtlProc; cc.hInstance = hi; cc.lpszClassName = L"ShellBloqueioControle";
    RegisterClassW(&cc);
    g_ctl = CreateWindowExW(WS_EX_TOOLWINDOW, cc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    if (!g_ctl) return;
    apps::ours.push_back(g_ctl);
    ChangeWindowMessageFilterEx(g_ctl, RegisterWindowMessageW(L"ShellBloquearTela"), MSGFLT_ALLOW, nullptr);
    WTSRegisterSessionNotification(g_ctl, NOTIFY_FOR_THIS_SESSION);
    RegisterPowerSettingNotification(g_ctl, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    std::thread([] {  // Windows Hello disponível? (PIN, rosto ou digital configurados)
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            using namespace winrt::Windows::Security::Credentials::UI;
            g_helloOk = UserConsentVerifier::CheckAvailabilityAsync().get() == UserConsentVerifierAvailability::Available;
        } catch (...) {}
    }).detach();
    if (!g_preview && Enabled() && shell::Setting(L"tecla_windows_launchpad", L"1") == L"1" && shell::Setting(L"bloqueio_winl", L"1") != L"0") {
        DWORD cur = 0, cb = sizeof(cur);
        bool already = RegGetValueW(HKEY_CURRENT_USER, POL_KEY, L"DisableLockWorkstation", RRF_RT_REG_DWORD, nullptr, &cur, &cb) == ERROR_SUCCESS && cur == 1;
        DWORD one = 1;
        if (!already && RegSetKeyValueW(HKEY_CURRENT_USER, POL_KEY, L"DisableLockWorkstation", REG_DWORD, &one, sizeof(one)) == ERROR_SUCCESS) g_policySet = true;
        Log(L"bloqueio: Win+L para o Shell (DisableLockWorkstation %ls)", g_policySet ? L"ligada enquanto o Shell roda" : already ? L"já estava ligada" : L"ERRO ao ligar");
    }
    if (!g_preview) StartWatchdog();
    Log(L"bloqueio: pronto (%ls)", Enabled() ? L"ligado" : L"desligado: Win+L vai para o Windows");
}

void bloqueio::Lock() {
    if (!Enabled()) { LockWindows(); return; }
    if (g_phase == LOCKED || g_phase == LOCKING) return;
    if (g_phase == UNLOCKING) Finish();
    LoadUser();
    Wipe(g_pw);
    g_checking = g_menuOpen = false; g_msg.clear(); g_shakeAt = -1e9;
    g_mons.clear();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(hm, &mi);
        Mon m; m.r = mi.rcMonitor; m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        g_mons.push_back(m);
        return TRUE;
    }, 0);
    for (auto& m : g_mons) {
        m.h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"ShellBloqueio", L"Tela de Bloqueio", WS_POPUP,
                              m.r.left, m.r.top, m.r.right - m.r.left, m.r.bottom - m.r.top, nullptr, nullptr, g_hi, nullptr);
        if (!m.h) continue;
        apps::ours.push_back(m.h);
        m.s.hwnd = m.h;
        BuildBitmaps(m, ui::Scale(m.h));
    }
    Mon* p = Primary();
    if (!p || !p->h) { Log(L"bloqueio: erro criando a janela; usando o bloqueio do Windows"); for (auto& m : g_mons) FreeMon(m); g_mons.clear(); LockWindows(); return; }
    g_phase = LOCKING; g_phaseAt = g_caretAt = anim::Now();
    if (!g_preview) { if (g_lockedEvent) SetEvent(g_lockedEvent); dock::SetFullscreen(true); menubar::SetFullscreen(true); }
    PaintAll();
    for (auto& m : g_mons) if (m.h) ShowWindow(m.h, m.primary ? SW_SHOW : SW_SHOWNOACTIVATE);
    Assert(true);
    SetTimer(p->h, 1, 250, nullptr);
    SetTimer(p->h, 2, 1000, nullptr);
    ui::FrameClock(p->h, WM_BQ_FRAME, true);
    Log(L"bloqueio: tela travada (%zu monitor(es))", g_mons.size());
}

void bloqueio::Fallback() {
    if (Active()) { Log(L"bloqueio: o Shell saiu com a tela travada: bloqueando o Windows"); LockWindows(); }
    else if (g_policySet) { PolicyOff(); g_policySet = false; }
}

void bloqueio::Stop() {  // Shell saindo: o Win+L volta para o Windows e o vigia é dispensado
    if (g_policySet) { PolicyOff(); g_policySet = false; Log(L"bloqueio: DisableLockWorkstation apagada"); }
    if (g_endEvent) SetEvent(g_endEvent);
}

bool bloqueio::Watchdog(const wchar_t* cmdLine) {
    const wchar_t* a = wcsstr(cmdLine, L"--vigia ");
    if (!a) return false;
    wchar_t* end = nullptr;
    DWORD pid = wcstoul(a + 8, &end, 10);
    bool pol = end && wcstoul(end, nullptr, 10) == 1;
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    wchar_t name[64]; swprintf_s(name, L"ShellBloqueioFim-%lu", pid);
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, name);
    swprintf_s(name, L"ShellBloqueioAtivo-%lu", pid);
    HANDLE locked = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (proc && ev) {
        HANDLE hs[2] = { ev, proc };
        if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1 && WaitForSingleObject(ev, 0) != WAIT_OBJECT_0) {
            // o Shell sumiu sem avisar: o Win+L volta para o Windows; se a tela estava travada, bloqueia de verdade
            if (pol) PolicyOff();
            if (locked && WaitForSingleObject(locked, 0) == WAIT_OBJECT_0) LockWindows();
        }
    } else if (pol) PolicyOff();
    if (proc) CloseHandle(proc);
    if (ev) CloseHandle(ev);
    if (locked) CloseHandle(locked);
    return true;
}

// Shell.exe --senha-bloqueio: define (ou remove) a senha própria da tela de bloqueio. Só o resumo é gravado.
namespace {
HWND g_e1, g_e2, g_info;
std::wstring EditText(HWND e) { int n = GetWindowTextLengthW(e); std::wstring s(n, L'\0'); GetWindowTextW(e, s.data(), n + 1); return s; }
LRESULT CALLBACK PassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if (id == 1) {
            std::wstring a = EditText(g_e1), b = EditText(g_e2);
            if (a.empty()) SetWindowTextW(g_info, Tr(L"Digite a senha."));
            else if (a != b) SetWindowTextW(g_info, Tr(L"As duas senhas não são iguais."));
            else if (SaveOwnPassword(a)) { MessageBoxW(h, Tr(L"Senha da tela de bloqueio definida."), Tr(L"Tela de Bloqueio"), MB_ICONINFORMATION); DestroyWindow(h); }
            else SetWindowTextW(g_info, Tr(L"Não consegui gravar a senha."));
            Wipe(a); Wipe(b);
            SetWindowTextW(g_e1, L""); SetWindowTextW(g_e2, L"");
            return 0;
        }
        if (id == 2) {
            DeleteFileW(PassFile().c_str());
            MessageBoxW(h, Tr(L"Senha própria removida: a tela volta a usar a senha da conta do Windows."), Tr(L"Tela de Bloqueio"), MB_ICONINFORMATION);
            DestroyWindow(h);
            return 0;
        }
        if (id == 3) { DestroyWindow(h); return 0; }
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
}
int bloqueio::SetPasswordUi(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = PassProc; wc.hInstance = hi; wc.lpszClassName = L"ShellBloqueioSenha";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Senha da Tela de Bloqueio", WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 380, 250, nullptr, nullptr, hi, nullptr);
    float k = GetDpiForWindow(h) / 96.f;
    auto S = [k](int v) { return (int)(v * k); };
    SetWindowPos(h, nullptr, 0, 0, S(380), S(250), SWP_NOMOVE | SWP_NOZORDER);
    HFONT f = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int x, int y, int w, int hh, int id) {
        HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, S(x), S(y), S(w), S(hh), h, (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    };
    mk(L"STATIC", HasOwnPassword() ? Tr(L"Nova senha da tela de bloqueio (substitui a atual):") : Tr(L"Senha da tela de bloqueio (só desta tela; a do Windows não muda):"), 0, 16, 12, 340, 36, 0);
    g_e1 = mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 16, 52, 330, 26, 10);
    mk(L"STATIC", Tr(L"Repita:"), 0, 16, 86, 330, 20, 0);
    g_e2 = mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 16, 106, 330, 26, 11);
    g_info = mk(L"STATIC", L"", 0, 16, 138, 330, 20, 0);
    mk(L"BUTTON", Tr(L"Definir"), WS_TABSTOP | BS_DEFPUSHBUTTON, 246, 166, 100, 30, 1);
    mk(L"BUTTON", Tr(L"Usar a do Windows"), WS_TABSTOP, 16, 166, 140, 30, 2);
    mk(L"BUTTON", Tr(L"Cancelar"), WS_TABSTOP, 160, 166, 80, 30, 3);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(g_e1);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(h, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return 0;
}

// Shell.exe --testar-bloqueio: a tela de verdade por ~3,5 s (chacoalha em 1,6 s), sem conferir senha
int bloqueio::Preview(HINSTANCE hi) {
    g_preview = true;
    Create(hi);
    Lock();
    if (g_mons.empty()) return 1;
    SetTimer(g_ctl, 10, 1600, [](HWND, UINT, UINT_PTR id, DWORD) { KillTimer(g_ctl, id); g_shakeAt = anim::Now(); g_fails = 2;
        g_msg = Tr(L"Senha incorreta. Esqueceu? Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear usa o PIN do Windows."); });
    SetTimer(g_ctl, 11, 3200, [](HWND, UINT, UINT_PTR id, DWORD) { KillTimer(g_ctl, id); g_demo = true; Unlock(L"teste"); });
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

// Shell.exe --bloqueio-png <saida.png> <largura> <altura> <fase 1..3> <ms> [chacoalho_ms] [senha_fake_n]
bool bloqueio::RenderPng(const wchar_t* cmdLine) {
    const wchar_t* a = wcsstr(cmdLine, L"--bloqueio-png ");
    if (!a) return false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(a, &argc);
    if (!argv || argc < 6) return true;
    std::wstring out = argv[1];
    int w = _wtoi(argv[2]), h = _wtoi(argv[3]), ph = _wtoi(argv[4]);
    double t = _wtof(argv[5]), shake = argc > 6 ? _wtof(argv[6]) : -1;
    int dots = argc > 7 ? _wtoi(argv[7]) : 0;
    LocalFree(argv);
    LoadUser();
    Mon m; m.r = { 0, 0, w, h }; m.primary = true;
    BuildBitmaps(m, 1);
    double now = anim::Now();
    g_phase = ph; g_phaseAt = now - t;
    if (shake >= 0) { g_shakeAt = now - shake; g_fails = 2; g_msg = Tr(L"Senha incorreta. Esqueceu? Ctrl⁠+⁠Alt⁠+⁠Del → Bloquear usa o PIN do Windows."); }
    g_pw.assign(dots, L'x');
    g_caretAt = now;
    auto* dc = ui::Gpu();
    ID2D1Bitmap1* target = MakeTarget(w, h, 1);
    // fundo de "mesa" para ver a transparência nas fases de entrada/saída
    dc->SetTarget(target); dc->SetDpi(96, 96); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0.3f, 0.3f, 0.3f));
    Draw(dc, m, (float)w, (float)h, now);
    dc->EndDraw(); dc->SetTarget(nullptr);
    bool ok = SavePng(target, w, h, out);
    target->Release();
    Wipe(g_pw);
    g_phase = HIDDEN;
    wprintf(L"%ls\n", ok ? L"ok" : L"erro");
    return true;
}
