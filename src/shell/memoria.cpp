// Memória como no macOS. Três partes:
//
// 1. Pressão de memória (comum/pressao.h, a mesma conta do Monitor de Atividade). A cada 2 s o Shell mede;
//    quando sobe para amarelo ele faz o que o Mac faz com a "memória descartável": solta o que dá para
//    refazer depois (cache da GPU dos três dispositivos D3D do Shell, páginas paradas do próprio Shell, da
//    Captura de Tela residente, do Finder reserva e dos apps em soneca; o Windows comprime essas páginas ou
//    as guarda em espera, sem perder nada). No vermelho abre a janela "Forçar Encerrar" no modo de memória
//    ("Seu sistema ficou sem memória para aplicativos"), no máximo uma vez a cada 10 minutos; quem encerra
//    é sempre o usuário, pelo botão.
//
// 2. App Nap (Soneca de Apps): um programa com janelas, mas nenhuma delas à vista (todas minimizadas, em
//    outra mesa ou cobertas por outras janelas) há 30 s, entra no modo eficiência do Windows (EcoQoS:
//    processador mais lento e timers agrupados, como o App Nap). Volta ao normal na hora em que alguma
//    janela dele aparece ou vem para a frente. Ficam de fora: o que está tocando ou gravando som, o
//    próprio Shell, o Explorador, programas elevados (o Windows não deixa) e a lista app_nap_excluir do
//    ajustes.txt (nomes de exe separados por vírgula; * no fim vale como prefixo; o padrão deixa de fora
//    Ableton, AudioCore, o agente de trabalho Insightful/Workpuls e o app do Claude). Nada é gravado no
//    sistema: o estado de antes de cada programa é guardado e devolvido quando ele acorda ou quando o
//    Shell sai. Ajustes > Bateria > "Soneca de Apps" (app_nap=1, padrão) liga e desliga.
//
// 3. Páginas paradas dos ajudantes do Shell: a Captura de Tela residente e o Finder reserva ficam escondidos
//    esperando; a cada 5 minutos as páginas deles vão para a memória comprimida (o Mac faz isso com o que
//    está ocioso). Ao serem usados, voltam da RAM sem ler o disco.
#include "shell.h"
#include "../comum/pressao.h"
#include "../comum/nomes.h"
#include <dwmapi.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

ID3D11Device* BackdropD3D();  // vidro real (backdrop.cpp)
ID3D11Device* GenioD3D();     // gênio (genio.cpp)

namespace {

const UINT_PTR T_MEM = 0x4D454D;
const DWORD NAP_AFTER = 30000;      // escondido há 30 s
const DWORD HELPERS_EVERY = 300000; // ajudantes: a cada 5 min

struct Nap {
    std::wstring exe;
    DWORD hiddenSince = 0;   // 0 = à vista
    DWORD retryAt = 0;       // a soneca falhou: só tenta de novo depois disso
    bool napped = false;
    PROCESS_POWER_THROTTLING_STATE before{};  // como estava antes da soneca
    bool hadBefore = false;
};

std::map<DWORD, Nap> g_procs;
HWINEVENTHOOK g_ev;
int g_level;
DWORD g_lastForceQuit, g_lastHelpers, g_startedAt;
IMMDeviceEnumerator* g_mm;

std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

std::wstring ExeOf(DWORD pid) {
    std::wstring r;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t p[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, p, &n)) { r = p; r = r.substr(r.find_last_of(L'\\') + 1); }
        CloseHandle(h);
    }
    return Lower(r);
}

bool NapOn() { return shell::Setting(L"app_nap", L"1") != L"0"; }

bool Excluded(const std::wstring& exe) {
    static const wchar_t* always[] = { L"liquinixshell.exe", L"liquinixwindow32host.exe", L"explorer.exe", L"dwm.exe", L"searchhost.exe",
                                       L"startmenuexperiencehost.exe", L"shellexperiencehost.exe", L"textinputhost.exe", L"applicationframehost.exe",
                                       L"lockapp.exe", L"taskmgr.exe", L"obs64.exe", L"obs32.exe" };
    for (auto* a : always) if (exe == a) return true;
    std::wstring list = Lower(shell::Setting(L"app_nap_excluir", L"ableton*,audiocore*,insightful*,workpuls*,claude.exe"));
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(L',', i); if (j == std::wstring::npos) j = list.size();
        std::wstring one = list.substr(i, j - i);
        while (!one.empty() && one.front() == L' ') one.erase(0, 1);
        while (!one.empty() && one.back() == L' ') one.pop_back();
        if (!one.empty()) {
            if (one.back() == L'*') { if (exe.rfind(one.substr(0, one.size() - 1), 0) == 0) return true; }
            else if (exe == one || exe == one + L".exe") return true;
        }
        i = j + 1;
    }
    return false;
}

// quem está tocando ou gravando som agora (sessões de áudio ativas em qualquer saída ou entrada)
std::set<DWORD> AudioPids() {
    std::set<DWORD> r;
    if (!g_mm) CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_mm));
    if (!g_mm) return r;
    for (EDataFlow flow : { eRender, eCapture }) {
        IMMDeviceCollection* col = nullptr;
        if (FAILED(g_mm->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || !col) continue;
        UINT n = 0; col->GetCount(&n);
        for (UINT d = 0; d < n; d++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(d, &dev)) || !dev) continue;
            IAudioSessionManager2* man = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&man)) && man) {
                IAudioSessionEnumerator* en = nullptr;
                if (SUCCEEDED(man->GetSessionEnumerator(&en)) && en) {
                    int cnt = 0; en->GetCount(&cnt);
                    for (int s = 0; s < cnt; s++) {
                        IAudioSessionControl* c = nullptr;
                        if (FAILED(en->GetSession(s, &c)) || !c) continue;
                        AudioSessionState st;
                        IAudioSessionControl2* c2 = nullptr;
                        if (SUCCEEDED(c->GetState(&st)) && st == AudioSessionStateActive && SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
                            DWORD pid = 0; c2->GetProcessId(&pid);
                            if (pid) r.insert(pid);
                        }
                        if (c2) c2->Release();
                        c->Release();
                    }
                    en->Release();
                }
                man->Release();
            }
            dev->Release();
        }
        col->Release();
    }
    return r;
}

bool SetThrottle(DWORD pid, bool on, Nap& n) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
    if (!h) return false;
    bool ok;
    if (on) {
        PROCESS_POWER_THROTTLING_STATE cur{ PROCESS_POWER_THROTTLING_CURRENT_VERSION };
        n.hadBefore = GetProcessInformation(h, ProcessPowerThrottling, &cur, sizeof(cur)) != FALSE;
        n.before = cur;
        if (n.hadBefore && (cur.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED)) { CloseHandle(h); return false; }  // já se cuida sozinho
        PROCESS_POWER_THROTTLING_STATE s{ PROCESS_POWER_THROTTLING_CURRENT_VERSION };
        s.ControlMask = s.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        ok = SetProcessInformation(h, ProcessPowerThrottling, &s, sizeof(s)) != FALSE;
    } else {
        PROCESS_POWER_THROTTLING_STATE s{ PROCESS_POWER_THROTTLING_CURRENT_VERSION };  // 0/0 = o Windows decide
        if (n.hadBefore) { s.ControlMask = n.before.ControlMask; s.StateMask = n.before.StateMask; }
        ok = SetProcessInformation(h, ProcessPowerThrottling, &s, sizeof(s)) != FALSE;
    }
    CloseHandle(h);
    return ok;
}

void Wake(DWORD pid, const wchar_t* why) {
    auto it = g_procs.find(pid);
    if (it == g_procs.end()) return;
    it->second.hiddenSince = 0;
    if (!it->second.napped) return;
    SetThrottle(pid, false, it->second);
    it->second.napped = false;
    Log(L"memoria: %ls acordou (%ls)", it->second.exe.c_str(), why);
}

void Trim(DWORD pid) {
    if (HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        SetProcessWorkingSetSizeEx(h, (SIZE_T)-1, (SIZE_T)-1, 0);
        CloseHandle(h);
    }
}

void TrimGpu(ID3D11Device* d) {
    if (!d) return;
    IDXGIDevice3* dx = nullptr;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&dx))) && dx) { dx->Trim(); dx->Release(); }
}

bool Covered(HRGN covered, const RECT& r) {
    HRGN me = CreateRectRgnIndirect(&r);
    int k = CombineRgn(me, me, covered, RGN_DIFF);
    DeleteObject(me);
    return k == NULLREGION;
}

// janelas por processo, de cima para baixo: quem tem alguma janela à vista
struct Scan { std::map<DWORD, bool> any, visible; HRGN covered; RECT desk; DWORD self; };

BOOL CALLBACK ScanWin(HWND h, LPARAM l) {
    Scan& s = *(Scan*)l;
    if (!IsWindowVisible(h)) return TRUE;
    BOOL cloaked = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == s.self) return TRUE;  // a barra, o Dock e os painéis do Shell não cobrem nada
    bool app = (st & WS_CAPTION) == WS_CAPTION || (ex & WS_EX_APPWINDOW);
    if (ex & WS_EX_TOOLWINDOW) app = false;
    bool iconic = IsIconic(h) != FALSE;
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    RECT on{};
    bool onScreen = IntersectRect(&on, &r, &s.desk) != FALSE;
    if (app) {
        s.any[pid];
        if (!cloaked && !iconic && onScreen && !Covered(s.covered, on)) s.visible[pid] = true;
    }
    // cobre o que está atrás: janelas normais e opacas (as de clique atravessado e as transparentes não)
    if (!cloaked && !iconic && onScreen && !((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) && (r.right - r.left) > 80 && (r.bottom - r.top) > 80) {
        HRGN add = CreateRectRgnIndirect(&on);
        CombineRgn(s.covered, s.covered, add, RGN_OR);
        DeleteObject(add);
    }
    return TRUE;
}

void NapTick(DWORD now) {
    if (!NapOn()) {
        for (auto& [pid, n] : g_procs) if (n.napped) { SetThrottle(pid, false, n); n.napped = false; }
        g_procs.clear();
        return;
    }
    Scan s{};
    s.self = GetCurrentProcessId();
    s.covered = CreateRectRgn(0, 0, 0, 0);
    s.desk = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
               GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    EnumWindows(ScanWin, (LPARAM)&s);
    DeleteObject(s.covered);
    DWORD fgPid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    std::set<DWORD> audio = AudioPids();

    // processos que sumiram
    for (auto it = g_procs.begin(); it != g_procs.end();) it = s.any.count(it->first) ? std::next(it) : g_procs.erase(it);

    // por programa, não por processo: o Chrome (Edge, Discord, Spotify...) toca o som num processo de áudio à parte e
    // desenha o vídeo em outro; só o da janela entrava em soneca e o YouTube tocando atrás de outra janela travava
    // (17/11/2026). Se qualquer processo do mesmo exe toca som ou está à vista, o programa inteiro fica acordado.
    std::set<std::wstring> soundExe, shownExe;
    for (DWORD a : audio) { std::wstring e = ExeOf(a); if (!e.empty()) soundExe.insert(Lower(e)); }
    for (auto& [pid, _] : s.any) {
        auto [it, fresh] = g_procs.try_emplace(pid);
        Nap& n = it->second;
        if (fresh) n.exe = ExeOf(pid);
        if (s.visible.count(pid) || pid == fgPid) shownExe.insert(Lower(n.exe));
    }
    for (auto& [pid, _] : s.any) {
        Nap& n = g_procs[pid];
        std::wstring le = Lower(n.exe);
        bool sound = audio.count(pid) || soundExe.count(le);
        bool awake = s.visible.count(pid) || pid == fgPid || shownExe.count(le) || sound || n.exe.empty() || Excluded(n.exe);
        if (awake) { if (n.napped) Wake(pid, sound ? L"som" : L"à vista"); n.hiddenSince = 0; continue; }
        if (!n.hiddenSince) n.hiddenSince = now ? now : 1;
        if (!n.napped && now - n.hiddenSince >= NAP_AFTER && (!n.retryAt || (int)(now - n.retryAt) >= 0)) {
            if (SetThrottle(pid, true, n)) { n.napped = true; Log(L"memoria: soneca para %ls (%u)", n.exe.c_str(), pid); }
            else n.retryAt = now + 600000;  // não deu (elevado ou já em eficiência): tenta de novo em 10 min
        }
    }
}

void Purge(const wchar_t* why) {
    TrimGpu(ui::D3D());
    TrimGpu(BackdropD3D());
    TrimGpu(GenioD3D());
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
    int n = 0;
    for (auto& [pid, p] : g_procs) if (p.napped) { Trim(pid); n++; }
    // ajudantes do Shell escondidos
    for (const wchar_t* cls : { L"SequoiaCapturaMsg" }) if (HWND w = FindWindowW(cls, nullptr)) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); Trim(pid); }
    if (HWND w = FindWindowW(L"ArquivosJanela", L"​ArquivosReserva")) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); Trim(pid); }
    Log(L"memoria: liberada (%ls; %d apps em soneca compactados)", why, n);
}

void HelpersTick(DWORD now) {
    if (now - g_startedAt < 90000 || now - g_lastHelpers < HELPERS_EVERY) return;
    g_lastHelpers = now;
    if (HWND w = FindWindowW(L"SequoiaCapturaMsg", nullptr)) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); Trim(pid); }
    if (HWND w = FindWindowW(L"ArquivosJanela", L"​ArquivosReserva")) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); Trim(pid); }
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    DWORD now = GetTickCount();
    PressaoMemoria p = MedirPressao();
    if (p.nivel != g_level) {
        Log(L"memoria: pressão %ls (disponível %.0f%%, commit %.0f%%)", p.nivel == 2 ? L"crítica" : p.nivel == 1 ? L"alta" : L"normal", p.livre * 100, p.commit * 100);
        if (p.nivel > g_level) Purge(p.nivel == 2 ? L"pressão crítica" : L"pressão alta");
        if (p.nivel == 2 && (!g_lastForceQuit || now - g_lastForceQuit > 600000)) { g_lastForceQuit = now; shell::OpenForceQuit(true); }
        g_level = p.nivel;
    }
    static int k;
    if (++k % 2 == 0) NapTick(now);  // a cada 4 s
    HelpersTick(now);
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h || (ev != EVENT_SYSTEM_FOREGROUND && ev != EVENT_SYSTEM_MINIMIZEEND)) return;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid) Wake(pid, L"veio para a frente");
}

}  // namespace

namespace memoria {

void Start() {
    g_startedAt = GetTickCount();
    g_level = MedirPressao().nivel;
    SetTimer(nullptr, T_MEM, 2000, Tick);
    g_ev = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    Log(L"memoria: ligado (pressão %ls; soneca de apps %ls)", g_level == 2 ? L"crítica" : g_level == 1 ? L"alta" : L"normal", NapOn() ? L"ligada" : L"desligada");
}

void Stop() {
    if (g_ev) { UnhookWinEvent(g_ev); g_ev = nullptr; }
    int n = 0;
    for (auto& [pid, p] : g_procs) if (p.napped) { SetThrottle(pid, false, p); p.napped = false; n++; }
    g_procs.clear();
    if (g_mm) { g_mm->Release(); g_mm = nullptr; }
    if (n) Log(L"memoria: %d apps acordados ao sair", n);
}

int Level() { return g_level; }

bool Napping(DWORD pid) { auto it = g_procs.find(pid); return it != g_procs.end() && it->second.napped; }

}  // namespace memoria
