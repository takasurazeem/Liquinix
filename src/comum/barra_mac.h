// Barra de título do Mac nos apps do WacOS que não desenham os próprios botões (os do app2d.h: Lembretes,
// Calendário, Música, Fotos, TextEdit..., e a Pré-Visualização): o próprio app carrega a WacOSWindow.dll da
// pasta dele e pede a barra na thread da interface (WacOSPropria). Antes só o Shell punha a DLL (gancho), e o
// app aberto sem o Shell ficava com a barra do Windows, ou sem botão nenhum no tema Golden Gate (Reddit,
// 20/11/2026: "duplicated title bar buttons or then missing all together"). Agora é igual com e sem o Shell;
// o Shell não põe mais o gancho nos WacOS*.exe (janelas.cpp) e o "desfazer" dele não tira a barra do app.
// Chamar antes de criar a janela principal, na thread dela.
#pragma once
#include <windows.h>

namespace barramac {

inline bool Ligar() {
    static int st = -1;
    if (st >= 0) return st == 1;
    st = 0;
    wchar_t p[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    wchar_t* s = n ? wcsrchr(p, L'\\') : nullptr;
    if (!s || (size_t)(s + 1 - p) + 20 >= MAX_PATH) return false;
    wcscpy_s(s + 1, MAX_PATH - (s + 1 - p), L"WacOSWindow.dll");
    HMODULE m = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto f = m ? (BOOL(WINAPI*)())GetProcAddress(m, "WacOSPropria") : nullptr;
    if (f && f()) st = 1;
    return st == 1;
}

}  // namespace barramac
