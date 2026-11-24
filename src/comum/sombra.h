// Sombra grande e difusa do macOS para qualquer janela: uma janela em camadas, transparente para
// cliques, logo abaixo da janela dona. O desfoque gaussiano de um retângulo é o produto de dois perfis
// 1D (função erro), então o bitmap sai rápido mesmo redimensionando. (Mesmo desenho do Finder.)
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <cmath>
#include <vector>

namespace sombra {

inline LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

struct Sombra {
    HWND wnd = nullptr, owner = nullptr;
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    SIZE last{};
    bool lastActive = true;
    // Tahoe (quem usa liga): medidas do macOS 26 (guia macOS26-Theme-Reference, seções 51 e 61; blur CSS = 2σ)
    bool tahoe = false, dark = true, lastTahoe = false;

    bool Create(HWND ownerWnd, HINSTANCE inst, const wchar_t* cls) {
        owner = ownerWnd;
        WNDCLASSEXW wc{ sizeof(wc) };
        if (!GetClassInfoExW(inst, cls, &wc)) {
            wc = { sizeof(wc) };
            wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = cls;
            RegisterClassExW(&wc);
        }
        wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        return wnd != nullptr;
    }

    void Rebuild(int w, int h, RECT win, float scale, bool active) {
        if (dib) { DeleteObject(dib); dib = nullptr; }
        if (!mem) mem = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib) return;
        SelectObject(mem, dib);
        // ativa: mais escura e maior, caindo para baixo (como no Mac); inativa: mais suave
        float sigma = (active ? 20.f : 16.f) * scale, amount = active ? 0.50f : 0.42f, dy = (active ? 16.f : 12.f) * scale; // inativa mais forte que a medida (a pedido: sumia em papel de parede escuro, 16/11/2026)
        if (tahoe) {
            if (active) { sigma = (dark ? 19.5f : 20.f) * scale; amount = dark ? 0.69f : 0.43f; dy = (dark ? 18.f : 17.f) * scale; }
            else { sigma = (dark ? 16.f : 15.f) * scale; amount = dark ? 0.62f : 0.42f; dy = (dark ? 12.f : 10.f) * scale; } // inativa mais forte que a medida (a pedido: sumia em papel de parede escuro, 16/11/2026)
        }
        float L = (float)win.left, R = (float)win.right, T = (float)win.top + dy, B = (float)win.bottom + dy;
        float k = 1.f / (sigma * 1.41421356f);
        std::vector<float> ax(w), ay(h);
        for (int x = 0; x < w; x++) ax[x] = 0.5f * (std::erf((x + 0.5f - L) * k) - std::erf((x + 0.5f - R) * k));
        for (int y = 0; y < h; y++) ay[y] = 0.5f * (std::erf((y + 0.5f - T) * k) - std::erf((y + 0.5f - B) * k));
        auto* px = (UINT32*)bits;
        for (int y = 0; y < h; y++) {
            bool rowInside = y >= win.top + 12 && y < win.bottom - 12;
            for (int x = 0; x < w; x++) {
                if (rowInside && x >= win.left + 12 && x < win.right - 12) { px[y * w + x] = 0; continue; }  // embaixo da janela: nada
                float a = amount * ax[x] * ay[y];
                px[y * w + x] = (UINT32)(BYTE)std::lround(std::fmin(a, 1.f) * 255) << 24;  // preto pré-multiplicado
            }
        }
    }

    // Chamar quando a dona mudar de lugar, tamanho, ordem ou estado (ativa/inativa)
    // visible: a parte visível da dona, quando quem chama sabe (janela recortada: o DWM informaria o retângulo todo)
    void Update(bool active, const RECT* visible = nullptr) {
        if (!wnd) return;
        if (!IsWindow(owner) || !IsWindowVisible(owner) || IsIconic(owner) || IsZoomed(owner)) { ShowWindow(wnd, SW_HIDE); return; }
        RECT vis;  // retângulo visível (sem a borda invisível de redimensionar)
        if (visible) vis = *visible;
        else if (FAILED(DwmGetWindowAttribute(owner, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) GetWindowRect(owner, &vis);
        // em tela cheia (cobre o monitor) não há sombra
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi) &&
            vis.left <= mi.rcMonitor.left && vis.top <= mi.rcMonitor.top && vis.right >= mi.rcMonitor.right && vis.bottom >= mi.rcMonitor.bottom) {
            ShowWindow(wnd, SW_HIDE); return;
        }
        float scale = GetDpiForWindow(owner) / 96.f;
        if (scale <= 0) scale = 1;
        int m = (int)(64 * scale), mb = (int)(90 * scale);
        RECT r{ vis.left - m, vis.top - m, vis.right + m, vis.bottom + mb };
        SIZE size{ r.right - r.left, r.bottom - r.top };
        if (size.cx <= 0 || size.cy <= 0 || size.cx > 16384 || size.cy > 16384) return;
        if (size.cx != last.cx || size.cy != last.cy || active != lastActive || tahoe != lastTahoe || !dib) {
            lastTahoe = tahoe;
            RECT win{ m, m, m + (vis.right - vis.left), m + (vis.bottom - vis.top) };
            Rebuild(size.cx, size.cy, win, scale, active);
            last = size;
            lastActive = active;
        }
        if (!dib) return;
        POINT pos{ r.left, r.top }, zero{ 0, 0 };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(wnd, nullptr, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA);
        SetWindowPos(wnd, owner, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);  // logo abaixo da dona
    }

    void Destroy() {
        if (wnd) DestroyWindow(wnd);
        if (dib) DeleteObject(dib);
        if (mem) DeleteDC(mem);
        wnd = nullptr; dib = nullptr; mem = nullptr; last = {};
    }
};

}  // namespace sombra
