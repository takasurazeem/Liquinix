// Sombra grande e difusa no estilo do macOS. O Windows 11 já desenha uma sombra curta;
// esta é uma janela própria, transparente para cliques, logo abaixo da janela principal.
// O desfoque gaussiano de um retângulo é o produto de dois perfis 1D (função erro), então
// o bitmap sai rápido mesmo durante o redimensionamento.
#include "finder.h"
#include <dwmapi.h>
#include <cmath>
#include <vector>

namespace {
HWND owner, wnd;
bool active = true;
SIZE lastSize{};
bool lastActive = true;
float radiusDip = 0, lastRadius = 0;
HDC mem;
HBITMAP dib;
void* bits;

void Rebuild(int w, int h, RECT win, float scale) {
    if (dib) { DeleteObject(dib); dib = nullptr; }
    if (!mem) mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, dib);

    // janela ativa: sombra mais escura e maior, como no Mac; inativa: mais suave
    float sigma = (active ? 20.f : 16.f) * scale, amount = active ? 0.55f : 0.44f, dy = (active ? 16.f : 12.f) * scale; // inativa mais forte que a medida (a pedido: sumia em papel de parede escuro, 16/11/2026)
    if (radiusDip > 0) {  // Tahoe: medido no macOS 26 (guia, seções 51 e 61; blur CSS = 2σ)
        // tema escuro dos apps (o Notas usa este arquivo sem o tahoe.cpp: lido direto do registro)
        DWORD light = 1, cb = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &cb);
        bool dk = light == 0;
        if (active) { sigma = (dk ? 19.5f : 20.f) * scale; amount = dk ? 0.69f : 0.43f; dy = (dk ? 18.f : 17.f) * scale; }
        else { sigma = (dk ? 16.f : 15.f) * scale; amount = dk ? 0.62f : 0.42f; dy = (dk ? 12.f : 10.f) * scale; } // inativa mais forte que a medida (a pedido: sumia em papel de parede escuro, 16/11/2026)
    }
    float L = (float)win.left, R = (float)win.right, T = (float)win.top + dy, B = (float)win.bottom + dy;
    float k = 1.f / (sigma * 1.41421356f);
    std::vector<float> ax(w), ay(h);
    for (int x = 0; x < w; x++) ax[x] = 0.5f * (std::erf((x + 0.5f - L) * k) - std::erf((x + 0.5f - R) * k));
    for (int y = 0; y < h; y++) ay[y] = 0.5f * (std::erf((y + 0.5f - T) * k) - std::erf((y + 0.5f - B) * k));
    auto* px = (UINT32*)bits;
    if (radiusDip > 0) {  // Tahoe: sombra de retângulo arredondado (distância até a forma, perfil erfc)
        float rad = radiusDip * scale;
        float cxL = L + rad, cxR = R - rad, cyT = T + rad, cyB = B - rad;
        float wr = (float)win.right, wl = (float)win.left, wt = (float)win.top, wb = (float)win.bottom, ir = rad;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float fx = x + 0.5f, fy = y + 0.5f;
                // dentro da janela (arredondada) não escurece
                float ix = std::fmax(std::fmax(wl + ir - fx, fx - (wr - ir)), 0.f), iy = std::fmax(std::fmax(wt + ir - fy, fy - (wb - ir)), 0.f);
                if (fx >= wl && fx < wr && fy >= wt && fy < wb && ix * ix + iy * iy <= ir * ir) { px[y * w + x] = 0; continue; }
                float dx = std::fmax(std::fmax(cxL - fx, fx - cxR), 0.f), dy2 = std::fmax(std::fmax(cyT - fy, fy - cyB), 0.f);
                float inside = std::fmin(std::fmax(std::fmax(cxL - fx, fx - cxR), std::fmax(cyT - fy, fy - cyB)), 0.f);
                float d = std::sqrt(dx * dx + dy2 * dy2) + inside - rad;
                float a = amount * 0.5f * std::erfc(d * k);
                BYTE A = (BYTE)std::lround(std::fmin(a, 1.f) * 255);
                px[y * w + x] = (UINT32)A << 24;
            }
        }
        return;
    }
    // Embaixo da janela não escurece (a barra lateral é translúcida), menos nos cantos: o DWM arredonda a janela e,
    // com o buraco retangular, sobrava um canto claro fora da curva. Com cantos maiores (mod "rounded corners" do
    // Windhawk, raio que não dá para saber) isso aparecia como um quadro quadrado em volta (Reddit, 20/11/2026).
    // O buraco agora tem os cantos redondos com raio folgado (24 pt): cobre qualquer canto até esse tamanho.
    float rc = 24.f * scale;
    for (int y = 0; y < h; y++) {
        bool rowInside = y >= win.top && y < win.bottom;
        float fy = y + 0.5f, cy = std::fmax(std::fmax(win.top + rc - fy, fy - (win.bottom - rc)), 0.f);
        for (int x = 0; x < w; x++) {
            float fx = x + 0.5f, cx = std::fmax(std::fmax(win.left + rc - fx, fx - (win.right - rc)), 0.f);
            if (rowInside && x >= win.left && x < win.right && cx * cx + cy * cy <= rc * rc) { px[y * w + x] = 0; continue; }
            float a = amount * ax[x] * ay[y];
            BYTE A = (BYTE)std::lround(std::fmin(a, 1.f) * 255);
            px[y * w + x] = (UINT32)A << 24;  // preto pré-multiplicado
        }
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

namespace shadow {

void Create(HWND main) {
    owner = main;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ArquivosSombra";
    RegisterClassExW(&wc);
    wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
                          WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) Log(L"erro: janela de sombra (%lu)", GetLastError());
}

void SetActive(bool a) { active = a; }
void SetRadius(float dip) { radiusDip = dip; }

void Update() {
    if (!wnd) return;
    BOOL cloaked = FALSE;  // escondida (abrindo animado, maximizando): a sombra espera junto
    if (!IsWindowVisible(owner) || IsIconic(owner) || IsZoomed(owner) ||
        (SUCCEEDED(DwmGetWindowAttribute(owner, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)) { ShowWindow(wnd, SW_HIDE); return; }
    RECT vis;  // retângulo visível (sem a borda invisível de redimensionar)
    if (FAILED(DwmGetWindowAttribute(owner, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) GetWindowRect(owner, &vis);
    float scale = GetDpiForWindow(owner) / 96.f;
    int m = (int)(64 * scale), mb = (int)(90 * scale);  // margem maior embaixo: a sombra cai para baixo
    RECT r{ vis.left - m, vis.top - m, vis.right + m, vis.bottom + mb };
    SIZE size{ r.right - r.left, r.bottom - r.top };
    if (size.cx != lastSize.cx || size.cy != lastSize.cy || active != lastActive || radiusDip != lastRadius) {
        RECT win{ m, m, m + (vis.right - vis.left), m + (vis.bottom - vis.top) };
        Rebuild(size.cx, size.cy, win, scale);
        lastSize = size;
        lastActive = active;
        lastRadius = radiusDip;
    }
    POINT pos{ r.left, r.top }, zero{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!UpdateLayeredWindow(wnd, nullptr, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA))
        Log(L"erro: UpdateLayeredWindow (%lu) %dx%d", GetLastError(), size.cx, size.cy);
    // logo abaixo da janela principal na ordem das janelas
    SetWindowPos(wnd, owner, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Destroy() {
    if (wnd) DestroyWindow(wnd);
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    wnd = nullptr; dib = nullptr; mem = nullptr;
}

}  // namespace shadow
