// Backend Windows da plataforma (ver ../plataforma.h): as mesmas chamadas que o Shell já fazia.
#include "../plataforma.h"
#include <windows.h>
#include <shlobj.h>

double plataforma::AgoraMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

std::wstring plataforma::PastaAjustes() {
    PWSTR p = nullptr;
    std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) f = std::wstring(p) + L"\\Shell";
    CoTaskMemFree(p);
    return f;
}
