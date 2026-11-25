// Linux platform backend (see ../platform.h). Standard C++ and POSIX only: CMakeLists.txt builds this file instead of
// win/platform_win.cpp.
#include "../platform.h"
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {
std::wstring Widen(const char* s) {  // UTF-8 (or whatever the environment uses) to wstring, by the process locale
    std::wstring w;
    if (!s) return w;
    std::mbstate_t st{};
    const char* p = s;
    size_t n = std::mbsrtowcs(nullptr, &p, 0, &st);
    if (n == (size_t)-1) { for (; *s; s++) w += (wchar_t)(unsigned char)*s; return w; }  // invalid: byte by byte
    w.resize(n);
    st = std::mbstate_t{}; p = s;
    std::mbsrtowcs(w.data(), &p, n, &st);
    return w;
}
}  // namespace

double platform::NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::wstring platform::SettingsDir() {
    // XDG Base Directory: $XDG_CONFIG_HOME only counts if it's an absolute path; otherwise ~/.config
    const char* x = std::getenv("XDG_CONFIG_HOME");
    if (x && x[0] == '/') return Widen(x) + L"/liquinix";
    const char* h = std::getenv("HOME");
    if (h && h[0] == '/') return Widen(h) + L"/.config/liquinix";
    return L"";
}
