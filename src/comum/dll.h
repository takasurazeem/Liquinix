#pragma once
// Carregar DLLs sem deixar o Windows procurar por elas. Um nome solto (LoadLibraryW(L"x.dll")) procura também na
// pasta atual e no PATH: uma DLL plantada lá rodaria com os direitos do usuário, dentro do Shell ou do app.
//  - DllAbsoluta: as nossas, sempre pelo caminho completo; recusa caminho relativo. As dependências dela vêm da
//    mesma pasta (LOAD_WITH_ALTERED_SEARCH_PATH), como em barra_mac.h.
//  - DllSistema: as do Windows, só de System32.
#include <windows.h>
#include <string>

inline bool CaminhoAbsoluto(const std::wstring& p) {
    if (p.size() < 3) return false;
    if (p[0] == L'\\' && p[1] == L'\\') return true;  // \\servidor\... e \\?\...
    return iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
}

inline HMODULE DllAbsoluta(const std::wstring& path) {
    if (!CaminhoAbsoluto(path)) return nullptr;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

inline HMODULE DllSistema(const wchar_t* name) {
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
