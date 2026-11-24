// Calculadora: a Calculadora do macOS Sequoia (Win32 + Direct2D). Modos Básica, Científica e
// Programador, Converter (unidades), Modo RPN e o histórico na barra lateral.
// Medidas tiradas das capturas de referência (2x): Básica de 229 × 405 pt com teclas redondas de
// 47,5 pt a cada 53,5 × 53,1 pt; Científica de 572 × 347 pt com pílulas de 44 × 41 pt (passo 55,5 × 46);
// fundo #24292D, dígitos #464A4C, linha de cima #717577, funções #303437, operadores #FF8B00.
// Guarda modo, posição e opções em %APPDATA%\Shell\calculadora.txt e o histórico em
// %APPDATA%\Shell\calculadora-historico.txt.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <shellscalingapi.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cwctype>
#include <random>
#include <share.h>
#include "../menu/context_menu.h"
#include "../comum/ampliacao.h"
#include "../comum/idioma.h"
#include "../comum/semaforo.h"
#include "../comum/leopard.h"  // tema Mac OS X Leopard: o metal da janela

#pragma comment(lib, "windowscodecs.lib")

FILE* g_log;
void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(g_log, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

namespace {

const double PI = 3.14159265358979323846, E = 2.71828182845904523536;
const float TB = 52;      // faixa das bolinhas e dos botões redondos
const float SBW = 220;    // barra lateral do histórico
const float CONV_H = 44;  // o que o Converter acrescenta ao visor
const float HIST_H = 54;  // altura de cada cálculo no histórico

enum Mode { M_BASIC, M_SCI, M_PROG };

// ---- teclas
enum KeyId {
    K_0 = 0, K_F = 15, K_DOT = 16, K_ADD, K_SUB, K_MUL, K_DIV, K_EQ, K_AC, K_DEL, K_PCT, K_NEG,
    K_LP, K_RP, K_MC, K_MP, K_MM, K_MR, K_2ND, K_SQ, K_CUBE, K_POW, K_EXP, K_10X, K_INV, K_SQRT, K_CBRT, K_YROOT,
    K_LN, K_LOG10, K_FACT, K_SIN, K_COS, K_TAN, K_E, K_EE, K_RAND, K_SINH, K_COSH, K_TANH, K_PI, K_RAD,
    K_AND, K_OR, K_XOR, K_NOR, K_SHL, K_SHR, K_XSHLY, K_XSHRY, K_ROL, K_ROR, K_2S, K_1S, K_00, K_FF, K_BFLIP, K_WFLIP,
};
enum Style { S_FN, S_NUM, S_TOP, S_OP };
struct KeyDef { int id, col, row, style; };

// ---- estado
HINSTANCE g_inst;
HWND g_hwnd;
ID2D1Factory* g_f;
IDWriteFactory* g_dw;
IWICImagingFactory* g_wic;
ID2D1HwndRenderTarget* g_rt;
ID2D1StrokeStyle* g_round;
bool g_dark = true, g_active = true, g_hoverLights, g_shown, g_userSizing;
std::wstring g_family = L"Segoe UI", g_textFamily = L"Segoe UI";
Mode g_mode = M_BASIC;
bool g_rpn, g_convert, g_sidebar, g_rad, g_second;
float g_sciW = 572, g_sciH = 347;
wchar_t g_dec = L'.', g_grp = L',';
std::vector<KeyDef> g_keys;
std::mt19937_64 g_rng{ std::random_device{}() };

// alvo sob o mouse
enum TgKind { TG_NONE, TG_LIGHT, TG_KEY, TG_SIDEBTN, TG_MODEBTN, TG_HIST, TG_CONV, TG_BIT, TG_BASE };
struct Target { int kind = TG_NONE, idx = 0; bool operator==(const Target& o) const { return kind == o.kind && idx == o.idx; } };
Target g_hover, g_down;
std::map<int, float> g_glow;   // tecla -> brilho do clique (1 = apertada), apaga em 250 ms
std::map<int, bool> g_held;
double g_lastAnim;   // ms (QueryPerformanceCounter) do último passo do brilho das teclas
bool g_anim;         // o brilho está apagando: cada WM_PAINT dá um passo e pede o próximo quadro
double g_animStart;  // para o log dos primeiros brilhos (quadros por segundo)
int g_animFrames;
float g_histScroll;

// ---- expressão (Básica e Científica)
enum TK { T_NUM, T_VAL, T_CONST, T_OP, T_NEG, T_LP, T_RP, T_FUNC, T_POST, T_POW, T_YROOT, T_LOGY };
// T_NUM: número digitado ("123.4", "5e-3"); T_VAL: valor pronto (resultado, memória, Rand);
// T_CONST: "π" ou "e"; T_OP: "+", "−", "×", "÷"; T_FUNC: sin, log10, sqrt... (sempre seguida de "(");
// T_POST: "!", "%", "²", "³", "⁻¹"; T_POW: o operando seguinte fica sobrescrito (com "(" oculto).
struct Tok { TK k; std::wstring s; double v = 0; bool hidden = false; };
std::vector<Tok> g_toks;
bool g_done;                  // acabou de calcular: o visor mostra a conta em cinza e o resultado
std::vector<Tok> g_doneExpr;
double g_ans = 0;
std::wstring g_repOp; double g_repVal = 0; bool g_repOk;  // "=" de novo repete a última operação
double g_mem = 0;

// ---- RPN
std::vector<double> g_stack;
std::wstring g_entry;
bool g_entering;

// ---- Programador
uint64_t g_pAcc, g_pEntry;
int g_pOp;
bool g_pEntering, g_pErr;
int g_base = 10;

// ---- Converter
struct Unit { const wchar_t* name; const wchar_t* sym; double f; };
struct Cat { const wchar_t* name; std::vector<Unit> units; };
const std::vector<Cat> CATS = {
    { Tr(L"Comprimento"), { { Tr(L"Metros"), L"m", 1 }, { Tr(L"Quilômetros"), L"km", 1000 }, { Tr(L"Centímetros"), L"cm", 0.01 }, { Tr(L"Milímetros"), L"mm", 0.001 },
                        { Tr(L"Milhas"), L"mi", 1609.344 }, { Tr(L"Jardas"), L"yd", 0.9144 }, { Tr(L"Pés"), L"ft", 0.3048 }, { Tr(L"Polegadas"), L"in", 0.0254 },
                        { Tr(L"Milhas Náuticas"), L"nmi", 1852 } } },
    { Tr(L"Massa"), { { Tr(L"Quilogramas"), L"kg", 1 }, { Tr(L"Gramas"), L"g", 0.001 }, { Tr(L"Miligramas"), L"mg", 1e-6 }, { Tr(L"Toneladas"), L"t", 1000 },
                  { Tr(L"Libras"), L"lb", 0.45359237 }, { Tr(L"Onças"), L"oz", 0.028349523125 }, { Tr(L"Stones"), L"st", 6.35029318 } } },
    { Tr(L"Temperatura"), { { Tr(L"Celsius"), L"°C", 0 }, { Tr(L"Fahrenheit"), L"°F", 0 }, { Tr(L"Kelvin"), L"K", 0 } } },
    { Tr(L"Volume"), { { Tr(L"Litros"), L"L", 1 }, { Tr(L"Mililitros"), L"mL", 0.001 }, { Tr(L"Metros Cúbicos"), L"m³", 1000 }, { Tr(L"Galões (EUA)"), L"gal", 3.785411784 },
                   { Tr(L"Quartos (EUA)"), L"qt", 0.946352946 }, { Tr(L"Xícaras (EUA)"), Tr(L"xíc."), 0.2365882365 }, { Tr(L"Onças Fluidas (EUA)"), L"fl oz", 0.0295735295625 },
                   { Tr(L"Colheres de Sopa"), Tr(L"c. sopa"), 0.01478676478125 } } },
    { Tr(L"Área"), { { Tr(L"Metros Quadrados"), L"m²", 1 }, { Tr(L"Quilômetros Quadrados"), L"km²", 1e6 }, { Tr(L"Hectares"), L"ha", 1e4 }, { Tr(L"Acres"), L"ac", 4046.8564224 },
                 { Tr(L"Pés Quadrados"), L"ft²", 0.09290304 }, { Tr(L"Centímetros Quadrados"), L"cm²", 1e-4 } } },
    { Tr(L"Velocidade"), { { Tr(L"Quilômetros por Hora"), L"km/h", 1 / 3.6 }, { Tr(L"Metros por Segundo"), L"m/s", 1 }, { Tr(L"Milhas por Hora"), L"mph", 0.44704 },
                       { Tr(L"Nós"), L"kn", 1852.0 / 3600 } } },
    { Tr(L"Tempo"), { { Tr(L"Segundos"), L"s", 1 }, { Tr(L"Minutos"), L"min", 60 }, { Tr(L"Horas"), L"h", 3600 }, { Tr(L"Dias"), L"d", 86400 }, { Tr(L"Semanas"), Tr(L"sem"), 604800 },
                  { Tr(L"Anos"), Tr(L"anos"), 31557600 } } },
    { Tr(L"Dados"), { { Tr(L"Bytes"), L"B", 1 }, { Tr(L"Bits"), L"bit", 0.125 }, { Tr(L"Kilobytes"), L"KB", 1e3 }, { Tr(L"Megabytes"), L"MB", 1e6 }, { Tr(L"Gigabytes"), L"GB", 1e9 },
                  { Tr(L"Terabytes"), L"TB", 1e12 }, { Tr(L"Kibibytes"), L"KiB", 1024 }, { Tr(L"Mebibytes"), L"MiB", 1048576 }, { Tr(L"Gibibytes"), L"GiB", 1073741824 } } },
    { Tr(L"Ângulo"), { { Tr(L"Graus"), L"°", 1 }, { Tr(L"Radianos"), L"rad", 180 / PI }, { Tr(L"Grados"), L"grad", 0.9 } } },
};
int g_convCat = 0, g_convFrom = 1, g_convTo = 4;

// ---- histórico
struct HistItem { std::vector<Tok> toks; double val; };
std::vector<HistItem> g_hist;  // o mais recente primeiro

// ---------------------------------------------------------------------------
// arquivos de dados
std::wstring DataDir() {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell"; CoTaskMemFree(p); }
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
std::map<std::wstring, std::wstring> ReadKv(const std::wstring& file) {
    std::map<std::wstring, std::wstring> kv;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"r, ccs=UTF-8") || !f) return kv;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        size_t eq = s.find(L'=');
        if (eq != std::wstring::npos) kv[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(f);
    return kv;
}

std::wstring g_forceTheme;  // --tema=claro|escuro na linha de comando (testes)
bool ReadDark() {  // o ajuste "tema" do Shell (auto/claro/escuro); auto segue o Windows
    if (!g_forceTheme.empty()) return g_forceTheme == L"escuro";
    auto kv = ReadKv(DataDir() + L"\\ajustes.txt");
    auto it = kv.find(L"tema");
    if (it != kv.end() && it->second == L"claro") return false;
    if (it != kv.end() && it->second == L"escuro") return true;
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}

std::wstring SerializeToks(const std::vector<Tok>& t) {
    std::wstring o;
    for (auto& k : t) {
        wchar_t b[64]; swprintf_s(b, L"%d\x1F%d\x1F%.17g\x1F", (int)k.k, k.hidden ? 1 : 0, k.v);
        o += b; o += k.s; o += L'\x1E';
    }
    return o;
}
std::vector<Tok> ParseToks(const std::wstring& s) {
    std::vector<Tok> t;
    size_t a = 0, b;
    while ((b = s.find(L'\x1E', a)) != std::wstring::npos) {
        std::wstring part = s.substr(a, b - a); a = b + 1;
        std::vector<std::wstring> f; size_t x = 0, y;
        while ((y = part.find(L'\x1F', x)) != std::wstring::npos && f.size() < 3) { f.push_back(part.substr(x, y - x)); x = y + 1; }
        if (f.size() < 3) continue;
        Tok k; k.k = (TK)_wtoi(f[0].c_str()); k.hidden = f[1] == L"1"; k.v = wcstod(f[2].c_str(), nullptr); k.s = part.substr(x);
        t.push_back(k);
    }
    return t;
}

void SaveHistory() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (DataDir() + L"\\calculadora-historico.txt").c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& h : g_hist) fwprintf(f, L"%.17g\t%ls\n", h.val, SerializeToks(h.toks).c_str());
    fclose(f);
}
void LoadHistory() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (DataDir() + L"\\calculadora-historico.txt").c_str(), L"r, ccs=UTF-8") || !f) return;
    static wchar_t line[8192];
    while (fgetws(line, 8192, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        size_t tab = s.find(L'\t');
        if (tab == std::wstring::npos) continue;
        g_hist.push_back({ ParseToks(s.substr(tab + 1)), wcstod(s.substr(0, tab).c_str(), nullptr) });
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// números
std::wstring Group(const std::wstring& digits) {
    std::wstring o;
    int n = (int)digits.size();
    for (int i = 0; i < n; i++) {
        o += digits[i];
        int left = n - 1 - i;
        if (left > 0 && left % 3 == 0) o += g_grp;
    }
    return o;
}

// número digitado, com separador de milhar e a vírgula/ponto do sistema
std::wstring FormatTyped(const std::wstring& s) {
    std::wstring sign, body = s;
    if (!body.empty() && body[0] == L'-') { sign = L"−"; body = body.substr(1); }
    size_t e = body.find(L'e');
    std::wstring mant = body.substr(0, e), ex = e == std::wstring::npos ? L"" : body.substr(e + 1);
    size_t dot = mant.find(L'.');
    std::wstring ip = mant.substr(0, dot), fp = dot == std::wstring::npos ? L"" : mant.substr(dot + 1);
    std::wstring out = sign + Group(ip.empty() ? L"0" : ip);
    if (dot != std::wstring::npos) { out += g_dec; out += fp; }
    if (e != std::wstring::npos) { out += L"e"; for (wchar_t c : ex) out += c == L'-' ? L'−' : c; }
    return out;
}

// valor com até `sig` algarismos significativos (notação científica para os muito grandes ou pequenos)
std::wstring FormatNum(double v, int sig, bool group = true) {
    if (!std::isfinite(v)) return Tr(L"Erro");
    if (v == 0) return L"0";
    double a = std::fabs(v);
    int ex = (int)std::floor(std::log10(a));
    std::wstring sign = v < 0 ? L"−" : L"";
    auto localize = [&](std::wstring s) {
        size_t dot = s.find(L'.');
        std::wstring ip = s.substr(0, dot), fp = dot == std::wstring::npos ? L"" : s.substr(dot + 1);
        while (!fp.empty() && fp.back() == L'0') fp.pop_back();
        std::wstring o = group ? Group(ip) : ip;
        if (!fp.empty()) { o += g_dec; o += fp; }
        return o;
    };
    if (ex < sig && ex >= -8) {
        int dec = std::clamp(sig - 1 - ex, 0, 17);
        wchar_t b[96]; swprintf_s(b, L"%.*f", dec, a);
        std::wstring s = localize(b);
        if (s != L"0") return sign + s;
    }
    wchar_t b[64]; swprintf_s(b, L"%.*e", std::max(1, sig - 5), a);
    std::wstring s = b;
    size_t e = s.find(L'e');
    int exp = _wtoi(s.c_str() + e + 1);
    std::wstring m = localize(s.substr(0, e));
    return sign + m + L"e" + (exp < 0 ? L"−" : L"") + std::to_wstring(std::abs(exp));
}

// texto simples (para copiar): sem separador de milhar
std::wstring PlainNum(double v) {
    std::wstring s = FormatNum(v, 15, false);
    for (auto& c : s) if (c == L'−') c = L'-';
    return s;
}

// ---------------------------------------------------------------------------
// funções
double Fn(const std::wstring& f, double x) {
    double r = NAN;
    double toRad = g_rad ? 1 : PI / 180, fromRad = g_rad ? 1 : 180 / PI;
    if (f == L"sin") r = std::sin(x * toRad);
    else if (f == L"cos") r = std::cos(x * toRad);
    else if (f == L"tan") { r = std::tan(x * toRad); if (!g_rad && std::fmod(std::fabs(x) - 90, 180) == 0) r = NAN; }
    else if (f == L"asin") r = std::asin(x) * fromRad;
    else if (f == L"acos") r = std::acos(x) * fromRad;
    else if (f == L"atan") r = std::atan(x) * fromRad;
    else if (f == L"sinh") r = std::sinh(x);
    else if (f == L"cosh") r = std::cosh(x);
    else if (f == L"tanh") r = std::tanh(x);
    else if (f == L"asinh") r = std::asinh(x);
    else if (f == L"acosh") r = std::acosh(x);
    else if (f == L"atanh") r = std::atanh(x);
    else if (f == L"ln") r = x > 0 ? std::log(x) : NAN;
    else if (f == L"log10") r = x > 0 ? std::log10(x) : NAN;
    else if (f == L"log2") r = x > 0 ? std::log2(x) : NAN;
    else if (f == L"sqrt") r = x >= 0 ? std::sqrt(x) : NAN;
    else if (f == L"cbrt") r = std::cbrt(x);
    else if (f == L"exp") r = std::exp(x);
    else if (f == L"exp10") r = std::pow(10.0, x);
    else if (f == L"exp2") r = std::pow(2.0, x);
    else if (f == L"!") r = (x < 0 && x == std::floor(x)) ? NAN : std::tgamma(x + 1);
    else if (f == L"²") r = x * x;
    else if (f == L"³") r = x * x * x;
    else if (f == L"⁻¹") r = 1 / x;
    else if (f == L"%") r = x / 100;
    if (std::isfinite(r) && std::fabs(r) < 1e-15) r = 0;  // sin(180°) = 0, não 1,2e−16
    return r;
}
double YRoot(double x, double y) {
    if (x < 0 && std::fmod(std::fabs(y), 2) == 1) return -std::pow(-x, 1 / y);
    return std::pow(x, 1 / y);
}
double Bin(const std::wstring& op, double a, double b) {
    if (op == L"+") return a + b;
    if (op == L"−") return a - b;
    if (op == L"×") return a * b;
    if (op == L"÷") return b == 0 ? NAN : a / b;
    if (op == L"^") return std::pow(a, b);
    if (op == L"√") return YRoot(a, b);
    if (op == L"log") return std::log(a) / std::log(b);
    return NAN;
}

// avaliação com precedência: + − < × ÷ (e multiplicação implícita) < sinal < potência < pós-fixos
struct Parser {
    const std::vector<Tok>& t;
    size_t i = 0;
    bool err = false;
    struct V { double v; bool pct; };
    explicit Parser(const std::vector<Tok>& toks) : t(toks) {}
    bool At(TK k) const { return i < t.size() && t[i].k == k; }
    bool AtOp(const wchar_t* a, const wchar_t* b) const { return At(T_OP) && (t[i].s == a || t[i].s == b); }
    bool StartsPrimary() const {
        if (i >= t.size()) return false;
        TK k = t[i].k;
        return k == T_NUM || k == T_VAL || k == T_CONST || k == T_LP || k == T_FUNC || k == T_NEG;
    }
    V Expr() {
        V a = Term();
        while (AtOp(L"+", L"−")) {
            bool plus = t[i].s == L"+"; i++;
            V b = Term();
            double bv = b.pct ? a.v * b.v : b.v;  // 50 + 10% = 55, como no Mac
            a = { plus ? a.v + bv : a.v - bv, false };
        }
        return a;
    }
    V Term() {
        V a = Unary();
        for (;;) {
            if (AtOp(L"×", L"÷")) {
                bool mul = t[i].s == L"×"; i++;
                V b = Unary();
                a = { mul ? a.v * b.v : (b.v == 0 ? NAN : a.v / b.v), false };
            } else if (StartsPrimary()) {
                V b = Unary();
                a = { a.v * b.v, false };
            } else break;
        }
        return a;
    }
    V Unary() {
        if (At(T_NEG)) { i++; V a = Unary(); return { -a.v, a.pct }; }
        return Power();
    }
    V Power() {
        V a = Postfix();
        if (At(T_POW) || At(T_YROOT) || At(T_LOGY)) {
            TK k = t[i].k; i++;
            V b = Unary();
            a = { k == T_POW ? std::pow(a.v, b.v) : k == T_YROOT ? YRoot(a.v, b.v) : std::log(a.v) / std::log(b.v), false };
        }
        return a;
    }
    V Postfix() {
        V a = Primary();
        while (At(T_POST)) {
            const std::wstring& s = t[i].s; i++;
            a.v = Fn(s, a.v);
            a.pct = s == L"%";
        }
        return a;
    }
    V Primary() {
        if (i >= t.size()) { err = true; return { 0, false }; }
        const Tok& k = t[i];
        switch (k.k) {
        case T_NUM: i++; return { wcstod(k.s.c_str(), nullptr), false };
        case T_VAL: i++; return { k.v, false };
        case T_CONST: i++; return { k.s == L"π" ? PI : E, false };
        case T_LP: {
            i++;
            V a = Expr();
            if (At(T_RP)) i++;
            return { a.v, false };
        }
        case T_FUNC: { std::wstring f = k.s; i++; V a = Primary(); return { Fn(f, a.v), false }; }
        default: err = true; i++; return { 0, false };
        }
    }
};

// tira o que está incompleto no fim (operador solto, "sin(" vazio...) e calcula
bool Eval(std::vector<Tok> t, double& out) {
    for (;;) {
        if (t.empty()) return false;
        TK k = t.back().k;
        if (k == T_OP || k == T_NEG || k == T_POW || k == T_YROOT || k == T_LOGY || k == T_FUNC) { t.pop_back(); continue; }
        if (k == T_LP) { t.pop_back(); continue; }
        if (k == T_NUM && (t.back().s == L"-" || t.back().s.empty())) { t.pop_back(); continue; }
        break;
    }
    Parser p(t);
    Parser::V v = p.Expr();
    while (p.i < t.size() && t[p.i].k == T_RP) p.i++;  // ")" a mais
    if (p.err || p.i < t.size()) return false;
    out = v.v;
    return true;
}

bool HasOperation(const std::vector<Tok>& t) {
    if (t.size() < 2) return false;
    for (auto& k : t) if (k.k != T_NUM && k.k != T_VAL && k.k != T_NEG) return true;
    return false;
}

double CurrentValue() {
    if (g_mode == M_PROG) { uint64_t v = g_pEntering ? g_pEntry : g_pAcc; return (double)(int64_t)v; }
    if (g_rpn) return g_entering ? wcstod(g_entry.c_str(), nullptr) : g_stack.empty() ? 0 : g_stack.back();
    if (g_done) return g_ans;
    double v = 0;
    if (!g_toks.empty() && Eval(g_toks, v)) return v;
    return 0;
}

// ---------------------------------------------------------------------------
// desenho: texto com sobrescritos e subscritos
struct Run { std::wstring t; int lvl = 0; bool ghost = false; bool serif = false; };

std::map<std::tuple<int, int, bool, int>, IDWriteTextFormat*> g_fonts;
std::map<IDWriteTextFormat*, float> g_baselines;
IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, bool serif = false, int fam = 0) {
    auto key = std::make_tuple((int)std::lround(size * 20), (int)w, serif, fam);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    IDWriteTextFormat* tf = nullptr;
    const wchar_t* family = serif ? L"Times New Roman" : fam ? g_textFamily.c_str() : g_family.c_str();
    g_dw->CreateTextFormat(family, nullptr, w, serif ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf);
    if (tf) {
        tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    g_fonts[key] = tf;
    return tf;
}
float Baseline(IDWriteTextFormat* tf) {
    auto it = g_baselines.find(tf);
    if (it != g_baselines.end()) return it->second;
    float b = tf->GetFontSize() * 0.8f;
    IDWriteTextLayout* l = nullptr;
    if (SUCCEEDED(g_dw->CreateTextLayout(L"0", 1, tf, 1000, 1000, &l))) {
        DWRITE_LINE_METRICS lm; UINT32 n = 0;
        if (SUCCEEDED(l->GetLineMetrics(&lm, 1, &n)) && n) b = lm.baseline;
        l->Release();
    }
    return g_baselines[tf] = b;
}
float TextW(IDWriteTextFormat* tf, const std::wstring& s) {
    IDWriteTextLayout* l = nullptr; float w = 0;
    if (tf && !s.empty() && SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 8000, 1000, &l))) {
        DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; l->Release();
    }
    return w;
}
// texto com a linha de base em `baseline`
void TextAt(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* tf, float x, float baseline, D2D1_COLOR_F c) {
    if (!tf || s.empty()) return;
    br->SetColor(c);
    float top = baseline - Baseline(tf), sz = tf->GetFontSize();
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), tf, { x, top, x + 8000, top + sz * 2 }, br);
}

float RunSize(float size, int lvl) { return lvl == 0 ? size : size * (std::abs(lvl) == 1 ? 0.62f : 0.5f); }
float RunDy(float size, int lvl) {
    if (lvl > 0) return -size * (0.38f + 0.2f * (lvl - 1));
    if (lvl < 0) return size * 0.17f;
    return 0;
}
float RunsWidth(const std::vector<Run>& rs, float size, DWRITE_FONT_WEIGHT w) {
    float t = 0;
    for (auto& r : rs) t += TextW(Font(RunSize(size, r.lvl), w, r.serif), r.t);
    return t;
}
void DrawRuns(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const std::vector<Run>& rs, float size, DWRITE_FONT_WEIGHT w,
              float x, float baseline, D2D1_COLOR_F col, D2D1_COLOR_F ghost) {
    for (auto& r : rs) {
        IDWriteTextFormat* tf = Font(RunSize(size, r.lvl), w, r.serif);
        TextAt(rt, br, r.t, tf, x, baseline + RunDy(size, r.lvl), r.ghost ? ghost : col);
        x += TextW(tf, r.t);
    }
}
// alinhado à direita; diminui até minSize para caber e, se nem assim couber, mostra o fim
void FitRuns(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const std::vector<Run>& rs, float size, float minSize, DWRITE_FONT_WEIGHT w,
             float left, float right, float baseline, D2D1_COLOR_F col, D2D1_COLOR_F ghost) {
    float avail = right - left, width = RunsWidth(rs, size, w);
    if (width > avail && width > 0) { size = std::max(minSize, size * avail / width); width = RunsWidth(rs, size, w); }
    float x = right - width;
    bool clip = x < left;
    if (clip) rt->PushAxisAlignedClip({ left, baseline - size * 1.6f, right, baseline + size * 0.6f }, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    DrawRuns(rt, br, rs, size, w, x, baseline, col, ghost);
    if (clip) rt->PopAxisAlignedClip();
}

// rótulo das teclas: ^{sobrescrito}, _{subscrito}, ~{e itálico}
std::vector<Run> Markup(const std::wstring& m) {
    std::vector<Run> rs;
    std::wstring cur;
    auto flush = [&]() { if (!cur.empty()) { rs.push_back({ cur }); cur.clear(); } };
    for (size_t i = 0; i < m.size(); i++) {
        wchar_t c = m[i];
        if ((c == L'^' || c == L'_' || c == L'~') && i + 1 < m.size() && m[i + 1] == L'{') {
            size_t end = m.find(L'}', i + 2);
            if (end == std::wstring::npos) break;
            flush();
            Run r; r.t = m.substr(i + 2, end - i - 2);
            if (c == L'^') r.lvl = 1; else if (c == L'_') r.lvl = -1; else r.serif = true;
            rs.push_back(r);
            i = end;
        } else cur += c;
    }
    flush();
    return rs;
}

// a expressão em pedaços: "1÷log₁₀(sin(e^(1÷π!)))" com o expoente elevado e os ")" que faltam em cinza
size_t OperandEnd(const std::vector<Tok>& t, size_t j) {
    if (j >= t.size()) return j;
    switch (t[j].k) {
    case T_LP: {
        int d = 0;
        for (size_t k = j; k < t.size(); k++) {
            if (t[k].k == T_LP) d++;
            else if (t[k].k == T_RP && --d == 0) return k + 1;
        }
        return t.size();
    }
    case T_FUNC: case T_NEG: return OperandEnd(t, j + 1);
    default: return j + 1;
    }
}

void RenderSeq(const std::vector<Tok>& t, size_t from, size_t to, int lvl, std::vector<Run>& rs, std::vector<int>& open) {
    auto emit = [&](const std::wstring& s, int l) {
        if (!rs.empty() && rs.back().lvl == l && !rs.back().serif && !rs.back().ghost) rs.back().t += s;
        else { Run r; r.t = s; r.lvl = l; rs.push_back(r); }
    };
    for (size_t i = from; i < to;) {
        const Tok& k = t[i];
        switch (k.k) {
        case T_NUM: emit(FormatTyped(k.s), lvl); break;
        case T_VAL: emit(FormatNum(k.v, 12), lvl); break;
        case T_CONST:
            if (k.s == L"e") { Run r; r.t = L"e"; r.lvl = lvl; r.serif = true; rs.push_back(r); }
            else emit(k.s, lvl);
            break;
        case T_OP: emit(k.s, lvl); break;
        case T_NEG: emit(L"−", lvl); break;
        case T_LP: if (!k.hidden) { emit(L"(", lvl); open.push_back(lvl); } else open.push_back(-100); break;
        case T_RP: if (!open.empty()) open.pop_back(); if (!k.hidden) emit(L")", lvl); break;
        case T_FUNC: {
            const std::wstring& f = k.s;
            if (f == L"sqrt") emit(L"√", lvl);
            else if (f == L"cbrt") { emit(L"³", lvl + 1 > 2 ? 2 : lvl + 1); emit(L"√", lvl); }
            else if (f == L"log10") { emit(L"log", lvl); emit(L"10", lvl - 1); }
            else if (f == L"log2") { emit(L"log", lvl); emit(L"2", lvl - 1); }
            else if (f.size() > 1 && f[0] == L'a' && f != L"a") { emit(f.substr(1), lvl); emit(L"−1", lvl + 1); }
            else emit(f, lvl);
            break;
        }
        case T_POST:
            if (k.s == L"²") emit(L"2", lvl + 1);
            else if (k.s == L"³") emit(L"3", lvl + 1);
            else if (k.s == L"⁻¹") emit(L"−1", lvl + 1);
            else emit(k.s, lvl);
            break;
        case T_POW: {
            size_t j = i + 1, e = std::min(OperandEnd(t, j), to);
            while (e < to && t[e].k == T_POST) e++;
            RenderSeq(t, j, e, std::min(lvl + 1, 3), rs, open);
            i = e;
            continue;
        }
        case T_YROOT: emit(L"ʸ√", lvl); break;
        case T_LOGY: emit(L"log", lvl); emit(L"y", lvl - 1); break;
        }
        i++;
    }
}
std::vector<Run> RenderToks(const std::vector<Tok>& t) {
    std::vector<Run> rs;
    std::vector<int> open;
    RenderSeq(t, 0, t.size(), 0, rs, open);
    for (int i = (int)open.size() - 1; i >= 0; i--)
        if (open[i] != -100) { Run r; r.t = L")"; r.lvl = open[i]; r.ghost = true; rs.push_back(r); }
    return rs;
}
std::wstring PlainOf(const std::vector<Run>& rs) {
    std::wstring o;
    for (auto& r : rs) {
        if (r.ghost) continue;
        if (r.lvl > 0) o += L"^";
        o += r.t;
    }
    return o;
}

// ---------------------------------------------------------------------------
// geometria
float Scale() { return GetDpiForWindow(g_hwnd) / 96.f; }
D2D1_SIZE_F Client() { RECT r; GetClientRect(g_hwnd, &r); float s = Scale(); return { r.right / s, r.bottom / s }; }
float Side() { return g_sidebar ? SBW : 0; }
bool ConvOn() { return g_convert && g_mode != M_PROG; }
D2D1_SIZE_F ContentSize() {  // sem a barra lateral
    float conv = ConvOn() ? CONV_H : 0;
    if (g_mode == M_BASIC) return { 229, (semaforo::Leopard() ? 282.f : 405.f) + conv };  // Leopard: teclas baixas, janela compacta
    if (g_mode == M_SCI) return { g_sciW, g_sciH + conv };
    return { 572, 442 };
}
float KeysTop() {
    if (semaforo::Leopard() && g_mode != M_PROG) return 96.f + (ConvOn() ? CONV_H : 0);  // Leopard: o visor baixo do 10.5
    return (g_mode == M_BASIC ? 132.f : g_mode == M_SCI ? 113.f : 206.f) + (ConvOn() ? CONV_H : 0);
}
D2D1_RECT_F KeyRect(const KeyDef& k) {
    auto c = Client();
    float x0 = Side(), W = c.width - x0, H = c.height, top = KeysTop();
    if (g_mode == M_BASIC) {
        float d = 47.5f, cx = x0 + 34.5f + 53.5f * k.col, cy = top + d / 2 + 53.1f * k.row;
        if (semaforo::Leopard()) { cy = top + 13 + 34 * k.row; return { cx - d / 2 - 2, cy - 14, cx + d / 2 + 2, cy + 14 }; }  // Leopard: passo de 34
        return { cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2 };
    }
    int cols = g_mode == M_SCI ? 10 : 8;
    float mx = 13, gx = 11.5f, gy = 5, bottom = 10.5f;
    float kw = (W - 2 * mx - gx * (cols - 1)) / cols, kh = (H - bottom - top - gy * 4) / 5;
    float x = x0 + mx + k.col * (kw + gx), y = top + k.row * (kh + gy);
    return { x, y, x + kw, y + kh };
}
D2D1_POINT_2F LightCenter(int k) { return semaforo::Leopard() ? D2D1_POINT_2F{ 14.5f + k * 21, 11 } : D2D1_POINT_2F{ 23.f + k * 23, 25 }; }  // Leopard: na barra de 22
// Leopard: os dois botões redondos ficam pequenos (metade) na ponta direita da barra de título de 22
D2D1_POINT_2F SideBtnCenter() { return semaforo::Leopard() ? D2D1_POINT_2F{ Client().width - 34, 11 } : D2D1_POINT_2F{ Side() + (g_mode == M_BASIC ? 123.5f : 105.5f), 26 }; }
D2D1_POINT_2F ModeBtnCenter() { return semaforo::Leopard() ? D2D1_POINT_2F{ Client().width - 13, 11 } : D2D1_POINT_2F{ Client().width - 26.5f, 26 }; }
const float ROUND_R = 18;
D2D1_RECT_F HistRect(int i) { float y = TB + 4 + i * HIST_H - g_histScroll; return { 8, y, SBW - 8, y + HIST_H - 4 }; }
bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
bool InCircle(D2D1_POINT_2F c, float r, D2D1_POINT_2F p) { float dx = p.x - c.x, dy = p.y - c.y; return dx * dx + dy * dy <= r * r; }

// Converter: duas linhas (de, para) com o botão da unidade à direita
float ConvMid() { return TB + (KeysTop() - 4 - TB) / 2; }
D2D1_RECT_F ConvUnitRect(int which) {
    const Cat& c = CATS[g_convCat];
    const Unit& u = c.units[which == 0 ? g_convFrom : g_convTo];
    float w = TextW(Font(14, DWRITE_FONT_WEIGHT_MEDIUM), u.sym) + 30;
    float right = Client().width - 13;
    float cy = which == 0 ? TB + (ConvMid() - TB) / 2 + 4 : ConvMid() + (KeysTop() - 4 - ConvMid()) / 2 + 2;
    return { right - w, cy - 12, right, cy + 12 };
}
D2D1_POINT_2F ConvSwapCenter() { return { Side() + 26, ConvMid() + 2 }; }

// Programador: bits (duas linhas de 32) e base
D2D1_RECT_F BitRect(int b) {
    auto c = Client();
    float left = Side() + 13, right = c.width - 13;
    int row = b >= 32 ? 0 : 1, pos = 31 - (b % 32);  // posição da esquerda para a direita
    float gw = (right - left) / 8, cw = (gw - 12) / 4;
    float x = left + (pos / 4) * gw + (pos % 4) * cw, y = 114 + row * 32;
    return { x, y, x + cw, y + 18 };
}
D2D1_RECT_F BaseRect(int i) { float x = Side() + 13 + i * 40; return { x, 180, x + 36, 200 }; }

// ---------------------------------------------------------------------------
// teclas de cada modo
void BuildKeys() {
    g_keys.clear();
    auto add = [](int id, int c, int r, int s) { g_keys.push_back({ id, c, r, s }); };
    auto keypad = [&](int c0) {
        add(K_DEL, c0, 0, S_TOP); add(K_AC, c0 + 1, 0, S_TOP); add(g_mode == M_PROG ? K_PCT : K_PCT, c0 + 2, 0, S_TOP); add(K_DIV, c0 + 3, 0, S_OP);
        add(7, c0, 1, S_NUM); add(8, c0 + 1, 1, S_NUM); add(9, c0 + 2, 1, S_NUM); add(K_MUL, c0 + 3, 1, S_OP);
        add(4, c0, 2, S_NUM); add(5, c0 + 1, 2, S_NUM); add(6, c0 + 2, 2, S_NUM); add(K_SUB, c0 + 3, 2, S_OP);
        add(1, c0, 3, S_NUM); add(2, c0 + 1, 3, S_NUM); add(3, c0 + 2, 3, S_NUM); add(K_ADD, c0 + 3, 3, S_OP);
        if (g_mode == M_PROG) { add(K_BFLIP, c0, 4, S_NUM); add(0, c0 + 1, 4, S_NUM); add(K_WFLIP, c0 + 2, 4, S_NUM); }
        else { add(K_NEG, c0, 4, S_NUM); add(0, c0 + 1, 4, S_NUM); add(K_DOT, c0 + 2, 4, S_NUM); }
        add(K_EQ, c0 + 3, 4, S_OP);
    };
    if (g_mode == M_BASIC) { keypad(0); return; }
    if (g_mode == M_SCI) {
        const int fn[5][6] = {
            { K_LP, K_RP, K_MC, K_MP, K_MM, K_MR },
            { K_2ND, K_SQ, K_CUBE, K_POW, K_EXP, K_10X },
            { K_INV, K_SQRT, K_CBRT, K_YROOT, K_LN, K_LOG10 },
            { K_FACT, K_SIN, K_COS, K_TAN, K_E, K_EE },
            { K_RAND, K_SINH, K_COSH, K_TANH, K_PI, K_RAD },
        };
        for (int r = 0; r < 5; r++) for (int c = 0; c < 6; c++) add(fn[r][c], c, r, S_FN);
        keypad(6);
        return;
    }
    const int fn[5][4] = {
        { K_AND, K_OR, 14, 15 },
        { K_NOR, K_XOR, 12, 13 },
        { K_SHL, K_SHR, 10, 11 },
        { K_XSHLY, K_XSHRY, K_FF, K_00 },
        { K_2S, K_1S, K_ROL, K_ROR },
    };
    for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) add(fn[r][c], c, r, c >= 2 && fn[r][c] <= 15 ? S_NUM : S_FN);
    keypad(4);
}

bool ClearIsC() {
    if (g_mode == M_PROG) return g_pEntering && g_pEntry != 0;
    if (g_rpn) return g_entering;
    return !g_done && !g_toks.empty();
}

std::wstring KeyLabel(int id) {
    if (id >= 0 && id <= 15) { wchar_t b[2] = { (wchar_t)(id < 10 ? L'0' + id : L'A' + id - 10), 0 }; return b; }
    bool s = g_second;
    switch (id) {
    case K_DOT: return std::wstring(1, g_dec);
    case K_AC: return ClearIsC() ? L"C" : L"AC";
    case K_PCT: return g_mode == M_PROG ? L"mod" : L"%";
    case K_NEG: return L"^{+}⁄_{−}";
    case K_EQ: return g_rpn && g_mode != M_PROG ? L"enter" : L"";
    case K_LP: return g_rpn ? L"x⇄y" : L"(";
    case K_RP: return g_rpn ? L"R↓" : L")";
    case K_MC: return L"mc"; case K_MP: return L"m+"; case K_MM: return L"m-"; case K_MR: return L"mr";
    case K_2ND: return L"2^{nd}";
    case K_SQ: return L"x^{2}"; case K_CUBE: return L"x^{3}"; case K_POW: return L"x^{y}";
    case K_EXP: return s ? L"y^{x}" : L"~{e}^{x}";
    case K_10X: return s ? L"2^{x}" : L"10^{x}";
    case K_INV: return L"^{1}⁄_{x}";
    case K_SQRT: return L"^{2}√x"; case K_CBRT: return L"^{3}√x"; case K_YROOT: return L"^{y}√x";
    case K_LN: return s ? L"log_{y}" : L"ln";
    case K_LOG10: return s ? L"log_{2}" : L"log_{10}";
    case K_FACT: return L"x!";
    case K_SIN: return s ? L"sin^{−1}" : L"sin"; case K_COS: return s ? L"cos^{−1}" : L"cos"; case K_TAN: return s ? L"tan^{−1}" : L"tan";
    case K_SINH: return s ? L"sinh^{−1}" : L"sinh"; case K_COSH: return s ? L"cosh^{−1}" : L"cosh"; case K_TANH: return s ? L"tanh^{−1}" : L"tanh";
    case K_E: return L"~{e}"; case K_EE: return L"EE"; case K_RAND: return L"Rand"; case K_PI: return L"π";
    case K_RAD: return g_rad ? L"Deg" : L"Rad";
    case K_AND: return L"AND"; case K_OR: return L"OR"; case K_XOR: return L"XOR"; case K_NOR: return L"NOR";
    case K_SHL: return L"<<"; case K_SHR: return L">>"; case K_XSHLY: return L"X<<Y"; case K_XSHRY: return L"X>>Y";
    case K_ROL: return L"RoL"; case K_ROR: return L"RoR"; case K_2S: return L"2's"; case K_1S: return L"1's";
    case K_00: return L"00"; case K_FF: return L"FF"; case K_BFLIP: return L"flip₈"; case K_WFLIP: return L"flip₁₆";
    }
    return L"";
}

bool KeyEnabled(int id) {
    if (g_mode != M_PROG) return true;
    if (id >= 0 && id <= 15) return id < g_base;
    if (id == K_FF) return g_base == 16;
    return true;
}

// ---------------------------------------------------------------------------
// estado: configurações
void SaveSettings() {
    if (!g_hwnd) return;
    RECT r; GetWindowRect(g_hwnd, &r);
    float sc = Scale();
    int x = r.left + (int)std::lround(Side() * sc), y = r.top;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (DataDir() + L"\\calculadora.txt").c_str(), L"w, ccs=UTF-8") || !f) return;
    fwprintf(f, L"modo=%ls\nx=%d\ny=%d\nlarg_cientifica=%d\nalt_cientifica=%d\nrpn=%d\nconverter=%d\nhistorico=%d\nradianos=%d\nbase=%d\nconv_cat=%d\nconv_de=%d\nconv_para=%d\n",
             g_mode == M_BASIC ? L"basica" : g_mode == M_SCI ? L"cientifica" : L"programador", x, y, (int)g_sciW, (int)g_sciH,
             g_rpn, g_convert, g_sidebar, g_rad, g_base, g_convCat, g_convFrom, g_convTo);
    fclose(f);
}

// tamanho da janela para o modo (mantém o canto do conteúdo; a barra lateral cresce para a esquerda)
void ApplyLayout(int sidebarDelta = 0) {
    BuildKeys();
    float sc = Scale();
    auto cs = ContentSize();
    int w = (int)std::lround((cs.width + Side()) * sc), h = (int)std::lround(cs.height * sc);
    RECT r; GetWindowRect(g_hwnd, &r);
    int x = r.left - (int)std::lround(sidebarDelta * sc), y = r.top;
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (x + w > mi.rcWork.right) x = std::max<int>(mi.rcWork.left, mi.rcWork.right - w);
    if (y + h > mi.rcWork.bottom) y = std::max<int>(mi.rcWork.top, mi.rcWork.bottom - h);
    SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    SaveSettings();
}

// ---------------------------------------------------------------------------
// Básica / Científica: entrada da expressão
int InnerOpenIn(const std::vector<Tok>& t);
bool EndsOperand() {
    if (g_toks.empty()) return false;
    TK k = g_toks.back().k;
    if (k == T_NUM) return g_toks.back().s != L"-";
    return k == T_VAL || k == T_CONST || k == T_RP || k == T_POST;
}
void FromAns() {  // continua a conta a partir do resultado
    if (!g_done) return;
    g_toks.clear();
    if (std::isfinite(g_ans)) { Tok t{ T_VAL }; t.v = g_ans; g_toks.push_back(t); }
    g_done = false;
}
void Fresh() { if (g_done) { g_toks.clear(); g_done = false; } }
int InnerOpen() {
    int d = 0;
    for (int i = (int)g_toks.size() - 1; i >= 0; i--) {
        if (g_toks[i].k == T_RP) d++;
        else if (g_toks[i].k == T_LP) { if (d == 0) return i; d--; }
    }
    return -1;
}
void CloseHidden() {  // o expoente (xʸ) termina quando vem um operador
    int i = InnerOpen();
    if (i >= 0 && g_toks[i].hidden && EndsOperand()) { Tok t{ T_RP, L")" }; t.hidden = true; g_toks.push_back(t); }
}
size_t OperandStart(size_t end) {
    size_t i = end;
    while (i > 0 && g_toks[i - 1].k == T_POST) i--;
    if (i == 0) return 0;
    i--;
    if (g_toks[i].k == T_RP) {
        int d = 0;
        for (;; i--) {
            if (g_toks[i].k == T_RP) d++;
            else if (g_toks[i].k == T_LP && --d == 0) break;
            if (i == 0) break;
        }
        if (i > 0 && g_toks[i - 1].k == T_FUNC) i--;
    }
    if (i > 0 && (g_toks[i - 1].k == T_POW || g_toks[i - 1].k == T_YROOT || g_toks[i - 1].k == T_LOGY)) return OperandStart(i - 1);
    return i;
}
// envolve o último operando: 30 sin → sin(30), 2 eˣ → e^(2)
void Wrap(std::vector<Tok> before, bool hidden) {
    size_t s = OperandStart(g_toks.size());
    Tok lp{ T_LP, L"(" }; lp.hidden = hidden;
    Tok rp{ T_RP, L")" }; rp.hidden = hidden;
    before.push_back(lp);
    g_toks.insert(g_toks.begin() + s, before.begin(), before.end());
    g_toks.push_back(rp);
}
Tok MakeTok(TK k, const std::wstring& s = L"", double v = 0, bool hidden = false) { Tok t{ k, s }; t.v = v; t.hidden = hidden; return t; }

std::wstring FuncName(int id) {
    bool s = g_second;
    switch (id) {
    case K_SIN: return s ? L"asin" : L"sin"; case K_COS: return s ? L"acos" : L"cos"; case K_TAN: return s ? L"atan" : L"tan";
    case K_SINH: return s ? L"asinh" : L"sinh"; case K_COSH: return s ? L"acosh" : L"cosh"; case K_TANH: return s ? L"atanh" : L"tanh";
    case K_LN: return L"ln"; case K_LOG10: return s ? L"log2" : L"log10";
    case K_SQRT: return L"sqrt"; case K_CBRT: return L"cbrt";
    }
    return L"";
}

void AddHistory(const std::vector<Tok>& expr, double val) {
    g_hist.insert(g_hist.begin(), { expr, val });
    if (g_hist.size() > 200) g_hist.resize(200);
    g_histScroll = 0;
    SaveHistory();
}

void StdEquals() {
    if (g_done) {  // repete a última operação: 2 + 3 = = → 8
        if (!g_repOk || !std::isfinite(g_ans)) return;
        std::vector<Tok> e = { MakeTok(T_VAL, L"", g_ans), MakeTok(T_OP, g_repOp), MakeTok(T_VAL, L"", g_repVal) };
        g_ans = Bin(g_repOp, g_ans, g_repVal);
        g_doneExpr = e;
        AddHistory(e, g_ans);
        return;
    }
    if (g_toks.empty()) return;
    double v = NAN;
    bool ok = Eval(g_toks, v);
    // a última operação de nível 0 ("+ 3") para repetir
    g_repOk = false;
    int d = 0;
    for (int i = (int)g_toks.size() - 1; i >= 0; i--) {
        if (g_toks[i].k == T_RP) d++;
        else if (g_toks[i].k == T_LP) d--;
        else if (d == 0 && g_toks[i].k == T_OP && i > 0) {
            std::vector<Tok> rest(g_toks.begin() + i + 1, g_toks.end());
            double rv;
            if (Eval(rest, rv)) { g_repOk = true; g_repOp = g_toks[i].s; g_repVal = rv; }
            break;
        }
    }
    std::vector<Tok> e = g_toks;
    while (!e.empty() && (e.back().k == T_OP || e.back().k == T_NEG)) e.pop_back();
    for (int i = InnerOpenIn(e); i >= 0; i = InnerOpenIn(e)) { Tok rp{ T_RP, L")" }; rp.hidden = e[i].hidden; e.push_back(rp); }
    g_doneExpr = e;
    g_ans = ok ? v : NAN;
    g_done = true;
    g_toks.clear();
    AddHistory(e, g_ans);
}

void StdPress(int id) {
    if (id >= 0 && id <= 9) {
        Fresh();
        wchar_t d = (wchar_t)(L'0' + id);
        if (!g_toks.empty() && g_toks.back().k == T_NUM) {
            std::wstring& s = g_toks.back().s;
            int digits = 0; for (wchar_t c : s) if (iswdigit(c)) digits++;
            if (digits >= 16) return;
            if (s == L"0") s = d; else if (s == L"-0") s = std::wstring(L"-") + d; else s += d;
        } else g_toks.push_back(MakeTok(T_NUM, std::wstring(1, d)));
        return;
    }
    switch (id) {
    case K_DOT:
        Fresh();
        if (!g_toks.empty() && g_toks.back().k == T_NUM) {
            std::wstring& s = g_toks.back().s;
            if (s.find(L'.') == std::wstring::npos && s.find(L'e') == std::wstring::npos) s += L'.';
        } else g_toks.push_back(MakeTok(T_NUM, L"0."));
        return;
    case K_EE:
        if (!g_toks.empty() && g_toks.back().k == T_NUM && g_toks.back().s.find(L'e') == std::wstring::npos) g_toks.back().s += L'e';
        return;
    case K_ADD: case K_SUB: case K_MUL: case K_DIV: {
        const wchar_t* op = id == K_ADD ? L"+" : id == K_SUB ? L"−" : id == K_MUL ? L"×" : L"÷";
        FromAns();
        if (g_toks.empty()) {
            if (id == K_SUB) { g_toks.push_back(MakeTok(T_NEG)); return; }
            g_toks.push_back(MakeTok(T_NUM, L"0"));
        }
        Tok& b = g_toks.back();
        if (b.k == T_NUM && !b.s.empty() && b.s.back() == L'e') { if (id == K_SUB) b.s += L'-'; return; }
        if (b.k == T_OP) { b.s = op; return; }
        if (b.k == T_NEG) { if (id != K_SUB) g_toks.pop_back(); return; }
        if (b.k == T_LP || b.k == T_POW || b.k == T_YROOT || b.k == T_LOGY || b.k == T_FUNC) { if (id == K_SUB) g_toks.push_back(MakeTok(T_NEG)); return; }
        CloseHidden();
        g_toks.push_back(MakeTok(T_OP, op));
        return;
    }
    case K_EQ: StdEquals(); return;
    case K_AC:
        if (ClearIsC()) { g_toks.clear(); return; }
        g_toks.clear(); g_done = false; g_ans = 0; g_doneExpr.clear(); g_repOk = false;
        return;
    case K_DEL: {
        if (g_done) {
            if (!std::isfinite(g_ans)) { g_done = false; g_toks.clear(); return; }
            std::wstring s = PlainNum(g_ans);
            for (auto& c : s) if (c == g_dec) c = L'.';
            if (s.find(L'e') != std::wstring::npos) return;
            g_toks = { MakeTok(T_NUM, s) };
            g_done = false;
        }
        if (g_toks.empty()) return;
        Tok& b = g_toks.back();
        if (b.k == T_NUM) {
            b.s.pop_back();
            if (b.s.empty() || b.s == L"-") g_toks.pop_back();
        } else if (b.k == T_LP) {
            bool hid = b.hidden;
            g_toks.pop_back();
            if (!g_toks.empty() && (g_toks.back().k == T_FUNC || (hid && g_toks.back().k == T_POW))) g_toks.pop_back();
        } else g_toks.pop_back();
        return;
    }
    case K_PCT: case K_SQ: case K_CUBE: case K_INV: case K_FACT: {
        FromAns();
        if (!EndsOperand()) return;
        const wchar_t* p = id == K_PCT ? L"%" : id == K_SQ ? L"²" : id == K_CUBE ? L"³" : id == K_INV ? L"⁻¹" : L"!";
        g_toks.push_back(MakeTok(T_POST, p));
        return;
    }
    case K_NEG: {
        FromAns();
        if (g_toks.empty()) { g_toks.push_back(MakeTok(T_NEG)); return; }
        Tok& b = g_toks.back();
        if (b.k == T_NUM && b.s.find(L'e') != std::wstring::npos) {  // sinal do expoente
            size_t e = b.s.find(L'e');
            if (e + 1 < b.s.size() && b.s[e + 1] == L'-') b.s.erase(e + 1, 1); else b.s.insert(e + 1, L"-");
            return;
        }
        if (!EndsOperand()) { if (b.k == T_NEG) g_toks.pop_back(); else g_toks.push_back(MakeTok(T_NEG)); return; }
        if (b.k == T_VAL) { b.v = -b.v; return; }
        size_t s = OperandStart(g_toks.size());
        if (s > 0 && g_toks[s - 1].k == T_NEG) g_toks.erase(g_toks.begin() + s - 1);
        else g_toks.insert(g_toks.begin() + s, MakeTok(T_NEG));
        return;
    }
    case K_LP: Fresh(); g_toks.push_back(MakeTok(T_LP, L"(")); return;
    case K_RP: {
        int i = InnerOpen();
        if (i < 0 || !EndsOperand()) return;
        g_toks.push_back(MakeTok(T_RP, L")", 0, g_toks[i].hidden));
        return;
    }
    case K_SIN: case K_COS: case K_TAN: case K_SINH: case K_COSH: case K_TANH: case K_LN: case K_LOG10: case K_SQRT: case K_CBRT: {
        if (id == K_LN && g_second) {  // log_y: x log_y y
            FromAns();
            if (EndsOperand()) g_toks.push_back(MakeTok(T_LOGY));
            return;
        }
        FromAns();
        Tok f = MakeTok(T_FUNC, FuncName(id));
        if (EndsOperand()) Wrap({ f }, false);
        else { g_toks.push_back(f); g_toks.push_back(MakeTok(T_LP, L"(")); }
        return;
    }
    case K_POW: FromAns(); if (EndsOperand()) { g_toks.push_back(MakeTok(T_POW)); g_toks.push_back(MakeTok(T_LP, L"(", 0, true)); } return;
    case K_YROOT: FromAns(); if (EndsOperand()) g_toks.push_back(MakeTok(T_YROOT)); return;
    case K_EXP: case K_10X: {
        if (id == K_EXP && g_second) { FromAns(); if (EndsOperand()) { g_toks.push_back(MakeTok(T_POW)); g_toks.push_back(MakeTok(T_LP, L"(", 0, true)); } return; }
        Tok base = id == K_EXP ? MakeTok(T_CONST, L"e") : MakeTok(T_VAL, L"", g_second ? 2 : 10);
        FromAns();
        if (EndsOperand()) Wrap({ base, MakeTok(T_POW) }, true);
        else { g_toks.push_back(base); g_toks.push_back(MakeTok(T_POW)); g_toks.push_back(MakeTok(T_LP, L"(", 0, true)); }
        return;
    }
    case K_PI: Fresh(); g_toks.push_back(MakeTok(T_CONST, L"π")); return;
    case K_E: Fresh(); g_toks.push_back(MakeTok(T_CONST, L"e")); return;
    case K_RAND: Fresh(); g_toks.push_back(MakeTok(T_VAL, L"", std::uniform_real_distribution<double>(0, 1)(g_rng))); return;
    case K_MC: g_mem = 0; return;
    case K_MP: g_mem += CurrentValue(); return;
    case K_MM: g_mem -= CurrentValue(); return;
    case K_MR: Fresh(); g_toks.push_back(MakeTok(T_VAL, L"", g_mem)); return;
    }
}
int InnerOpenIn(const std::vector<Tok>& t);

// ---------------------------------------------------------------------------
// RPN
void Commit() {
    if (!g_entering) return;
    g_stack.push_back(wcstod(g_entry.c_str(), nullptr));
    g_entry.clear();
    g_entering = false;
}
void RpnPress(int id) {
    if ((id >= 0 && id <= 9) || id == K_DOT || id == K_EE) {
        if (!g_entering) { g_entering = true; g_entry.clear(); }
        if (id == K_DOT) { if (g_entry.find(L'.') == std::wstring::npos && g_entry.find(L'e') == std::wstring::npos) g_entry += g_entry.empty() ? L"0." : L"."; }
        else if (id == K_EE) { if (!g_entry.empty() && g_entry.find(L'e') == std::wstring::npos) g_entry += L'e'; }
        else if (g_entry.size() < 18) { if (g_entry == L"0") g_entry.clear(); g_entry += (wchar_t)(L'0' + id); }
        return;
    }
    auto bin = [&](const wchar_t* op) {
        Commit();
        if (g_stack.size() < 2) return;
        double x = g_stack.back(); g_stack.pop_back();
        double y = g_stack.back(); g_stack.pop_back();
        g_stack.push_back(Bin(op, y, x));
    };
    auto un = [&](const std::wstring& f) {
        Commit();
        if (g_stack.empty()) return;
        g_stack.back() = Fn(f, g_stack.back());
    };
    auto push = [&](double v) { Commit(); g_stack.push_back(v); };
    switch (id) {
    case K_EQ: if (g_entering) Commit(); else if (!g_stack.empty()) g_stack.push_back(g_stack.back()); return;
    case K_ADD: bin(L"+"); return; case K_SUB: bin(L"−"); return; case K_MUL: bin(L"×"); return; case K_DIV: bin(L"÷"); return;
    case K_POW: bin(L"^"); return; case K_YROOT: bin(L"√"); return;
    case K_SQ: un(L"²"); return; case K_CUBE: un(L"³"); return; case K_INV: un(L"⁻¹"); return; case K_FACT: un(L"!"); return;
    case K_EXP: if (g_second) bin(L"^"); else un(L"exp"); return;
    case K_10X: un(g_second ? L"exp2" : L"exp10"); return;
    case K_LN: if (g_second) bin(L"log"); else un(L"ln"); return;
    case K_SIN: case K_COS: case K_TAN: case K_SINH: case K_COSH: case K_TANH: case K_LOG10: case K_SQRT: case K_CBRT: un(FuncName(id)); return;
    case K_PCT:
        Commit();
        if (g_stack.size() >= 2) g_stack.back() = g_stack[g_stack.size() - 2] * g_stack.back() / 100;
        else if (!g_stack.empty()) g_stack.back() /= 100;
        return;
    case K_NEG:
        if (g_entering) {
            size_t e = g_entry.find(L'e');
            size_t at = e == std::wstring::npos ? 0 : e + 1;
            if (at < g_entry.size() && g_entry[at] == L'-') g_entry.erase(at, 1); else g_entry.insert(at, L"-");
        } else if (!g_stack.empty()) g_stack.back() = -g_stack.back();
        return;
    case K_DEL:
        if (g_entering) { if (!g_entry.empty()) g_entry.pop_back(); if (g_entry.empty() || g_entry == L"-") { g_entry.clear(); g_entering = false; } }
        else if (!g_stack.empty()) g_stack.pop_back();
        return;
    case K_AC: if (g_entering) { g_entry.clear(); g_entering = false; } else g_stack.clear(); return;
    case K_LP: Commit(); if (g_stack.size() >= 2) std::swap(g_stack[g_stack.size() - 1], g_stack[g_stack.size() - 2]); return;
    case K_RP: Commit(); if (g_stack.size() >= 2) { double x = g_stack.back(); g_stack.pop_back(); g_stack.insert(g_stack.begin(), x); } return;
    case K_PI: push(PI); return; case K_E: push(E); return;
    case K_RAND: push(std::uniform_real_distribution<double>(0, 1)(g_rng)); return;
    case K_MC: g_mem = 0; return;
    case K_MP: g_mem += CurrentValue(); return;
    case K_MM: g_mem -= CurrentValue(); return;
    case K_MR: push(g_mem); return;
    }
}

// ---------------------------------------------------------------------------
// Programador (inteiros de 64 bits)
uint64_t ProgCur() { return g_pEntering ? g_pEntry : g_pAcc; }
void ProgSet(uint64_t v) { if (g_pEntering || g_pOp) { g_pEntry = v; g_pEntering = true; } else g_pAcc = v; }
uint64_t ProgApply(uint64_t a, int op, uint64_t b) {
    switch (op) {
    case K_ADD: return a + b; case K_SUB: return a - b; case K_MUL: return a * b;
    case K_DIV: if (!b) { g_pErr = true; return 0; } return (uint64_t)((int64_t)a / (int64_t)b);
    case K_PCT: if (!b) { g_pErr = true; return 0; } return (uint64_t)((int64_t)a % (int64_t)b);
    case K_AND: return a & b; case K_OR: return a | b; case K_XOR: return a ^ b; case K_NOR: return ~(a | b);
    case K_XSHLY: return a << (b & 63); case K_XSHRY: return a >> (b & 63);
    }
    return b;
}
std::wstring ProgFmt(uint64_t v, int base) {
    if (base == 10) {
        int64_t s = (int64_t)v;
        std::wstring d = s < 0 ? std::to_wstring(0 - (uint64_t)s) : std::to_wstring(v);
        return (s < 0 ? L"−" : L"") + Group(d);
    }
    if (!v) return L"0";
    std::wstring o;
    while (v) { int d = (int)(v % base); o.insert(o.begin(), (wchar_t)(d < 10 ? L'0' + d : L'A' + d - 10)); v /= base; }
    return o;
}
const wchar_t* ProgOpSym(int op) {
    switch (op) {
    case K_ADD: return L"+"; case K_SUB: return L"−"; case K_MUL: return L"×"; case K_DIV: return L"÷"; case K_PCT: return L"mod";
    case K_AND: return L"AND"; case K_OR: return L"OR"; case K_XOR: return L"XOR"; case K_NOR: return L"NOR";
    case K_XSHLY: return L"<<"; case K_XSHRY: return L">>";
    }
    return L"";
}
void ProgPress(int id) {
    if (id >= 0 && id <= 15) {
        if (id >= g_base) return;
        if (!g_pEntering) { g_pEntry = 0; g_pEntering = true; g_pErr = false; }
        g_pEntry = g_pEntry * g_base + id;
        return;
    }
    switch (id) {
    case K_00: ProgPress(0); ProgPress(0); return;
    case K_FF: if (g_base == 16) { ProgPress(15); ProgPress(15); } return;
    case K_ADD: case K_SUB: case K_MUL: case K_DIV: case K_PCT: case K_AND: case K_OR: case K_XOR: case K_NOR: case K_XSHLY: case K_XSHRY:
        if (g_pOp && g_pEntering) g_pAcc = ProgApply(g_pAcc, g_pOp, g_pEntry);
        else if (!g_pOp && g_pEntering) g_pAcc = g_pEntry;
        g_pOp = id; g_pEntering = false;
        return;
    case K_EQ:
        if (g_pOp) { g_pAcc = ProgApply(g_pAcc, g_pOp, g_pEntering ? g_pEntry : g_pAcc); g_pOp = 0; }
        else if (g_pEntering) g_pAcc = g_pEntry;
        g_pEntering = false;
        return;
    case K_AC:
        if (ClearIsC()) { g_pEntry = 0; return; }
        g_pAcc = g_pEntry = 0; g_pOp = 0; g_pEntering = false; g_pErr = false;
        return;
    case K_DEL: if (g_pEntering) g_pEntry /= g_base; return;
    case K_SHL: ProgSet(ProgCur() << 1); return;
    case K_SHR: ProgSet(ProgCur() >> 1); return;
    case K_ROL: { uint64_t v = ProgCur(); ProgSet((v << 1) | (v >> 63)); return; }
    case K_ROR: { uint64_t v = ProgCur(); ProgSet((v >> 1) | (v << 63)); return; }
    case K_2S: ProgSet(0 - ProgCur()); return;
    case K_1S: ProgSet(~ProgCur()); return;
    case K_BFLIP: ProgSet(_byteswap_uint64(ProgCur())); return;
    case K_WFLIP: { uint64_t v = ProgCur(); ProgSet((v >> 48) | ((v >> 16) & 0xFFFF0000ull) | ((v << 16) & 0xFFFF00000000ull) | (v << 48)); return; }
    }
}

void Press(int id) {
    if (id == K_2ND) { g_second = !g_second; return; }
    if (id == K_RAD) { g_rad = !g_rad; SaveSettings(); return; }
    if (g_mode == M_PROG) ProgPress(id);
    else if (g_rpn) RpnPress(id);
    else StdPress(id);
}

int InnerOpenIn(const std::vector<Tok>& t) {
    int d = 0;
    for (int i = (int)t.size() - 1; i >= 0; i--) {
        if (t[i].k == T_RP) d++;
        else if (t[i].k == T_LP) { if (d == 0) return i; d--; }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// cores
D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) { return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t); }
struct Pal { D2D1_COLOR_F bg, key[4], text, opText, sub, round, roundBorder, icon, sidebar, line, rim, hover, disabled; };
Pal Colors() {
    // Leopard (Calculadora do 10.5/10.6): corpo de metal cinza, teclas em pílula perolada com texto preto, visor amarelo
    if (semaforo::Leopard())
        return { Rgb(196, 196, 196), { Rgb(250, 250, 250), Rgb(250, 250, 250), Rgb(250, 250, 250), Rgb(250, 250, 250) }, Rgb(0, 0, 0), Rgb(0, 0, 0),
                 Rgb(0, 0, 0, 0.55f), Rgb(240, 240, 240), Rgb(0, 0, 0, 0.35f), Rgb(40, 40, 40), Rgb(214, 221, 229), Rgb(0, 0, 0, 0.15f),
                 Rgb(0, 0, 0, 0.10f), Rgb(0, 0, 0, 0.06f), Rgb(0, 0, 0, 0.30f) };
    if (g_dark) return { Rgb(36, 41, 45), { Rgb(48, 52, 55), Rgb(70, 74, 76), Rgb(113, 117, 119), Rgb(255, 139, 0) }, Rgb(236, 237, 237), Rgb(255, 255, 255),
                         Rgb(255, 255, 255, 0.52f), Rgb(25, 29, 32), Rgb(255, 255, 255, 0.07f), Rgb(233, 234, 234), Rgb(29, 33, 36), Rgb(255, 255, 255, 0.09f),
                         Rgb(255, 255, 255, 0.06f), Rgb(255, 255, 255, 0.07f), Rgb(255, 255, 255, 0.28f) };
    return { Rgb(232, 233, 235), { Rgb(246, 246, 247), Rgb(255, 255, 255), Rgb(206, 208, 211), Rgb(255, 139, 0) }, Rgb(29, 29, 31), Rgb(255, 255, 255),
             Rgb(0, 0, 0, 0.45f), Rgb(250, 250, 251), Rgb(0, 0, 0, 0.08f), Rgb(60, 60, 64), Rgb(222, 223, 226), Rgb(0, 0, 0, 0.09f),
             Rgb(0, 0, 0, 0.07f), Rgb(0, 0, 0, 0.05f), Rgb(0, 0, 0, 0.25f) };
}

// ---------------------------------------------------------------------------
// desenho
void Line(ID2D1RenderTarget* rt, ID2D1Brush* b, float x1, float y1, float x2, float y2, float w) { rt->DrawLine({ x1, y1 }, { x2, y2 }, b, w, g_round); }

void DrawLights(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br) {
    static const D2D1_COLOR_F fill_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.345f), D2D1::ColorF(0.996f, 0.737f, 0.180f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
    static const D2D1_COLOR_F ring_c[3] = { D2D1::ColorF(0.886f, 0.275f, 0.247f), D2D1::ColorF(0.882f, 0.631f, 0.086f), D2D1::ColorF(0.071f, 0.675f, 0.157f) }; const D2D1_COLOR_F* ring = semaforo::Pick(ring_c, semaforo::RING);
    static const D2D1_COLOR_F glyph_c[3] = { D2D1::ColorF(0.302f, 0, 0), D2D1::ColorF(0.6f, 0.341f, 0), D2D1::ColorF(0, 0.396f, 0) }; const D2D1_COLOR_F* glyph = semaforo::Pick(glyph_c, semaforo::GLYPH);
    bool lit = g_active || g_hoverLights;
    D2D1_COLOR_F off = g_dark ? Rgb(71, 76, 80) : Rgb(208, 208, 210), offRing = g_dark ? D2D1::ColorF(0, 0, 0, 0.3f) : Rgb(186, 186, 189);
    for (int k = 0; k < 3; k++) {
        auto c = LightCenter(k);
        bool enabled = k < 2;  // a Calculadora não amplia (o verde fica apagado, como no Mac)
        br->SetColor(lit && enabled ? fill[k] : off);
        rt->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
        semaforo::Gloss(rt, c, 7, br);
        br->SetColor(lit && enabled ? ring[k] : offRing);
        rt->DrawEllipse(D2D1::Ellipse(c, 6.75f, 6.75f), br, 0.5f);
        if (!g_hoverLights || !enabled) continue;
        br->SetColor(glyph[k]);
        if (k == 0) { Line(rt, br, c.x - 2.5f, c.y - 2.5f, c.x + 2.5f, c.y + 2.5f, 1.1f); Line(rt, br, c.x - 2.5f, c.y + 2.5f, c.x + 2.5f, c.y - 2.5f, 1.1f); }
        else Line(rt, br, c.x - 3.3f, c.y, c.x + 3.3f, c.y, 1.2f);
    }
}

// Brilho de vidro na borda (medido na referência): a luz acende os arcos de cima-esquerda e de
// baixo-direita e some nos de cima-direita e baixo-esquerda; no laranja o brilho puxa para amarelo.
// Um gradiente linear do canto de cima-direita ao de baixo-esquerda, claro no meio, faz exatamente isso.
// kind: 0 dígito/função, 1 linha de cima (mais clara), 2 operador laranja, 3 botão redondo da barra
void GlassRim(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, int kind) {
    D2D1_COLOR_F c = kind == 2 ? Rgb(255, 214, 10) : g_dark ? Rgb(255, 255, 255) : Rgb(255, 255, 255);
    float peak = kind == 2 ? 0.85f : g_dark ? (kind == 1 ? 0.42f : kind == 3 ? 0.2f : 0.3f) : 0.9f;
    float lo = kind == 2 ? 0.05f : g_dark ? 0.0f : 0.2f;
    auto A = [&](float a) { D2D1_COLOR_F x = c; x.a = a; return x; };
    D2D1_GRADIENT_STOP st[5] = { { 0, A(lo) }, { 0.25f, A(peak * 0.55f) }, { 0.5f, A(peak) }, { 0.75f, A(peak * 0.55f) }, { 1, A(lo) } };
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(rt->CreateGradientStopCollection(st, 5, &sc))) return;
    // os cantos do quadrado que contém a tecla (para pílulas, os do círculo de cada ponta ficam parecidos)
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, hw = (r.right - r.left) / 2, hh = (r.bottom - r.top) / 2;
    ID2D1LinearGradientBrush* gb = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ cx + hw, cy - hh }, { cx - hw, cy + hh }), sc, &gb);
    sc->Release();
    if (!gb) return;
    float w = 1.3f;
    rt->DrawRoundedRectangle({ { r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2 }, rad - w / 2, rad - w / 2 }, gb, w);
    gb->Release();
    if (!g_dark && kind != 2) {  // no claro, um contorno leve para a tecla branca não sumir no fundo
        ID2D1SolidColorBrush* b = nullptr;
        rt->CreateSolidColorBrush(Rgb(0, 0, 0, 0.07f), &b);
        if (b) { rt->DrawRoundedRectangle({ { r.left - 0.25f, r.top - 0.25f, r.right + 0.25f, r.bottom + 0.25f }, rad + 0.25f, rad + 0.25f }, b, 0.6f); b->Release(); }
    }
}

void DrawRoundButtons(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    for (int which = 0; which < 2; which++) {
        auto c = which == 0 ? SideBtnCenter() : ModeBtnCenter();
        bool hov = g_hover.kind == (which == 0 ? TG_SIDEBTN : TG_MODEBTN);
        bool on = which == 0 && g_sidebar;
        D2D1_MATRIX_3X2_F oldT; rt->GetTransform(&oldT);
        if (semaforo::Leopard()) rt->SetTransform(D2D1::Matrix3x2F::Scale(0.5f, 0.5f, c) * oldT);
        br->SetColor(hov || on ? Mix(pal.round, g_dark ? Rgb(255, 255, 255) : Rgb(0, 0, 0), g_dark ? 0.1f : 0.06f) : pal.round);
        rt->FillEllipse(D2D1::Ellipse(c, ROUND_R, ROUND_R), br);
        GlassRim(rt, { c.x - ROUND_R, c.y - ROUND_R, c.x + ROUND_R, c.y + ROUND_R }, ROUND_R, 3);
        br->SetColor(pal.icon);
        if (which == 0) {  // sidebar.left
            D2D1_ROUNDED_RECT r{ { c.x - 9.5f, c.y - 7.2f, c.x + 9.5f, c.y + 7.2f }, 3, 3 };
            rt->DrawRoundedRectangle(r, br, 1.4f);
            Line(rt, br, c.x - 3.2f, c.y - 7.2f, c.x - 3.2f, c.y + 7.2f, 1.4f);
            for (int i = 0; i < 3; i++) Line(rt, br, c.x - 7.4f, c.y - 3.6f + i * 2.6f, c.x - 5.4f, c.y - 3.6f + i * 2.6f, 1.1f);
        } else {  // calculator
            D2D1_ROUNDED_RECT r{ { c.x - 5.8f, c.y - 8.2f, c.x + 5.8f, c.y + 8.2f }, 2.2f, 2.2f };
            rt->FillRoundedRectangle(r, br);
            br->SetColor(hov ? Mix(pal.round, g_dark ? Rgb(255, 255, 255) : Rgb(0, 0, 0), g_dark ? 0.1f : 0.06f) : pal.round);
            rt->FillRectangle({ c.x - 3.9f, c.y - 6.3f, c.x + 3.9f, c.y - 3.3f }, br);
            for (int yy = 0; yy < 4; yy++) for (int xx = 0; xx < 3; xx++) {
                float x = c.x - 3.9f + xx * 2.9f, y = c.y - 1.6f + yy * 2.5f;
                rt->FillRectangle({ x, y, x + 1.9f, y + 1.6f }, br);
            }
        }
        rt->SetTransform(oldT);
    }
}

// ÷ × − + = e ⌫ desenhados (SF Symbols divide, multiply, minus, plus, equal, delete.left)
void DrawGlyph(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, int id, D2D1_RECT_F r) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, h = r.bottom - r.top;
    float s = std::min(h * 0.36f, 18.f), w = g_mode == M_BASIC ? 1.6f : 1.4f;
    switch (id) {
    case K_ADD: Line(rt, br, cx - s / 2, cy, cx + s / 2, cy, w); Line(rt, br, cx, cy - s / 2, cx, cy + s / 2, w); break;
    case K_SUB: Line(rt, br, cx - s / 2, cy, cx + s / 2, cy, w); break;
    case K_MUL: { float d = s * 0.36f; Line(rt, br, cx - d, cy - d, cx + d, cy + d, w); Line(rt, br, cx - d, cy + d, cx + d, cy - d, w); break; }
    case K_DIV:
        Line(rt, br, cx - s / 2, cy, cx + s / 2, cy, w);
        rt->FillEllipse(D2D1::Ellipse({ cx, cy - s * 0.32f }, w, w), br);
        rt->FillEllipse(D2D1::Ellipse({ cx, cy + s * 0.32f }, w, w), br);
        break;
    case K_EQ: Line(rt, br, cx - s / 2, cy - s * 0.19f, cx + s / 2, cy - s * 0.19f, w); Line(rt, br, cx - s / 2, cy + s * 0.19f, cx + s / 2, cy + s * 0.19f, w); break;
    case K_DEL: {
        float gw = g_mode == M_BASIC ? 22 : 19, gh = gw * 0.72f;
        float l = cx - gw / 2, t = cy - gh / 2, rr = cx + gw / 2, b = cy + gh / 2, tip = l + gh * 0.5f;
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(g_f->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* sk = nullptr;
            g->Open(&sk);
            float rad = 2.5f;
            sk->BeginFigure({ l, cy }, D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine({ tip, t });
            sk->AddLine({ rr - rad, t });
            sk->AddArc(D2D1::ArcSegment({ rr, t + rad }, { rad, rad }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine({ rr, b - rad });
            sk->AddArc(D2D1::ArcSegment({ rr - rad, b }, { rad, rad }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine({ tip, b });
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close(); sk->Release();
            ID2D1StrokeStyle* st = nullptr;
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            g_f->CreateStrokeStyle(sp, nullptr, 0, &st);
            rt->DrawGeometry(g, br, 1.6f, st);
            if (st) st->Release();
            g->Release();
        }
        float xc = (tip + rr) / 2 + 1, d = gh * 0.2f;
        Line(rt, br, xc - d, cy - d, xc + d, cy + d, 1.6f);
        Line(rt, br, xc - d, cy + d, xc + d, cy - d, 1.6f);
        break;
    }
    }
}

void DrawKeys(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    for (size_t i = 0; i < g_keys.size(); i++) {
        const KeyDef& k = g_keys[i];
        D2D1_RECT_F r = KeyRect(k);
        float rad = std::min(r.right - r.left, r.bottom - r.top) / 2;
        int style = k.style;
        if (k.id == K_2ND && g_second) style = S_TOP;
        if (g_mode == M_PROG && k.id == K_EQ) style = S_OP;
        D2D1_COLOR_F c = pal.key[style];
        bool en = KeyEnabled(k.id);
        auto gl = g_glow.find(k.id);
        float glow = gl != g_glow.end() ? gl->second : 0;
        bool hov = g_hover.kind == TG_KEY && g_hover.idx == (int)i && en;
        D2D1_COLOR_F toward = (g_dark || style == S_OP) ? Rgb(255, 255, 255) : Rgb(0, 0, 0);
        if (hov) c = Mix(c, toward, style == S_OP ? 0.1f : g_dark ? 0.07f : 0.04f);
        if (glow > 0) c = Mix(c, toward, glow * (style == S_OP ? 0.4f : g_dark ? 0.32f : 0.14f));
        if (semaforo::Leopard()) {
            // Leopard: tecla de canto 5 com o degradê perolado (branco em cima, cinza no meio) e o contorno cinza;
            // apertada ou acesa pelo teclado, azul do Aqua
            float cy0 = (r.top + r.bottom) / 2, hh = std::min(r.bottom - r.top - 4, k.id == K_EQ && r.bottom - r.top > 60 ? 999.f : 26.f);
            D2D1_RECT_F q{ r.left + 2, cy0 - hh / 2, r.right - 2, cy0 + hh / 2 };
            bool blue = glow > 0.3f;
            D2D1_GRADIENT_STOP st[4] = { { 0, blue ? Rgb(170, 205, 250) : Rgb(255, 255, 255) }, { 0.5f, blue ? Rgb(100, 155, 235) : Rgb(242, 242, 242) },
                                         { 0.5f, blue ? Rgb(60, 120, 222) : Rgb(226, 226, 226) }, { 1, blue ? Rgb(150, 205, 255) : Rgb(248, 248, 248) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            br->SetColor(Rgb(255, 255, 255, 0.5f));
            rt->FillRoundedRectangle({ { q.left, q.top + 1, q.right, q.bottom + 1 }, 5, 5 }, br);
            if (SUCCEEDED(rt->CreateGradientStopCollection(st, 4, &gs)) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, q.top }, { 0, q.bottom }), gs, &lb)))
                rt->FillRoundedRectangle({ q, 5, 5 }, lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            br->SetColor(hov ? Rgb(0, 0, 0, 0.55f) : Rgb(0, 0, 0, 0.38f));
            rt->DrawRoundedRectangle({ { q.left + 0.5f, q.top + 0.5f, q.right - 0.5f, q.bottom - 0.5f }, 4.5f, 4.5f }, br, 1);
            r = q;
        } else {
        br->SetColor(c);
        D2D1_ROUNDED_RECT rr{ r, rad, rad };
        rt->FillRoundedRectangle(rr, br);
        GlassRim(rt, r, rad, style == S_OP ? 2 : style == S_TOP ? 1 : 0);
        }
        D2D1_COLOR_F tc = style == S_OP ? pal.opText : (style == S_TOP && !g_dark ? pal.text : pal.text);
        if (!en) tc = pal.disabled;
        br->SetColor(tc);
        if (k.id == K_ADD || k.id == K_SUB || k.id == K_MUL || k.id == K_DIV || (k.id == K_EQ && KeyLabel(K_EQ).empty()) || k.id == K_DEL) {
            DrawGlyph(rt, br, k.id, r);
            continue;
        }
        std::wstring lab = KeyLabel(k.id);
        std::vector<Run> rs = Markup(lab);
        bool digit = (k.id >= 0 && k.id <= 15) || k.id == K_DOT;
        float size;
        if (g_mode == M_BASIC) size = digit ? 23 : 20;
        else if (g_mode == M_SCI) size = digit ? 19 : (lab.size() > 4 && lab.find(L'{') == std::wstring::npos ? 15 : 16.5f);
        else size = digit ? 19 : lab.size() > 3 ? 14 : 16;
        if (k.id == K_NEG) size = g_mode == M_BASIC ? 22 : 19;
        if (k.id == K_PCT && g_mode == M_BASIC) size = 24;
        if (semaforo::Leopard()) size = std::min(size, 14.f);  // Leopard: o texto pequeno das teclas do 10.5
        size = std::min(size, (r.bottom - r.top) * 0.5f);
        float w = RunsWidth(rs, size, DWRITE_FONT_WEIGHT_NORMAL);
        float maxW = (r.right - r.left) - 8;
        if (w > maxW) { size *= maxW / w; w = RunsWidth(rs, size, DWRITE_FONT_WEIGHT_NORMAL); }
        float cy = (r.top + r.bottom) / 2, base = cy + size * 0.705f / 2;
        bool lower = true;  // só minúsculas sem hastes ("mc", "sin"): centra pela altura x
        for (auto& run : rs) if (run.lvl == 0) for (wchar_t ch : run.t) if (!(ch == L'm' || ch == L'c' || ch == L'r' || ch == L'n' || ch == L's' || ch == L'x' || ch == L'+' || ch == L'−' || ch == L'e' || ch == L'a' || ch == L'o' || ch == L'w' || ch == L'v' || ch == L'u')) lower = false;
        if (lower) base = cy + size * 0.53f / 2;
        DrawRuns(rt, br, rs, size, DWRITE_FONT_WEIGHT_NORMAL, (r.left + r.right) / 2 - w / 2, base, tc, tc);
    }
}

float DisplayRight() { return Client().width - 13; }

void DrawStdDisplay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    float left = Side() + 14, right = DisplayRight();
    bool basic = g_mode == M_BASIC;
    float greySize = basic ? 21 : 17, bigSize = basic ? 32 : 27;
    float greyBase = basic ? 80 : 68, bigBase = basic ? 118 : 101;
    if (semaforo::Leopard()) { greySize = 13; bigSize = 22; greyBase = 50; bigBase = 78; }  // Leopard: no visor de cristal líquido
    std::vector<Run> grey, big;
    auto num = [&](double v) { std::vector<Run> r(1); r[0].t = FormatNum(v, basic ? 10 : 15); return r; };
    if (g_rpn) {
        if (g_stack.size() >= (g_entering ? 1u : 2u)) grey = num(g_stack[g_stack.size() - (g_entering ? 1 : 2)]);
        if (g_entering) { big.resize(1); big[0].t = FormatTyped(g_entry); }
        else big = num(g_stack.empty() ? 0 : g_stack.back());
    } else if (g_done) {
        grey = RenderToks(g_doneExpr);
        big = num(g_ans);
    } else if (g_toks.empty()) {
        big = num(0);
    } else {
        big = RenderToks(g_toks);
        double v;
        if (HasOperation(g_toks) && Eval(g_toks, v)) grey = num(v);  // prévia do resultado
    }
    if (!grey.empty()) FitRuns(rt, br, grey, greySize, greySize * 0.6f, DWRITE_FONT_WEIGHT_NORMAL, left, right, greyBase, pal.sub, pal.sub);
    D2D1_COLOR_F ghost = pal.sub;
    FitRuns(rt, br, big, bigSize, bigSize * 0.5f, DWRITE_FONT_WEIGHT_NORMAL, left + (g_rad && !basic ? 34 : 0), right, bigBase, pal.text, ghost);
    if (g_rad && !basic) TextAt(rt, br, L"Rad", Font(13), left, bigBase, pal.sub);
}

std::wstring ConvFormat(double v) { return FormatNum(v, 10); }
double ConvertValue(double v) {
    const Cat& c = CATS[g_convCat];
    if (g_convCat == 2) {  // temperatura
        double k = g_convFrom == 0 ? v + 273.15 : g_convFrom == 1 ? (v - 32) * 5 / 9 + 273.15 : v;
        return g_convTo == 0 ? k - 273.15 : g_convTo == 1 ? (k - 273.15) * 9 / 5 + 32 : k;
    }
    return v * c.units[g_convFrom].f / c.units[g_convTo].f;
}

void DrawConvert(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    float left = Side() + 48, mid = ConvMid();
    for (int which = 0; which < 2; which++) {
        D2D1_RECT_F ur = ConvUnitRect(which);
        bool hov = g_hover.kind == TG_CONV && g_hover.idx == which;
        br->SetColor(hov ? Mix(pal.key[0], g_dark ? Rgb(255, 255, 255) : Rgb(0, 0, 0), 0.08f) : pal.key[0]);
        rt->FillRoundedRectangle({ ur, 12, 12 }, br);
        const Unit& u = CATS[g_convCat].units[which == 0 ? g_convFrom : g_convTo];
        float cy = (ur.top + ur.bottom) / 2;
        TextAt(rt, br, u.sym, Font(14, DWRITE_FONT_WEIGHT_MEDIUM), ur.left + 10, cy + 5, pal.text);
        br->SetColor(pal.sub);  // chevron.down
        Line(rt, br, ur.right - 15, cy - 1.5f, ur.right - 12, cy + 1.5f, 1.3f);
        Line(rt, br, ur.right - 12, cy + 1.5f, ur.right - 9, cy - 1.5f, 1.3f);
        float size = 30, base = cy + 10;
        std::vector<Run> rs;
        if (which == 0) {
            if (!g_rpn && !g_done && !g_toks.empty()) rs = RenderToks(g_toks);
            else { rs.resize(1); rs[0].t = g_rpn && g_entering ? FormatTyped(g_entry) : ConvFormat(CurrentValue()); }
        } else { rs.resize(1); rs[0].t = ConvFormat(ConvertValue(CurrentValue())); }
        FitRuns(rt, br, rs, size, 14, DWRITE_FONT_WEIGHT_NORMAL, left, ur.left - 10, base, which == 0 ? pal.text : pal.text, pal.sub);
    }
    br->SetColor(pal.line);
    rt->DrawLine({ Side() + 48, mid + 2 }, { DisplayRight(), mid + 2 }, br, 1);
    auto sc = ConvSwapCenter();  // arrow.up.arrow.down
    bool hov = g_hover.kind == TG_CONV && g_hover.idx == 2;
    br->SetColor(hov ? Mix(pal.key[0], g_dark ? Rgb(255, 255, 255) : Rgb(0, 0, 0), 0.08f) : pal.key[0]);
    rt->FillEllipse(D2D1::Ellipse(sc, 14, 14), br);
    br->SetColor(pal.text);
    Line(rt, br, sc.x - 3.5f, sc.y - 6, sc.x - 3.5f, sc.y + 6, 1.4f);
    Line(rt, br, sc.x - 6.5f, sc.y - 3, sc.x - 3.5f, sc.y - 6, 1.4f); Line(rt, br, sc.x - 0.5f, sc.y - 3, sc.x - 3.5f, sc.y - 6, 1.4f);
    Line(rt, br, sc.x + 3.5f, sc.y - 6, sc.x + 3.5f, sc.y + 6, 1.4f);
    Line(rt, br, sc.x + 0.5f, sc.y + 3, sc.x + 3.5f, sc.y + 6, 1.4f); Line(rt, br, sc.x + 6.5f, sc.y + 3, sc.x + 3.5f, sc.y + 6, 1.4f);
    TextAt(rt, br, CATS[g_convCat].name, Font(11, DWRITE_FONT_WEIGHT_MEDIUM, false, 1), Side() + 14, TB + 6, pal.sub);
}

void DrawProgDisplay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    float left = Side() + 14, right = DisplayRight();
    if (g_pOp) {
        std::vector<Run> g(1); g[0].t = ProgFmt(g_pAcc, g_base) + L" " + ProgOpSym(g_pOp);
        FitRuns(rt, br, g, 17, 11, DWRITE_FONT_WEIGHT_NORMAL, left + 120, right, 66, pal.sub, pal.sub);
    }
    std::vector<Run> b(1); b[0].t = g_pErr ? Tr(L"Erro") : ProgFmt(ProgCur(), g_base);
    FitRuns(rt, br, b, 32, 14, DWRITE_FONT_WEIGHT_NORMAL, left + 120, right, 104, pal.text, pal.sub);
    // a mesma conta nas outras bases, pequena, à esquerda
    uint64_t v = ProgCur();
    TextAt(rt, br, L"HEX  " + ProgFmt(v, 16), Font(11, DWRITE_FONT_WEIGHT_NORMAL, false, 1), left, 70, pal.sub);
    TextAt(rt, br, L"DEC  " + ProgFmt(v, 10), Font(11, DWRITE_FONT_WEIGHT_NORMAL, false, 1), left, 86, pal.sub);
    TextAt(rt, br, L"OCT  " + ProgFmt(v, 8), Font(11, DWRITE_FONT_WEIGHT_NORMAL, false, 1), left, 102, pal.sub);
    // bits
    IDWriteTextFormat* bf = Font(13, DWRITE_FONT_WEIGHT_MEDIUM);
    IDWriteTextFormat* lf = Font(9.5f, DWRITE_FONT_WEIGHT_NORMAL, false, 1);
    for (int bit = 63; bit >= 0; bit--) {
        D2D1_RECT_F r = BitRect(bit);
        bool on = (v >> bit) & 1;
        bool hov = g_hover.kind == TG_BIT && g_hover.idx == bit;
        if (hov) { br->SetColor(pal.hover); rt->FillRoundedRectangle({ r, 3, 3 }, br); }
        std::wstring t = on ? L"1" : L"0";
        float w = TextW(bf, t);
        TextAt(rt, br, t, bf, (r.left + r.right) / 2 - w / 2, r.top + 13.5f, on ? pal.text : pal.sub);
        if (bit % 16 == 15 || bit % 32 == 0 || bit % 16 == 0) {
            if (bit % 16 == 15 || bit % 32 == 0) {
                std::wstring n = std::to_wstring(bit);
                float nw = TextW(lf, n);
                float x = bit % 32 == 0 ? r.right - nw : r.left;
                TextAt(rt, br, n, lf, x, r.bottom + 9, pal.sub);
            }
        }
    }
    // base
    const int bases[3] = { 8, 10, 16 };
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = BaseRect(i);
        bool sel = g_base == bases[i], hov = g_hover.kind == TG_BASE && g_hover.idx == i;
        br->SetColor(sel ? pal.key[2] : hov ? Mix(pal.key[0], g_dark ? Rgb(255, 255, 255) : Rgb(0, 0, 0), 0.08f) : pal.key[0]);
        rt->FillRoundedRectangle({ r, 10, 10 }, br);
        std::wstring t = std::to_wstring(bases[i]);
        IDWriteTextFormat* tf = Font(12.5f, DWRITE_FONT_WEIGHT_MEDIUM);
        TextAt(rt, br, t, tf, (r.left + r.right) / 2 - TextW(tf, t) / 2, (r.top + r.bottom) / 2 + 4.5f, pal.text);
    }
}

void DrawSidebar(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Pal& pal) {
    if (!g_sidebar) return;
    auto c = Client();
    br->SetColor(pal.sidebar);
    rt->FillRectangle({ 0, 0, SBW, c.height }, br);
    br->SetColor(pal.line);
    rt->DrawLine({ SBW - 0.5f, 0 }, { SBW - 0.5f, c.height }, br, 1);
    rt->PushAxisAlignedClip({ 0, TB, SBW, c.height }, D2D1_ANTIALIAS_MODE_ALIASED);
    if (g_hist.empty()) {
        IDWriteTextFormat* tf = Font(13, DWRITE_FONT_WEIGHT_NORMAL, false, 1);
        std::wstring t = Tr(L"Sem Histórico");
        TextAt(rt, br, t, tf, SBW / 2 - TextW(tf, t) / 2, c.height / 2, pal.sub);
    }
    for (int i = 0; i < (int)g_hist.size(); i++) {
        D2D1_RECT_F r = HistRect(i);
        if (r.bottom < TB || r.top > c.height) continue;
        if (g_hover.kind == TG_HIST && g_hover.idx == i) { br->SetColor(pal.hover); rt->FillRoundedRectangle({ r, 8, 8 }, br); }
        std::vector<Run> e = RenderToks(g_hist[i].toks);
        FitRuns(rt, br, e, 13, 9, DWRITE_FONT_WEIGHT_NORMAL, r.left + 10, r.right - 10, r.top + 19, pal.sub, pal.sub);
        std::vector<Run> v(1); v[0].t = FormatNum(g_hist[i].val, 12);
        FitRuns(rt, br, v, 19, 11, DWRITE_FONT_WEIGHT_MEDIUM, r.left + 10, r.right - 10, r.top + 42, pal.text, pal.sub);
    }
    rt->PopAxisAlignedClip();
}

void Render() {
    if (!g_rt) {
        RECT rc; GetClientRect(g_hwnd, &rc);
        float dpi = (float)GetDpiForWindow(g_hwnd);
        g_f->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi),
                                    D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
        if (!g_rt) return;
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    Pal pal = Colors();
    g_rt->BeginDraw();
    g_rt->Clear(pal.bg);
    ID2D1SolidColorBrush* br = nullptr;
    g_rt->CreateSolidColorBrush(pal.text, &br);
    if (semaforo::Leopard()) {
        // Leopard: a janela inteira de metal (degradê cinza), o título no meio da barra e o visor de cristal líquido
        auto cs = Client();
        D2D1_GRADIENT_STOP st[2] = { { 0, Rgb(214, 214, 214) }, { 1, Rgb(170, 170, 170) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(g_rt->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, cs.height }), gs, &lb)))
            g_rt->FillRectangle({ 0, 0, cs.width, cs.height }, lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
        leopard::Metal(g_rt, { 0, 0, cs.width, 22 }, g_active, false, true, 0);
        static IDWriteTextFormat* tt = [] {
            IDWriteTextFormat* f = nullptr; IDWriteFactory* dw = nullptr;
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
            if (dw) { dw->CreateTextFormat(L"Lucida Sans Unicode", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"pt-br", &f); dw->Release(); }
            if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
            return f;
        }();
        std::wstring title = Tr(L"Calculadora");
        br->SetColor(Rgb(0, 0, 0, g_active ? 0.85f : 0.45f));
        if (tt) g_rt->DrawTextW(title.c_str(), (UINT32)title.size(), tt, { 80, 0, cs.width - 80, 22 }, br);
        if (g_mode != M_PROG) {
            D2D1_RECT_F lcd{ Side() + 10, 30, cs.width - 10, KeysTop() - 10 };
            D2D1_GRADIENT_STOP s2[2] = { { 0, Rgb(226, 234, 190) }, { 1, Rgb(240, 245, 205) } };
            if (SUCCEEDED(g_rt->CreateGradientStopCollection(s2, 2, &gs)) &&
                SUCCEEDED(g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, lcd.top }, { 0, lcd.bottom }), gs, &lb)))
                g_rt->FillRoundedRectangle({ lcd, 4, 4 }, lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            br->SetColor(Rgb(0, 0, 0, 0.45f));
            g_rt->DrawRoundedRectangle({ { lcd.left + 0.5f, lcd.top + 0.5f, lcd.right - 0.5f, lcd.bottom - 0.5f }, 3.5f, 3.5f }, br, 1);
            br->SetColor(Rgb(0, 0, 0, 0.12f));  // a sombra por dentro em cima
            g_rt->FillRectangle({ lcd.left + 1, lcd.top + 1, lcd.right - 1, lcd.top + 3 }, br);
        }
    }
    DrawSidebar(g_rt, br, pal);
    if (g_mode == M_PROG) DrawProgDisplay(g_rt, br, pal);
    else if (ConvOn()) DrawConvert(g_rt, br, pal);
    else DrawStdDisplay(g_rt, br, pal);
    DrawKeys(g_rt, br, pal);
    DrawRoundButtons(g_rt, br, pal);
    DrawLights(g_rt, br);
    br->Release();
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) { g_rt->Release(); g_rt = nullptr; }
    if (!g_shown) {  // aparece só com o primeiro quadro pronto (sem piscar branco)
        g_shown = true;
        ampliacao::Revelar(g_hwnd);  // abrir animado (o Shell mostra no fim) ou já
    }
}
void Invalidate() { InvalidateRect(g_hwnd, nullptr, FALSE); }

// ---------------------------------------------------------------------------
// interação
Target HitTarget(D2D1_POINT_2F p) {
    Target t;
    for (int k = 0; k < 3; k++) if (InCircle(LightCenter(k), 8, p)) return { TG_LIGHT, k };
    if (InCircle(SideBtnCenter(), semaforo::Leopard() ? ROUND_R / 2 : ROUND_R, p)) return { TG_SIDEBTN, 0 };
    if (InCircle(ModeBtnCenter(), semaforo::Leopard() ? ROUND_R / 2 : ROUND_R, p)) return { TG_MODEBTN, 0 };
    if (g_sidebar && p.x < SBW && p.y >= TB)
        for (int i = 0; i < (int)g_hist.size(); i++) if (In(HistRect(i), p)) return { TG_HIST, i };
    for (size_t i = 0; i < g_keys.size(); i++) if (In(KeyRect(g_keys[i]), p)) return { TG_KEY, (int)i };
    if (ConvOn()) {
        for (int w = 0; w < 2; w++) if (In(ConvUnitRect(w), p)) return { TG_CONV, w };
        if (InCircle(ConvSwapCenter(), 14, p)) return { TG_CONV, 2 };
    }
    if (g_mode == M_PROG) {
        for (int b = 0; b < 64; b++) if (In(BitRect(b), p)) return { TG_BIT, b };
        for (int i = 0; i < 3; i++) if (In(BaseRect(i), p)) return { TG_BASE, i };
    }
    return t;
}

// O brilho das teclas anda no relógio da tela: o EndDraw espera o vsync, então cada WM_PAINT é um quadro.
// Sem SetTimer (15,6 ms engasgava nas telas de 120/144/174 Hz) e o passo é pelo tempo real.
double NowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}
void StartAnim() {
    if (!g_anim) { g_anim = true; g_lastAnim = g_animStart = NowMs(); g_animFrames = 0; }
    Invalidate();
}
// um passo do brilho; true enquanto alguma tecla ainda apaga (as seguradas ficam acesas e paradas)
bool StepAnim() {
    if (!g_anim) return false;
    double now = NowMs();
    float dt = (float)std::min(100.0, now - g_lastAnim) / 1000.f;
    g_lastAnim = now;
    bool moving = false;
    for (auto it = g_glow.begin(); it != g_glow.end();) {
        if (!g_held[it->first]) {
            it->second -= dt / 0.25f;
            if (it->second <= 0) { g_held.erase(it->first); it = g_glow.erase(it); continue; }
            moving = true;
        }
        ++it;
    }
    g_animFrames++;
    g_anim = moving;
    static int logged = 0;  // só os 3 primeiros: dá a medida sem escrever no disco a cada tecla
    if (!moving && logged < 3) {
        logged++;
        double ms = now - g_animStart;
        Log(L"brilho das teclas: %d quadros em %.0f ms (%.0f quadros/s)", g_animFrames, ms, ms > 0 ? g_animFrames * 1000.0 / ms : 0.0);
    }
    return moving;
}

void Flash(int id) {  // tecla apertada pelo teclado: acende e apaga
    g_glow[id] = 1;
    g_held[id] = false;
    StartAnim();
}

void PressKey(int id) {
    Press(id);
    Invalidate();
}

void Copy(const std::wstring& t) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t n = (t.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(h), t.c_str(), n); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    CloseClipboard();
}
std::wstring CopyText() {
    if (g_mode == M_PROG) { std::wstring s = ProgFmt(ProgCur(), g_base); s.erase(std::remove(s.begin(), s.end(), g_grp), s.end()); for (auto& c : s) if (c == L'−') c = L'-'; return s; }
    double v = CurrentValue();
    if (!std::isfinite(v)) return L"";
    return PlainNum(v);
}

int CharKey(wchar_t c);
void Paste() {
    if (!OpenClipboard(g_hwnd)) return;
    std::wstring t;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) { if (auto p = (const wchar_t*)GlobalLock(h)) { t = p; GlobalUnlock(h); } }
    CloseClipboard();
    if (t.size() > 200) t.resize(200);
    for (size_t i = 0; i < t.size(); i++) {
        wchar_t c = t[i];
        if (c == g_grp && g_grp != g_dec && g_mode != M_PROG) continue;
        if ((c == L'e' || c == L'E') && g_mode != M_PROG && i > 0 && iswdigit(t[i - 1])) { Press(K_EE); continue; }
        int k = CharKey(c);
        if (k >= 0 && k != K_EQ && k != K_AC) Press(k);
    }
    Invalidate();
}

int CharKey(wchar_t c) {
    if (g_mode == M_PROG) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return 10 + c - L'a';
        if (c >= L'A' && c <= L'F') return 10 + c - L'A';
        switch (c) {
        case L'+': return K_ADD; case L'-': return K_SUB; case L'*': return K_MUL; case L'/': return K_DIV; case L'%': return K_PCT;
        case L'&': return K_AND; case L'|': return K_OR; case L'^': return K_XOR; case L'~': return K_1S; case L'<': return K_SHL; case L'>': return K_SHR;
        case L'=': case L'\r': return K_EQ;
        }
        return -1;
    }
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c == g_dec || c == L'.' || c == L',') return K_DOT;
    switch (c) {
    case L'+': return K_ADD; case L'-': case L'−': return K_SUB; case L'*': case L'x': case L'X': case L'×': return K_MUL;
    case L'/': case L'÷': return K_DIV; case L'%': return K_PCT; case L'=': case L'\r': return K_EQ;
    case L'c': case L'C': return K_AC;
    case L'(': return g_mode == M_SCI ? K_LP : -1;
    case L')': return g_mode == M_SCI ? K_RP : -1;
    case L'^': return g_mode == M_SCI ? K_POW : -1;
    case L'!': return g_mode == M_SCI ? K_FACT : -1;
    case L'π': case L'p': return g_mode == M_SCI ? K_PI : -1;
    case L'e': return g_mode == M_SCI ? K_E : -1;
    case L'E': return g_mode == M_SCI ? K_EE : -1;
    }
    return -1;
}

void SetMode(Mode m) {
    if (m == g_mode) return;
    if (m == M_PROG) {
        double v = CurrentValue();
        g_pAcc = std::isfinite(v) ? (uint64_t)(int64_t)std::llround(std::clamp(v, -9.2e18, 9.2e18)) : 0;
        g_pEntry = 0; g_pOp = 0; g_pEntering = false; g_pErr = false;
    } else if (g_mode == M_PROG) {
        g_toks.clear(); g_doneExpr.clear(); g_done = true; g_ans = (double)(int64_t)ProgCur();
        g_stack.clear(); g_entering = false;
        if (g_rpn) g_stack.push_back(g_ans);
    }
    g_mode = m;
    g_second = false;
    ApplyLayout();
}

void ModeMenu() {
    using ctxmenu::Item;
    auto I = [](const wchar_t* t, int id, bool chk, bool dis, const wchar_t* sc) { Item i; i.label = t; i.id = id; i.checked = chk; i.disabled = dis; i.shortcut = sc; return i; };
    Item sep; sep.separator = true;
    float sc = Scale();
    auto c = ModeBtnCenter();
    POINT pt{ (LONG)std::lround((c.x + ROUND_R - 196) * sc), (LONG)std::lround((c.y + ROUND_R + 6) * sc) };
    ClientToScreen(g_hwnd, &pt);
    int id = ctxmenu::ShowItems(g_hwnd, pt, {
        I(Tr(L"Básica"), 1, g_mode == M_BASIC, false, L"Ctrl+1"), I(Tr(L"Científica"), 2, g_mode == M_SCI, false, L"Ctrl+2"),
        I(Tr(L"Programador"), 3, g_mode == M_PROG, false, L"Ctrl+3"), sep,
        I(Tr(L"Converter"), 4, g_convert && g_mode != M_PROG, g_mode == M_PROG, L""), I(Tr(L"Modo RPN"), 5, g_rpn && g_mode != M_PROG, g_mode == M_PROG, L"Ctrl+R"), sep,
        I(Tr(L"Notas Matemáticas…"), 6, false, true, L"") });
    switch (id) {
    case 1: SetMode(M_BASIC); break;
    case 2: SetMode(M_SCI); break;
    case 3: SetMode(M_PROG); break;
    case 4: g_convert = !g_convert; ApplyLayout(); break;
    case 5: {
        g_rpn = !g_rpn;
        double v = g_rpn ? CurrentValue() : (g_stack.empty() ? 0 : g_stack.back());
        if (g_rpn) { g_stack.clear(); if (v != 0 && std::isfinite(v)) g_stack.push_back(v); g_entering = false; g_entry.clear(); }
        else { g_toks.clear(); g_doneExpr.clear(); g_done = v != 0; g_ans = v; }
        BuildKeys(); SaveSettings();
        break;
    }
    }
    g_hover = {};
    Invalidate();
}

void UnitMenu(int which) {
    using ctxmenu::Item;
    std::vector<Item> items;
    int cur = which == 0 ? g_convFrom : g_convTo;
    for (int c = 0; c < (int)CATS.size(); c++) {
        Item cat; cat.label = CATS[c].name; cat.checked = c == g_convCat; cat.id = 0;
        for (int u = 0; u < (int)CATS[c].units.size(); u++) {
            Item it; it.label = std::wstring(CATS[c].units[u].name) + L" (" + CATS[c].units[u].sym + L")"; it.id = 1 + c * 100 + u;
            it.checked = c == g_convCat && u == cur;
            cat.sub.push_back(it);
        }
        items.push_back(cat);
    }
    D2D1_RECT_F r = ConvUnitRect(which);
    float sc = Scale();
    POINT pt{ (LONG)std::lround(r.left * sc), (LONG)std::lround((r.bottom + 4) * sc) };
    ClientToScreen(g_hwnd, &pt);
    int id = ctxmenu::ShowItems(g_hwnd, pt, items);
    if (id <= 0) return;
    int c = (id - 1) / 100, u = (id - 1) % 100;
    if (c != g_convCat) {  // outra categoria: a outra unidade vira a segunda da lista
        g_convCat = c;
        int other = u == 0 ? 1 : 0;
        if (which == 0) { g_convFrom = u; g_convTo = other; } else { g_convTo = u; g_convFrom = other; }
    } else if (which == 0) g_convFrom = u;
    else g_convTo = u;
    SaveSettings();
}

void UseHistory(int i) {
    if (i < 0 || i >= (int)g_hist.size()) return;
    const HistItem& h = g_hist[i];
    if (g_mode == M_PROG) { g_pEntry = std::isfinite(h.val) ? (uint64_t)(int64_t)std::llround(h.val) : 0; g_pEntering = true; return; }
    if (g_rpn) { Commit(); g_stack.push_back(h.val); return; }
    g_doneExpr = h.toks; g_ans = h.val; g_done = true; g_toks.clear(); g_repOk = false;
}

void HistoryMenu(int i, POINT screen) {
    using ctxmenu::Item;
    auto I = [](const wchar_t* t, int id, bool dis = false) { Item x; x.label = t; x.id = id; x.disabled = dis; return x; };
    Item sep; sep.separator = true;
    int id = ctxmenu::ShowItems(g_hwnd, screen, { I(Tr(L"Copiar Resultado"), 1, i < 0), I(Tr(L"Copiar Expressão"), 2, i < 0), I(Tr(L"Apagar"), 3, i < 0), sep,
                                                  I(Tr(L"Limpar Histórico"), 4, g_hist.empty()) });
    if (id == 1 && i >= 0) Copy(PlainNum(g_hist[i].val));
    else if (id == 2 && i >= 0) Copy(PlainOf(RenderToks(g_hist[i].toks)));
    else if (id == 3 && i >= 0) { g_hist.erase(g_hist.begin() + i); SaveHistory(); }
    else if (id == 4) { g_hist.clear(); g_histScroll = 0; SaveHistory(); }
    g_hover = {};
    Invalidate();
}

void ToggleSidebar() {
    g_sidebar = !g_sidebar;
    g_histScroll = 0;
    ApplyLayout(g_sidebar ? (int)SBW : -(int)SBW);
}

LRESULT HitTestNC(HWND h, LPARAM l) {
    POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(h, &pt);
    RECT rc; GetClientRect(h, &rc);
    float sc = Scale();
    if (g_mode == M_SCI) {  // só a Científica muda de tamanho
        int b = (int)(6 * sc);
        bool L = pt.x < b, R = pt.x >= rc.right - b, T = pt.y < b, B = pt.y >= rc.bottom - b;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
    }
    D2D1_POINT_2F p{ pt.x / sc, pt.y / sc };
    if (HitTarget(p).kind != TG_NONE) return HTCLIENT;
    if (g_sidebar && p.x < SBW && p.y >= TB) return HTCLIENT;  // lista do histórico (rolagem, menu)
    return HTCAPTION;  // o resto (visor, fundo) arrasta a janela
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE: if (w) return 0; break;
    case WM_NCHITTEST: return HitTestNC(h, l);
    case WM_GETMINMAXINFO: {
        if (!g_hwnd) break;
        float sc = Scale();
        auto* mm = (MINMAXINFO*)l;
        auto cs = ContentSize();
        if (g_mode == M_SCI) {
            mm->ptMinTrackSize = { (LONG)((480 + Side()) * sc), (LONG)((300 + (ConvOn() ? CONV_H : 0)) * sc) };
        } else {
            LONG ww = (LONG)std::lround((cs.width + Side()) * sc), hh = (LONG)std::lround(cs.height * sc);
            mm->ptMinTrackSize = mm->ptMaxTrackSize = { ww, hh };
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE: g_userSizing = true; return 0;
    case WM_EXITSIZEMOVE: g_userSizing = false; SaveSettings(); return 0;
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        if (g_userSizing && g_mode == M_SCI) {
            auto c = Client();
            g_sciW = c.width - Side();
            g_sciH = c.height - (ConvOn() ? CONV_H : 0);
        }
        Invalidate();
        return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        if (g_rt) { g_rt->Release(); g_rt = nullptr; }
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); bool moving = StepAnim(); Render(); EndPaint(h, &ps); if (moving) Invalidate(); return 0; }
    case WM_ACTIVATE: g_active = LOWORD(w) != WA_INACTIVE; Invalidate(); return 0;
    case WM_SETTINGCHANGE: {
        g_dark = ReadDark();
        BOOL dm = g_dark; DwmSetWindowAttribute(h, 20, &dm, sizeof(dm));
        COLORREF border = g_dark ? RGB(70, 74, 78) : RGB(196, 196, 200); DwmSetWindowAttribute(h, 34, &border, sizeof(border));
        Invalidate();
        return 0;
    }
    case WM_TIMER: if (w == 2) { bool d = ReadDark(); if (d != g_dark) SendMessageW(h, WM_SETTINGCHANGE, 0, 0); } return 0;
    case WM_MOUSEWHEEL: {
        if (!g_sidebar) return 0;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
        if (pt.x / Scale() >= SBW) return 0;
        float maxS = std::max(0.f, g_hist.size() * HIST_H + 8 - (Client().height - TB));
        g_histScroll = std::clamp(g_histScroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * HIST_H, 0.f, maxS);
        Invalidate();
        return 0;
    }
    case WM_NCMOUSEMOVE: case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (m == WM_NCMOUSEMOVE) ScreenToClient(h, &pt);
        float sc = Scale(); D2D1_POINT_2F p{ pt.x / sc, pt.y / sc };
        TRACKMOUSEEVENT t{ sizeof(t), (DWORD)(m == WM_NCMOUSEMOVE ? TME_LEAVE | TME_NONCLIENT : TME_LEAVE), h, 0 }; TrackMouseEvent(&t);
        Target tg = HitTarget(p);
        bool hl = p.x < 82 && p.y < TB;
        if (!(tg == g_hover) || hl != g_hoverLights) { g_hover = tg; g_hoverLights = hl; Invalidate(); }
        if (m == WM_NCMOUSEMOVE) break;
        return 0;
    }
    case WM_NCMOUSELEAVE: case WM_MOUSELEAVE:
        if (g_hover.kind != TG_NONE || g_hoverLights) { g_hover = {}; g_hoverLights = false; Invalidate(); }
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN: {
        float sc = Scale(); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        Target tg = HitTarget(p);
        g_down = tg;
        if (tg.kind == TG_KEY) {
            int id = g_keys[tg.idx].id;
            if (!KeyEnabled(id)) { g_down = {}; return 0; }
            g_glow[id] = 1; g_held[id] = true;
            SetCapture(h);
            Invalidate();
        } else if (tg.kind == TG_LIGHT) {
            if (tg.idx == 0) PostMessageW(h, WM_CLOSE, 0, 0);
            else if (tg.idx == 1) ShowWindow(h, SW_MINIMIZE);
        } else if (tg.kind == TG_SIDEBTN) ToggleSidebar();
        else if (tg.kind == TG_MODEBTN) ModeMenu();
        else if (tg.kind == TG_HIST) { UseHistory(tg.idx); Invalidate(); }
        else if (tg.kind == TG_CONV) {
            if (tg.idx == 2) { std::swap(g_convFrom, g_convTo); SaveSettings(); }
            else UnitMenu(tg.idx);
            g_hover = {};
            Invalidate();
        } else if (tg.kind == TG_BIT) { ProgSet(ProgCur() ^ (1ull << tg.idx)); Invalidate(); }
        else if (tg.kind == TG_BASE) { const int b[3] = { 8, 10, 16 }; g_base = b[tg.idx]; SaveSettings(); Invalidate(); }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_down.kind == TG_KEY) {
            ReleaseCapture();
            int id = g_keys[g_down.idx].id;
            g_held[id] = false;
            StartAnim();
            float sc = Scale(); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
            if (HitTarget(p) == g_down) PressKey(id);
        }
        g_down = {};
        return 0;
    }
    case WM_RBUTTONUP: {
        float sc = Scale(); D2D1_POINT_2F p{ GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc };
        POINT sp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &sp);
        if (g_sidebar && p.x < SBW && p.y >= TB) {
            Target tg = HitTarget(p);
            HistoryMenu(tg.kind == TG_HIST ? tg.idx : -1, sp);
        }
        return 0;
    }
    case WM_NCRBUTTONUP: {  // visor: Copiar / Colar
        using ctxmenu::Item;
        auto I = [](const wchar_t* t, int id, const wchar_t* s) { Item x; x.label = t; x.id = id; x.shortcut = s; return x; };
        POINT sp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int id = ctxmenu::ShowItems(h, sp, { I(Tr(L"Copiar"), 1, L"Ctrl+C"), I(Tr(L"Colar"), 2, L"Ctrl+V") });
        if (id == 1) Copy(CopyText()); else if (id == 2) Paste();
        return 0;
    }
    case WM_SYSKEYDOWN: break;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl) {
            if (w == 'C') Copy(CopyText());
            else if (w == 'V') Paste();
            else if (w == '1') SetMode(M_BASIC);
            else if (w == '2') SetMode(M_SCI);
            else if (w == '3') SetMode(M_PROG);
            else if (w == 'R' && g_mode != M_PROG) { g_rpn = !g_rpn; g_stack.clear(); g_entering = false; g_toks.clear(); g_done = false; BuildKeys(); SaveSettings(); }
            else if (w == 'W' || w == 'Q') PostMessageW(h, WM_CLOSE, 0, 0);
            else if (w == 'S') ToggleSidebar();
            Invalidate();
            return 0;
        }
        int k = -1;
        if (w == VK_ESCAPE) k = K_AC;
        else if (w == VK_BACK) k = K_DEL;
        else if (w == VK_DELETE) k = K_AC;
        else if (w == VK_RETURN) k = K_EQ;
        if (k >= 0) { Flash(k); PressKey(k); return 0; }
        return 0;
    }
    case WM_CHAR: {
        wchar_t c = (wchar_t)w;
        if (c < 32 || c == 127) return 0;
        int k = CharKey(c);
        if (k >= 0 && KeyEnabled(k)) { Flash(k); PressKey(k); }
        return 0;
    }
    case WM_CLOSE: SaveSettings(); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring PickFamily(std::initializer_list<const wchar_t*> names) {
    IDWriteFontCollection* fc = nullptr;
    g_dw->GetSystemFontCollection(&fc);
    std::wstring r = L"Segoe UI";
    for (auto n : names) {
        UINT32 i; BOOL ok = FALSE;
        if (fc) fc->FindFamilyName(n, &i, &ok);
        if (ok) { r = n; break; }
    }
    if (fc) fc->Release();
    return r;
}

// ícone da janela: o da Calculadora do pacote do Sequoia (icones\sequoia), se existir
HICON PackIcon(int px) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe); PathRemoveFileSpecW(exe);
    std::wstring png = std::wstring(exe) + L"\\icones\\sequoia\\Calculator.png";
    if (!g_wic) return nullptr;
    HICON ic = nullptr;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* scl = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(g_wic->CreateBitmapScaler(&scl)) && SUCCEEDED(scl->Initialize(fr, px, px, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(scl, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        std::vector<BYTE> bits((size_t)px * px * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, px * 4, (UINT)bits.size(), bits.data()))) {
            BITMAPV5HEADER bh{}; bh.bV5Size = sizeof(bh); bh.bV5Width = px; bh.bV5Height = -px; bh.bV5Planes = 1; bh.bV5BitCount = 32; bh.bV5Compression = BI_BITFIELDS;
            bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
            void* p = nullptr; HDC dc = GetDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &p, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (color && p) {
                memcpy(p, bits.data(), bits.size());
                HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
                ICONINFO ii{ TRUE, 0, 0, mask, color };
                ic = CreateIconIndirect(&ii);
                DeleteObject(mask);
            }
            if (color) DeleteObject(color);
        }
    }
    if (cv) cv->Release(); if (scl) scl->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    return ic;
}

void LoadSettings(int& x, int& y) {
    auto kv = ReadKv(DataDir() + L"\\calculadora.txt");
    auto get = [&](const wchar_t* k, int def) { auto it = kv.find(k); return it == kv.end() ? def : _wtoi(it->second.c_str()); };
    std::wstring mode = kv.count(L"modo") ? kv[L"modo"] : L"basica";
    g_mode = mode == L"cientifica" ? M_SCI : mode == L"programador" ? M_PROG : M_BASIC;
    x = get(L"x", INT_MIN); y = get(L"y", INT_MIN);
    g_sciW = (float)std::max(480, get(L"larg_cientifica", 572));
    g_sciH = (float)std::max(300, get(L"alt_cientifica", 347));
    g_rpn = get(L"rpn", 0) != 0; g_convert = get(L"converter", 0) != 0; g_sidebar = get(L"historico", 0) != 0; g_rad = get(L"radianos", 0) != 0;
    g_base = get(L"base", 10); if (g_base != 8 && g_base != 16) g_base = 10;
    g_convCat = std::clamp(get(L"conv_cat", 0), 0, (int)CATS.size() - 1);
    int n = (int)CATS[g_convCat].units.size();
    g_convFrom = std::clamp(get(L"conv_de", 1), 0, n - 1); g_convTo = std::clamp(get(L"conv_para", std::min(4, n - 1)), 0, n - 1);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int show) {
    HANDLE one = CreateMutexW(nullptr, TRUE, L"CalculadoraMacUmaJanela");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // já aberta: traz para a frente
        if (HWND w = FindWindowW(L"CalculadoraJanela", nullptr)) { if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        return 0;
    }
    g_inst = hi;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    g_log = _wfsopen((std::wstring(exe) + L"\\calculadora.log").c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    ctxmenu::Init(hi, g_log);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_f);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    g_f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &g_round);
    g_family = PickFamily({ L"SF Pro Display", L"SF Pro", L"Segoe UI Variable Display" });
    g_textFamily = PickFamily({ L"SF Pro Text", L"SF Pro", L"Segoe UI Variable Text" });
    wchar_t buf[8];
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, buf, 8) > 1) g_dec = buf[0];
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, buf, 8) > 1) g_grp = buf[0] == 0xA0 ? L' ' : buf[0];
    if (g_grp == g_dec) g_grp = g_dec == L',' ? L'.' : L',';
    if (cmd && wcsstr(cmd, L"--tema=claro")) g_forceTheme = L"claro";
    if (cmd && wcsstr(cmd, L"--tema=escuro")) g_forceTheme = L"escuro";
    g_dark = ReadDark();
    int sx, sy;
    LoadSettings(sx, sy);
    LoadHistory();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CalculadoraJanela";
    wc.hIcon = PackIcon(48); wc.hIconSm = PackIcon(16);
    RegisterClassExW(&wc);
    POINT cur; GetCursorPos(&cur);
    POINT want{ sx, sy };
    HMONITOR mon = sx != INT_MIN ? MonitorFromPoint(want, MONITOR_DEFAULTTONULL) : nullptr;
    if (!mon) mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float sc = dx / 96.f;
    float side = g_sidebar ? SBW : 0;
    float cw = g_mode == M_BASIC ? 229 : g_mode == M_SCI ? g_sciW : 572;
    float ch = g_mode == M_BASIC ? (semaforo::Leopard() ? 282.f : 405.f) : g_mode == M_SCI ? g_sciH : 442;
    if (g_convert && g_mode != M_PROG) ch += CONV_H;
    int w = (int)std::lround((cw + side) * sc), hgt = (int)std::lround(ch * sc);
    int x, y;
    if (sx != INT_MIN && MonitorFromPoint(want, MONITOR_DEFAULTTONULL)) { x = sx - (int)std::lround(side * sc); y = sy; }
    else { x = (mi.rcWork.left + mi.rcWork.right - w) / 2; y = (mi.rcWork.top + mi.rcWork.bottom - hgt) / 2; }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, TrPick(L"Calculadora", L"Calculator"), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
                             x, y, w, hgt, nullptr, nullptr, hi, nullptr);
    ampliacao::Janela(g_hwnd);  // abrir e fechar animados (o Shell anima)
    BOOL cloak = TRUE; DwmSetWindowAttribute(g_hwnd, 13 /*DWMWA_CLOAK*/, &cloak, sizeof(cloak));
    BOOL dm = g_dark; DwmSetWindowAttribute(g_hwnd, 20, &dm, sizeof(dm));
    COLORREF border = g_dark ? RGB(70, 74, 78) : RGB(196, 196, 200); DwmSetWindowAttribute(g_hwnd, 34, &border, sizeof(border));
    int corner = 2; DwmSetWindowAttribute(g_hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    MARGINS mg{ 0, 0, 1, 0 }; DwmExtendFrameIntoClientArea(g_hwnd, &mg);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    BuildKeys();
    ShowWindow(g_hwnd, show == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
    SetTimer(g_hwnd, 2, 2000, nullptr);  // o tema do Shell (ajustes.txt) não avisa quando muda
    Log(L"calculadora aberta: modo %d, fonte %ls / %ls, decimal '%lc', milhar '%lc'", (int)g_mode, g_family.c_str(), g_textFamily.c_str(), g_dec, g_grp);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_log) fclose(g_log);
    ReleaseMutex(one);
    return 0;
}
