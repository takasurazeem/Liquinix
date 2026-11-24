#pragma once
// Arquivos que no Mac abrem nos apps da Apple abrem nos nossos, sem trocar o app padrão do Windows:
// áudio → Música, vídeo → QuickTime Player. Usado pelo Finder, pela mesa, pelo Spotlight, pela pilha de Downloads e pelo Quick Look.
// Os nossos apps ficam todos na mesma pasta (bin\), a do programa que chama.
#ifndef NOMINMAX
#define NOMINMAX  // sem as macros min/max do windows.h (quebram std::min/std::max)
#endif
#include <windows.h>
#include <shellapi.h>
#include <string>
#include "nomes.h"

inline std::wstring AppMacPara(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.'), sl = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sl != std::wstring::npos && dot < sl)) return L"";
    std::wstring x = path.substr(dot);
    for (auto& c : x) c = towlower(c);
    const wchar_t* exe = nullptr;
    for (auto e : { L".mp3", L".m4a", L".flac", L".wav", L".aac", L".ogg", L".wma", L".aif", L".aiff", L".opus" }) if (x == e) exe = EXE_MUSIC;
    for (auto e : { L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".wmv", L".webm", L".mpg", L".mpeg", L".3gp" }) if (x == e) exe = EXE_QUICKTIME;  // vídeo: QuickTime Player
    if (!exe) return L"";
    wchar_t me[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    std::wstring dir = me; dir = dir.substr(0, dir.find_last_of(L'\\'));
    std::wstring full = dir + L"\\" + exe;
    return GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES ? full : L"";
}

// true = abriu no nosso app (quem chamou não abre pelo Windows)
inline bool AbrirNoAppMac(const std::wstring& path) {
    std::wstring app = AppMacPara(path);
    if (app.empty()) return false;
    return (INT_PTR)ShellExecuteW(nullptr, L"open", app.c_str(), (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL) > 32;
}
