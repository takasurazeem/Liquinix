// Botão verde dos nossos apps (Finder, Ajustes, Notas, Terminal, Monitor de Atividade): maximizar/restaurar
// com a mesma animação do "ampliar" do Shell (ampliar.cpp). O Shell mostra a foto da janela parada; a janela
// muda de tamanho escondida (cloak, só vale para a própria janela); ~100 ms depois o Shell tira a foto já no
// tamanho novo e a anima do retângulo antigo ao novo; no fim manda "MacJanelaMostrar" e a janela reaparece.
// Sem o Shell (ou se ele não responder), só maximiza/restaura como antes.
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace ampliacao {

const UINT_PTR T_SAFETY = 0x4D4B, T_PHOTO = 0x4D4C, SUBCLASS = 0x414D504C;

inline RECT Visivel(HWND h) {
    RECT r;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    return r;
}

inline void Mostrar(HWND h) {
    KillTimer(h, T_SAFETY);
    BOOL off = FALSE; DwmSetWindowAttribute(h, DWMWA_CLOAK, &off, sizeof(off));
    RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);  // a sombra volta
    static UINT pronto = RegisterWindowMessageW(L"ShellAmpliarPronto");  // a foto do Shell pode sumir
    if (HWND s = FindWindowW(L"ShellMensagens", nullptr)) PostMessageW(s, pronto, (WPARAM)h, 0);
}

inline void CALLBACK Seguranca(HWND h, UINT, UINT_PTR, DWORD) { Mostrar(h); }  // o Shell não avisou: não fica escondida
inline void CALLBACK Foto(HWND h, UINT, UINT_PTR id, DWORD) {
    KillTimer(h, id);
    static UINT msg = RegisterWindowMessageW(L"ShellAmpliarFoto");
    if (HWND s = FindWindowW(L"ShellMensagens", nullptr)) PostMessageW(s, msg, (WPARAM)h, 0);
}

// recebe o "MacJanelaMostrar" do Shell sem precisar mexer no WndProc de cada app
inline LRESULT CALLBACK Sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    static UINT msg = RegisterWindowMessageW(L"MacJanelaMostrar");
    if (m == msg && msg) { Mostrar(h); return 1; }
    return DefSubclassProc(h, m, w, l);
}

// ---- menu do botão verde (macOS Sequoia): o ponteiro parado ~0,65 s em cima do verde pede ao Shell o menu
// de encaixe (verde.cpp). O app chama Verde(h, sobreOVerde) a cada movimento do mouse.
const UINT_PTR T_VERDE = 0x4D4F;
inline void CALLBACK VerdeTick(HWND h, UINT, UINT_PTR id, DWORD) {
    KillTimer(h, id);
    if (!RemovePropW(h, L"MacVerdeEspera") || GetAsyncKeyState(VK_LBUTTON) < 0) return;
    POINT c; GetCursorPos(&c);
    POINT p = c; ScreenToClient(h, &p);  // ainda em cima do verde (o app pode não avisar que o mouse saiu)
    float k = GetDpiForWindow(h) / 96.f; if (k <= 0) k = 1;
    if (p.x < 54 * k || p.x > 84 * k || p.y < 0 || p.y > 52 * k) return;
    static UINT msg = RegisterWindowMessageW(L"ShellMenuVerde");
    if (HWND sh = FindWindowW(L"ShellMensagens", nullptr)) PostMessageW(sh, msg, (WPARAM)h, MAKELPARAM(c.x, c.y + 14));
}
inline void Verde(HWND h, bool over) {
    if (over) { if (!GetPropW(h, L"MacVerdeEspera")) { SetPropW(h, L"MacVerdeEspera", (HANDLE)1); SetTimer(h, T_VERDE, 650, VerdeTick); } }
    else if (RemovePropW(h, L"MacVerdeEspera")) KillTimer(h, T_VERDE);
}

inline void Alternar(HWND h) {
    Verde(h, false);  // clicou no verde antes do menu aparecer: só amplia
    int cmd = IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE;
    SetWindowSubclass(h, Sub, SUBCLASS, 0);
    BOOL noAnim = TRUE;  // sem a animação de maximizar do Windows por baixo da nossa (piscava no fim)
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &noAnim, sizeof(noAnim));
    RECT from = Visivel(h);
    // flags 4: o retângulo final é medido pelo Shell depois (a janela já no tamanho novo)
    struct Msg { unsigned long long hwnd; RECT from, to; int flags; COLORREF cap; } m{ (unsigned long long)(UINT_PTR)h, from, from, 4, RGB(0, 0, 0) };
    HWND sh = FindWindowW(L"ShellMensagens", nullptr);
    COPYDATASTRUCT cd{ 0x414D504C /*'AMPL'*/, sizeof(m), &m };
    DWORD_PTR ok = 0;
    bool anim = sh && IsWindowVisible(h) && !IsIconic(h) &&
                SendMessageTimeoutW(sh, WM_COPYDATA, (WPARAM)h, (LPARAM)&cd, SMTO_ABORTIFHUNG, 400, &ok) && ok;
    if (anim) {
        BOOL on = TRUE; DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof(on));
        SetTimer(h, T_SAFETY, 1500, Seguranca);
    }
    ShowWindow(h, cmd);
    if (anim) {
        UpdateWindow(h);
        SetTimer(h, T_PHOTO, 40, Foto);  // o Shell só mede o retângulo novo
    }
}

// ---- abrir e fechar como no Mac (o Shell anima: abrir.cpp e ampliar::Fechar), o mesmo que a
// LiquinixWindow.dll faz nos apps dos outros. Chamar Janela(h) logo depois de criar a janela, antes de mostrar.
const UINT_PTR SUB_ABRIR = 0x41425249, T_ABRIR = 0x4D4D, T_ABRIR_SEG = 0x4D4E;

inline void AbrirMostrar(HWND h) {
    KillTimer(h, T_ABRIR); KillTimer(h, T_ABRIR_SEG);
    RemovePropW(h, L"MacJanelaAbrindoAvisou");
    if (!RemovePropW(h, L"MacJanelaAbrindo")) return;
    BOOL off = FALSE; DwmSetWindowAttribute(h, DWMWA_CLOAK, &off, sizeof(off));
    // a sombra (janela separada) só agora: o app a atualiza no WM_WINDOWPOSCHANGED
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
inline void CALLBACK AbrirSeg(HWND h, UINT, UINT_PTR, DWORD) { AbrirMostrar(h); }  // o Shell não respondeu
inline void CALLBACK AbrirPronto(HWND h, UINT, UINT_PTR id, DWORD) {  // já pintou: o Shell anima
    KillTimer(h, id);
    if (!GetPropW(h, L"MacJanelaAbrindo") || GetPropW(h, L"MacJanelaAbrindoAvisou")) return;
    SetPropW(h, L"MacJanelaAbrindoAvisou", (HANDLE)1);
    DwmFlush();  // o primeiro quadro já chegou ao DWM (a miniatura nasce com conteúdo)
    static UINT msg = RegisterWindowMessageW(L"ShellJanelaAbrir");
    HWND sh = FindWindowW(L"ShellMensagens", nullptr);
    if (!sh || !PostMessageW(sh, msg, (WPARAM)h, 0)) AbrirMostrar(h);
}
inline bool AnimaJanela(HWND h) {
    if (IsZoomed(h) || IsIconic(h) || GetAsyncKeyState(VK_LBUTTON) < 0) return false;
    RECT r; GetWindowRect(h, &r);  // antes de aparecer a moldura do DWM pode vir vazia (Finder)
    if (r.right - r.left < 200 || r.bottom - r.top < 120) return false;
    return FindWindowW(L"ShellMensagens", nullptr) != nullptr;
}
inline LRESULT CALLBACK SubAbrir(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    static UINT abriu = RegisterWindowMessageW(L"MacJanelaAbriu");
    if (m == abriu && abriu) { AbrirMostrar(h); return 1; }
    // aparecendo: pelo WM_SHOWWINDOW ou por SetWindowPos(SWP_SHOWWINDOW), que não manda WM_SHOWWINDOW (a
    // reserva do Finder: o primeiro ShowWindow de um processo aberto escondido é ignorado pelo Windows)
    bool showing = (m == WM_SHOWWINDOW && w) ||
                   (m == WM_WINDOWPOSCHANGING && l && (((WINDOWPOS*)l)->flags & SWP_SHOWWINDOW) && !IsWindowVisible(h));
    if (showing && !GetPropW(h, L"MacJanelaAbrindo") && AnimaJanela(h)) {
        BOOL on = TRUE, already = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &already, sizeof(already));  // o app se esconde até o 1º quadro (Revelar avisa)
        if (SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof(on)))) {
            SetPropW(h, L"MacJanelaAbrindo", (HANDLE)1);
            SetTimer(h, T_ABRIR, already ? 400 : 250, AbrirPronto);  // limite; o normal é a primeira pintura
            SetTimer(h, T_ABRIR_SEG, 1200, AbrirSeg);
        }
    }
    if (m == WM_PAINT && GetPropW(h, L"MacJanelaAbrindo") && !GetPropW(h, L"MacJanelaAbrindoAvisou")) {
        LRESULT r = DefSubclassProc(h, m, w, l);  // pinta primeiro; o quadro chega ao DWM logo depois
        SetTimer(h, T_ABRIR, 16, AbrirPronto);  // uma vez só: dá tempo ao quadro de chegar (não é animação)
        return r;
    }
    if (m == WM_WINDOWPOSCHANGING && l && (((WINDOWPOS*)l)->flags & SWP_HIDEWINDOW) && IsWindowVisible(h) && !IsIconic(h) &&
        !GetPropW(h, L"MacJanelaAbrindo")) {
        BOOL cl = FALSE;
        if (!(SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof(cl))) && cl)) {
            static UINT msg = RegisterWindowMessageW(L"ShellJanelaFechar");
            DWORD_PTR ok = 0;
            if (HWND sh = FindWindowW(L"ShellMensagens", nullptr)) SendMessageTimeoutW(sh, msg, (WPARAM)h, 0, SMTO_ABORTIFHUNG, 150, &ok);
        }
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, SubAbrir, SUB_ABRIR);
    return DefSubclassProc(h, m, w, l);
}
inline void Janela(HWND h) { if (h) SetWindowSubclass(h, SubAbrir, SUB_ABRIR, 0); }
// Para os apps que se escondem até o primeiro quadro: no lugar de tirar o cloak. Abrindo animado, avisa o
// Shell (que anima e depois mostra); senão, mostra já.
inline void Revelar(HWND h) {
    if (GetPropW(h, L"MacJanelaAbrindo")) { if (!GetPropW(h, L"MacJanelaAbrindoAvisou")) AbrirPronto(h, 0, T_ABRIR, 0); return; }
    BOOL off = FALSE; DwmSetWindowAttribute(h, DWMWA_CLOAK, &off, sizeof(off));
}

}  // namespace ampliacao
