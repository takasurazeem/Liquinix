// Foto de uma janela de outro programa: a miniatura do DWM numa janela nossa escondida (cloak) e
// PrintWindow(PW_RENDERFULLCONTENT) dela. Vale para janelas minimizadas (o DWM guarda a última imagem) e
// para apps Chromium/DirectX, sem tocar no programa. Uns poucos ms por janela.
#include "shell.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

namespace {
HWND g_cap;

bool Ensure() {
    if (g_cap) return true;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = hi; wc.lpszClassName = L"ShellFoto";
    RegisterClassW(&wc);
    g_cap = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!g_cap) return false;
    apps::ours.push_back(g_cap);
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(g_cap, DWMWA_CLOAK, &cloak, sizeof(cloak));
    return true;
}

// cantos arredondados (o Windows 11 arredonda as janelas normais): alfa por pixel, pré-multiplicado
void Corners(std::vector<BYTE>& px, UINT w, UINT h, float r) {
    int R = (int)std::ceil(r);
    for (int y = 0; y < R && y < (int)h; y++)
        for (int x = 0; x < R && x < (int)w; x++) {
            float dx = r - (x + 0.5f), dy = r - (y + 0.5f);
            float a = std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
            if (a >= 1) continue;
            for (auto [cx, cy] : { std::pair<UINT, UINT>{ (UINT)x, (UINT)y }, { w - 1 - x, (UINT)y }, { (UINT)x, h - 1 - y }, { w - 1 - x, h - 1 - y } }) {
                BYTE* c = &px[((size_t)cy * w + cx) * 4];
                for (int k = 0; k < 4; k++) c[k] = (BYTE)std::lround(c[k] * a);
            }
        }
}
}  // namespace

bool foto::Window(HWND h, const RECT& src, float corner, std::vector<BYTE>& px, UINT& w, UINT& hh) {
    if (!Ensure() || !IsWindow(h)) return false;
    w = (UINT)std::max(0L, src.right - src.left); hh = (UINT)std::max(0L, src.bottom - src.top);
    if (w < 4 || hh < 4 || w > 16384 || hh > 16384) return false;
    HTHUMBNAIL th = nullptr;
    if (FAILED(DwmRegisterThumbnail(g_cap, h, &th))) return false;
    SetWindowPos(g_cap, nullptr, 0, 0, (int)w, (int)hh, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    DWM_THUMBNAIL_PROPERTIES tp{};
    tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    tp.rcSource = src; tp.rcDestination = { 0, 0, (LONG)w, (LONG)hh };
    tp.fVisible = TRUE; tp.opacity = 255; tp.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(th, &tp);
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)hh, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool got = false;
    if (bm) {
        HGDIOBJ old = SelectObject(mdc, bm);
        for (int tries = 0; tries < 2 && !got; tries++) {
            if (tries) DwmFlush();
            if (!PrintWindow(g_cap, mdc, PW_RENDERFULLCONTENT)) continue;
            GdiFlush();
            const uint32_t* p = (const uint32_t*)bits;
            for (UINT y = 0; y < hh && !got; y += 7)
                for (UINT x = 0; x < w; x += 7) if (p[(size_t)y * w + x] & 0xFFFFFF) { got = true; break; }
        }
        if (got) {
            px.assign((const BYTE*)bits, (const BYTE*)bits + (size_t)w * hh * 4);
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // opaca
            if (corner > 0) Corners(px, w, hh, corner);
        }
        SelectObject(mdc, old);
        DeleteObject(bm);
    }
    DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    DwmUnregisterThumbnail(th);
    ShowWindow(g_cap, SW_HIDE);
    return got;
}
