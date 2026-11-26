// The interface's translations without anything platform-specific: the same tables as the Windows apps
// (src/comum/idioma_<lang>.inc: Portuguese text -> that language), for code shared with Linux (#76).
// Header-only on purpose: the Windows build compiles every src\core\*.cpp into the shell, which already carries these
// tables through idioma.h; here they're only compiled where this header is included.
//
// Usage, as on Windows: the source text is Portuguese (it's the key), and Tr() returns it in the current language.
//   translate::SetLanguage(translate::FromCode(L"fr"));
//   translate::Tr(L"Lixeira")  ->  L"Corbeille"
#pragma once
#include <cwctype>
#include <string_view>
#include <unordered_map>

namespace translate {

// the same order and codes as idioma.h on Windows
enum Lang { PT, EN, ES, FR, DE, IT, JA, ZH, KO, RU, PL, TR, COUNT };
inline constexpr const wchar_t* kCodes[COUNT] = { L"pt", L"en", L"es", L"fr", L"de", L"it", L"ja", L"zh", L"ko", L"ru", L"pl", L"tr" };

// a language code ("fr", "pt-BR", "zh_CN"...) -> Lang; unknown: English
inline int FromCode(std::wstring_view code) {
    if (code.size() < 2) return EN;
    for (int i = 0; i < COUNT; i++)
        if (wchar_t(std::towlower(code[0])) == kCodes[i][0] && wchar_t(std::towlower(code[1])) == kCodes[i][1]) return i;
    return EN;
}

namespace detail {
struct Pair { const wchar_t* pt; const wchar_t* text; };
using Dict = std::unordered_map<std::wstring_view, const wchar_t*>;
inline Dict Build(const Pair* t, size_t n) { Dict d; d.reserve(n); for (size_t i = 0; i < n; i++) d.emplace(t[i].pt, t[i].text); return d; }
inline const Dict& En() { static const Pair t[] = {
#include "../comum/idioma_en.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Es() { static const Pair t[] = {
#include "../comum/idioma_es.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Fr() { static const Pair t[] = {
#include "../comum/idioma_fr.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& De() { static const Pair t[] = {
#include "../comum/idioma_de.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& It() { static const Pair t[] = {
#include "../comum/idioma_it.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Ja() { static const Pair t[] = {
#include "../comum/idioma_ja.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Zh() { static const Pair t[] = {
#include "../comum/idioma_zh.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Ko() { static const Pair t[] = {
#include "../comum/idioma_ko.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Ru() { static const Pair t[] = {
#include "../comum/idioma_ru.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Pl() { static const Pair t[] = {
#include "../comum/idioma_pl.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict& Tr() { static const Pair t[] = {
#include "../comum/idioma_tr.inc"
}; static const Dict d = Build(t, sizeof(t) / sizeof(t[0])); return d; }
inline const Dict* For(int lang) {
    switch (lang) {
    case EN: return &En(); case ES: return &Es(); case FR: return &Fr(); case DE: return &De(); case IT: return &It();
    case JA: return &Ja(); case ZH: return &Zh(); case KO: return &Ko(); case RU: return &Ru(); case PL: return &Pl();
    case TR: return &Tr(); default: return nullptr;  // Portuguese: the keys themselves
    }
}
inline int g_lang = EN;
}  // namespace detail

inline void SetLanguage(int lang) { detail::g_lang = lang >= 0 && lang < COUNT ? lang : EN; }
inline int Language() { return detail::g_lang; }
inline const wchar_t* Code() { return kCodes[detail::g_lang]; }

// the text in the current language; a missing translation falls back to English, then to the Portuguese key itself
inline const wchar_t* Tr(const wchar_t* pt) {
    if (detail::g_lang == PT) return pt;
    for (const detail::Dict* d : { detail::For(detail::g_lang), &detail::En() }) {
        if (!d) continue;
        auto it = d->find(pt);
        if (it != d->end()) return it->second;
    }
    return pt;
}

}  // namespace translate
