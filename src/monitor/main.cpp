// Monitor de Atividade: o gerenciador de tarefas no visual do macOS Sequoia.
// Barra de cima com as bolinhas, título e subtítulo, Encerrar (X), Informações (i), Ações (⋯), as abas
// CPU | Memória | Energia | Disco | Rede | GPU e a busca; tabela de processos com listras, ordenada pela
// coluna clicada; embaixo, o painel de cada aba (números, gráfico e números), como no Mac.
// Coleta a cada 2 s com uma chamada só (NtQuerySystemInformation: todos os processos, até os
// protegidos), mais GetSystemTimes, memória, placas de rede e conexões por processo.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <winternl.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include "../comum/vidro.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <cstdarg>
#include <share.h>
#include <shellscalingapi.h>
#include "../menu/context_menu.h"
#include "../comum/ampliacao.h"  // botão verde com a animação do ampliar do Shell
#include "../comum/idioma.h"
#include "../comum/pressao.h"  // pressão de memória como a do Mac (a mesma conta do Shell)
#include "../comum/semaforo.h"
#include "../comum/leopard.h"  // tema Mac OS X: o metal da barra unificada
#define LEO (semaforo::Leopard())

// Peças do Finder (src/finder): desenho com alfa real (Direct2D sobre DirectComposition), a janela do
// Tahoe com cantos de 26 pt e vidro da Composição, e a sombra própria que segue o canto redondo. As mesmas
// que o Ajustes do Sistema usa. Declaradas aqui (o finder.h traz o Finder inteiro).
namespace gfx { bool Init(HWND hwnd); void Resize(UINT w, UINT h); ID2D1DeviceContext* Begin(float dpi); void End(); }
namespace shadow { void Create(HWND main); void SetActive(bool active); void Update(); void Destroy(); void SetRadius(float dip); }
namespace tahoe {
float WindowR();  // 26 pt no Tahoe, 16 pt no Golden Gate
bool On(); bool Reload(); void Frame(HWND h, bool on); void Size(HWND h, float wpx, float hpx, bool dark); float Radius(HWND h);
bool PushWindowClip(ID2D1DeviceContext* dc, D2D1_SIZE_F s, float radius);
void WindowRim(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_SIZE_F s, float radius, bool dark);
D2D1_COLOR_F Tint(D2D1_COLOR_F base);  // 10 % da cor do papel de parede (Tahoe)
D2D1_COLOR_F Accent(bool dark);        // cor de destaque escolhida no Ajustes
}

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

FILE* g_log;
void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(g_log, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

namespace {

// ---------------------------------------------------------------------------
// Medidas (pt), tiradas da referência do Monitor de Atividade (940 × 500)
// referência escura (2x): bolinhas a 23 pt umas das outras, botões em "pílulas" de 35 pt, abas de 76,6 pt,
// cabeçalhos de 13 pt com divisórias de 15 pt, painel de 115 com a caixa de 579 × 90 em três partes
const float TOOLBAR_H = 52, HEADER_H = 28, ROW_H = 24, PANEL_H = 115, NAME_X = 51, ICON_X = 25;
const float PILL_TOP = 7.5f, PILL_H = 35;
const float SEG_X = 426, SEG_W = 76.6f, SEG_H = PILL_H, SEARCH_W = 323;  // Tahoe: busca larga (326 na referência), 8 pt da borda
const int HIST = 90;              // pontos no gráfico (3 min a cada 2 s)
const UINT_PTR T_SAMPLE = 1;
const UINT SAMPLE_MS = 2000;

enum Tab { T_CPU, T_MEM, T_ENERGY, T_DISK, T_NET, T_GPU, T_COUNT };
const wchar_t* TAB_NAMES[T_COUNT] = { Tr(L"CPU"), Tr(L"Memória"), Tr(L"Energia"), Tr(L"Disco"), Tr(L"Rede"), Tr(L"GPU") };

enum ColKey { C_NAME, C_CPU, C_CPUTIME, C_THREADS, C_HANDLES, C_MEM, C_ENERGY, C_WRITTEN, C_READ, C_CONN, C_PID, C_USER,
              C_CTX, C_KIND, C_GPU, C_GPUTIME, C_COMMIT, C_GPUMEM, C_RDOPS, C_WROPS, C_ENGINE, C_NAP };
struct Col { ColKey key; const wchar_t* title; float w; };
const std::vector<Col> COLS[T_COUNT] = {
    { { C_NAME, Tr(L"Nome do Processo"), 0 }, { C_CPU, Tr(L"% CPU"), 80 }, { C_CPUTIME, Tr(L"Tempo de CPU"), 114 }, { C_THREADS, Tr(L"Threads"), 76 },
      { C_CTX, Tr(L"Trocas de Contexto"), 146 }, { C_KIND, Tr(L"Tipo"), 70 }, { C_GPU, Tr(L"% GPU"), 66 }, { C_GPUTIME, Tr(L"Tempo de GPU"), 114 }, { C_PID, Tr(L"PID"), 66 }, { C_USER, Tr(L"Usuário"), 90 } },  // PID de 5 dígitos inteiro
    { { C_NAME, Tr(L"Nome do Processo"), 0 }, { C_MEM, Tr(L"Memória"), 100 }, { C_COMMIT, Tr(L"Mem. Comprometida"), 162 }, { C_GPUMEM, Tr(L"Memória da GPU"), 142 },
      { C_THREADS, Tr(L"Threads"), 77 }, { C_HANDLES, Tr(L"Identificadores"), 128 }, { C_PID, Tr(L"PID"), 70 }, { C_USER, Tr(L"Usuário"), 110 } },
    { { C_NAME, Tr(L"Nome do Processo"), 0 }, { C_ENERGY, Tr(L"Impacto Energético"), 158 }, { C_CPU, Tr(L"% CPU"), 80 }, { C_GPU, Tr(L"% GPU"), 80 }, { C_NAP, Tr(L"App Nap"), 80 }, { C_PID, Tr(L"PID"), 70 }, { C_USER, Tr(L"Usuário"), 110 } },
    { { C_NAME, Tr(L"Nome do Processo"), 0 }, { C_WRITTEN, Tr(L"Bytes Gravados"), 132 }, { C_READ, Tr(L"Bytes Lidos"), 110 }, { C_WROPS, Tr(L"Gravações"), 100 }, { C_RDOPS, Tr(L"Leituras"), 100 },
      { C_PID, Tr(L"PID"), 70 }, { C_USER, Tr(L"Usuário"), 110 } },
    { { C_NAME, Tr(L"Nome do Processo"), 0 }, { C_CONN, Tr(L"Conexões"), 95 }, { C_WRITTEN, Tr(L"Bytes Gravados"), 132 }, { C_READ, Tr(L"Bytes Lidos"), 110 }, { C_PID, Tr(L"PID"), 70 }, { C_USER, Tr(L"Usuário"), 110 } },
    { { C_NAME, Tr(L"Nome do Processo"), 0 }, { C_GPU, Tr(L"% GPU"), 80 }, { C_GPUTIME, Tr(L"Tempo de GPU"), 122 }, { C_GPUMEM, Tr(L"Memória da GPU"), 142 }, { C_ENGINE, Tr(L"Motor"), 150 },
      { C_PID, Tr(L"PID"), 70 }, { C_USER, Tr(L"Usuário"), 110 } },
};

// ---------------------------------------------------------------------------
// Coleta

// SYSTEM_PROCESS_INFORMATION completo (o do winternl.h esconde os campos)
struct SPI {  // 0x100 bytes no x64; as threads começam logo depois
    ULONG NextEntryOffset, NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount, NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId, InheritedFromUniqueProcessId;
    ULONG HandleCount, SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
};
using NtQSI = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

struct Proc {
    DWORD pid = 0, ppid = 0;
    ULONGLONG create = 0;
    std::wstring exe, path, name, user;
    std::wstring pack;                 // ícone do pacote do Sequoia (os nossos apps: Finder, Dock...)
    ULONGLONG cpu = 0, cpuPrev = 0;   // 100 ns
    double cpuPct = 0, energy = 0;
    ULONGLONG mem = 0;
    DWORD threads = 0, handles = 0, session = 0, conns = 0;
    ULONGLONG rd = 0, wr = 0, rdPrev = 0, wrPrev = 0;
    ULONGLONG rdOps = 0, wrOps = 0, commit = 0;
    ULONGLONG ctx = 0, ctxPrev = 0; double ctxSec = 0;   // trocas de contexto (soma das threads)
    std::wstring kind;                                    // 64 bits / 32 bits / ARM64
    double gpuPct = 0; ULONGLONG gpuTime = 0, gpuMem = 0; std::wstring engine;
    bool fresh = true, seen = false, sys = false;
};

std::unordered_map<DWORD, Proc> g_procs;
std::vector<BYTE> g_buf;
NtQSI g_ntqsi;
ULONGLONG g_lastSample;      // 100 ns
ULONGLONG g_sysIdle, g_sysKernel, g_sysUser;
std::wstring g_winDir, g_me, g_binDir, g_packDir;
DWORD g_myPid;

// totais do sistema
double g_cpuSys, g_cpuUser, g_cpuIdle;
DWORD g_threadsTotal, g_procCount;
MEMORYSTATUSEX g_ms{ sizeof(g_ms) };
PERFORMANCE_INFORMATION g_pi{ sizeof(g_pi) };
ULONGLONG g_appMem, g_compressed;
ULONGLONG g_rdOps, g_wrOps, g_rdBytes, g_wrBytes, g_rdOpsPrev, g_wrOpsPrev, g_rdBytesPrev, g_wrBytesPrev;
double g_rdOpsSec, g_wrOpsSec, g_rdBytesSec, g_wrBytesSec;
ULONGLONG g_netIn, g_netOut, g_pktIn, g_pktOut, g_netInPrev, g_netOutPrev, g_pktInPrev, g_pktOutPrev;
double g_netInSec, g_netOutSec, g_pktInSec, g_pktOutSec;
double g_energyTotal;
bool g_first = true;

// históricos para os gráficos (0..1)
PressaoMemoria g_press;
std::vector<float> g_hCpuSys(HIST), g_hCpuUser(HIST), g_hMem(HIST), g_hEnergy(HIST), g_hRd(HIST), g_hWr(HIST), g_hNetIn(HIST), g_hNetOut(HIST);
int g_samples;  // amostras feitas: o gráfico mostra só as que já existem, encostadas à direita
void Push(std::vector<float>& h, float v) { h.erase(h.begin()); h.push_back(v); }

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!n) return L"";
    std::vector<BYTE> v(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, v.data())) return L"";
    struct LC { WORD lang, cp; } *lc = nullptr; UINT cb = 0;
    if (!VerQueryValueW(v.data(), L"\\VarFileInfo\\Translation", (void**)&lc, &cb) || cb < sizeof(LC)) return L"";
    wchar_t key[64]; swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", lc->lang, lc->cp);
    wchar_t* d = nullptr; UINT dl = 0;
    if (!VerQueryValueW(v.data(), key, (void**)&d, &dl) || !d || !dl) return L"";
    std::wstring r = d;
    while (!r.empty() && r.back() == L' ') r.pop_back();
    return r;
}

// App Nap: o processo está no modo eficiência (o Shell põe os apps fora de vista: shell\memoria.cpp; o
// Windows e alguns programas, como o navegador, também se põem sozinhos)
bool Napped(DWORD pid) {
    bool r = false;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        PROCESS_POWER_THROTTLING_STATE s{ PROCESS_POWER_THROTTLING_CURRENT_VERSION };
        if (GetProcessInformation(h, ProcessPowerThrottling, &s, sizeof(s))) r = (s.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
        CloseHandle(h);
    }
    return r;
}

void Identify(Proc& p) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
    if (h) {
        wchar_t buf[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) p.path = buf;
        USHORT pm = 0, nm = 0;
        if (IsWow64Process2(h, &pm, &nm)) {
            USHORT m = pm == IMAGE_FILE_MACHINE_UNKNOWN ? nm : pm;
            p.kind = m == IMAGE_FILE_MACHINE_AMD64 ? L"64 bits" : m == IMAGE_FILE_MACHINE_ARM64 ? L"ARM64" : m == IMAGE_FILE_MACHINE_I386 ? L"32 bits" : L"—";
        }
        HANDLE tok = nullptr;
        if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
            DWORD sz = 0; GetTokenInformation(tok, TokenUser, nullptr, 0, &sz);
            std::vector<BYTE> tb(sz);
            if (sz && GetTokenInformation(tok, TokenUser, tb.data(), sz, &sz)) {
                wchar_t nm[128], dom[128]; DWORD nn = 128, dn = 128; SID_NAME_USE use;
                if (LookupAccountSidW(nullptr, ((TOKEN_USER*)tb.data())->User.Sid, nm, &nn, dom, &dn, &use)) p.user = nm;
            }
            CloseHandle(tok);
        }
        CloseHandle(h);
    }
    if (p.user.empty()) p.user = p.session == 0 ? L"SYSTEM" : g_me;
    if (p.kind.empty()) p.kind = L"64 bits";  // protegidos do sistema: sempre nativos
    // apps (fora da pasta do Windows): nome do app e ícone; serviços do sistema: o nome do executável, como no Mac
    std::wstring base = p.exe;
    if (base.size() > 4 && !_wcsicmp(base.c_str() + base.size() - 4, L".exe")) base.resize(base.size() - 4);
    p.sys = p.path.empty() || Lower(p.path).rfind(g_winDir, 0) == 0;
    p.name = base;
    if (!p.sys) { std::wstring d = FileDescription(p.path); if (!d.empty() && d.size() < 60) p.name = d; }
    if (p.pid == 4) p.name = L"System";
    // os apps deste projeto aparecem com o nome e o ícone do Mac
    if (!p.path.empty() && Lower(p.path).rfind(g_binDir, 0) == 0) {
        struct M { const wchar_t* exe; const wchar_t* name; const wchar_t* png; };
        static const M ours[] = { { L"arquivos.exe", Tr(L"Finder"), L"Finder.png" }, { L"shell.exe", Tr(L"Dock"), L"Finder.png" },
                                  { L"ajustes.exe", Tr(L"Ajustes do Sistema"), L"Settings.png" }, { L"monitoratividade.exe", Tr(L"Monitor de Atividade"), L"Activity Monitor.png" },
                                  { L"liquinixfinder.exe", Tr(L"Finder"), L"Finder.png" }, { L"liquinixshell.exe", Tr(L"Dock"), L"Launchpad.png" },
                                  { L"liquinixsettings.exe", Tr(L"Ajustes do Sistema"), L"Settings.png" }, { L"liquinixactivitymonitor.exe", Tr(L"Monitor de Atividade"), L"Activity Monitor.png" },
                                  { L"liquinixterminal.exe", Tr(L"Terminal"), L"Terminal.png" }, { L"liquinixcalculator.exe", Tr(L"Calculadora"), L"Calculator.png" },
                                  { L"liquinixnotes.exe", Tr(L"Notas"), L"Notes.png" }, { L"liquinixwindow32host.exe", Tr(L"Janelas do Mac (32 bits)"), L"Mission Control.png" } };
        for (auto& m : ours) if (!_wcsicmp(p.exe.c_str(), m.exe)) { p.name = m.name; p.pack = g_packDir + m.png; }
    }
    if (p.name.empty()) p.name = Tr(L"(pid ") + std::to_wstring(p.pid) + L")";
}

void CountConnections() {
    for (auto& [pid, p] : g_procs) p.conns = 0;
    auto add = [](DWORD pid) { auto it = g_procs.find(pid); if (it != g_procs.end()) it->second.conns++; };
    for (ULONG fam : { (ULONG)AF_INET, (ULONG)AF_INET6 }) {
        DWORD sz = 0;
        GetExtendedTcpTable(nullptr, &sz, FALSE, fam, TCP_TABLE_OWNER_PID_ALL, 0);
        std::vector<BYTE> b(sz + 4096); sz = (DWORD)b.size();
        if (GetExtendedTcpTable(b.data(), &sz, FALSE, fam, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            if (fam == AF_INET) { auto* t = (MIB_TCPTABLE_OWNER_PID*)b.data(); for (DWORD i = 0; i < t->dwNumEntries; i++) add(t->table[i].dwOwningPid); }
            else { auto* t = (MIB_TCP6TABLE_OWNER_PID*)b.data(); for (DWORD i = 0; i < t->dwNumEntries; i++) add(t->table[i].dwOwningPid); }
        }
        sz = 0;
        GetExtendedUdpTable(nullptr, &sz, FALSE, fam, UDP_TABLE_OWNER_PID, 0);
        std::vector<BYTE> u(sz + 4096); sz = (DWORD)u.size();
        if (GetExtendedUdpTable(u.data(), &sz, FALSE, fam, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            if (fam == AF_INET) { auto* t = (MIB_UDPTABLE_OWNER_PID*)u.data(); for (DWORD i = 0; i < t->dwNumEntries; i++) add(t->table[i].dwOwningPid); }
            else { auto* t = (MIB_UDP6TABLE_OWNER_PID*)u.data(); for (DWORD i = 0; i < t->dwNumEntries; i++) add(t->table[i].dwOwningPid); }
        }
    }
}

ULONGLONG Ft(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// GPU: os mesmos contadores do Gerenciador de Tarefas ("GPU Engine", "GPU Process Memory", "GPU Adapter Memory")
PDH_HQUERY g_q;
PDH_HCOUNTER g_cUtil, g_cTime, g_cMem, g_cAdapterMem;
std::wstring g_gpuName;
ULONGLONG g_gpuTotalMem, g_gpuUsedMem;
double g_gpuTotal;
std::map<std::wstring, double> g_engineTotals;  // 3D, Cópia, Vídeo... (% do sistema)
std::vector<float> g_hGpu(HIST);

std::wstring EngineName(const std::wstring& t) {
    if (t == L"3D") return L"3D";
    if (t.rfind(L"VideoDecode", 0) == 0) return Tr(L"Vídeo (decodificação)");
    if (t.rfind(L"VideoEncode", 0) == 0) return Tr(L"Vídeo (codificação)");
    if (t.rfind(L"VideoProcessing", 0) == 0) return Tr(L"Processamento de vídeo");
    if (t.rfind(L"Copy", 0) == 0) return Tr(L"Cópia");
    if (t.rfind(L"Compute", 0) == 0) return Tr(L"Computação");
    if (t.rfind(L"Security", 0) == 0) return Tr(L"Segurança");
    return t;
}
DWORD PidOf(const wchar_t* inst) { const wchar_t* p = wcsstr(inst, L"pid_"); return p ? wcstoul(p + 4, nullptr, 10) : 0; }
std::wstring EngTypeOf(const wchar_t* inst) { const wchar_t* p = wcsstr(inst, L"engtype_"); return p ? std::wstring(p + 8) : L""; }

void GpuInit() {
    if (PdhOpenQueryW(nullptr, 0, &g_q) != ERROR_SUCCESS) { g_q = nullptr; return; }
    PdhAddEnglishCounterW(g_q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_cUtil);
    PdhAddEnglishCounterW(g_q, L"\\GPU Engine(*)\\Running Time", 0, &g_cTime);
    PdhAddEnglishCounterW(g_q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &g_cMem);
    PdhAddEnglishCounterW(g_q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cAdapterMem);
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > g_gpuTotalMem) { g_gpuTotalMem = d.DedicatedVideoMemory; g_gpuName = d.Description; }
            a->Release();
        }
        f->Release();
    }
}

template <class T> std::vector<BYTE> PdhArray(PDH_HCOUNTER c, bool raw, DWORD& count) {
    DWORD size = 0; count = 0;
    std::vector<BYTE> b;
    PDH_STATUS st = raw ? PdhGetRawCounterArrayW(c, &size, &count, nullptr) : PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
    if (st != (PDH_STATUS)PDH_MORE_DATA || !size) { count = 0; return b; }
    b.resize(size);
    st = raw ? PdhGetRawCounterArrayW(c, &size, &count, (PDH_RAW_COUNTER_ITEM_W*)b.data()) : PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data());
    if (st != ERROR_SUCCESS) { count = 0; b.clear(); }
    return b;
}

void GpuSample() {
    for (auto& [pid, p] : g_procs) { p.gpuPct = 0; p.gpuTime = 0; p.gpuMem = 0; p.engine.clear(); }
    g_engineTotals.clear(); g_gpuTotal = 0; g_gpuUsedMem = 0;
    if (!g_q || PdhCollectQueryData(g_q) != ERROR_SUCCESS) return;
    std::map<DWORD, std::map<std::wstring, double>> per;  // pid -> motor -> %
    DWORD n = 0;
    auto u = PdhArray<int>(g_cUtil, false, n);
    for (DWORD i = 0; i < n; i++) {
        auto& it = ((PDH_FMT_COUNTERVALUE_ITEM_W*)u.data())[i];
        if (it.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        std::wstring eng = EngineName(EngTypeOf(it.szName));
        if (eng.empty()) continue;  // instância sem tipo de motor
        per[PidOf(it.szName)][eng] += it.FmtValue.doubleValue;
        g_engineTotals[eng] += it.FmtValue.doubleValue;
    }
    for (auto& [pid, engs] : per) {  // % de um processo = o motor que ele mais usa (como no Gerenciador)
        auto f = g_procs.find(pid);
        if (f == g_procs.end()) continue;
        for (auto& [e, v] : engs) if (v > f->second.gpuPct) { f->second.gpuPct = std::min(100.0, v); f->second.engine = e; }
    }
    for (auto& [e, v] : g_engineTotals) { v = std::min(100.0, v); g_gpuTotal = std::max(g_gpuTotal, v); }
    auto t = PdhArray<int>(g_cTime, true, n);
    for (DWORD i = 0; i < n; i++) {
        auto& it = ((PDH_RAW_COUNTER_ITEM_W*)t.data())[i];
        auto f = g_procs.find(PidOf(it.szName));
        if (f != g_procs.end()) f->second.gpuTime += (ULONGLONG)it.RawValue.FirstValue;
    }
    auto m = PdhArray<int>(g_cMem, true, n);
    for (DWORD i = 0; i < n; i++) {
        auto& it = ((PDH_RAW_COUNTER_ITEM_W*)m.data())[i];
        auto f = g_procs.find(PidOf(it.szName));
        if (f != g_procs.end()) f->second.gpuMem += (ULONGLONG)it.RawValue.FirstValue;
    }
    auto a = PdhArray<int>(g_cAdapterMem, true, n);
    for (DWORD i = 0; i < n; i++) g_gpuUsedMem = std::max(g_gpuUsedMem, (ULONGLONG)((PDH_RAW_COUNTER_ITEM_W*)a.data())[i].RawValue.FirstValue);
}

void Sample() {
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = Ft(nowFt);
    double elapsed = g_lastSample ? (double)(now - g_lastSample) : 0;  // 100 ns
    g_lastSample = now;
    if (!g_ntqsi) g_ntqsi = (NtQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (g_buf.empty()) g_buf.resize(1 << 20);
    ULONG need = 0;
    LONG st;
    while ((st = g_ntqsi(5 /*SystemProcessInformation*/, g_buf.data(), (ULONG)g_buf.size(), &need)) == (LONG)0xC0000004L)
        g_buf.resize(std::max<size_t>(need + 65536, g_buf.size() * 2));
    if (st < 0) return;
    for (auto& [pid, p] : g_procs) p.seen = false;
    g_threadsTotal = 0; g_procCount = 0; g_appMem = 0; g_compressed = 0;
    ULONGLONG rdOps = 0, wrOps = 0, rdB = 0, wrB = 0;
    BYTE* at = g_buf.data();
    for (;;) {
        auto* s = (SPI*)at;
        DWORD pid = (DWORD)(ULONG_PTR)s->UniqueProcessId;
        std::wstring exe = s->ImageName.Buffer ? std::wstring(s->ImageName.Buffer, s->ImageName.Length / 2) : L"";
        if (pid != 0) {
            auto it = g_procs.find(pid);
            if (it != g_procs.end() && it->second.create != (ULONGLONG)s->CreateTime.QuadPart) { g_procs.erase(it); it = g_procs.end(); }
            Proc& p = it != g_procs.end() ? it->second : g_procs[pid];
            if (p.fresh) {
                p.pid = pid; p.create = s->CreateTime.QuadPart; p.exe = exe; p.session = s->SessionId;
                p.ppid = (DWORD)(ULONG_PTR)s->InheritedFromUniqueProcessId;
                Identify(p);
            }
            p.seen = true;
            p.cpuPrev = p.fresh ? s->UserTime.QuadPart + s->KernelTime.QuadPart : p.cpu;
            p.cpu = s->UserTime.QuadPart + s->KernelTime.QuadPart;
            p.cpuPct = elapsed > 0 && !p.fresh ? (double)(p.cpu - p.cpuPrev) / elapsed * 100.0 : 0;  // por núcleo, como no Mac (passa de 100)
            p.mem = s->WorkingSetPrivateSize.QuadPart ? (ULONGLONG)s->WorkingSetPrivateSize.QuadPart : s->WorkingSetSize;
            p.threads = s->NumberOfThreads; p.handles = s->HandleCount;
            p.rdPrev = p.fresh ? s->ReadTransferCount.QuadPart : p.rd;
            p.wrPrev = p.fresh ? s->WriteTransferCount.QuadPart : p.wr;
            p.rd = s->ReadTransferCount.QuadPart; p.wr = s->WriteTransferCount.QuadPart;
            p.rdOps = s->ReadOperationCount.QuadPart; p.wrOps = s->WriteOperationCount.QuadPart;
            p.commit = s->PrivatePageCount;
            {   // as threads vêm logo depois (SYSTEM_THREAD_INFORMATION, 80 bytes; trocas de contexto em +64)
                ULONGLONG cs = 0;
                BYTE* th = at + sizeof(SPI);
                for (ULONG k = 0; k < s->NumberOfThreads; k++) cs += *(ULONG*)(th + k * 80 + 64);
                p.ctxPrev = p.ctx ? p.ctx : cs;
                p.ctx = cs;
                p.ctxSec = elapsed > 0 && cs >= p.ctxPrev ? (double)(cs - p.ctxPrev) / (elapsed / 1e7) : 0;
            }
            double ioMBs = elapsed > 0 ? (double)((p.rd - p.rdPrev) + (p.wr - p.wrPrev)) / (elapsed / 1e7) / 1e6 : 0;
            p.energy = p.cpuPct + ioMBs * 0.5;  // impacto aproximado: CPU + um pouco de disco
            p.fresh = false;
            g_threadsTotal += s->NumberOfThreads;
            g_procCount++;
            if (exe == L"MemCompression" || exe == L"Memory Compression") g_compressed = s->WorkingSetSize;
            else g_appMem += p.mem;
            rdOps += s->ReadOperationCount.QuadPart; wrOps += s->WriteOperationCount.QuadPart;
            rdB += s->ReadTransferCount.QuadPart; wrB += s->WriteTransferCount.QuadPart;
        }
        if (!s->NextEntryOffset) break;
        at += s->NextEntryOffset;
    }
    for (auto it = g_procs.begin(); it != g_procs.end();) it = it->second.seen ? std::next(it) : g_procs.erase(it);
    CountConnections();
    GpuSample();

    // CPU do sistema
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        ULONGLONG i = Ft(fi), k = Ft(fk), u = Ft(fu);
        if (g_sysKernel) {
            double di = (double)(i - g_sysIdle), dk = (double)(k - g_sysKernel), du = (double)(u - g_sysUser), tot = dk + du;
            if (tot > 0) { g_cpuIdle = di / tot * 100; g_cpuSys = (dk - di) / tot * 100; g_cpuUser = du / tot * 100; }
        }
        g_sysIdle = i; g_sysKernel = k; g_sysUser = u;
    }
    GlobalMemoryStatusEx(&g_ms);
    GetPerformanceInfo(&g_pi, sizeof(g_pi));

    // disco (soma dos processos)
    double secs = elapsed / 1e7;
    g_rdOpsPrev = g_first ? rdOps : g_rdOps; g_wrOpsPrev = g_first ? wrOps : g_wrOps; g_rdBytesPrev = g_first ? rdB : g_rdBytes; g_wrBytesPrev = g_first ? wrB : g_wrBytes;
    g_rdOps = rdOps; g_wrOps = wrOps; g_rdBytes = rdB; g_wrBytes = wrB;
    auto rate = [&](ULONGLONG a, ULONGLONG b) { return secs > 0 && a >= b ? (double)(a - b) / secs : 0.0; };
    g_rdOpsSec = rate(g_rdOps, g_rdOpsPrev); g_wrOpsSec = rate(g_wrOps, g_wrOpsPrev);
    g_rdBytesSec = rate(g_rdBytes, g_rdBytesPrev); g_wrBytesSec = rate(g_wrBytes, g_wrBytesPrev);

    // rede (placas físicas que estão ligadas)
    MIB_IF_TABLE2* t = nullptr;
    ULONGLONG ni = 0, no = 0, pi = 0, po = 0;
    if (GetIfTable2(&t) == NO_ERROR && t) {
        for (ULONG k = 0; k < t->NumEntries; k++) {
            auto& r = t->Table[k];
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.OperStatus != IfOperStatusUp || !r.InterfaceAndOperStatusFlags.HardwareInterface) continue;
            ni += r.InOctets; no += r.OutOctets; pi += r.InUcastPkts + r.InNUcastPkts; po += r.OutUcastPkts + r.OutNUcastPkts;
        }
        FreeMibTable(t);
    }
    g_netInPrev = g_first ? ni : g_netIn; g_netOutPrev = g_first ? no : g_netOut; g_pktInPrev = g_first ? pi : g_pktIn; g_pktOutPrev = g_first ? po : g_pktOut;
    g_netIn = ni; g_netOut = no; g_pktIn = pi; g_pktOut = po;
    g_netInSec = rate(g_netIn, g_netInPrev); g_netOutSec = rate(g_netOut, g_netOutPrev);
    g_pktInSec = rate(g_pktIn, g_pktInPrev); g_pktOutSec = rate(g_pktOut, g_pktOutPrev);

    g_energyTotal = 0;
    for (auto& [pid, p] : g_procs) g_energyTotal += p.energy;

    // gráficos
    Push(g_hCpuSys, (float)(g_cpuSys / 100)); Push(g_hCpuUser, (float)(g_cpuUser / 100));
    g_press = MedirPressao();
    Push(g_hMem, g_press.valor);
    Push(g_hEnergy, (float)std::min(1.0, g_energyTotal / 400.0));
    auto logScale = [](double v, double full) { return (float)std::min(1.0, std::log10(1 + v) / std::log10(1 + full)); };
    Push(g_hRd, logScale(g_rdOpsSec, 5000)); Push(g_hWr, logScale(g_wrOpsSec, 5000));
    Push(g_hNetIn, logScale(g_pktInSec, 20000)); Push(g_hNetOut, logScale(g_pktOutSec, 20000));
    Push(g_hGpu, (float)(g_gpuTotal / 100));
    for (auto& [pid, p] : g_procs) p.energy += p.gpuPct * 0.5;  // a GPU também gasta energia
    g_first = false;
    g_samples++;
}

// ---------------------------------------------------------------------------
// Textos (pt-BR, unidades decimais como no Mac)

std::wstring Thousands(ULONGLONG v) {
    std::wstring s = std::to_wstring(v), o;
    int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) { o.insert(o.begin(), s[i]); if (++n % 3 == 0 && i > 0) o.insert(o.begin(), L'.'); }
    return o;
}
std::wstring Comma(std::wstring s) { for (auto& c : s) if (c == L'.') c = L','; return s; }
std::wstring Bytes(double b) {
    wchar_t buf[48];
    if (b < 1000) swprintf_s(buf, Tr(L"%.0f bytes"), b);
    else if (b < 1e6) swprintf_s(buf, L"%.0f KB", b / 1e3);
    else if (b < 1e9) swprintf_s(buf, L"%.1f MB", b / 1e6);
    else if (b < 1e12) swprintf_s(buf, L"%.2f GB", b / 1e9);
    else swprintf_s(buf, L"%.2f TB", b / 1e12);
    return Comma(buf);
}
std::wstring Pct(double v) { wchar_t b[32]; swprintf_s(b, L"%.1f", v); return Comma(b); }
std::wstring CpuTime(ULONGLONG t100ns) {
    ULONGLONG cs = t100ns / 100000, s = cs / 100, h = s / 3600, m = (s / 60) % 60; s %= 60; cs %= 100;
    wchar_t b[48];
    if (h) swprintf_s(b, L"%llu:%02llu:%02llu,%02llu", h, m, s, cs);
    else swprintf_s(b, L"%llu:%02llu,%02llu", m, s, cs);
    return b;
}

// ---------------------------------------------------------------------------
// Janela

HWND g_hwnd;
HINSTANCE g_inst;
ID2D1Factory* g_f;
IDWriteFactory* g_dw;
ID2D1RenderTarget* g_rt;    // o contexto do quadro atual (gfx::Begin); os bitmaps valem entre quadros (mesmo dispositivo)
bool g_gfx = false;
IWICImagingFactory* g_wic;
std::wstring g_family = L"Segoe UI";
bool g_dark = true, g_active = true;
// Tema macOS Tahoe (Ajustes do Sistema > Aparência; liquid_glass=1 em %APPDATA%\Shell\ajustes.txt):
// barra de ferramentas sem faixa (o fundo da janela vai até em cima), botões, abas e busca em cápsulas de
// vidro com sombra suave, listras e seleção arredondadas e afastadas das bordas, e o painel de baixo sem a
// faixa cinza (só a caixa arredondada no meio), como o Monitor de Atividade do Tahoe.
bool g_tahoe = false;
bool ReadTahoe() {
    PWSTR ad = nullptr; std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad))) { path = std::wstring(ad) + L"\\Shell\\ajustes.txt"; CoTaskMemFree(ad); }
    FILE* f = nullptr;
    if (path.empty() || _wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") || !f) return false;
    bool on = false; wchar_t line[512];
    while (fgetws(line, 512, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r' || l.back() == L' ')) l.pop_back();
        if (l == L"liquid_glass=1") on = true;
        if (l == L"liquid_glass=0") on = false;
    }
    fclose(f);
    return on;
}
int g_tab = T_CPU;
// Tahoe (guia macOS26-Theme-Reference, seção 11): no segmentado, o item escolhido é uma "gota" que desliza entre
// as opções com mola (a mesma do switch medido: resposta 0,44 s, amortecimento 0,74)
float g_segFrom = -1;   // posição (em abas) de onde a gota saiu
DWORD g_segAt;
const UINT_PTR T_SEG = 9;
float SegPos() {
    if (g_segFrom < 0) return (float)g_tab;
    float s = (GetTickCount() - g_segAt) / 1000.f, w0 = 2 * 3.14159265f / 0.44f, z = 0.74f, wd = w0 * std::sqrt(1 - z * z);
    float e = 1 - std::exp(-z * w0 * s) * (std::cos(wd * s) + z * w0 / wd * std::sin(wd * s));
    if (s >= 0.6f) { g_segFrom = -1; return (float)g_tab; }
    return g_segFrom + (g_tab - g_segFrom) * e;
}
void SetTab(int i);
int g_sortCol[T_COUNT] = { 1, 1, 1, 1, 1, 1 };  // índice em COLS[tab]
bool g_sortAsc[T_COUNT] = { false, false, false, false, false, false };
DWORD g_sel = 0;                // PID escolhido
float g_scroll = 0;
std::wstring g_search;
bool g_searchFocus;
std::vector<const Proc*> g_rows;
std::unordered_map<std::wstring, ID2D1Bitmap*> g_icons;
int g_hoverBtn = -1, g_pressBtn = -1;  // 0 encerrar, 1 info, 2 ações, 10+ bolinhas
bool g_hoverLights;
enum Sheet { SH_NONE, SH_QUIT, SH_INFO };
Sheet g_sheet = SH_NONE;
DWORD g_sheetPid;
std::vector<std::pair<D2D1_RECT_F, int>> g_sheetBtns;
std::wstring g_sheetMsg;
bool g_shown;

float Scale() { return GetDpiForWindow(g_hwnd) / 96.f; }
D2D1_SIZE_F Client() { RECT r; GetClientRect(g_hwnd, &r); float s = Scale(); return { r.right / s, r.bottom / s }; }

struct Pal { D2D1_COLOR_F bg, bar, text, text2, line, stripe, panel, box, accent, seg, segSel, field; };
Pal ColorsBase();
Pal Colors() {  // Tahoe: o fundo da janela leva 10 % da cor do papel de parede (tahoe::Tint)
    Pal p = ColorsBase();
    if (g_tahoe) { p.bg = tahoe::Tint(p.bg); p.bar = tahoe::Tint(p.bar); p.panel = tahoe::Tint(p.panel); }
    p.accent = tahoe::Accent(false);  // Ajustes > Aparência > Cor de destaque
    return p;
}
Pal ColorsBase() {
    // Mac OS X (Leopard): fundo branco, listras azul-acinzentadas bem claras, seleção azul do Aqua, painel de metal claro
    if (LEO) return { D2D1::ColorF(1, 1, 1), D2D1::ColorF(0.80f, 0.80f, 0.80f), D2D1::ColorF(0, 0, 0, 0.90f), D2D1::ColorF(0, 0, 0, 0.55f),
                      D2D1::ColorF(0, 0, 0, 0.15f), D2D1::ColorF(243 / 255.f, 246 / 255.f, 250 / 255.f), D2D1::ColorF(0.89f, 0.89f, 0.89f), D2D1::ColorF(1, 1, 1),
                      D2D1::ColorF(56 / 255.f, 117 / 255.f, 215 / 255.f), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0.55f, 0.55f, 0.56f), D2D1::ColorF(1, 1, 1) };
    if (g_tahoe) {  // a barra e o painel são o próprio fundo; cápsulas de vidro claras por cima
        if (g_dark) return { D2D1::ColorF(0.110f, 0.110f, 0.118f), D2D1::ColorF(0.110f, 0.110f, 0.118f), D2D1::ColorF(1, 1, 1, 0.90f), D2D1::ColorF(1, 1, 1, 0.50f),
                             D2D1::ColorF(1, 1, 1, 0.10f), D2D1::ColorF(1, 1, 1, 0.045f), D2D1::ColorF(0.110f, 0.110f, 0.118f), D2D1::ColorF(1, 1, 1, 0.035f),
                             D2D1::ColorF(0.04f, 0.42f, 0.92f), D2D1::ColorF(0.20f, 0.20f, 0.21f), D2D1::ColorF(1, 1, 1, 0.14f), D2D1::ColorF(0.20f, 0.20f, 0.21f) };
        return { D2D1::ColorF(1, 1, 1), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0, 0.85f), D2D1::ColorF(0, 0, 0, 0.50f),
                 D2D1::ColorF(0, 0, 0, 0.08f), D2D1::ColorF(0, 0, 0, 0.035f), D2D1::ColorF(1, 1, 1), D2D1::ColorF(1, 1, 1),
                 D2D1::ColorF(0.0f, 0.39f, 0.88f), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0, 0.075f), D2D1::ColorF(1, 1, 1) };
    }
    if (g_dark) return { D2D1::ColorF(0.118f, 0.118f, 0.122f), D2D1::ColorF(0.118f, 0.118f, 0.122f), D2D1::ColorF(1, 1, 1, 0.90f), D2D1::ColorF(1, 1, 1, 0.50f),
                         D2D1::ColorF(1, 1, 1, 0.12f), D2D1::ColorF(1, 1, 1, 0.055f), D2D1::ColorF(0.118f, 0.118f, 0.122f), D2D1::ColorF(0.118f, 0.118f, 0.122f),
                         D2D1::ColorF(0.04f, 0.42f, 0.92f), D2D1::ColorF(1, 1, 1, 0.055f), D2D1::ColorF(1, 1, 1, 0.16f), D2D1::ColorF(1, 1, 1, 0.055f) };
    return { D2D1::ColorF(1, 1, 1), D2D1::ColorF(0.969f, 0.969f, 0.969f), D2D1::ColorF(0, 0, 0, 0.85f), D2D1::ColorF(0, 0, 0, 0.50f),
             D2D1::ColorF(0, 0, 0, 0.10f), D2D1::ColorF(0.957f, 0.961f, 0.961f), D2D1::ColorF(0.925f, 0.925f, 0.925f), D2D1::ColorF(1, 1, 1),
             D2D1::ColorF(0.0f, 0.39f, 0.88f), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0.86f, 0.86f, 0.86f), D2D1::ColorF(1, 1, 1) };
}

IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(g_dw->CreateTextFormat(g_family.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf))) return nullptr;
    tf->SetTextAlignment(a);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    IDWriteInlineObject* el = nullptr; g_dw->CreateEllipsisTrimmingSign(tf, &el); tf->SetTrimming(&tr, el); if (el) el->Release();
    return tf;
}
IDWriteTextFormat *g_f13, *g_f13r, *g_f11, *g_f11r, *g_f11b, *g_f11c, *g_f13b, *g_f13c, *g_f10c;
IDWriteTextFormat *g_f14b, *g_f12, *g_f14c, *g_fHead, *g_fHeadR, *g_fStat, *g_fStatR;

void Text(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (!tf || s.empty()) return;
    br->SetColor(c);
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
float TextW(IDWriteTextFormat* tf, const std::wstring& s) {
    IDWriteTextLayout* l = nullptr; float w = 0;
    if (tf && SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 4000, 100, &l))) { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; l->Release(); }
    return w;
}
bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

// ---- ícones dos apps (só fora da pasta do Windows, como no Mac: serviços ficam sem ícone)
ID2D1Bitmap* PngBitmap(const std::wstring& file) {
    auto it = g_icons.find(file);
    if (it != g_icons.end()) return it->second;
    ID2D1Bitmap* bmp = nullptr;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* scl = nullptr; IWICFormatConverter* cv = nullptr;
    if (g_wic && SUCCEEDED(g_wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(g_wic->CreateBitmapScaler(&scl)) && SUCCEEDED(scl->Initialize(fr, 64, 64, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(scl, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        g_rt->CreateBitmapFromWicBitmap(cv, nullptr, &bmp);
    if (cv) cv->Release(); if (scl) scl->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    g_icons[file] = bmp;
    return bmp;
}

ID2D1Bitmap* IconFor(const Proc& p) {
    if (!p.pack.empty()) if (ID2D1Bitmap* b = PngBitmap(p.pack)) return b;
    if (p.sys || p.path.empty()) return nullptr;
    auto it = g_icons.find(p.path);
    if (it != g_icons.end()) return it->second;
    ID2D1Bitmap* bmp = nullptr;
    SHFILEINFOW sfi{};
    bool big = Scale() > 1.25f;
    if (g_wic && SHGetFileInfoW(p.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | (big ? SHGFI_LARGEICON : SHGFI_SMALLICON)) && sfi.hIcon) {
        IWICBitmap* wb = nullptr; IWICFormatConverter* cv = nullptr;
        if (SUCCEEDED(g_wic->CreateBitmapFromHICON(sfi.hIcon, &wb)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            g_rt->CreateBitmapFromWicBitmap(cv, nullptr, &bmp);
        if (cv) cv->Release(); if (wb) wb->Release();
        DestroyIcon(sfi.hIcon);
    }
    g_icons[p.path] = bmp;
    return bmp;
}
void DropIcons() { for (auto& [k, b] : g_icons) if (b) b->Release(); g_icons.clear(); }

// ---- linhas visíveis (busca + ordem)
double NumKey(const Proc& p, ColKey k) {
    switch (k) {
    case C_CPU: return p.cpuPct; case C_CPUTIME: return (double)p.cpu; case C_THREADS: return p.threads; case C_HANDLES: return p.handles;
    case C_MEM: return (double)p.mem; case C_ENERGY: return p.energy; case C_WRITTEN: return (double)p.wr; case C_READ: return (double)p.rd;
    case C_CONN: return p.conns; case C_PID: return p.pid; case C_NAP: return Napped(p.pid) ? 1 : 0;
    case C_CTX: return p.ctxSec; case C_GPU: return p.gpuPct; case C_GPUTIME: return (double)p.gpuTime; case C_COMMIT: return (double)p.commit;
    case C_GPUMEM: return (double)p.gpuMem; case C_RDOPS: return (double)p.rdOps; case C_WROPS: return (double)p.wrOps;
    default: return 0;
    }
}
void BuildRows() {
    g_rows.clear();
    std::wstring q = Lower(g_search);
    for (auto& [pid, p] : g_procs) {
        if (!q.empty() && Lower(p.name).find(q) == std::wstring::npos && Lower(p.exe).find(q) == std::wstring::npos && std::to_wstring(pid).find(q) == std::wstring::npos) continue;
        g_rows.push_back(&p);
    }
    const Col& c = COLS[g_tab][g_sortCol[g_tab]];
    bool asc = g_sortAsc[g_tab];
    std::stable_sort(g_rows.begin(), g_rows.end(), [&](const Proc* a, const Proc* b) {
        if (c.key == C_NAME || c.key == C_USER || c.key == C_KIND || c.key == C_ENGINE) {
            auto txt = [&](const Proc* x) -> const std::wstring& { return c.key == C_NAME ? x->name : c.key == C_USER ? x->user : c.key == C_KIND ? x->kind : x->engine; };
            int r = _wcsicmp(txt(a).c_str(), txt(b).c_str());
            if (r) return asc ? r < 0 : r > 0;
            return a->pid < b->pid;
        }
        double x = NumKey(*a, c.key), y = NumKey(*b, c.key);
        if (x != y) return asc ? x < y : x > y;
        return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
    });
}
std::wstring Cell(const Proc& p, ColKey k) {
    switch (k) {
    case C_NAME: return p.name;
    case C_CPU: return Pct(p.cpuPct);
    case C_CPUTIME: return CpuTime(p.cpu);
    case C_THREADS: return Thousands(p.threads);
    case C_HANDLES: return Thousands(p.handles);
    case C_MEM: return Bytes((double)p.mem);
    case C_ENERGY: return Pct(p.energy);
    case C_WRITTEN: return Bytes((double)p.wr);
    case C_READ: return Bytes((double)p.rd);
    case C_CONN: return Thousands(p.conns);
    case C_PID: return std::to_wstring(p.pid);
    case C_NAP: return Napped(p.pid) ? Tr(L"Sim") : Tr(L"Não");
    case C_USER: return p.user;
    case C_CTX: return Thousands((ULONGLONG)std::llround(p.ctxSec));
    case C_KIND: return p.kind;
    case C_GPU: return Pct(p.gpuPct);
    case C_GPUTIME: return CpuTime(p.gpuTime);
    case C_COMMIT: return Bytes((double)p.commit);
    case C_GPUMEM: return Bytes((double)p.gpuMem);
    case C_RDOPS: return Thousands(p.rdOps);
    case C_WROPS: return Thousands(p.wrOps);
    case C_ENGINE: return p.gpuPct >= 0.05 ? p.engine : L"";
    }
    return L"";
}
const Proc* Selected() { auto it = g_procs.find(g_sel); return it == g_procs.end() ? nullptr : &it->second; }

// ---- geometria
float ListTop() { return TOOLBAR_H + HEADER_H; }
float ListBottom() { return Client().height - PANEL_H; }
float MaxScroll() { return std::max(0.f, g_rows.size() * ROW_H + 8 - (ListBottom() - ListTop())); }
float NameW() {  // o nome fica com o que sobra (de 180 a 440; no Tahoe ~420 numa janela de 1228)
    float others = 0;
    for (auto& c : COLS[g_tab]) others += c.w;
    return std::clamp(Client().width - 26 - others, 180.f, 440.f);
}
std::vector<std::pair<float, float>> ColSpans() {  // (esquerda, direita) de cada coluna
    std::vector<std::pair<float, float>> v;
    float x = 10;
    for (auto& c : COLS[g_tab]) {
        float w = c.key == C_NAME ? NameW() : c.w;
        v.push_back({ x, x + w });
        x += w;
    }
    return v;
}
// pílula 1: Encerrar (273) e Informações (307,5); pílula 2: Ações (354,5) com a setinha (371)
const D2D1_RECT_F PILL_A{ 282, PILL_TOP, 350, PILL_TOP + PILL_H }, PILL_B{ 362, PILL_TOP, 406, PILL_TOP + PILL_H };  // medidas do Tahoe
D2D1_RECT_F BtnRect(int k) {  // área de clique de cada botão
    // Mac OS X: na linha de ferramentas (embaixo do título de 22): Encerrar, Informações e a engrenagem ▾ em molduras de metal
    if (LEO) return k == 0 ? D2D1_RECT_F{ 12, 26, 44, 48 } : k == 1 ? D2D1_RECT_F{ 50, 26, 82, 48 } : D2D1_RECT_F{ 88, 26, 128, 48 };
    if (k == 0) return { PILL_A.left, PILL_A.top, 317, PILL_A.bottom };
    if (k == 1) return { 317, PILL_A.top, PILL_A.right, PILL_A.bottom };
    return PILL_B;
}
D2D1_RECT_F SegRect(int i) {
    if (LEO) { float w = 62, x0 = std::round(Client().width / 2 - w * T_COUNT / 2); return { x0 + i * w, 27, x0 + (i + 1) * w, 47 }; }  // Mac OS X: abas no meio
    return { SEG_X + i * SEG_W, PILL_TOP, SEG_X + (i + 1) * SEG_W, PILL_TOP + SEG_H };
}
D2D1_RECT_F SearchRect() {
    auto s = Client();
    if (LEO) return { s.width - 12 - 130, 27, s.width - 12, 47 };
    float r = s.width - 8; return { std::max(SEG_X + SEG_W * T_COUNT + 10, r - SEARCH_W), PILL_TOP, r, PILL_TOP + PILL_H };
}
D2D1_POINT_2F LightCenter(int k) { return LEO ? D2D1_POINT_2F{ 14.5f + k * 21, 11 } : D2D1_POINT_2F{ 26.f + k * 23, 25 }; }  // como na referência

// ---- desenho
void Line(ID2D1RenderTarget* rt, ID2D1Brush* b, float x1, float y1, float x2, float y2, float w = 1.4f) { rt->DrawLine({ x1, y1 }, { x2, y2 }, b, w); }

void DrawLights(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br) {
    static const D2D1_COLOR_F fill_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.345f), D2D1::ColorF(0.996f, 0.737f, 0.180f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
    static const D2D1_COLOR_F ring_c[3] = { D2D1::ColorF(0.886f, 0.275f, 0.247f), D2D1::ColorF(0.882f, 0.631f, 0.086f), D2D1::ColorF(0.071f, 0.675f, 0.157f) }; const D2D1_COLOR_F* ring = semaforo::Pick(ring_c, semaforo::RING);
    static const D2D1_COLOR_F glyph_c[3] = { D2D1::ColorF(0.302f, 0, 0), D2D1::ColorF(0.6f, 0.341f, 0), D2D1::ColorF(0, 0.396f, 0) }; const D2D1_COLOR_F* glyph = semaforo::Pick(glyph_c, semaforo::GLYPH);
    bool lit = g_active || g_hoverLights;
    for (int k = 0; k < 3; k++) {
        auto c = LightCenter(k);
        br->SetColor(lit ? fill[k] : g_dark ? D2D1::ColorF(0.30f, 0.30f, 0.31f) : D2D1::ColorF(0.84f, 0.84f, 0.84f));
        rt->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
        semaforo::Gloss(rt, c, 7, br);
        br->SetColor(lit ? ring[k] : g_dark ? D2D1::ColorF(0, 0, 0, 0.3f) : D2D1::ColorF(0.73f, 0.73f, 0.74f));
        rt->DrawEllipse(D2D1::Ellipse(c, 6.75f, 6.75f), br, 0.5f);
        if (!g_hoverLights) continue;
        br->SetColor(glyph[k]);
        if (k == 0) { Line(rt, br, c.x - 2.5f, c.y - 2.5f, c.x + 2.5f, c.y + 2.5f, 1.1f); Line(rt, br, c.x - 2.5f, c.y + 2.5f, c.x + 2.5f, c.y - 2.5f, 1.1f); }
        else if (k == 1) Line(rt, br, c.x - 3.3f, c.y, c.x + 3.3f, c.y, 1.2f);
        else { Line(rt, br, c.x - 3, c.y, c.x + 3, c.y, 1.2f); Line(rt, br, c.x, c.y - 3, c.x, c.y + 3, 1.2f); }
    }
}

void Grad(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, std::initializer_list<D2D1_GRADIENT_STOP> st) {
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(st.begin(), (UINT32)st.size(), &gs)) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    if (lb) lb->Release();
    if (gs) gs->Release();
}
// Mac OS X: a barra unificada de metal (título de 22 no meio e a linha de ferramentas embaixo)
void DrawToolbarLeopard(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    auto s = Client();
    leopard::Metal(rt, D2D1::RectF(0, 0, s.width, TOOLBAR_H), g_active, false, true, 0);
    DrawLights(rt, br);
    static IDWriteTextFormat* tt = [] {
        IDWriteTextFormat* f = nullptr;
        g_dw->CreateTextFormat(L"Lucida Sans Unicode", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"pt-br", &f);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        return f;
    }();
    std::wstring title = Tr(L"Monitor de Atividade") + std::wstring(L" (") + Tr(L"Todos os Processos") + L")";
    Text(rt, br, title, tt, D2D1::RectF(80, 0, s.width - 80, 22), D2D1::ColorF(0, 0, 0, g_active ? 0.85f : 0.45f));
    auto bezel = [&](D2D1_RECT_F r, bool down) {  // moldura de metal clara dos botões do 10.5
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.45f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1), 4, 4), br);
        if (down) Grad(rt, r, 4, { { 0, D2D1::ColorF(0.50f, 0.50f, 0.51f) }, { 1, D2D1::ColorF(0.66f, 0.66f, 0.67f) } });
        else Grad(rt, r, 4, { { 0, D2D1::ColorF(0.99f, 0.99f, 0.99f) }, { 1, D2D1::ColorF(0.84f, 0.84f, 0.84f) } });
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 3.5f, 3.5f), br, 1);
    };
    bool sel = Selected() != nullptr;
    for (int k = 0; k < 3; k++) {
        auto r = BtnRect(k);
        bezel(r, false);
        bool en = k == 2 || sel;
        br->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.1f, en ? 0.85f : 0.30f));
        float cx = k == 2 ? r.left + 15 : (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        if (k == 0) {  // o círculo com o X (Encerrar Processo)
            rt->FillEllipse(D2D1::Ellipse({ cx, cy }, 7, 7), br);
            br->SetColor(D2D1::ColorF(0.95f, 0.95f, 0.95f));
            Line(rt, br, cx - 3, cy - 3, cx + 3, cy + 3, 1.6f); Line(rt, br, cx - 3, cy + 3, cx + 3, cy - 3, 1.6f);
        } else if (k == 1) {  // o "i" num círculo
            rt->FillEllipse(D2D1::Ellipse({ cx, cy }, 7, 7), br);
            br->SetColor(D2D1::ColorF(0.95f, 0.95f, 0.95f));
            rt->FillEllipse(D2D1::Ellipse({ cx, cy - 3.3f }, 1.1f, 1.1f), br);
            Line(rt, br, cx, cy - 0.8f, cx, cy + 3.8f, 1.7f);
        } else {  // a engrenagem ▾
            for (int i = 0; i < 8; i++) { float a = i * 3.14159265f / 4; Line(rt, br, cx + std::cos(a) * 4.8f, cy + std::sin(a) * 4.8f, cx + std::cos(a) * 7.2f, cy + std::sin(a) * 7.2f, 2.2f); }
            rt->FillEllipse(D2D1::Ellipse({ cx, cy }, 5, 5), br);
            br->SetColor(D2D1::ColorF(0.9f, 0.9f, 0.9f)); rt->FillEllipse(D2D1::Ellipse({ cx, cy }, 1.8f, 1.8f), br);
            br->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.85f));
            float ax = r.right - 10;
            Line(rt, br, ax - 3, cy - 1.5f, ax, cy + 1.5f, 1.6f); Line(rt, br, ax, cy + 1.5f, ax + 3, cy - 1.5f, 1.6f);
        }
    }
    // as abas: controle segmentado de metal, a escolhida afundada
    D2D1_RECT_F all{ SegRect(0).left, SegRect(0).top, SegRect(T_COUNT - 1).right, SegRect(0).bottom };
    bezel(all, false);
    {
        auto r = SegRect(g_tab);
        Grad(rt, D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), g_tab == 0 || g_tab == T_COUNT - 1 ? 3.f : 0.f,
             { { 0, D2D1::ColorF(0.52f, 0.52f, 0.53f) }, { 1, D2D1::ColorF(0.68f, 0.68f, 0.69f) } });
    }
    static IDWriteTextFormat* ts = [] {
        IDWriteTextFormat* f = nullptr;
        g_dw->CreateTextFormat(L"Lucida Sans Unicode", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"pt-br", &f);
        if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        return f;
    }();
    for (int i = 0; i < T_COUNT; i++) {
        auto r = SegRect(i);
        if (i + 1 < T_COUNT) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f)); Line(rt, br, std::floor(r.right) + 0.5f, r.top + 1, std::floor(r.right) + 0.5f, r.bottom - 1, 1); }
        Text(rt, br, TAB_NAMES[i], ts, r, i == g_tab ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0, 0, 0, 0.88f));
    }
    // a busca: o campo branco redondo afundado com a lupa ▾
    auto sr = SearchRect();
    float rad = (sr.bottom - sr.top) / 2;
    br->SetColor(D2D1::ColorF(1, 1, 1));
    rt->FillRoundedRectangle(D2D1::RoundedRect(sr, rad, rad), br);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.12f));
    rt->FillRectangle(D2D1::RectF(sr.left + rad, sr.top + 1, sr.right - rad, sr.top + 3), br);
    br->SetColor(g_searchFocus ? D2D1::ColorF(0.36f, 0.60f, 0.95f, 0.9f) : D2D1::ColorF(0, 0, 0, 0.45f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 0.5f, sr.top + 0.5f, sr.right - 0.5f, sr.bottom - 0.5f), rad - 0.5f, rad - 0.5f), br, g_searchFocus ? 2.f : 1.f);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
    float mx = sr.left + 12, my = (sr.top + sr.bottom) / 2 - 1;
    rt->DrawEllipse(D2D1::Ellipse({ mx, my }, 4, 4), br, 1.4f);
    Line(rt, br, mx + 2.8f, my + 2.8f, mx + 5.5f, my + 5.5f, 1.6f);
    Text(rt, br, g_search + (g_searchFocus && GetTickCount() / 530 % 2 == 0 ? L"|" : L""), g_f13, D2D1::RectF(sr.left + 24, sr.top, sr.right - 10, sr.bottom), pal.text);
}

void DrawToolbar(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    if (LEO) { DrawToolbarLeopard(rt, br, pal); return; }
    auto s = Client();
    br->SetColor(pal.bar);
    rt->FillRectangle(D2D1::RectF(0, 0, s.width, TOOLBAR_H), br);
    if (!g_dark && !g_tahoe) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f)); Line(rt, br, 0, TOOLBAR_H - 0.5f, s.width, TOOLBAR_H - 0.5f, 1); }
    DrawLights(rt, br);
    Text(rt, br, Tr(L"Monitor de Atividade"), g_f14b, D2D1::RectF(108, 8, PILL_A.left - 6, 28), pal.text);
    Text(rt, br, Tr(L"Todos os Processos"), g_f12, D2D1::RectF(108, 26, PILL_A.left - 6, 42), pal.text2);
    // pílulas: fundo um tom acima da barra e borda fina, raio da metade da altura
    auto pill = [&](D2D1_RECT_F r) {
        if (g_tahoe) {
            float rad = (r.bottom - r.top) / 2;
            // sombra: algumas cápsulas crescendo, bem transparentes (desfoque barato)
            for (int k = 4; k >= 1; k--) {
                br->SetColor(D2D1::ColorF(0, 0, 0, (g_dark ? 0.10f : 0.035f) / k));
                float g = k * 1.4f;
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - g, r.top - g + 1.5f, r.right + g, r.bottom + g + 1.5f), rad + g, rad + g), br);
            }
            br->SetColor(pal.seg);
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
            // borda de luz do vidro: clara em cima, some embaixo
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0, 0, 0, 0.06f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f), br, 1);
            return;
        }
        br->SetColor(pal.seg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, PILL_H / 2, PILL_H / 2), br);
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.09f) : D2D1::ColorF(0, 0, 0, 0.10f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), PILL_H / 2 - 0.5f, PILL_H / 2 - 0.5f), br, 1);
    };
    pill(PILL_A); pill(PILL_B);
    bool sel = Selected() != nullptr;
    const float bx[3] = { 300, 334, 377 };
    for (int k = 0; k < 3; k++) {
        auto r = BtnRect(k);
        bool en = k == 2 || sel;
        if (en && g_hoverBtn == k) {
            br->SetColor(pal.line);
            float cx = bx[k], hw = k == 2 ? 21.f : 15.f;
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - hw + (k == 2 ? 8 : 0), r.top + 3, cx + hw + (k == 2 ? 8 : 0), r.bottom - 3), 14.5f, 14.5f), br);
        }
        br->SetColor(en ? (g_dark ? D2D1::ColorF(1, 1, 1, 0.85f) : D2D1::ColorF(0, 0, 0, 0.70f)) : (g_dark ? D2D1::ColorF(1, 1, 1, 0.32f) : D2D1::ColorF(0, 0, 0, 0.25f)));
        float cx = bx[k], cy = (r.top + r.bottom) / 2;
        rt->DrawEllipse(D2D1::Ellipse({ cx, cy }, 8.5f, 8.5f), br, 1.4f);
        if (k == 0) { Line(rt, br, cx - 3.2f, cy - 3.2f, cx + 3.2f, cy + 3.2f, 1.4f); Line(rt, br, cx - 3.2f, cy + 3.2f, cx + 3.2f, cy - 3.2f, 1.4f); }
        else if (k == 1) { rt->FillEllipse(D2D1::Ellipse({ cx, cy - 3.8f }, 1.15f, 1.15f), br); Line(rt, br, cx, cy - 0.8f, cx, cy + 4.2f, 1.6f); }
        else {
            for (int i = -1; i <= 1; i++) rt->FillEllipse(D2D1::Ellipse({ cx + i * 4.2f, cy }, 1.3f, 1.3f), br);
            float ax = 394;
            Line(rt, br, ax - 3.2f, cy - 1.6f, ax, cy + 1.6f, 1.4f); Line(rt, br, ax, cy + 1.6f, ax + 3.2f, cy - 1.6f, 1.4f);
        }
    }
    // abas: uma pílula larga; a escolhida é uma pílula clara por dentro; divisórias entre as não escolhidas
    D2D1_RECT_F all{ SegRect(0).left, SegRect(0).top, SegRect(T_COUNT - 1).right, SegRect(0).bottom };
    pill(all);
    for (int i = 0; i < T_COUNT; i++) {
        auto r = SegRect(i);
        if (i == g_tab && !g_tahoe) {
            br->SetColor(pal.segSel);
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 4, r.top + 4, r.right - 4, r.bottom - 4), (PILL_H - 8) / 2, (PILL_H - 8) / 2), br);
        } else if (i == 0 && g_tahoe) {  // a gota: desenhada uma vez, na posição animada
            float pos = SegPos(), w = r.right - r.left;
            float l = r.left + pos * w;
            br->SetColor(pal.segSel);
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l + 4, r.top + 4, l + w - 4, r.bottom - 4), (PILL_H - 8) / 2, (PILL_H - 8) / 2), br);
            vidro::Borda(rt, D2D1::RectF(l + 4, r.top + 4, l + w - 4, r.bottom - 4), (PILL_H - 8) / 2, 1, g_dark, 0.6f, true);
            if (g_segFrom >= 0) SetTimer(g_hwnd, T_SEG, 8, nullptr);
        } else if (g_tahoe) {
        } else if (i + 1 < T_COUNT && i + 1 != g_tab) {
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.12f));
            Line(rt, br, r.right, r.top + 10, r.right, r.bottom - 10, 1);
        }
        Text(rt, br, TAB_NAMES[i], g_f14c, r, pal.text);
    }
    // busca (pílula também)
    auto sr = SearchRect();
    pill(sr);
    if (g_searchFocus && g_tahoe) {
        br->SetColor(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.75f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left - 1.5f, sr.top - 1.5f, sr.right + 1.5f, sr.bottom + 1.5f), PILL_H / 2 + 1.5f, PILL_H / 2 + 1.5f), br, 3);
    } else if (g_searchFocus) {
        br->SetColor(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.6f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 1, sr.top + 1, sr.right - 1, sr.bottom - 1), PILL_H / 2 - 1, PILL_H / 2 - 1), br, 2);
    }
    br->SetColor(pal.text2);
    float mx = sr.left + 18, my = (sr.top + sr.bottom) / 2 - 1;
    rt->DrawEllipse(D2D1::Ellipse({ mx, my }, 5, 5), br, 1.4f);
    Line(rt, br, mx + 3.6f, my + 3.6f, mx + 7, my + 7, 1.6f);
    std::wstring shown = g_search.empty() ? Tr(L"Buscar") : g_search;
    Text(rt, br, shown + (g_searchFocus && GetTickCount() / 530 % 2 == 0 ? L"|" : L""), g_f13, D2D1::RectF(sr.left + 32, sr.top, sr.right - 14, sr.bottom), g_search.empty() ? pal.text2 : pal.text);
}

void DrawHeader(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    auto s = Client();
    br->SetColor(pal.bg);
    rt->FillRectangle(D2D1::RectF(0, TOOLBAR_H, s.width, TOOLBAR_H + HEADER_H), br);
    auto spans = ColSpans();
    if (LEO) {  // Mac OS X: cabeçalho branco com degradê, a coluna da ordem em azul claro do Aqua
        Grad(rt, D2D1::RectF(0, TOOLBAR_H, s.width, TOOLBAR_H + HEADER_H), 0, { { 0, D2D1::ColorF(1, 1, 1) }, { 1, D2D1::ColorF(0.90f, 0.90f, 0.90f) } });
        int sc = g_sortCol[g_tab];
        if (sc >= 0 && sc < (int)spans.size())
            Grad(rt, D2D1::RectF(spans[sc].first, TOOLBAR_H, spans[sc].second, TOOLBAR_H + HEADER_H), 0,
                 { { 0, D2D1::ColorF(0.80f, 0.89f, 0.99f) }, { 0.5f, D2D1::ColorF(0.64f, 0.79f, 0.97f) }, { 1, D2D1::ColorF(0.74f, 0.86f, 0.99f) } });
    }
    for (size_t i = 0; i < COLS[g_tab].size(); i++) {
        const Col& c = COLS[g_tab][i];
        bool sorted = (int)i == g_sortCol[g_tab];
        bool right = c.key != C_NAME && c.key != C_USER && c.key != C_ENGINE && c.key != C_KIND;
        auto [l, r] = spans[i];
        D2D1_RECT_F tr{ l + (c.key == C_NAME ? 19 : 8), TOOLBAR_H, r - (right ? 12 : 4), TOOLBAR_H + HEADER_H };
        if (sorted && right) tr.right -= 12;
        Text(rt, br, c.title, right ? g_fHeadR : g_fHead, tr, pal.text);
        if (sorted) {  // setinha da ordem
            br->SetColor(pal.text2);
            float ax = right ? r - 14 : std::min(r - 10, tr.left + TextW(g_fHead, c.title) + 10), ay = TOOLBAR_H + HEADER_H / 2;
            if (g_sortAsc[g_tab]) { Line(rt, br, ax - 3.5f, ay + 1.5f, ax, ay - 2, 1.2f); Line(rt, br, ax, ay - 2, ax + 3.5f, ay + 1.5f, 1.2f); }
            else { Line(rt, br, ax - 3.5f, ay - 1.5f, ax, ay + 2, 1.2f); Line(rt, br, ax, ay + 2, ax + 3.5f, ay - 1.5f, 1.2f); }
        }
        if (i + 1 < COLS[g_tab].size() || true) {  // divisórias finas entre os títulos
            br->SetColor(pal.line);
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.15f));
            Line(rt, br, r + 0.5f, TOOLBAR_H + HEADER_H / 2 - 7.5f, r + 0.5f, TOOLBAR_H + HEADER_H / 2 + 7.5f, 1);
        }
    }
    br->SetColor(pal.line);
    if (!g_tahoe) Line(rt, br, 0, TOOLBAR_H + HEADER_H - 0.5f, s.width, TOOLBAR_H + HEADER_H - 0.5f, 1);  // o Tahoe não tem a linha
}

void DrawRows(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    auto s = Client();
    float top = ListTop(), bottom = ListBottom();
    rt->PushAxisAlignedClip(D2D1::RectF(0, top, s.width, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    auto spans = ColSpans();
    float rowsRight = std::max(spans.back().second, s.width - 26);
    int first = std::max(0, (int)(g_scroll / ROW_H));
    for (int i = first; i < (int)g_rows.size(); i++) {
        float y = top + 4 + i * ROW_H - g_scroll;  // 4 de folga depois do cabeçalho
        if (y > bottom) break;
        const Proc& p = *g_rows[i];
        bool sel = p.pid == g_sel;
        D2D1_RECT_F rr{ 0, y, s.width, y + ROW_H };  // listras de ponta a ponta, como na referência
        (void)rowsRight;
        if (g_tahoe) {  // Tahoe: listras e seleção em cápsulas afastadas das bordas
            D2D1_ROUNDED_RECT round = D2D1::RoundedRect(D2D1::RectF(10, y, s.width - 10, y + ROW_H), 7, 7);
            if (sel) { br->SetColor(g_active ? pal.accent : (g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f))); rt->FillRoundedRectangle(round, br); }
            else if (i % 2 == 1) { br->SetColor(pal.stripe); rt->FillRoundedRectangle(round, br); }
        } else if (sel) { br->SetColor(g_active ? pal.accent : (g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f))); rt->FillRectangle(rr, br); }
        else if (i % 2 == 1) { br->SetColor(pal.stripe); rt->FillRectangle(rr, br); }
        D2D1_COLOR_F tc = sel && g_active ? D2D1::ColorF(1, 1, 1) : pal.text;
        for (size_t c = 0; c < COLS[g_tab].size(); c++) {
            const Col& col = COLS[g_tab][c];
            auto [l, r] = spans[c];
            bool right = col.key != C_NAME && col.key != C_USER && col.key != C_ENGINE && col.key != C_KIND;
            if (col.key == C_NAME) {
                if (ID2D1Bitmap* ic = IconFor(p)) rt->DrawBitmap(ic, D2D1::RectF(ICON_X, y + 3, ICON_X + 18, y + 21), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                Text(rt, br, p.name, g_f13, D2D1::RectF(NAME_X, y, r - 6, y + ROW_H), tc);
            } else Text(rt, br, Cell(p, col.key), right ? g_f13r : g_f13, D2D1::RectF(l + 8, y, r - (right ? 12 : 4), y + ROW_H), tc);
        }
    }
    rt->PopAxisAlignedClip();
    // barra de rolagem fina
    float ms = MaxScroll();
    if (ms > 0) {
        float h = bottom - top, th = std::max(30.f, h * h / (h + ms)), ty = top + (h - th) * (g_scroll / ms);
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.30f) : D2D1::ColorF(0, 0, 0, 0.30f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(s.width - 11, ty + 2, s.width - 4, ty + th - 2), 3.5f, 3.5f), br);
    }
}

// gráfico de área; mirror: a segunda série vai para baixo do eixo (disco e rede, como no Mac)
void Graph(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, D2D1_RECT_F r, const std::vector<float>& a, D2D1_COLOR_F ca,
           const std::vector<float>* b, D2D1_COLOR_F cb, bool mirror, bool stack) {
    rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    float w = r.right - r.left, h = r.bottom - r.top;
    float base = mirror ? r.top + h * 0.62f : r.bottom;
    float amp = mirror ? h * 0.55f : h;
    auto path = [&](const std::vector<float>& v, const std::vector<float>* under, float sign, D2D1_COLOR_F col) {
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* sk = nullptr;
        if (FAILED(g_f->CreatePathGeometry(&g)) || FAILED(g->Open(&sk))) { if (g) g->Release(); return; }
        int n = (int)v.size();
        int have = std::clamp(g_samples - 1, 2, n);  // como no Mac: o gráfico nasce na direita e anda para a esquerda
        auto X = [&](int i) { return r.left + w * (i - (n - have)) / (have - 1); };
        auto Y = [&](int i) { float u = under ? (*under)[i] : 0; return base - sign * (u + v[i]) * amp; };
        auto Yu = [&](int i) { float u = under ? (*under)[i] : 0; return base - sign * u * amp; };
        int i0 = n - have;
        sk->BeginFigure({ X(i0), Yu(i0) }, D2D1_FIGURE_BEGIN_FILLED);
        for (int i = i0; i < n; i++) sk->AddLine({ X(i), Y(i) });
        for (int i = n - 1; i >= i0; i--) sk->AddLine({ X(i), Yu(i) });
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        sk->Close(); sk->Release();
        br->SetColor(D2D1::ColorF(col.r, col.g, col.b, 0.35f));
        rt->FillGeometry(g, br);
        g->Release();
        // contorno de cima
        br->SetColor(col);
        for (int i = i0 + 1; i < n; i++) rt->DrawLine({ X(i - 1), Y(i - 1) }, { X(i), Y(i) }, br, 1.2f);
    };
    if (b && stack) { path(*b, nullptr, 1, cb); path(a, b, 1, ca); }
    else {
        path(a, nullptr, 1, ca);
        if (b) path(*b, nullptr, mirror ? -1.f : 1.f, cb);
    }
    if (mirror) { br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.25f) : D2D1::ColorF(0, 0, 0, 0.25f)); rt->DrawLine({ r.left, base }, { r.right, base }, br, 1); }
    rt->PopAxisAlignedClip();
}

void Stats(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal, D2D1_RECT_F r, const std::vector<std::tuple<std::wstring, std::wstring, D2D1_COLOR_F>>& rows) {
    float rh = 20, y = r.top + 5;  // de cima para baixo, com linha embaixo de cada uma menos a última
    for (size_t k = 0; k < rows.size(); k++) {
        auto& [l, v, c] = rows[k];
        Text(rt, br, l, g_fStat, D2D1::RectF(r.left + 11.5f, y, r.right - 10, y + rh), pal.text);
        Text(rt, br, v, g_fStatR, D2D1::RectF(r.left + 11.5f, y, r.right - 10, y + rh), c);
        if (k + 1 < rows.size()) {
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.14f));
            Line(rt, br, r.left + 8, y + rh - 0.5f, r.right - 8, y + rh - 0.5f, 1);
        }
        y += rh;
    }
}

void DrawPanel(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    auto s = Client();
    float top = s.height - PANEL_H;
    br->SetColor(pal.panel);
    rt->FillRectangle(D2D1::RectF(0, top, s.width, s.height), br);
    if (!g_tahoe) { br->SetColor(pal.line); Line(rt, br, 0, top + 0.5f, s.width, top + 0.5f, 1); }
    float bw = 579, bx = std::round((s.width - bw) / 2), by = top + 12.5f, bh = 90;
    D2D1_RECT_F box{ bx, by, bx + bw, by + bh };
    float br0 = g_tahoe ? 10.f : 6.f;  // Tahoe: caixa mais redonda, borda mais leve
    br->SetColor(pal.box);
    rt->FillRoundedRectangle(D2D1::RoundedRect(box, br0, br0), br);
    br->SetColor(g_tahoe ? (g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.12f)) : g_dark ? D2D1::ColorF(1, 1, 1, 0.30f) : D2D1::ColorF(0, 0, 0, 0.20f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), br0 - 0.5f, br0 - 0.5f), br, 1);
    D2D1_RECT_F L{ bx, by, bx + 197.5f, by + bh }, M{ bx + 197.5f, by, bx + 381.5f, by + bh }, R{ bx + 381.5f, by, bx + bw, by + bh };
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.30f) : D2D1::ColorF(0, 0, 0, 0.20f));
    Line(rt, br, M.left + 0.5f, by, M.left + 0.5f, by + bh, 1);
    Line(rt, br, R.left + 0.5f, by, R.left + 0.5f, by + bh, 1);
    D2D1_COLOR_F blue = g_dark ? D2D1::ColorF(0.39f, 0.82f, 1.0f) : D2D1::ColorF(0.18f, 0.56f, 0.93f), red = D2D1::ColorF(1.0f, 0.33f, 0.30f), green = D2D1::ColorF(0.29f, 0.78f, 0.36f);
    // título do gráfico com uma linha embaixo; o gráfico ocupa o resto da parte do meio
    auto titleIn = [&](D2D1_RECT_F sec, const wchar_t* t) {
        Text(rt, br, t, g_f10c, D2D1::RectF(sec.left, by + 4, sec.right, by + 19), pal.text2);
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.14f));
        Line(rt, br, sec.left + 12, by + 22.5f, sec.right - 12, by + 22.5f, 1);
    };
    auto title = [&](const wchar_t* t) { titleIn(M, t); };
    D2D1_RECT_F G{ M.left + 12, by + 25, M.right - 4, by + bh - 2 };
    auto pct = [](double v) { return Pct(v) + L"%"; };
    switch (g_tab) {
    case T_CPU:
        Stats(rt, br, pal, L, { { Tr(L"Sistema:"), pct(g_cpuSys), red }, { Tr(L"Usuário:"), pct(g_cpuUser), blue }, { Tr(L"Inativo:"), pct(g_cpuIdle), pal.text } });
        title(Tr(L"CARGA DA CPU"));
        Graph(rt, br, G, g_hCpuUser, blue, &g_hCpuSys, red, false, true);
        Stats(rt, br, pal, R, { { Tr(L"Threads:"), Thousands(g_threadsTotal), pal.text }, { Tr(L"Processos:"), Thousands(g_procCount), pal.text } });
        break;
    case T_MEM: {
        titleIn(L, Tr(L"PRESSÃO DE MEMÓRIA"));
        D2D1_COLOR_F pc = g_press.nivel == 0 ? green : g_press.nivel == 1 ? D2D1::ColorF(0.98f, 0.78f, 0.18f) : red;  // verde, amarelo, vermelho como no Mac
        Graph(rt, br, D2D1::RectF(L.left + 12, by + 25, L.right - 4, by + bh - 2), g_hMem, pc, nullptr, pc, false, false);
        double page = (double)g_pi.PageSize;
        ULONGLONG used = g_ms.ullTotalPhys - g_ms.ullAvailPhys;
        double commitUsed = (double)(g_pi.CommitTotal) * page;
        double swap = std::max(0.0, commitUsed - (double)used);
        Stats(rt, br, pal, M, { { Tr(L"Memória Física:"), Bytes((double)g_ms.ullTotalPhys), pal.text }, { Tr(L"Memória Usada:"), Bytes((double)used), pal.text },
                                { Tr(L"Arquivos em Cache:"), Bytes(g_pi.SystemCache * page), pal.text }, { Tr(L"Swap Usado:"), Bytes(swap), pal.text } });
        Stats(rt, br, pal, R, { { Tr(L"Memória de Apps:"), Bytes((double)g_appMem), pal.text }, { Tr(L"Memória do Kernel:"), Bytes(g_pi.KernelTotal * page), pal.text },
                                { Tr(L"Comprimida:"), Bytes((double)g_compressed), pal.text } });
        break;
    }
    case T_ENERGY: {
        Stats(rt, br, pal, L, { { Tr(L"Impacto Energético:"), Pct(g_energyTotal), pal.text }, { Tr(L"Processos:"), Thousands(g_procCount), pal.text } });
        title(Tr(L"IMPACTO ENERGÉTICO"));
        Graph(rt, br, G, g_hEnergy, blue, nullptr, blue, false, false);
        SYSTEM_POWER_STATUS ps{};
        GetSystemPowerStatus(&ps);
        std::wstring charge = ps.BatteryLifePercent <= 100 && ps.BatteryFlag != 128 ? std::to_wstring(ps.BatteryLifePercent) + L"%" : Tr(L"Sem bateria");
        std::wstring source = ps.ACLineStatus == 1 ? Tr(L"Tomada") : ps.ACLineStatus == 0 ? Tr(L"Bateria") : L"—";
        std::wstring left = ps.BatteryLifeTime != (DWORD)-1 ? std::to_wstring(ps.BatteryLifeTime / 3600) + L":" + (ps.BatteryLifeTime / 60 % 60 < 10 ? L"0" : L"") + std::to_wstring(ps.BatteryLifeTime / 60 % 60) : L"—";
        Stats(rt, br, pal, R, { { Tr(L"Carga Restante:"), charge, pal.text }, { Tr(L"Alimentação:"), source, pal.text }, { Tr(L"Tempo Restante:"), left, pal.text } });
        break;
    }
    case T_DISK:
        Stats(rt, br, pal, L, { { Tr(L"Leituras:"), Thousands(g_rdOps), pal.text }, { Tr(L"Gravações:"), Thousands(g_wrOps), pal.text },
                                { Tr(L"Leituras/s:"), Thousands((ULONGLONG)std::llround(g_rdOpsSec)), blue }, { Tr(L"Gravações/s:"), Thousands((ULONGLONG)std::llround(g_wrOpsSec)), red } });
        title(Tr(L"IO"));
        Graph(rt, br, G, g_hRd, blue, &g_hWr, red, true, false);
        Stats(rt, br, pal, R, { { Tr(L"Dados lidos:"), Bytes((double)g_rdBytes), pal.text }, { Tr(L"Dados gravados:"), Bytes((double)g_wrBytes), pal.text },
                                { Tr(L"Dados lidos/s:"), Bytes(g_rdBytesSec), blue }, { Tr(L"Dados gravados/s:"), Bytes(g_wrBytesSec), red } });
        break;
    case T_NET:
        Stats(rt, br, pal, L, { { Tr(L"Pacotes recebidos:"), Thousands(g_pktIn), pal.text }, { Tr(L"Pacotes enviados:"), Thousands(g_pktOut), pal.text },
                                { Tr(L"Pacotes receb./s:"), Thousands((ULONGLONG)std::llround(g_pktInSec)), blue }, { Tr(L"Pacotes env./s:"), Thousands((ULONGLONG)std::llround(g_pktOutSec)), red } });
        title(Tr(L"PACOTES"));
        Graph(rt, br, G, g_hNetIn, blue, &g_hNetOut, red, true, false);
        Stats(rt, br, pal, R, { { Tr(L"Dados recebidos:"), Bytes((double)g_netIn), pal.text }, { Tr(L"Dados enviados:"), Bytes((double)g_netOut), pal.text },
                                { Tr(L"Dados receb./s:"), Bytes(g_netInSec), blue }, { Tr(L"Dados env./s:"), Bytes(g_netOutSec), red } });
        break;
    case T_GPU: {
        std::wstring name = g_gpuName.empty() ? L"—" : g_gpuName;
        for (auto w : { L"NVIDIA ", L"AMD ", L"Intel(R) ", L"GeForce " }) if (name.rfind(w, 0) == 0 && name.size() > 22) name = name.substr(wcslen(w));
        Stats(rt, br, pal, L, { { Tr(L"Placa:"), name, pal.text }, { Tr(L"Uso:"), pct(g_gpuTotal), blue },
                                { Tr(L"Memória da GPU:"), Bytes((double)g_gpuUsedMem), pal.text }, { Tr(L"Total:"), Bytes((double)g_gpuTotalMem), pal.text } });
        title(Tr(L"USO DA GPU"));
        Graph(rt, br, G, g_hGpu, blue, nullptr, blue, false, false);
        std::vector<std::pair<std::wstring, double>> eng(g_engineTotals.begin(), g_engineTotals.end());
        std::sort(eng.begin(), eng.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::vector<std::tuple<std::wstring, std::wstring, D2D1_COLOR_F>> rows;
        for (auto& [e, v] : eng) { rows.push_back({ e + L":", pct(v), pal.text }); if (rows.size() >= 4) break; }
        if (rows.empty()) rows.push_back({ Tr(L"Motores:"), L"—", pal.text });
        Stats(rt, br, pal, R, rows);
        break;
    }
    }
}

// ---- folhas (alerta de encerrar e informações do processo), descendo da barra de cima como no Mac
void SheetButton(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal, D2D1_RECT_F r, const wchar_t* t, bool def, int id) {
    br->SetColor(def ? pal.accent : (g_dark ? D2D1::ColorF(1, 1, 1, 0.20f) : D2D1::ColorF(1, 1, 1)));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
    if (!def && !g_dark) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.15f)); rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 5.5f, 5.5f), br, 1); }
    Text(rt, br, t, g_f13c, r, def ? D2D1::ColorF(1, 1, 1) : pal.text);
    g_sheetBtns.push_back({ r, id });
}

void DrawSheet(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    g_sheetBtns.clear();
    if (g_sheet == SH_NONE) return;
    auto it = g_procs.find(g_sheetPid);
    if (it == g_procs.end()) { g_sheet = SH_NONE; return; }
    const Proc& p = it->second;
    auto s = Client();
    br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.35f : 0.18f));
    rt->FillRectangle(D2D1::RectF(0, TOOLBAR_H, s.width, s.height), br);
    float w = g_sheet == SH_QUIT ? 440.f : 460.f, h = g_sheet == SH_QUIT ? 150.f : 330.f;
    D2D1_RECT_F r{ (s.width - w) / 2, TOOLBAR_H, (s.width + w) / 2, TOOLBAR_H + h };
    br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f) : D2D1::ColorF(0.96f, 0.96f, 0.96f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 10, 10), br);
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.16f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 9.5f, 9.5f), br, 1);
    IDWriteTextFormat* wrap = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL);
    if (wrap) { wrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); wrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR); }
    if (g_sheet == SH_QUIT) {
        // ícone do app (ou o do Monitor) à esquerda, pergunta em negrito, botões embaixo à direita
        if (ID2D1Bitmap* ic = IconFor(p)) rt->DrawBitmap(ic, D2D1::RectF(r.left + 20, r.top + 20, r.left + 68, r.top + 68), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else { br->SetColor(D2D1::ColorF(0.98f, 0.78f, 0.18f)); rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 24, r.top + 24, r.left + 64, r.top + 64), 9, 9), br);
               Text(rt, br, L"!", g_f13c, D2D1::RectF(r.left + 24, r.top + 24, r.left + 64, r.top + 64), D2D1::ColorF(1, 1, 1)); }
        IDWriteTextFormat* tb = Fmt(13, DWRITE_FONT_WEIGHT_BOLD);
        if (tb) { tb->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); tb->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR); }
        Text(rt, br, Tr(L"Tem certeza de que deseja encerrar este processo?"), tb, D2D1::RectF(r.left + 84, r.top + 20, r.right - 20, r.top + 56), pal.text);
        Text(rt, br, g_sheetMsg.empty() ? L"“" + p.name + Tr(L"” (PID ") + std::to_wstring(p.pid) + Tr(L"). Forçar o encerramento fecha o processo na hora, sem salvar nada.") : g_sheetMsg,
             wrap, D2D1::RectF(r.left + 84, r.top + 58, r.right - 20, r.top + 100), g_sheetMsg.empty() ? pal.text2 : D2D1::ColorF(0.92f, 0.27f, 0.25f));
        if (tb) tb->Release();
        float by = r.bottom - 40;
        SheetButton(rt, br, pal, D2D1::RectF(r.left + 84, by, r.left + 236, by + 26), Tr(L"Forçar Encerramento"), false, 2);
        SheetButton(rt, br, pal, D2D1::RectF(r.right - 196, by, r.right - 108, by + 26), Tr(L"Cancelar"), false, 0);
        SheetButton(rt, br, pal, D2D1::RectF(r.right - 100, by, r.right - 20, by + 26), Tr(L"Encerrar"), true, 1);
    } else {
        if (ID2D1Bitmap* ic = IconFor(p)) rt->DrawBitmap(ic, D2D1::RectF(r.left + 20, r.top + 18, r.left + 52, r.top + 50), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        IDWriteTextFormat* tb = Fmt(15, DWRITE_FONT_WEIGHT_BOLD);
        Text(rt, br, p.name + L" (" + std::to_wstring(p.pid) + L")", tb, D2D1::RectF(r.left + 62, r.top + 18, r.right - 20, r.top + 50), pal.text);
        if (tb) tb->Release();
        auto parent = g_procs.find(p.ppid);
        FILETIME cft{ (DWORD)p.create, (DWORD)(p.create >> 32) }, lft; SYSTEMTIME st{};
        FileTimeToLocalFileTime(&cft, &lft); FileTimeToSystemTime(&lft, &st);
        wchar_t started[64]; swprintf_s(started, Tr(L"%02d/%02d/%04d às %02d:%02d:%02d"), st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
        std::vector<std::pair<std::wstring, std::wstring>> rows = {
            { Tr(L"Processo pai:"), parent != g_procs.end() ? parent->second.name + L" (" + std::to_wstring(p.ppid) + L")" : std::to_wstring(p.ppid) },
            { Tr(L"Usuário:"), p.user }, { Tr(L"Executável:"), p.exe }, { Tr(L"Caminho:"), p.path.empty() ? L"—" : p.path },
            { Tr(L"% CPU:"), Pct(p.cpuPct) }, { Tr(L"Tempo de CPU:"), CpuTime(p.cpu) }, { Tr(L"Memória:"), Bytes((double)p.mem) },
            { Tr(L"Threads:"), Thousands(p.threads) }, { Tr(L"Identificadores:"), Thousands(p.handles) },
            { Tr(L"Bytes lidos:"), Bytes((double)p.rd) }, { Tr(L"Bytes gravados:"), Bytes((double)p.wr) }, { Tr(L"Iniciado em:"), started } };
        float y = r.top + 60;
        IDWriteTextFormat* lab = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
        for (auto& [l, v] : rows) {
            Text(rt, br, l, lab, D2D1::RectF(r.left + 10, y, r.left + 130, y + 17), pal.text);
            Text(rt, br, v, g_f11, D2D1::RectF(r.left + 138, y, r.right - 16, y + 17), pal.text);
            y += 18;
        }
        if (lab) lab->Release();
        float by = r.bottom - 40;
        SheetButton(rt, br, pal, D2D1::RectF(r.left + 20, by, r.left + 170, by + 26), Tr(L"Mostrar no Finder"), false, 3);
        SheetButton(rt, br, pal, D2D1::RectF(r.right - 196, by, r.right - 108, by + 26), Tr(L"Encerrar…"), false, 4);
        SheetButton(rt, br, pal, D2D1::RectF(r.right - 100, by, r.right - 20, by + 26), Tr(L"Fechar"), true, 0);
    }
    if (wrap) wrap->Release();
}

void ApplyTheme() {
    BOOL dm = g_dark; DwmSetWindowAttribute(g_hwnd, 20, &dm, sizeof(dm));
    if (g_tahoe) {  // Tahoe: cantos de 26 pt com o vidro da Composição (finder/tahoe.cpp), como o Ajustes
        tahoe::Frame(g_hwnd, true);
        RECT rc; GetClientRect(g_hwnd, &rc);
        tahoe::Size(g_hwnd, (float)rc.right, (float)rc.bottom, g_dark);
        shadow::SetRadius(tahoe::WindowR());
        return;
    }
    tahoe::Frame(g_hwnd, false);
    shadow::SetRadius(0);
    COLORREF border = g_dark ? RGB(88, 88, 92) : RGB(196, 196, 200); DwmSetWindowAttribute(g_hwnd, 34, &border, sizeof(border));
}

void Render() {
    if (!g_gfx) return;
    Pal pal = Colors();
    auto* dc = gfx::Begin((float)GetDpiForWindow(g_hwnd));
    if (!dc) return;
    g_rt = dc;
    {   // geometrias (gráficos) têm de vir da mesma fábrica do contexto de desenho
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        if (f && f != g_f) { if (g_f) g_f->Release(); g_f = f; } else if (f) f->Release();
    }
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_SIZE_F sz = Client();
    float winR = g_tahoe ? tahoe::Radius(g_hwnd) : 0;
    bool clip = g_tahoe && tahoe::PushWindowClip(dc, sz, winR);  // fora do canto fica transparente
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(pal.bg, &br);
    dc->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), br);
    g_scroll = std::clamp(g_scroll, 0.f, MaxScroll());
    DrawRows(g_rt, br, pal);
    DrawHeader(g_rt, br, pal);
    DrawPanel(g_rt, br, pal);
    DrawToolbar(g_rt, br, pal);
    DrawSheet(g_rt, br, pal);
    if (clip) { dc->PopLayer(); tahoe::WindowRim(dc, br, sz, winR, g_dark); }
    br->Release();
    gfx::End();
    if (!g_shown) {  // aparece só com o primeiro quadro pronto (sem piscar branco)
        g_shown = true;
        ampliacao::Revelar(g_hwnd);  // abrir animado (o Shell mostra no fim) ou já
        shadow::Create(g_hwnd);
        shadow::SetActive(g_active);
        shadow::Update();
    }
}
void Invalidate() { InvalidateRect(g_hwnd, nullptr, FALSE); }
void SetTab(int i) {
    if (i == g_tab) return;
    g_segFrom = g_tahoe ? SegPos() : -1;
    g_segAt = GetTickCount();
    g_tab = i; BuildRows(); g_scroll = 0;
    if (g_tahoe) SetTimer(g_hwnd, T_SEG, 8, nullptr);
    Invalidate();
}

// ---- ações
BOOL CALLBACK CloseWindowsOf(HWND h, LPARAM l) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == (DWORD)l && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}
bool HasWindows(DWORD pid) {
    struct S { DWORD pid; bool any; } s{ pid, false };
    EnumWindows([](HWND h, LPARAM l) -> BOOL { auto* s = (S*)l; DWORD p = 0; GetWindowThreadProcessId(h, &p);
                                               if (p == s->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { s->any = true; return FALSE; } return TRUE; }, (LPARAM)&s);
    return s.any;
}
// Encerrar: pede para as janelas fecharem (o app pode perguntar se quer salvar); sem janela, encerra.
// Forçar: TerminateProcess. Sem permissão: avisa na própria folha.
bool QuitProcess(DWORD pid, bool force) {
    if (pid == g_myPid) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return true; }
    if (!force && HasWindows(pid)) { EnumWindows(CloseWindowsOf, pid); Log(L"encerrar (janelas) pid %lu", pid); return true; }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    bool ok = h && TerminateProcess(h, 1);
    if (h) CloseHandle(h);
    Log(L"forçar encerramento pid %lu: %ls", pid, ok ? L"ok" : L"sem permissão");
    return ok;
}
void ShowInFinder(const Proc& p) {
    if (p.path.empty()) return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    std::wstring finder = std::wstring(exe) + L"\\LiquinixFinder.exe";
    if (GetFileAttributesW(finder.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(nullptr, L"open", finder.c_str(), (L"\"" + p.path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + p.path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
void Copy(const std::wstring& t) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t n = (t.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(g), t.c_str(), n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard();
}
void OpenSheet(Sheet s) { const Proc* p = Selected(); if (!p) return; g_sheet = s; g_sheetPid = p->pid; g_sheetMsg.clear(); Invalidate(); }
void SheetAction(int id) {
    auto it = g_procs.find(g_sheetPid);
    if (id == 0) { g_sheet = SH_NONE; Invalidate(); return; }
    if (it == g_procs.end()) { g_sheet = SH_NONE; Invalidate(); return; }
    if (id == 1 || id == 2) {
        if (QuitProcess(g_sheetPid, id == 2)) { g_sheet = SH_NONE; SetTimer(g_hwnd, T_SAMPLE, 400, nullptr); }  // atualiza logo
        else g_sheetMsg = Tr(L"Não foi possível encerrar este processo: ele pertence ao sistema ou a outro usuário. Abra o Monitor de Atividade como administrador.");
    } else if (id == 3) { ShowInFinder(it->second); g_sheet = SH_NONE; }
    else if (id == 4) { g_sheet = SH_QUIT; g_sheetMsg.clear(); }
    Invalidate();
}
void ActionMenu(POINT screen) {
    const Proc* p = Selected();
    using ctxmenu::Item;
    auto I = [](const wchar_t* t, int id, bool dis = false) { Item i; i.label = t; i.id = id; i.disabled = dis; return i; };
    Item sep; sep.separator = true;
    int id = ctxmenu::ShowItems(g_hwnd, screen, { I(Tr(L"Encerrar Processo…"), 1, !p), I(Tr(L"Obter Informações"), 2, !p), sep,
                                                  I(Tr(L"Mostrar no Finder"), 3, !p || p->path.empty()), I(Tr(L"Copiar Caminho"), 4, !p || p->path.empty()), I(Tr(L"Copiar PID"), 5, !p) });
    if (!p) return;
    if (id == 1) OpenSheet(SH_QUIT);
    else if (id == 2) OpenSheet(SH_INFO);
    else if (id == 3) ShowInFinder(*p);
    else if (id == 4) Copy(p->path);
    else if (id == 5) Copy(std::to_wstring(p->pid));
}

void EnsureVisible(int i) {
    float y = 4 + i * ROW_H, h = ListBottom() - ListTop();
    if (y < g_scroll) g_scroll = y;
    else if (y + ROW_H > g_scroll + h) g_scroll = y + ROW_H - h;
}
int HitRow(D2D1_POINT_2F p) {
    if (p.y < ListTop() || p.y >= ListBottom()) return -1;
    int i = (int)((p.y - ListTop() - 4 + g_scroll) / ROW_H);
    return i >= 0 && i < (int)g_rows.size() ? i : -1;
}

LRESULT HitTestNC(HWND h, LPARAM l) {
    POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(h, &pt);
    RECT rc; GetClientRect(h, &rc);
    float sc = Scale();
    if (!IsZoomed(h)) {
        int b = (int)(6 * sc);
        bool L = pt.x < b, R = pt.x >= rc.right - b, T = pt.y < b, B = pt.y >= rc.bottom - b;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
    }
    D2D1_POINT_2F p{ pt.x / sc, pt.y / sc };
    if (p.y < TOOLBAR_H && g_sheet == SH_NONE) {
        bool control = (p.x < 82 && (!LEO || p.y < 22)) || In(SearchRect(), p) || In(BtnRect(0), p) || In(BtnRect(1), p) || In(BtnRect(2), p) ||
                       (LEO ? (p.x >= SegRect(0).left && p.x < SegRect(T_COUNT - 1).right && p.y >= 22) : (p.x >= SEG_X && p.x < SEG_X + T_COUNT * SEG_W));
        for (int k = 0; k < 3; k++) if (In(BtnRect(k), p)) control = true;
        if (!control) return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            if (IsZoomed(h)) {
                auto* p = (NCCALCSIZE_PARAMS*)l;
                UINT dpi = GetDpiForWindow(h);
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: return HitTestNC(h, l);
    case WM_NCLBUTTONDBLCLK:  // duplo clique na barra: a mesma animação do botão verde
        if (w == HTCAPTION) { ampliacao::Alternar(h); return 0; }
        break;
    case WM_GETMINMAXINFO: { float sc = Scale(); ((MINMAXINFO*)l)->ptMinTrackSize = { (LONG)(1000 * sc), (LONG)(440 * sc) }; return 0; }
    case WM_SIZE:
        if (g_gfx && w != SIZE_MINIMIZED) { gfx::Resize(LOWORD(l), HIWORD(l)); if (g_tahoe) tahoe::Size(h, LOWORD(l), HIWORD(l), g_dark); }
        Invalidate();
        return 0;
    case WM_WINDOWPOSCHANGED: if (g_gfx && g_shown) shadow::Update(); break;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATEAPP:
        if (w && tahoe::Reload()) { g_tahoe = tahoe::On(); ApplyTheme(); shadow::Update(); Invalidate(); }  // tema trocado no Ajustes
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); Render(); EndPaint(h, &ps); return 0; }
    case WM_ACTIVATE: g_active = LOWORD(w) != WA_INACTIVE; if (g_gfx && g_shown) { shadow::SetActive(g_active); shadow::Update(); } Invalidate(); return 0;
    case WM_SETTINGCHANGE: {
        if (tahoe::Reload()) g_tahoe = tahoe::On();
        DWORD v = 1, cb = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
        g_dark = v == 0; if (g_gfx) { ApplyTheme(); shadow::Update(); } Invalidate();
        return 0;
    }
    case WM_TIMER:
        if (w == T_SAMPLE) { SetTimer(h, T_SAMPLE, SAMPLE_MS, nullptr); Sample(); BuildRows(); Invalidate(); }
        else if (w == 2 && g_searchFocus) Invalidate();  // cursor da busca piscando
        else if (w == T_SEG) { if (g_segFrom < 0) KillTimer(h, T_SEG); Invalidate(); }  // a gota do segmentado deslizando
        else if (w == 3 && tahoe::Reload()) { g_tahoe = tahoe::On(); ApplyTheme(); shadow::Update(); Invalidate(); }  // tema trocado no Ajustes: muda na hora
        return 0;
    case WM_MOUSEWHEEL:
        if (g_sheet != SH_NONE) return 0;
        g_scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120.f * ROW_H * 3;
        Invalidate();
        return 0;
    case WM_MOUSEMOVE: {
        float sc = Scale(); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        int hb = -1; for (int k = 0; k < 3; k++) if (In(BtnRect(k), p)) hb = k;
        bool hl = p.x < 82 && p.y < TOOLBAR_H;
        ampliacao::Verde(h, hl && p.x >= 58 && p.x < 81);
        if (hb != g_hoverBtn || hl != g_hoverLights) { g_hoverBtn = hb; g_hoverLights = hl; Invalidate(); }
        return 0;
    }
    case WM_MOUSELEAVE: ampliacao::Verde(h, false); if (g_hoverBtn >= 0 || g_hoverLights) { g_hoverBtn = -1; g_hoverLights = false; Invalidate(); } return 0;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN: {
        float sc = Scale(); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        if (g_sheet != SH_NONE) {
            for (auto& [r, id] : g_sheetBtns) if (In(r, p)) { SheetAction(id); return 0; }
            return 0;
        }
        for (int k = 0; k < 3; k++) {
            auto c = LightCenter(k);
            if (std::fabs(p.x - c.x) <= 8 && std::fabs(p.y - c.y) <= 8) {
                if (k == 0) PostMessageW(h, WM_CLOSE, 0, 0);
                else if (k == 1) ShowWindow(h, SW_MINIMIZE);
                else ampliacao::Alternar(h);
                return 0;
            }
        }
        bool wasFocus = g_searchFocus;
        g_searchFocus = In(SearchRect(), p);
        if (g_searchFocus != wasFocus) { if (g_searchFocus) SetTimer(h, 2, 530, nullptr); else KillTimer(h, 2); Invalidate(); }
        if (g_searchFocus) return 0;
        if (In(BtnRect(0), p)) { OpenSheet(SH_QUIT); return 0; }
        if (In(BtnRect(1), p)) { OpenSheet(SH_INFO); return 0; }
        if (In(BtnRect(2), p)) { auto r = BtnRect(2); POINT sp{ (LONG)(r.left * sc), (LONG)((r.bottom + 4) * sc) }; ClientToScreen(h, &sp); ActionMenu(sp); Invalidate(); return 0; }
        for (int i = 0; i < T_COUNT; i++) if (In(SegRect(i), p)) { SetTab(i); return 0; }
        if (p.y >= TOOLBAR_H && p.y < TOOLBAR_H + HEADER_H) {  // ordenar pela coluna
            auto spans = ColSpans();
            for (size_t i = 0; i < spans.size(); i++)
                if (p.x >= spans[i].first && p.x < spans[i].second) {
                    if (g_sortCol[g_tab] == (int)i) g_sortAsc[g_tab] = !g_sortAsc[g_tab];
                    else { g_sortCol[g_tab] = (int)i; ColKey k = COLS[g_tab][i].key; g_sortAsc[g_tab] = k == C_NAME || k == C_USER || k == C_PID; }
                    BuildRows(); Invalidate();
                }
            return 0;
        }
        int r = HitRow(p);
        g_sel = r >= 0 ? g_rows[r]->pid : 0;
        if (m == WM_LBUTTONDBLCLK && r >= 0) OpenSheet(SH_INFO);
        Invalidate();
        return 0;
    }
    case WM_RBUTTONUP: {
        float sc = Scale(); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        if (g_sheet != SH_NONE) return 0;
        int r = HitRow(p);
        if (r >= 0) { g_sel = g_rows[r]->pid; Invalidate(); POINT sp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &sp); ActionMenu(sp); }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (g_sheet != SH_NONE) {
            if (w == VK_ESCAPE) SheetAction(0);
            else if (w == VK_RETURN) SheetAction(g_sheet == SH_QUIT ? 1 : 0);
            return 0;
        }
        if (ctrl && w == 'F') { g_searchFocus = true; SetTimer(h, 2, 530, nullptr); Invalidate(); return 0; }
        if (ctrl && w == 'W') { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (ctrl && w == 'I') { OpenSheet(SH_INFO); return 0; }
        if (ctrl && w >= '1' && w <= '6') { SetTab((int)(w - '1')); return 0; }
        if (g_searchFocus) {
            if (w == VK_ESCAPE) { g_search.clear(); g_searchFocus = false; KillTimer(h, 2); }
            else if (w == VK_BACK && !g_search.empty()) g_search.pop_back();
            BuildRows(); Invalidate();
            if (w != VK_DOWN && w != VK_UP) return 0;
        }
        if (w == VK_DOWN || w == VK_UP) {
            int cur = -1;
            for (int i = 0; i < (int)g_rows.size(); i++) if (g_rows[i]->pid == g_sel) cur = i;
            int n = std::clamp(cur + (w == VK_DOWN ? 1 : -1), 0, (int)g_rows.size() - 1);
            if (n >= 0 && n < (int)g_rows.size()) { g_sel = g_rows[n]->pid; EnsureVisible(n); }
            Invalidate();
        } else if (w == VK_DELETE && Selected()) OpenSheet(SH_QUIT);
        return 0;
    }
    case WM_CHAR:
        if (g_searchFocus && w >= 32 && w != 127 && g_search.size() < 60) { g_search += (wchar_t)w; BuildRows(); g_scroll = 0; Invalidate(); }
        return 0;
    case WM_DESTROY: shadow::Destroy(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring PickFamily() {
    IDWriteFontCollection* fc = nullptr;
    g_dw->GetSystemFontCollection(&fc);
    std::wstring r = L"Segoe UI";
    for (auto n : { L"SF Pro Text", L"Inter", L"Segoe UI Variable Text" }) {
        UINT32 i; BOOL ok = FALSE;
        if (fc) fc->FindFamilyName(n, &i, &ok);
        if (ok) { r = n; break; }
    }
    if (fc) fc->Release();
    return r;
}

// ícone da janela: o do Monitor de Atividade do pacote do Sequoia (icones\sequoia), se existir
HICON PackIcon(int px) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe); PathRemoveFileSpecW(exe);
    std::wstring png = std::wstring(exe) + L"\\icones\\sequoia\\Activity Monitor.png";
    if (!g_wic) return nullptr;
    HICON ic = nullptr;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* scl = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(g_wic->CreateBitmapScaler(&scl)) && SUCCEEDED(scl->Initialize(fr, px, px, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(scl, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        std::vector<BYTE> bits((size_t)px * px * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, px * 4, (UINT)bits.size(), bits.data()))) {
            BITMAPV5HEADER bh{}; bh.bV5Size = sizeof(bh); bh.bV5Width = px; bh.bV5Height = -px; bh.bV5Planes = 1; bh.bV5BitCount = 32; bh.bV5Compression = BI_BITFIELDS;
            bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
            void* p = nullptr; HDC dc = GetDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &p, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (color && p) {
                memcpy(p, bits.data(), bits.size());
                HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
                ICONINFO ii{ TRUE, 0, 0, mask, color };
                ic = CreateIconIndirect(&ii);
                DeleteObject(mask);
            }
            if (color) DeleteObject(color);
        }
    }
    if (cv) cv->Release(); if (scl) scl->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    return ic;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    HANDLE one = CreateMutexW(nullptr, TRUE, L"MonitorAtividadeUmaJanela");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // já aberto: traz para a frente
        if (HWND w = FindWindowW(L"MonitorAtividadeJanela", nullptr)) { if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        return 0;
    }
    g_inst = hi;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    g_log = _wfsopen((std::wstring(exe) + L"\\monitor.log").c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    ctxmenu::Init(hi, g_log);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_f);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    g_family = PickFamily();
    g_tahoe = tahoe::On();
    auto D = DWRITE_FONT_WEIGHT_NORMAL;
    g_f13 = Fmt(13, D); g_f13r = Fmt(13, D, DWRITE_TEXT_ALIGNMENT_TRAILING); g_f13c = Fmt(13, D, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f13b = Fmt(13, DWRITE_FONT_WEIGHT_BOLD);
    g_f11 = Fmt(11, DWRITE_FONT_WEIGHT_MEDIUM); g_f11r = Fmt(11, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_f11b = Fmt(11, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_f11c = Fmt(11, D, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f10c = Fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f14b = Fmt(13, DWRITE_FONT_WEIGHT_BOLD); /* "Monitor de Atividade" cabe antes dos botões */ g_f12 = Fmt(12, D); g_f14c = Fmt(14, D, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fHead = Fmt(13, DWRITE_FONT_WEIGHT_MEDIUM); g_fHeadR = Fmt(13, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fStat = Fmt(13, D); g_fStatR = Fmt(13, D, DWRITE_TEXT_ALIGNMENT_TRAILING);
    // o nome sem ordenação fica à esquerda em negrito também quando é a coluna escolhida
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH); g_winDir = Lower(std::wstring(win) + L"\\");
    g_binDir = Lower(std::wstring(exe) + L"\\");
    { std::wstring proj = exe; proj = proj.substr(0, proj.find_last_of(L'\\')); g_packDir = proj + L"\\icones\\sequoia\\"; }
    wchar_t me[128]; DWORD mn = 128; if (GetUserNameW(me, &mn)) g_me = me;
    g_myPid = GetCurrentProcessId();
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    g_dark = v == 0;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MonitorAtividadeJanela";
    wc.hIcon = PackIcon(48); wc.hIconSm = PackIcon(16);
    RegisterClassExW(&wc);
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float sc = dx / 96.f;
    int w = (int)(1228 * sc), hgt = (int)(634 * sc);  // largura da referência do Tahoe
    g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, TrPick(L"Monitor de Atividade", L"Activity Monitor"), WS_OVERLAPPEDWINDOW,
                             (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - hgt) / 2, w, hgt, nullptr, nullptr, hi, nullptr);
    ampliacao::Janela(g_hwnd);  // abrir e fechar animados (o Shell anima)
    BOOL cloak = TRUE; DwmSetWindowAttribute(g_hwnd, 13 /*DWMWA_CLOAK*/, &cloak, sizeof(cloak));
    MARGINS mg{ -1, -1, -1, -1 }; DwmExtendFrameIntoClientArea(g_hwnd, &mg);
    ApplyTheme();
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    g_gfx = gfx::Init(g_hwnd);
    if (!g_gfx) Log(L"erro: Direct2D sobre a Composição não iniciou");
    // a sombra nasce com o primeiro quadro (Render): antes, com a janela ainda escondida, o miolo dela
    // aparecia como um retângulo escuro vazio por ~1 s
    GpuInit();
    Sample();
    Sleep(250);
    Sample();  // segunda amostra: já com % de CPU
    BuildRows();
    ShowWindow(g_hwnd, show == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
    SetTimer(g_hwnd, T_SAMPLE, SAMPLE_MS, nullptr);
    SetTimer(g_hwnd, 3, 500, nullptr);  // confere o tema (só a data do ajustes.txt)
    Log(L"monitor aberto: %zu processos, fonte %ls", g_procs.size(), g_family.c_str());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DropIcons();
    if (g_log) fclose(g_log);
    ReleaseMutex(one);
    return 0;
}
