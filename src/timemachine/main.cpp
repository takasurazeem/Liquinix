// Time Machine (Mac) sobre o Histórico de Arquivos do Windows: o ícone grande à esquerda e, à direita, o disco de
// backup, o último backup, o estado e "Fazer backup automaticamente". Escolher Disco… lista os discos (externos
// e de rede primeiro); Fazer Backup Agora pede ao serviço do Windows; Restaurar Arquivos… abre a restauração do
// Windows (a "viagem no tempo" das versões); Opções: de quanto em quanto tempo e por quanto tempo guardar.
// Nada é guardado pelo app: tudo é a configuração do Histórico de Arquivos (fhsvc).
#include "../comum/app2d.h"
#include <FhCfg.h>
#include <FhSvcCtl.h>
#include <FhStatus.h>
#include <FhErrors.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <shellapi.h>
#pragma comment(lib, "FhSvcCtl.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")

using namespace app2d;

namespace {

IFhConfigMgr* g_cfg;
bool g_hasConfig = false;
std::wstring g_target, g_targetUrl, g_until, g_state, g_msg;
FH_BACKUP_STATUS g_status = FH_STATUS_DISABLED;
ULONGLONG g_freq = 3600;
ID2D1Bitmap* g_icon;
IDWriteTextFormat *g_tfTitle, *g_tfRow, *g_tfLabel, *g_tfBtn, *g_tfSmall;
struct Hit { int kind; D2D1_RECT_F r; };
std::vector<Hit> g_hits;
enum { H_TOGGLE = 1, H_DISK, H_NOW, H_RESTORE, H_OPTIONS, H_SETUP };

std::wstring StateText(DWORD s) {
    switch (s) {
    case FH_STATE_OFF: return Tr(L"Desligado");
    case FH_STATE_DISABLED_BY_GP: return Tr(L"Desligado pela política do sistema");
    case FH_STATE_FATAL_CONFIG_ERROR: return Tr(L"Erro na configuração");
    case FH_STATE_TARGET_ACCESS_DENIED: return Tr(L"Sem acesso ao disco de backup");
    case FH_STATE_TARGET_FULL: case FH_STATE_TARGET_FULL_RETENTION_MAX: return Tr(L"O disco de backup está cheio");
    case FH_STATE_TARGET_LOW_SPACE: return Tr(L"Pouco espaço no disco de backup");
    case FH_STATE_TARGET_ABSENT: return Tr(L"O disco de backup não está conectado");
    case FH_STATE_TOO_MUCH_BEHIND: return Tr(L"Os backups estão atrasados");
    case FH_STATE_NO_ERROR: return Tr(L"Tudo em dia");
    case FH_STATE_RUNNING: return Tr(L"Fazendo backup…");
    case FH_STATE_BACKUP_NOT_SUPPORTED: return Tr(L"Backup não suportado neste disco");
    }
    return L"";
}

void Refresh() {
    g_target.clear(); g_targetUrl.clear(); g_until.clear(); g_state.clear();
    if (!g_cfg) return;
    g_hasConfig = SUCCEEDED(g_cfg->LoadConfiguration());
    if (!g_hasConfig) return;
    g_cfg->GetBackupStatus(&g_status);
    IFhTarget* t = nullptr;
    if (SUCCEEDED(g_cfg->GetDefaultTarget(&t)) && t) {
        BSTR s = nullptr;
        if (SUCCEEDED(t->GetStringProperty(FH_TARGET_NAME, &s)) && s) { g_target = s; SysFreeString(s); }
        if (SUCCEEDED(t->GetStringProperty(FH_TARGET_URL, &s)) && s) { g_targetUrl = s; SysFreeString(s); }
        t->Release();
    }
    DWORD st = 0; BSTR until = nullptr;
    if (SUCCEEDED(g_cfg->QueryProtectionStatus(&st, &until))) { g_state = StateText(st); if (until) { g_until = until; SysFreeString(until); } }
    ULONGLONG f = 0; if (SUCCEEDED(g_cfg->GetLocalPolicy(FH_FREQUENCY, &f)) && f) g_freq = f;
}

void Reload() {  // avisa o serviço da configuração nova
    FH_SERVICE_PIPE_HANDLE p = nullptr;
    if (SUCCEEDED(FhServiceOpenPipe(TRUE, &p))) { FhServiceReloadConfiguration(p); FhServiceClosePipe(p); }
}

ID2D1Bitmap* LoadIcon() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); PathRemoveFileSpecW(exe); PathRemoveFileSpecW(exe);
    std::wstring png = std::wstring(exe) + L"\\icones\\sequoia\\Time Machine.png";
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr; ID2D1Bitmap* b = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        rt->CreateBitmapFromWicBitmap(cv, &b);
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); f->Release();
    return b;
}

void Button(D2D1_RECT_F r, const std::wstring& t, int kind, bool primary, bool dark) {
    Fill(r, primary ? Rgb(10, 132, 255) : (dark ? Rgb(72, 72, 74) : Rgb(229, 229, 234)), 7);
    Text(t, g_tfBtn, r, primary ? Rgb(255, 255, 255) : (dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31)));
    g_hits.push_back({ kind, r });
}

void Paint() {
    if (!BeginPaint()) return;
    g_hits.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139), line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    rt->Clear(dark ? Rgb(36, 36, 38) : Rgb(242, 242, 245));
    if (!g_icon) g_icon = LoadIcon();
    if (g_icon) rt->DrawBitmap(g_icon, D2D1::RectF(36, 40, 196, 200), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    float X = 230, W = sz.width - 30;
    Text(L"Time Machine", g_tfTitle, D2D1::RectF(X, 34, W, 70), text);
    if (!g_cfg) { Text(Tr(L"O Histórico de Arquivos do Windows não está disponível neste computador."), g_tfRow, D2D1::RectF(X, 80, W, 110), gray); EndPaint(); return; }
    if (!g_hasConfig) {
        Text(Tr(L"Os backups ainda não foram configurados."), g_tfRow, D2D1::RectF(X, 80, W, 110), gray);
        Button(D2D1::RectF(X, 124, X + 220, 156), Tr(L"Escolher Disco de Backup…"), H_DISK, true, dark);
        if (!g_msg.empty()) Text(g_msg, g_tfSmall, D2D1::RectF(X, 170, W, 190), Rgb(255, 69, 58));
        EndPaint();
        return;
    }
    // interruptor "Fazer backup automaticamente"
    bool on = g_status == FH_STATUS_ENABLED;
    D2D1_RECT_F sw = D2D1::RectF(W - 50, 80, W, 108);
    Fill(sw, on ? Rgb(48, 209, 88) : (dark ? Rgb(72, 72, 74) : Rgb(209, 209, 214)), 14);
    Circle({ on ? sw.right - 14 : sw.left + 14, (sw.top + sw.bottom) / 2 }, 12, Rgb(255, 255, 255), true);
    g_hits.push_back({ H_TOGGLE, sw });
    Text(Tr(L"Fazer backup automaticamente"), g_tfRow, D2D1::RectF(X, 80, W - 60, 108), text);
    float y = 126;
    auto row = [&](const wchar_t* label, const std::wstring& value) {
        Fill(D2D1::RectF(X, y - 6, W, y - 5), line);
        Text(Tr(label), g_tfLabel, D2D1::RectF(X, y, X + 170, y + 26), gray);
        Text(value, g_tfRow, D2D1::RectF(X + 180, y, W, y + 26), text);
        y += 40;
    };
    row(L"Disco de backup", g_target.empty() ? std::wstring(Tr(L"Nenhum")) : g_target + (g_targetUrl.empty() ? L"" : L"  (" + g_targetUrl + L")"));
    row(L"Último backup", g_until.empty() ? std::wstring(Tr(L"Nenhum")) : g_until);
    row(L"Estado", g_state);
    wchar_t fq[64];
    if (g_freq < 3600) swprintf_s(fq, Tr(L"A cada %llu minutos"), g_freq / 60);
    else if (g_freq < 86400) swprintf_s(fq, g_freq == 3600 ? Tr(L"A cada hora") : Tr(L"A cada %llu horas"), g_freq / 3600);
    else wcscpy_s(fq, Tr(L"Diariamente"));
    row(L"Frequência", fq);
    y += 10;
    float bx = X;
    Button(D2D1::RectF(bx, y, bx + 190, y + 32), Tr(L"Fazer Backup Agora"), H_NOW, true, dark); bx += 200;
    Button(D2D1::RectF(bx, y, bx + 190, y + 32), semaforo::Leopard() ? Tr(L"Entrar no Time Machine") : Tr(L"Restaurar Arquivos…"), H_RESTORE, false, dark); bx += 200;
    y += 44; bx = X;
    Button(D2D1::RectF(bx, y, bx + 190, y + 32), Tr(L"Escolher Disco…"), H_DISK, false, dark); bx += 200;
    Button(D2D1::RectF(bx, y, bx + 190, y + 32), Tr(L"Opções…"), H_OPTIONS, false, dark);
    if (!g_msg.empty()) Text(g_msg, g_tfSmall, D2D1::RectF(X, y + 44, W, y + 64), gray);
    EndPaint();
}

void ChooseDisk(POINT scr) {
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> roots;
    wchar_t sysDrive[4] = L"C:\\"; GetEnvironmentVariableW(L"SystemDrive", sysDrive, 3); wcscat_s(sysDrive, L"\\");
    DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; d++) {
        if (!(mask & (1 << d))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + d), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_REMOVABLE && t != DRIVE_FIXED && t != DRIVE_REMOTE) continue;
        if (!_wcsicmp(root, sysDrive)) continue;  // o disco do sistema não serve de backup
        wchar_t label[MAX_PATH] = {};
        GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        ULARGE_INTEGER freeB{}, total{}; GetDiskFreeSpaceExW(root, &freeB, &total, nullptr);
        wchar_t item[200]; swprintf_s(item, L"%ls  %ls  (%.0f GB %ls)", root, label[0] ? label : (t == DRIVE_REMOVABLE ? Tr(L"Disco Removível") : Tr(L"Disco Local")), freeB.QuadPart / 1e9, Tr(L"livres"));
        AppendMenuW(m, MF_STRING, roots.size() + 1, item);
        roots.push_back(root);
    }
    if (roots.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, Tr(L"Nenhum outro disco encontrado"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c <= 0) return;
    std::wstring root = roots[c - 1];
    if (!g_hasConfig) { HRESULT hr = g_cfg->CreateDefaultConfiguration(TRUE); if (FAILED(hr)) { g_msg = Tr(L"Não foi possível criar a configuração do Histórico de Arquivos."); return; } g_hasConfig = true; }
    FH_DEVICE_VALIDATION_RESULT vr{};
    BSTR url = SysAllocString(root.c_str());
    g_cfg->ValidateTarget(url, &vr);
    if (vr != FH_VALID_TARGET && vr != FH_CURRENT_DEFAULT) {
        g_msg = vr == FH_TARGET_PART_OF_LIBRARY ? Tr(L"Esse disco faz parte das pastas protegidas; escolha outro.") : Tr(L"Esse disco não pode ser usado para backup.");
        SysFreeString(url); return;
    }
    wchar_t label[MAX_PATH] = {}; GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
    BSTR name = SysAllocString(label[0] ? label : root.c_str());
    HRESULT hr = g_cfg->ProvisionAndSetNewTarget(url, name);
    SysFreeString(url); SysFreeString(name);
    if (SUCCEEDED(hr)) { g_cfg->SetBackupStatus(FH_STATUS_ENABLED); g_cfg->SaveConfiguration(); Reload(); g_msg = Tr(L"Disco escolhido. O primeiro backup começa em instantes."); }
    else g_msg = Tr(L"Não foi possível usar esse disco.");
    Refresh();
}

void Options(POINT scr) {
    HMENU m = CreatePopupMenu(), fq = CreatePopupMenu(), keep = CreatePopupMenu();
    ULONGLONG fv[] = { 600, 900, 1200, 1800, 3600, 3 * 3600, 6 * 3600, 12 * 3600, 86400 };
    const wchar_t* fn[] = { L"10 minutos", L"15 minutos", L"20 minutos", L"30 minutos", L"1 hora", L"3 horas", L"6 horas", L"12 horas", L"Diariamente" };
    for (int i = 0; i < 9; i++) AppendMenuW(fq, MF_STRING | (g_freq == fv[i] ? MF_CHECKED : 0), 10 + i, Tr(fn[i]));
    ULONGLONG ret = 0, age = 0; g_cfg->GetLocalPolicy(FH_RETENTION_TYPE, &ret); g_cfg->GetLocalPolicy(FH_RETENTION_AGE, &age);
    AppendMenuW(keep, MF_STRING | (ret == FH_RETENTION_UNLIMITED ? MF_CHECKED : 0), 30, Tr(L"Para sempre"));
    AppendMenuW(keep, MF_STRING | (ret == FH_RETENTION_AGE_BASED && age == 30 ? MF_CHECKED : 0), 31, Tr(L"1 mês"));
    AppendMenuW(keep, MF_STRING | (ret == FH_RETENTION_AGE_BASED && age == 365 ? MF_CHECKED : 0), 32, Tr(L"1 ano"));
    AppendMenuW(keep, MF_STRING | (ret == FH_RETENTION_AGE_BASED && age == 730 ? MF_CHECKED : 0), 33, Tr(L"2 anos"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fq, Tr(L"Fazer backup"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)keep, Tr(L"Manter os backups"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c >= 10 && c < 19) g_cfg->SetLocalPolicy(FH_FREQUENCY, fv[c - 10]);
    else if (c == 30) g_cfg->SetLocalPolicy(FH_RETENTION_TYPE, FH_RETENTION_UNLIMITED);
    else if (c >= 31 && c <= 33) { g_cfg->SetLocalPolicy(FH_RETENTION_TYPE, FH_RETENTION_AGE_BASED); g_cfg->SetLocalPolicy(FH_RETENTION_AGE, c == 31 ? 30 : c == 32 ? 365 : 730); }
    if (c) { g_cfg->SaveConfiguration(); Reload(); Refresh(); }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); return 0;
    case WM_DPICHANGED: OnDpi(w, l); return 0;
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_TIMER: Refresh(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        for (auto& ht : g_hits) if (In(ht.r, x, y)) {
            POINT scr{ (LONG)std::lround(ht.r.left * s), (LONG)std::lround(ht.r.bottom * s) }; ClientToScreen(h, &scr);
            g_msg.clear();
            switch (ht.kind) {
            case H_TOGGLE: g_cfg->SetBackupStatus(g_status == FH_STATUS_ENABLED ? FH_STATUS_DISABLED : FH_STATUS_ENABLED); g_cfg->SaveConfiguration(); Reload(); Refresh(); break;
            case H_DISK: ChooseDisk(scr); break;
            case H_NOW: {
                FH_SERVICE_PIPE_HANDLE p = nullptr;
                if (SUCCEEDED(FhServiceOpenPipe(TRUE, &p))) { HRESULT hr = FhServiceStartBackup(p, FALSE); FhServiceClosePipe(p); g_msg = SUCCEEDED(hr) ? Tr(L"Backup iniciado.") : Tr(L"Não foi possível iniciar o backup (o disco está conectado?)."); }
                else g_msg = Tr(L"O serviço do Histórico de Arquivos não respondeu.");
                SetTimer(h, 1, 3000, nullptr);
                break;
            }
            case H_RESTORE: {
                // tema Leopard: a "viagem no tempo" do Mac (galáxia e a pilha de janelas), desenhada pelo Shell
                // (tempo.cpp); nos outros temas, a restauração do Windows
                HWND sh = semaforo::Leopard() ? FindWindowW(L"ShellMensagens", nullptr) : nullptr;
                if (sh) {
                    DWORD pid = 0; GetWindowThreadProcessId(sh, &pid);
                    AllowSetForegroundWindow(pid);
                    PostMessageW(sh, RegisterWindowMessageW(L"ShellAbrirPainel"), 18, 0);
                } else ShellExecuteW(nullptr, L"open", L"FileHistory.exe", nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            case H_OPTIONS: Options(scr); break;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: if (GetKeyState(VK_CONTROL) < 0 && w == 'W') DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    CoCreateInstance(__uuidof(FhConfigMgr), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_cfg));
    Refresh();
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfTitle = Fmt(26, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfRow = Fmt(14);
    g_tfLabel = Fmt(13);
    g_tfBtn = Fmt(13, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfSmall = Fmt(12);
    if (!CreateMain(hi, L"TimeMachineJanela", L"Time Machine", Proc, 780, 470)) return 1;
    SetTimer(wnd, 1, 10000, nullptr);
    ShowWindow(wnd, show);
    return Run();
}
