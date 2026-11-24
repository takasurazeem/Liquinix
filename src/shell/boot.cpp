// Tela de inicialização do Mac: preto, maçã branca no centro e a barrinha de progresso embaixo.
// Não mexe no boot do Windows (logo do firmware, bolinhas girando): aparece quando o Shell abre logo
// depois do login, cobre todos os monitores por cima de tudo e some num esmaecimento revelando a mesa.
//
// Medidas (vídeo de um iMac Pro com Sonoma e foto de um MacBook Pro, em pt):
//   maçã ~96 de altura, um pouco acima do centro; barra 220 × 6, cantos redondos, ~145 abaixo da maçã;
//   trilho cinza-escuro, preenchimento branco. A barra surge depois da maçã, anda devagar, empaca e
//   dá saltos; no fim maçã e barra somem de uma vez, a tela fica preta um instante e a próxima tela
//   aparece num esmaecimento.
// Progresso real: partes do Shell (main.cpp chama Progress) → Explorador pronto → apps de
// inicialização (CPU sossega). Mínimo 3 s, máximo 25 s. Sem som.
// Desligada por padrão (o usuário quer a maçã só no boot do Windows): boot=1 liga logo depois do login;
// boot=sempre mostra até quando o Shell é reaberto à mão.
// Prévia: Shell.exe --testar-boot (só a tela, ~6 s, não abre o Shell).
#include "shell.h"
#include "../comum/simbolos.h"
#include <tlhelp32.h>
#include <dwmapi.h>
#include <atomic>
#include <cmath>

namespace {

std::atomic<bool> g_on;          // a tela está (ou vai estar) aberta
std::atomic<float> g_shell;      // 0..1 informado pelo main.cpp
std::atomic<bool> g_shellDone;
std::atomic<bool> g_skip;        // clique: termina já
bool g_preview, g_fresh;
HANDLE g_thread;
ID2D1Factory* g_fac;
simbolo::Sym* g_logo;

struct Win { HWND h; bool primary; ID2D1HwndRenderTarget* rt; };
std::vector<Win> g_wins;

// quando o usuário entrou: criação do sihost.exe desta sessão (começa no login); sem ele, desde o boot
double SecondsSinceLogon() {
    DWORD sess = 0; ProcessIdToSessionId(GetCurrentProcessId(), &sess);
    ULONGLONG best = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, L"sihost.exe")) continue;
            DWORD s = 0; ProcessIdToSessionId(pe.th32ProcessID, &s);
            if (s != sess) continue;
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                FILETIME c, e, k, u;
                if (GetProcessTimes(p, &c, &e, &k, &u)) best = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
                CloseHandle(p);
            }
        }
        CloseHandle(snap);
    }
    if (!best) return GetTickCount64() / 1000.0;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return n > best ? (n - best) / 1e7 : 0;
}

// uso total da CPU (0..1) desde a última chamada
float CpuBusy() {
    static ULONGLONG li, lk, lu;
    FILETIME i, k, u;
    if (!GetSystemTimes(&i, &k, &u)) return 0;
    auto v = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    ULONGLONG di = v(i) - li, dk = v(k) - lk, du = v(u) - lu;
    li = v(i); lk = v(k); lu = v(u);
    ULONGLONG total = dk + du;  // o tempo do kernel já inclui o ocioso
    return total ? 1.f - (float)di / total : 0.f;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;  // sem ponteiro, como no Mac
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: g_skip = true; return 0;
    case WM_ERASEBKGND: { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

void Draw(Win& w, float shown, float barAlpha, bool content) {
    RECT rc; GetClientRect(w.h, &rc);
    if (!w.rt) {
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(g_fac->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(w.h, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY), &w.rt))) return;
    }
    float s = GetDpiForWindow(w.h) / 96.f;
    float W = (float)rc.right, H = (float)rc.bottom;
    ID2D1HwndRenderTarget* rt = w.rt;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0));
    if (content) {
        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f), &br);
        float cy = H / 2 - 40 * s;  // centro da maçã
        if (g_logo && br) {
            D2D1_RECT_F g{}; g_logo->geo->GetBounds(nullptr, &g);
            float gh = std::max(g.bottom - g.top, 1.f), gw = g.right - g.left;
            float k = 96 * s / gh;
            float x = W / 2 - gw * k / 2 - g.left * k, y = cy - gh * k / 2 - g.top * k;
            rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x, y));
            rt->FillGeometry(g_logo->geo, br);
            rt->DrawGeometry(g_logo->geo, br, 1.0f);  // contorno de 1 unidade do SVG (peso do original)
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        if (barAlpha > 0 && br) {
            float bw = 220 * s, bh = std::max(5.f, std::round(6 * s)), bx = std::round(W / 2 - bw / 2), by = std::round(cy + 145 * s);
            float r = bh / 2;
            br->SetColor(D2D1::ColorF(0.24f, 0.24f, 0.25f, barAlpha));  // trilho
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), r, r), br);
            float fw = std::max(bh, bw * shown);
            br->SetColor(D2D1::ColorF(0.96f, 0.96f, 0.96f, barAlpha));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + fw, by + bh), r, r), br);
        }
        if (br) br->Release();
    }
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { rt->Release(); w.rt = nullptr; }
}

// alvo da barra (0..1) a partir do que já aconteceu de verdade
float Target(double t, bool& finished) {
    static double explorerAt = -1, idleSince = -1;
    finished = false;
    float p = 0.45f * std::clamp(g_shell.load(), 0.f, 1.f);
    if (!g_shellDone) return p;
    if (g_preview) {  // prévia: explorador e apps "prontos" com o ritmo do vídeo (empaca e salta)
        static double at = t;
        double e = t - at;
        p = e < 0.4 ? 0.5f : e < 1.6 ? 0.62f : e < 2.2 ? 0.66f : 1.f;
        finished = e >= 2.2;
        return p;
    }
    if (explorerAt < 0) {
        if (!FindWindowW(L"Shell_TrayWnd", nullptr) || (!FindWindowW(L"Progman", nullptr) && !FindWindowW(L"SequoiaMesa", nullptr))) return 0.5f;
        explorerAt = t; CpuBusy();
        Log(L"boot: Explorador pronto (%.1f s)", t);
    }
    if (!g_fresh) { finished = true; return 1.f; }
    // apps de inicialização: espera a CPU sossegar (<35% por 1,5 s), no máximo 12 s
    double e = t - explorerAt;
    static double lastCpu = 0;
    if (t - lastCpu >= 0.5) {
        lastCpu = t;
        float busy = CpuBusy();
        if (busy < 0.35f) { if (idleSince < 0) idleSince = t; } else idleSince = -1;
    }
    if ((idleSince >= 0 && t - idleSince >= 1.5 && e > 1) || e > 12) { finished = true; return 1.f; }
    return 0.6f + 0.33f * (float)std::min(1.0, e / 8);
}

DWORD WINAPI Thread(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellBoot";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
        RECT r = mi.rcMonitor;
        HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"ShellBoot", L"",
                                 WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, shell::inst, nullptr);
        if (h) {
            SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
            g_wins.push_back({ h, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, nullptr });
        }
        return TRUE;
    }, 0);
    for (auto& w : g_wins) { Draw(w, 0, 0, w.primary); ShowWindow(w.h, SW_SHOWNOACTIVATE); UpdateWindow(w.h); }

    double t0 = anim::Now(), last = t0, lastTop = 0, endAt = -1;
    float shown = 0;
    int phase = 0;  // 0 carregando, 1 cheia (segura), 2 preto, 3 esmaecendo
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        double now = anim::Now(), t = (now - t0) / 1000, dt = std::min(0.1, (now - last) / 1000);
        last = now;
        bool finished;
        float target = Target(t, finished);
        if (t > 25 || g_skip) { target = 1; finished = true; }
        // anda devagar sozinha, corre atrás de saltos e para onde o progresso real parou
        shown = std::min(target, shown + (float)dt * (0.06f + (target - shown) * 2.2f));
        if (g_skip) shown = 1;
        if (phase == 0 && finished && shown >= 0.999f && (t >= 3 || g_skip)) { phase = 1; endAt = t; }
        if (phase == 1 && t - endAt >= (g_skip ? 0 : 0.25)) { phase = 2; endAt = t; }
        if (phase == 2 && t - endAt >= (g_skip ? 0 : 0.35)) { phase = 3; endAt = t; Log(L"boot: esmaecendo (%.1f s)", t); }
        if (phase == 3) {
            float a = std::clamp((float)(t - endAt) / 0.7f, 0.f, 1.f);
            for (auto& w : g_wins) SetLayeredWindowAttributes(w.h, 0, (BYTE)std::lround(255 * (1 - anim::EaseInOut(a))), LWA_ALPHA);
            if (a >= 1) break;
        } else {
            float barA = phase >= 2 ? 0.f : std::clamp((float)(t - 0.5) / 0.05f, 0.f, 1.f);  // a barra surge depois da maçã
            for (auto& w : g_wins) if (w.primary) Draw(w, shown, barA, phase < 2);
        }
        if (now - lastTop > 250) {  // Dock e barra de menus também são "topmost": fica por cima deles
            lastTop = now;
            for (auto& w : g_wins) SetWindowPos(w.h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        DwmFlush();  // um quadro por atualização da tela
    }
    for (auto& w : g_wins) { if (w.rt) w.rt->Release(); DestroyWindow(w.h); }
    g_wins.clear();
    g_on = false;
    Log(L"boot: fim");
    return 0;
}

}  // namespace

namespace boot {

void Show(HINSTANCE hi) {
    std::wstring mode = shell::Setting(L"boot", L"0");
    if (!g_preview) {
        if (mode == L"0") return;
        double since = SecondsSinceLogon();
        g_fresh = since < 120;
        if (!g_fresh && mode != L"sempre") { Log(L"boot: não mostra (login há %.0f s)", since); return; }
    }
    shell::inst = hi;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, &g_fac))) return;
    if (simbolo::Dir().empty()) {  // o main.cpp ainda não definiu: mesma pasta (projeto\icones\simbolos)
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring d = exe; d = d.substr(0, d.find_last_of(L'\\')); d = d.substr(0, d.find_last_of(L'\\'));
        simbolo::Dir() = d + L"\\icones\\simbolos";
    }
    g_logo = simbolo::Load(g_fac, L"apple.logo");  // carregado aqui, antes da barra de menus usar o mesmo cache
    g_on = true;
    g_thread = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
    Log(L"boot: tela de inicialização");
}

void Progress(float p) {
    if (!g_on) return;
    float cur = g_shell.load();
    if (p > cur) g_shell = p;
}

void Done() {
    if (!g_on) return;
    g_shell = 1;
    g_shellDone = true;
}

int Preview(HINSTANCE hi) {
    g_preview = true;
    Show(hi);
    if (!g_thread) return 1;
    // ritmo do vídeo: anda, empaca, salta
    const struct { DWORD ms; float p; } steps[] = { { 700, 0.08f }, { 1300, 0.25f }, { 1800, 0.3f }, { 3200, 0.55f }, { 3400, 1 } };
    DWORD start = GetTickCount();
    for (auto& s : steps) {
        DWORD el = GetTickCount() - start;
        if (s.ms > el && WaitForSingleObject(g_thread, s.ms - el) == WAIT_OBJECT_0) break;
        Progress(s.p);
    }
    Done();
    WaitForSingleObject(g_thread, 30000);
    return 0;
}

}  // namespace boot
