// Modo ajudante do LiquinixShell.exe: chamadas do Windows que os scripts do instalador faziam com Add-Type/DllImport
// (C# compilado na memória do PowerShell). Esse padrão, junto com o resto do instalador, fazia a análise por IA do
// Microsoft Defender acusar o pacote ("Trojan:Script/Wacatac.H!ml", Reddit 20/11/2026). Agora os scripts chamam o
// nosso exe, que faz a mesma coisa às claras e sai:
//   --sair               fecha o Shell aberto do jeito certo (o mesmo do Ctrl+Alt+Shift+F12) e espera até 20 s
//   --aplicar-cursor     avisa o Windows que o esquema de cursor do registro mudou (SPI_SETCURSORS)
//   --avisar-icones      avisa o Explorador que as associações e os ícones mudaram (SHCNE_ASSOCCHANGED)
//   --avisar-pasta "<p>" avisa que o desktop.ini de uma pasta mudou (SHCNE_UPDATEDIR)
//   --desfazer-janelas   pede a cada janela com a barra do Mac que volte ao visual do Windows; o código de saída é
//                        quantas não responderam
// Funciona com qualquer versão do Shell aberta (só usa a janela ShellMensagens e as mensagens de sempre).
#include "shell.h"
#include <shlobj.h>

namespace {
bool Tem(const wchar_t* cmd, const wchar_t* op) { return cmd && wcsstr(cmd, op); }

int Sair() {
    HWND h = FindWindowW(L"ShellMensagens", nullptr);
    if (!h) return 0;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(h, WM_HOTKEY, 1, 0);  // o mesmo do atalho "sair do Shell"
    DWORD r = ph ? WaitForSingleObject(ph, 20000) : WAIT_FAILED;
    if (ph) CloseHandle(ph);
    if (r == WAIT_OBJECT_0) return 0;
    for (int i = 0; i < 80 && FindWindowW(L"ShellMensagens", nullptr); i++) Sleep(250);
    return FindWindowW(L"ShellMensagens", nullptr) ? 1 : 0;
}

int DesfazerJanelas() {
    struct C { UINT msg; bool send; int n; } c{ RegisterWindowMessageW(L"MacJanelaDesfazer"), true, 0 };
    auto pass = [](C& c) {
        c.n = 0;
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            auto* c = (C*)l;
            if (GetPropW(h, L"MacJanela") || GetPropW(h, L"MacJanelaLeve")) {
                c->n++;
                if (c->send) SendMessageTimeoutW(h, c->msg, 0, 0, SMTO_ABORTIFHUNG, 3000, nullptr);
            }
            return TRUE;
        }, (LPARAM)&c);
    };
    pass(c);
    Sleep(500);
    c.send = false;
    pass(c);  // as que ainda estão marcadas: não responderam (app travado)
    return c.n;
}
}  // namespace

// -1: não é um pedido do modo ajudante (o Shell abre normalmente)
int AjudanteRodar(const wchar_t* cmd) {
    if (Tem(cmd, L"--sair")) return Sair();
    if (Tem(cmd, L"--aplicar-cursor")) return SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) ? 0 : 1;
    if (Tem(cmd, L"--avisar-icones")) { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); return 0; }
    if (Tem(cmd, L"--desfazer-janelas")) return DesfazerJanelas();
    if (Tem(cmd, L"--avisar-pasta")) {  // --avisar-pasta "<pasta>": o desktop.ini dela mudou (ícone da pasta)
        int n = 0; LPWSTR* a = CommandLineToArgvW(cmd, &n);
        for (int i = 1; a && i + 1 < n; i++)
            if (!wcscmp(a[i], L"--avisar-pasta")) SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH, a[i + 1], nullptr);
        if (a) LocalFree(a);
        return 0;
    }
    return -1;
}
