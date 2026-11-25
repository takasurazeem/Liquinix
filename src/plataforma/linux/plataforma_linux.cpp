// Backend Linux da plataforma (ver ../plataforma.h). Só C++ padrão e POSIX: o CMake do #18 compila este arquivo
// no lugar de win/plataforma_win.cpp.
#include "../plataforma.h"
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {
std::wstring Largo(const char* s) {  // UTF-8 (ou o que o ambiente usar) para wstring, pelo locale do processo
    std::wstring w;
    if (!s) return w;
    std::mbstate_t st{};
    const char* p = s;
    size_t n = std::mbsrtowcs(nullptr, &p, 0, &st);
    if (n == (size_t)-1) { for (; *s; s++) w += (wchar_t)(unsigned char)*s; return w; }  // inválido: byte a byte
    w.resize(n);
    st = std::mbstate_t{}; p = s;
    std::mbsrtowcs(w.data(), &p, n, &st);
    return w;
}
}  // namespace

double plataforma::AgoraMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::wstring plataforma::PastaAjustes() {
    // XDG Base Directory: $XDG_CONFIG_HOME só vale se for um caminho absoluto; senão ~/.config
    const char* x = std::getenv("XDG_CONFIG_HOME");
    if (x && x[0] == '/') return Largo(x) + L"/liquinix";
    const char* h = std::getenv("HOME");
    if (h && h[0] == '/') return Largo(h) + L"/.config/liquinix";
    return L"";
}
