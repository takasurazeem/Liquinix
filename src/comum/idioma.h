#pragma once
// Idioma da interface (Ajustes do Sistema > Geral > Idioma): `idioma=<código>` em %APPDATA%\Shell\ajustes.txt.
// Códigos: pt (português do Brasil), en (inglês, o padrão desde 16/11/2026), es, fr, de, it, ja, zh (chinês
// simplificado), ko, ru, pl, tr. O texto fica escrito em português no código e passa por Tr():
//   Tr(L"Abrir")                 -> L"Open" com idioma=en, L"Abrir" com idioma=pt, L"Ouvrir" com idioma=fr
//   swprintf(buf, Tr(L"%d itens"), n)  -> o formato traduzido tem os mesmos %d/%ls, na mesma ordem
// Os dicionários pt -> <código> ficam em idioma_<código>.inc (compilados junto). O que faltar no dicionário
// do idioma sai em inglês (idioma_en.inc); o que faltar também no inglês sai em português.
// Os apps leem o idioma uma vez (na primeira chamada); o Shell chama idioma::Reload() quando o
// Ajustes avisa (ShellAjustesMudaram) e redesenha.
// Datas: use idioma::Date()/MonthName()/DayName() (formato e nomes do idioma escolhido, pelo Windows),
// não "%d de %ls" nem tabelas de meses fixas.
#include <windows.h>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

namespace idioma {

// A ordem importa: PT = 0 e EN = 1, como o antigo Set(bool en).
enum Lang { PT, EN, ES, FR, DE, IT, JA, ZH, KO, RU, PL, TR, COUNT };

struct Info { const wchar_t* code; const wchar_t* locale; const wchar_t* native; };
// O nome de cada idioma na própria língua (lista em Ajustes > Geral > Idioma).
inline constexpr Info kInfo[COUNT] = {
    { L"pt", L"pt-BR", L"Português (Brasil)" }, { L"en", L"en-US", L"English" },
    { L"es", L"es-ES", L"Español" }, { L"fr", L"fr-FR", L"Français" }, { L"de", L"de-DE", L"Deutsch" },
    { L"it", L"it-IT", L"Italiano" }, { L"ja", L"ja-JP", L"日本語" }, { L"zh", L"zh-CN", L"简体中文" },
    { L"ko", L"ko-KR", L"한국어" }, { L"ru", L"ru-RU", L"Русский" }, { L"pl", L"pl-PL", L"Polski" },
    { L"tr", L"tr-TR", L"Türkçe" },
};
// Ordem da lista no Ajustes (inglês primeiro, depois as línguas europeias e as asiáticas).
inline constexpr int kMenuOrder[COUNT] = { EN, PT, ES, FR, DE, IT, PL, RU, TR, JA, KO, ZH };

inline int g_lang = -1;  // -1 = ainda não leu o ajustes.txt

// Código ("fr", "pt-BR", "zh-Hans"...) -> Lang. Desconhecido: inglês.
inline int FromCode(std::wstring_view c) {
    if (c.size() < 2) return EN;
    wchar_t a = (wchar_t)towlower(c[0]), b = (wchar_t)towlower(c[1]);
    for (int i = 0; i < COUNT; i++)
        if (kInfo[i].code[0] == a && kInfo[i].code[1] == b) return i;
    return EN;
}

// Lê `idioma=` em %APPDATA%\Shell\ajustes.txt (sem shell32: a DLL das janelas também pode usar isto).
inline int ReadFile() {
    wchar_t ad[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (!n || n >= MAX_PATH) return EN;  // inglês é o padrão
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(ad) + L"\\Shell\\ajustes.txt").c_str(), L"r, ccs=UTF-8") || !f) return EN;
    int lang = EN;  // sem a linha idioma=: inglês
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        if (wcsncmp(line, L"idioma=", 7) == 0) lang = FromCode(line + 7);
    }
    fclose(f);
    return lang;
}

inline int Current() {
    if (g_lang < 0 || g_lang >= COUNT) g_lang = ReadFile();
    return g_lang;
}
inline const wchar_t* Code() { return kInfo[Current()].code; }      // "pt", "en", "fr"...
inline const wchar_t* Locale() { return kInfo[Current()].locale; }  // "pt-BR", "en-US", "fr-FR"... (datas, DirectWrite, APIs)
inline bool En() { return Current() == EN; }
inline bool Pt() { return Current() == PT; }
inline void Set(int lang) { g_lang = lang >= 0 && lang < COUNT ? lang : EN; }  // Set(true) = inglês, Set(false) = português
inline void SetCode(std::wstring_view code) { g_lang = FromCode(code); }
inline void Reload() { g_lang = -1; }

// ---- dicionários (um por idioma, montados na primeira consulta)
struct Pair { const wchar_t* pt; const wchar_t* tr; };
using Dict = std::unordered_map<std::wstring_view, const wchar_t*>;
inline Dict Build(const Pair* t, size_t n) {
    Dict r;
    r.reserve(n * 2);
    for (size_t i = 0; i < n; i++) r.emplace(t[i].pt, t[i].tr);
    return r;
}
#define IDIOMA_TABELA(nome) static const Dict m = Build(nome, sizeof(nome) / sizeof(nome[0])); return m
inline const Dict& MapEn() { static const Pair t[] = {
#include "idioma_en.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapEs() { static const Pair t[] = {
#include "idioma_es.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapFr() { static const Pair t[] = {
#include "idioma_fr.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapDe() { static const Pair t[] = {
#include "idioma_de.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapIt() { static const Pair t[] = {
#include "idioma_it.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapJa() { static const Pair t[] = {
#include "idioma_ja.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapZh() { static const Pair t[] = {
#include "idioma_zh.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapKo() { static const Pair t[] = {
#include "idioma_ko.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapRu() { static const Pair t[] = {
#include "idioma_ru.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapPl() { static const Pair t[] = {
#include "idioma_pl.inc"
}; IDIOMA_TABELA(t); }
inline const Dict& MapTr() { static const Pair t[] = {
#include "idioma_tr.inc"
}; IDIOMA_TABELA(t); }
#undef IDIOMA_TABELA

inline const Dict* MapFor(int lang) {
    switch (lang) {
    case EN: return &MapEn();
    case ES: return &MapEs();
    case FR: return &MapFr();
    case DE: return &MapDe();
    case IT: return &MapIt();
    case JA: return &MapJa();
    case ZH: return &MapZh();
    case KO: return &MapKo();
    case RU: return &MapRu();
    case PL: return &MapPl();
    case TR: return &MapTr();
    default: return nullptr;  // português: o texto do código
    }
}
inline const Dict& Map() { return MapEn(); }  // compatibilidade: o dicionário inglês

// pt -> idioma escolhido; sem tradução no idioma: inglês; sem inglês: o próprio pt. nullptr = não achou.
inline const wchar_t* Lookup(std::wstring_view pt) {
    int l = Current();
    if (l == PT) return nullptr;
    if (auto* m = MapFor(l)) {
        if (auto it = m->find(pt); it != m->end()) return it->second;
    }
    if (l != EN) {
        auto& m = MapEn();
        if (auto it = m.find(pt); it != m.end()) return it->second;
    }
    return nullptr;
}

// ---- datas e números no idioma escolhido (pelo Windows: GetDateFormatEx/GetLocaleInfoEx com Locale())
enum DateKind {
    D_WEEKDAY_LONG,   // "sexta-feira, 20 de novembro"    "Friday, November 20"
    D_WEEKDAY_SHORT,  // "sex. 20 de nov."                 "Fri Nov 20"
    D_LONG,           // "20 de novembro de 2026"          "November 20, 2026"
    D_MONTH_YEAR,     // "novembro de 2026"                "November 2026"
    D_SHORT,          // "20 de nov. de 2026"              "Nov 20, 2026"
    D_COUNT
};
inline const wchar_t* DatePattern(int kind) {
    static const wchar_t* const P[COUNT][D_COUNT] = {
        /* pt */ { L"dddd, d 'de' MMMM", L"ddd d 'de' MMM", L"d 'de' MMMM 'de' yyyy", L"MMMM 'de' yyyy", L"d 'de' MMM 'de' yyyy" },
        /* en */ { L"dddd, MMMM d", L"ddd MMM d", L"MMMM d, yyyy", L"MMMM yyyy", L"MMM d, yyyy" },
        /* es */ { L"dddd, d 'de' MMMM", L"ddd d MMM", L"d 'de' MMMM 'de' yyyy", L"MMMM 'de' yyyy", L"d MMM yyyy" },
        /* fr */ { L"dddd d MMMM", L"ddd d MMM", L"d MMMM yyyy", L"MMMM yyyy", L"d MMM yyyy" },
        /* de */ { L"dddd, d. MMMM", L"ddd d. MMM", L"d. MMMM yyyy", L"MMMM yyyy", L"d. MMM yyyy" },
        /* it */ { L"dddd d MMMM", L"ddd d MMM", L"d MMMM yyyy", L"MMMM yyyy", L"d MMM yyyy" },
        /* ja */ { L"M'月'd'日' dddd", L"M'月'd'日'(ddd)", L"yyyy'年'M'月'd'日'", L"yyyy'年'M'月'", L"yyyy/MM/dd" },
        /* zh */ { L"M'月'd'日' dddd", L"M'月'd'日' ddd", L"yyyy'年'M'月'd'日'", L"yyyy'年'M'月'", L"yyyy'年'M'月'd'日'" },
        /* ko */ { L"M'월' d'일' dddd", L"M'월' d'일' (ddd)", L"yyyy'년' M'월' d'일'", L"yyyy'년' M'월'", L"yyyy. M. d." },
        /* ru */ { L"dddd, d MMMM", L"ddd, d MMM", L"d MMMM yyyy 'г.'", L"MMMM yyyy", L"d MMM yyyy 'г.'" },
        /* pl */ { L"dddd, d MMMM", L"ddd, d MMM", L"d MMMM yyyy", L"MMMM yyyy", L"d MMM yyyy" },
        /* tr */ { L"d MMMM dddd", L"d MMM ddd", L"d MMMM yyyy", L"MMMM yyyy", L"d MMM yyyy" },
    };
    return P[Current()][kind >= 0 && kind < D_COUNT ? kind : D_LONG];
}
// Primeira letra maiúscula, pelas regras do idioma (serve para "sexta-feira" e "пятница").
inline std::wstring Cap(std::wstring s) {
    if (!s.empty()) LCMapStringEx(Locale(), LCMAP_UPPERCASE, &s[0], 1, &s[0], 1, nullptr, nullptr, 0);
    return s;
}
inline std::wstring Date(const SYSTEMTIME& st, int kind) {
    wchar_t b[128] = {};
    GetDateFormatEx(Locale(), 0, &st, DatePattern(kind), b, 128, nullptr);
    return b;
}
// "20 de nov. de 2026 às 14:30" / "Nov 20, 2026 at 14:30" / "2026/11/20 14:30"
inline std::wstring DateTime(const SYSTEMTIME& st, int kind);  // abaixo de Tr()
// Nome do mês (1..12) no idioma; abrev = "nov."/"Nov". Forma do nominativo (para "novembro de 2026" use Date()).
inline std::wstring MonthName(int m, bool abrev = false) {
    m = ((m - 1) % 12 + 12) % 12;
    if (Current() == ZH || Current() == JA) return std::to_wstring(m + 1) + L"月";  // como no macOS ("11月", não "十一月")
    if (Current() == KO) return std::to_wstring(m + 1) + L"월";
    wchar_t b[80] = {};
    GetLocaleInfoEx(Locale(), (abrev ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + m, b, 80);
    return b;
}
// Nome do dia (0 = domingo, como SYSTEMTIME::wDayOfWeek). forma: 0 inteiro, 1 abreviado, 2 o mais curto ("D"/"S").
inline std::wstring DayName(int d, int forma = 0) {
    d = ((d % 7) + 7) % 7;
    int i = (d + 6) % 7;  // o Windows começa na segunda-feira
    LCTYPE base = forma == 0 ? LOCALE_SDAYNAME1 : forma == 1 ? LOCALE_SABBREVDAYNAME1 : LOCALE_SSHORTESTDAYNAME1;
    wchar_t b[80] = {};
    GetLocaleInfoEx(Locale(), base + i, b, 80);
    return b;
}
// Vírgula decimal (pt, es, fr, de, it, ru, pl, tr) ou ponto (en, ja, zh, ko).
inline bool DecimalComma() {
    switch (Current()) { case EN: case JA: case ZH: case KO: return false; default: return true; }
}
// Pontos cardeais da bússola: 0 N, 1 L/E, 2 S, 3 O/W.
inline const wchar_t* Compass(int k) {
    static const wchar_t* const C[COUNT][4] = {
        { L"N", L"L", L"S", L"O" }, { L"N", L"E", L"S", L"W" }, { L"N", L"E", L"S", L"O" }, { L"N", L"E", L"S", L"O" },
        { L"N", L"O", L"S", L"W" }, { L"N", L"E", L"S", L"O" }, { L"北", L"東", L"南", L"西" }, { L"北", L"东", L"南", L"西" },
        { L"북", L"동", L"남", L"서" }, { L"С", L"В", L"Ю", L"З" }, { L"N", L"E", L"S", L"W" }, { L"K", L"D", L"G", L"B" },
    };
    return C[Current()][k & 3];
}

}  // namespace idioma

// Texto da interface no idioma escolhido. O ponteiro devolvido vale enquanto o argumento valer.
inline const wchar_t* Tr(const wchar_t* pt) {
    if (!pt || idioma::Pt()) return pt;
    const wchar_t* r = idioma::Lookup(pt);
    return r ? r : pt;
}
inline std::wstring Tr(const std::wstring& pt) {
    if (idioma::Pt()) return pt;
    const wchar_t* r = idioma::Lookup(pt);
    return r ? std::wstring(r) : pt;
}
// Escolhe entre os dois textos (nomes de apps e afins). Nos outros idiomas: a tradução de `pt` quando ela tem o
// mesmo sentido de `en` (o dicionário inglês dá `en`) ou a chave "pt|en" do dicionário; senão, o inglês.
inline const wchar_t* TrPick(const wchar_t* pt, const wchar_t* en) {
    int l = idioma::Current();
    if (l == idioma::PT) return pt;
    if (l == idioma::EN) return en;
    const idioma::Dict* m = idioma::MapFor(l);
    if (!m) return en;
    if (auto it = m->find(std::wstring(pt) + L"|" + en); it != m->end()) return it->second;
    auto& e = idioma::MapEn();
    if (auto ie = e.find(pt); ie != e.end() && wcscmp(ie->second, en) == 0)
        if (auto it = m->find(pt); it != m->end()) return it->second;
    return en;
}

inline std::wstring idioma::DateTime(const SYSTEMTIME& st, int kind) {
    wchar_t t[16];
    swprintf_s(t, L"%02d:%02d", st.wHour, st.wMinute);
    std::wstring sep = Tr(L" às ");
    if (sep.empty()) sep = L" ";
    return Date(st, kind) + sep + t;
}
