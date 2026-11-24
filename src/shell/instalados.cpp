// Programas instalados ou removidos: o Spotlight e o Launchpad releem a lista de apps sozinhos.
// Vigia as pastas do Menu Iniciar (a do usuário e a de todos, onde os instaladores criam os atalhos)
// e o registro dos pacotes da Microsoft Store. Mudanças em sequência (um instalador cria vários
// atalhos) esperam 3 s de calma antes da nova leitura.
#include "shell.h"
#include <shlobj.h>
#include <thread>
#include <vector>

namespace {

const wchar_t* STORE_KEY = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { r = p; CoTaskMemFree(p); }
    return r;
}

void Watch() {
    const DWORD FILTER = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;
    std::vector<HANDLE> dirs;
    for (auto& d : { Known(FOLDERID_Programs), Known(FOLDERID_CommonPrograms) }) {
        if (d.empty()) continue;
        HANDLE h = FindFirstChangeNotificationW(d.c_str(), TRUE, FILTER);
        if (h != INVALID_HANDLE_VALUE) dirs.push_back(h);
        else Log(L"instalados: não deu para vigiar %ls (%lu)", d.c_str(), GetLastError());
    }
    HKEY store = nullptr;
    HANDLE storeEv = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STORE_KEY, 0, KEY_NOTIFY, &store) == ERROR_SUCCESS) {
        storeEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (RegNotifyChangeKeyValue(store, TRUE, REG_NOTIFY_CHANGE_NAME, storeEv, TRUE) != ERROR_SUCCESS) { CloseHandle(storeEv); storeEv = nullptr; }
    }
    std::vector<HANDLE> all = dirs;
    if (storeEv) all.push_back(storeEv);
    Log(L"instalados: vigiando %zu pastas do Menu Iniciar%ls", dirs.size(), storeEv ? L" e os apps da Store" : L"");
    if (all.empty()) return;

    auto rearm = [&](DWORD i) {
        if (i < dirs.size()) FindNextChangeNotification(dirs[i]);
        else RegNotifyChangeKeyValue(store, TRUE, REG_NOTIFY_CHANGE_NAME, storeEv, TRUE);
    };
    bool confirm = false;  // a pasta Aplicativos do Windows (shell:AppsFolder) atualiza uns segundos depois dos atalhos
    for (;;) {
        DWORD w = WaitForMultipleObjects((DWORD)all.size(), all.data(), FALSE, confirm ? 15000 : INFINITE);
        if (w == WAIT_TIMEOUT) {  // segunda leitura, com a pasta Aplicativos já em dia (sobretudo ao remover)
            confirm = false;
            spotlight::Rescan();
            launchpad::Rescan();
            continue;
        }
        if (w >= WAIT_OBJECT_0 + all.size()) { Log(L"instalados: espera falhou (%lu)", GetLastError()); return; }
        rearm(w - WAIT_OBJECT_0);
        // um instalador mexe em vários atalhos seguidos: espera 3 s sem mudanças (no máximo 30 s)
        ULONGLONG first = GetTickCount64();
        for (;;) {
            DWORD more = WaitForMultipleObjects((DWORD)all.size(), all.data(), FALSE, 3000);
            if (more == WAIT_TIMEOUT || GetTickCount64() - first > 30000) break;
            if (more < WAIT_OBJECT_0 + all.size()) rearm(more - WAIT_OBJECT_0);
            else break;
        }
        Log(L"instalados: a lista de programas mudou; Spotlight e Launchpad releem os apps");
        spotlight::Rescan();
        launchpad::Rescan();
        confirm = true;
    }
}

}  // namespace

void instalados::Start() {
    std::thread(Watch).detach();
}
