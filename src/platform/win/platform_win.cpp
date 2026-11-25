// Windows platform backend (see ../platform.h): the same calls the shell always made.
#include "../platform.h"
#include <windows.h>
#include <shlobj.h>

double platform::NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

std::wstring platform::SettingsDir() {
    PWSTR p = nullptr;
    std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) f = std::wstring(p) + L"\\Shell";
    CoTaskMemFree(p);
    return f;
}
