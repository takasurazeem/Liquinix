// Perfil de CPU simples de um processo (para achar o que o Shell gasta parado):
//   perfil.exe <pid> [segundos]
// A cada ~2 ms suspende as threads que gastaram CPU desde a última olhada, lê a pilha (StackWalk64) e
// conta por função. Mostra as funções do próprio programa mais vistas (com símbolos do .pdb, se houver)
// e a pilha completa mais comum.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#pragma comment(lib, "dbghelp.lib")

static HANDLE g_proc;

static std::string Sym(DWORD64 a) {
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_MODULE64 mi{ sizeof(mi) };
    std::string mod = SymGetModuleInfo64(g_proc, a, &mi) ? mi.ModuleName : "?";
    if (SymFromAddr(g_proc, a, &disp, si)) return mod + "!" + si->Name;
    char t[64]; sprintf_s(t, "+%llX", a - mi.BaseOfImage);
    return mod + t;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"perfil <pid> [segundos]\n"); return 1; }
    DWORD pid = _wtoi(argv[1]);
    int secs = argc > 2 ? _wtoi(argv[2]) : 10;
    g_proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!g_proc) { wprintf(L"sem acesso ao processo\n"); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitializeW(g_proc, nullptr, TRUE);
    std::map<std::string, int> self, incl, stacks;
    std::map<DWORD, ULONG64> last;
    int samples = 0;
    DWORD end = GetTickCount() + secs * 1000;
    while (GetTickCount() < end) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{ sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            ULONG64 cyc = 0;
            QueryThreadCycleTime(th, &cyc);
            auto it = last.find(te.th32ThreadID);
            bool ran = it != last.end() && cyc - it->second > 20000;  // gastou ciclos desde a última olhada
            last[te.th32ThreadID] = cyc;
            if (ran && SuspendThread(th) != (DWORD)-1) {
                CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &ctx)) {
                    STACKFRAME64 sf{};
                    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
                    std::vector<std::string> fr;
                    for (int d = 0; d < 24; d++) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_proc, th, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                        if (!sf.AddrPC.Offset) break;
                        fr.push_back(Sym(sf.AddrPC.Offset));
                    }
                    if (!fr.empty()) {
                        samples++;
                        self[fr[0]]++;
                        std::map<std::string, bool> seen;
                        for (auto& f : fr) if (!seen[f]) { seen[f] = true; incl[f]++; }
                        std::string s;
                        for (size_t i = 0; i < fr.size() && i < 14; i++) s += "    " + fr[i] + "\n";
                        stacks[s]++;
                    }
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        Sleep(2);
    }
    auto top = [](std::map<std::string, int>& m, int n, const char* title, int total) {
        std::vector<std::pair<int, std::string>> v;
        for (auto& [k, c] : m) v.push_back({ c, k });
        std::sort(v.rbegin(), v.rend());
        printf("%s\n", title);
        for (int i = 0; i < n && i < (int)v.size(); i++) printf("  %5d %5.1f%%  %s\n", v[i].first, 100.0 * v[i].first / std::max(1, total), v[i].second.c_str());
    };
    printf("amostras: %d\n", samples);
    std::map<std::string, int> ours;
    for (auto& [k, c] : incl) if (k.rfind("WacOSShell!", 0) == 0) ours[k] = c;
    top(ours, 40, "funcoes do Shell (inclusivo):", samples);
    top(self, 15, "onde estava (proprio):", samples);
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, c] : stacks) v.push_back({ c, k });
    std::sort(v.rbegin(), v.rend());
    for (int i = 0; i < 6 && i < (int)v.size(); i++) printf("pilha %d (%d):\n%s", i + 1, v[i].first, v[i].second.c_str());
    return 0;
}
