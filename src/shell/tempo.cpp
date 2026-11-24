// A "viagem no tempo" do Time Machine do Mac OS X Leopard (tema Leopard): a tela vira o espaço (galáxia com estrelas
// piscando), a janela do Finder da pasta Documentos fica na frente e as versões antigas dela se empilham para trás,
// sumindo no fundo; a linha do tempo fica na direita ("Hoje (Agora)" embaixo, os backups subindo), as setas
// andam no tempo, e embaixo ficam Cancelar, a data da janela da frente e Restaurar.
// Os backups são os do Histórico de Arquivos do Windows (o que o app Time Machine configura): a cópia de cada
// arquivo fica em <disco>\FileHistory\<usuário>\<PC>\Data\<C>\<caminho> com a data no nome
// ("nome (2026_11_19 22_00_00 UTC).ext"). Restaurar copia a versão escolhida de volta para a pasta, sem apagar a
// atual (se já existir, a restaurada ganha " (restaurado)" no nome). Abre pelo app Time Machine ou ShellAbrirPainel 18.
#include "shell.h"
#include <shlobj.h>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include "../comum/idioma.h"
#include "../comum/leopard.h"

namespace {

const UINT T_ANIM = 1;
struct Item { std::wstring name, src; bool dir = false; ULONGLONG mtime = 0; };
struct Snap { ULONGLONG t = 0; std::vector<Item> items; };  // t = 0: agora

HWND g_wnd;
ui::GpuSurface g_s;
bool g_open, g_closing;
double g_at, g_lastTick;
float g_W, g_H, g_pos;      // g_pos anda até g_sel (índice em g_snaps: 0 = agora, 1 = o backup mais novo...)
int g_sel, g_row = -1, g_hoverTick = -1;
std::vector<Snap> g_snaps;
std::wstring g_folder, g_root;  // a pasta (Documentos) e a cópia dela no backup ("" = sem backup)
std::wstring g_msg;

float Sc() { return ui::Scale(g_wnd); }

ULONGLONG FtOf(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// "nome (2026_11_19 22_00_00 UTC).ext" -> nome.ext e a hora (UTC, FILETIME)
bool ParseVersion(const std::wstring& fn, std::wstring& orig, ULONGLONG& t) {
    size_t p = fn.rfind(L" UTC)");
    if (p == std::wstring::npos || p < 21 || fn[p - 20] != L'(' || fn[p - 21] != L' ') return false;
    SYSTEMTIME st{};
    if (swscanf_s(fn.c_str() + p - 19, L"%hu_%hu_%hu %hu_%hu_%hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond) != 6) return false;
    FILETIME ft; if (!SystemTimeToFileTime(&st, &ft)) return false;
    t = FtOf(ft);
    orig = fn.substr(0, p - 21) + fn.substr(p + 5);
    return true;
}

std::wstring FindRoot() {
    wchar_t user[256] = {}, pc[256] = {};
    GetEnvironmentVariableW(L"USERNAME", user, 256); GetEnvironmentVariableW(L"COMPUTERNAME", pc, 256);
    std::vector<std::wstring> bases;
    // o disco escolhido no Histórico de Arquivos (Config1.xml guarda o caminho do destino)
    wchar_t la[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    for (const wchar_t* cfg : { L"\\Microsoft\\Windows\\FileHistory\\Configuration\\Config1.xml", L"\\Microsoft\\Windows\\FileHistory\\Configuration\\Config2.xml" }) {
        HANDLE f = CreateFileW((std::wstring(la) + cfg).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::string s(1 << 16, '\0'); DWORD n = 0;
        ReadFile(f, s.data(), (DWORD)s.size(), &n, nullptr); CloseHandle(f);
        s.resize(n);
        std::wstring ws;
        if (n >= 2 && (BYTE)s[0] == 0xFF && (BYTE)s[1] == 0xFE) ws.assign((const wchar_t*)(s.data() + 2), (n - 2) / 2);
        else ws.assign(s.begin(), s.end());
        for (const wchar_t* tag : { L"<TargetUrl>", L"<TargetDriveSignature>" }) {
            size_t a = ws.find(tag); if (a == std::wstring::npos) continue;
            a += wcslen(tag); size_t b = ws.find(L'<', a);
            if (b != std::wstring::npos && tag[7] == L'U') bases.push_back(ws.substr(a, b - a));
        }
    }
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++) if (drives & (1u << d)) { wchar_t r[4] = { (wchar_t)(L'A' + d), L':', L'\\', 0 }; UINT ty = GetDriveTypeW(r); if (ty == DRIVE_FIXED || ty == DRIVE_REMOVABLE || ty == DRIVE_REMOTE) bases.push_back(r); }
    for (auto b : bases) {
        if (!b.empty() && b.back() != L'\\') b += L'\\';
        std::wstring p = b + L"FileHistory\\" + user + L"\\" + pc + L"\\Data";
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return p;
    }
    return L"";
}

void Load() {
    g_snaps.clear(); g_msg.clear();
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) { g_folder = docs; CoTaskMemFree(docs); }
    // agora: a pasta como está
    Snap now;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_folder + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            now.items.push_back({ fd.cFileName, g_folder + L"\\" + fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, FtOf(fd.ftLastWriteTime) });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_snaps.push_back(now);
    // os backups
    std::wstring root = FindRoot();
    g_root.clear();
    if (!root.empty() && g_folder.size() > 2 && g_folder[1] == L':') g_root = root + L"\\" + g_folder.substr(0, 1) + g_folder.substr(2);
    std::map<std::wstring, std::vector<std::pair<ULONGLONG, std::wstring>>> files;  // nome -> versões
    std::set<ULONGLONG> times;
    std::vector<std::wstring> dirs;
    if (!g_root.empty()) {
        h = FindFirstFileW((g_root + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.') continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs.push_back(fd.cFileName); continue; }
                std::wstring orig; ULONGLONG t;
                if (!ParseVersion(fd.cFileName, orig, t)) continue;
                t -= t % 600000000ull;  // junta as versões do mesmo backup (mesmo minuto)
                files[orig].push_back({ t, g_root + L"\\" + fd.cFileName });
                times.insert(t);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (auto it = times.rbegin(); it != times.rend(); ++it) {  // do mais novo para o mais antigo
        Snap s; s.t = *it;
        for (auto& d : dirs) s.items.push_back({ d, L"", true, 0 });
        for (auto& [name, vs] : files) {
            const std::pair<ULONGLONG, std::wstring>* best = nullptr;
            for (auto& v : vs) if (v.first <= s.t && (!best || v.first > best->first)) best = &v;
            if (best) s.items.push_back({ name, best->second, false, best->first });
        }
        g_snaps.push_back(s);
    }
    for (auto& s : g_snaps)
        std::sort(s.items.begin(), s.items.end(), [](const Item& a, const Item& b) { return a.dir != b.dir ? a.dir : _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    if (g_root.empty()) g_msg = Tr(L"Nenhum backup encontrado. Escolha o disco de backup no app Time Machine.");
    else if (g_snaps.size() == 1) g_msg = Tr(L"Ainda não há backups desta pasta.");
}

std::wstring DateText(ULONGLONG t, bool full) {
    if (!t) return Tr(L"Hoje (Agora)");
    FILETIME ft{ (DWORD)t, (DWORD)(t >> 32) }, lt; SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
    wchar_t b[96];
    if (full) swprintf_s(b, L"%ls, %02d:%02d", idioma::Date(st, idioma::D_LONG).c_str(), st.wHour, st.wMinute);  // "20 de novembro de 2026, 14:30"
    else if (idioma::DecimalComma()) swprintf_s(b, L"%02d/%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
    else swprintf_s(b, L"%02d/%02d %02d:%02d", st.wMonth, st.wDay, st.wHour, st.wMinute);  // en, ja, zh, ko: mês antes do dia
    return b;
}

IDWriteTextFormat* F(float size, bool bold, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
    auto* f = ui::FormatFace(L"Lucida Sans Unicode", size, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    if (f) { f->SetTextAlignment(al); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
             DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; f->SetTrimming(&tr, nullptr); }
    return f;
}

D2D1_RECT_F FrontRect() {
    float w = std::min(900.f, g_W * 0.56f), h = g_H * 0.52f;
    return D2D1::RectF(g_W / 2 - w / 2, g_H * 0.84f - 60 - h, g_W / 2 + w / 2, g_H * 0.84f - 60);
}
D2D1_POINT_2F VP() { return { g_W / 2, g_H * 0.10f }; }
const float ROW = 19, HEAD = 22 + 17;
D2D1_RECT_F TimelineRect() { return D2D1::RectF(g_W - 84, g_H * 0.08f, g_W - 20, g_H * 0.84f - 60); }
D2D1_POINT_2F TickPt(int i) {  // 0 = agora (embaixo), os backups subindo
    auto t = TimelineRect();
    int n = (int)g_snaps.size();
    float step = n > 1 ? std::min(28.f, (t.bottom - t.top - 20) / (n - 1)) : 0;
    return { (t.left + t.right) / 2, t.bottom - 10 - i * step };
}
D2D1_RECT_F ArrowRect(bool up) { auto f = FrontRect(); float x = f.right + 30; return up ? D2D1::RectF(x, f.bottom - 120, x + 56, f.bottom - 70) : D2D1::RectF(x, f.bottom - 62, x + 56, f.bottom - 12); }
D2D1_RECT_F BtnRect(int k) { float cx = g_W / 2, y = g_H - 44; return k == 0 ? D2D1::RectF(cx - 280, y, cx - 180, y + 26) : D2D1::RectF(cx + 180, y, cx + 280, y + 26); }

void Grad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, std::initializer_list<D2D1_GRADIENT_STOP> st) {
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st.begin(), (UINT32)st.size(), &gs)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    if (lb) lb->Release();
    if (gs) gs->Release();
}
void Nebula(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float rx, float ry, D2D1_COLOR_F col) {
    D2D1_GRADIENT_STOP st[2] = { { 0, col }, { 1, D2D1::ColorF(col.r, col.g, col.b, 0) } };
    ID2D1GradientStopCollection* gs = nullptr; ID2D1RadialGradientBrush* rb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
        SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, { 0, 0 }, rx, ry), gs, &rb)))
        dc->FillEllipse(D2D1::Ellipse(c, rx, ry), rb);
    if (rb) rb->Release();
    if (gs) gs->Release();
}

void DrawWindow(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Snap& s, D2D1_RECT_F r, bool front) {
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 2, r.top + 2, r.right + 2, r.bottom + 8), 7, 7), br);
    br->SetColor(D2D1::ColorF(1, 1, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 5, 5), br);
    float k = (r.right - r.left) / (FrontRect().right - FrontRect().left);  // escala desta janela
    D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(r.left, r.top) * old);
    float W = FrontRect().right - FrontRect().left, H = FrontRect().bottom - FrontRect().top;
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, W, H), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    leopard::Metal(dc, D2D1::RectF(0, 0, W, 22), front, false, true, 5);
    for (int i = 0; i < 3; i++) {  // os três botões (cinza nas de trás)
        D2D1_POINT_2F c{ 14.5f + i * 21.f, 11 };
        D2D1_COLOR_F col = !front ? D2D1::ColorF(0.75f, 0.75f, 0.76f) : i == 0 ? D2D1::ColorF(0.93f, 0.33f, 0.31f) : i == 1 ? D2D1::ColorF(0.97f, 0.74f, 0.25f) : D2D1::ColorF(0.40f, 0.76f, 0.26f);
        br->SetColor(col); dc->FillEllipse(D2D1::Ellipse(c, 6, 6), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f)); dc->DrawEllipse(D2D1::Ellipse(c, 6, 6), br, 0.8f);
    }
    static IDWriteTextFormat *ft = F(13, false, DWRITE_TEXT_ALIGNMENT_CENTER), *fr = F(12, false), *fh = F(11, false);
    std::wstring title = g_folder.substr(g_folder.find_last_of(L'\\') + 1);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.85f));
    if (ft) dc->DrawTextW(title.c_str(), (UINT32)title.size(), ft, D2D1::RectF(80, 0, W - 80, 22), br);
    // cabeçalho da lista e as linhas (listras azul-claras do 10.5)
    Grad(dc, D2D1::RectF(0, 22, W, HEAD), 0, { { 0, D2D1::ColorF(1, 1, 1) }, { 1, D2D1::ColorF(0.90f, 0.90f, 0.90f) } });
    br->SetColor(D2D1::ColorF(0.62f, 0.62f, 0.62f)); dc->DrawLine({ 0, HEAD - 0.5f }, { W, HEAD - 0.5f }, br, 1);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.85f));
    std::wstring hn = Tr(L"Nome"), hd = Tr(L"Data de Modificação");
    if (fh) { dc->DrawTextW(hn.c_str(), (UINT32)hn.size(), fh, D2D1::RectF(30, 22, W * 0.6f, HEAD), br); dc->DrawTextW(hd.c_str(), (UINT32)hd.size(), fh, D2D1::RectF(W * 0.62f, 22, W - 10, HEAD), br); }
    int rows = (int)((H - HEAD) / ROW) + 1;
    for (int i = 0; i < rows; i++) {
        float y = HEAD + i * ROW;
        bool sel = front && i == g_row && i < (int)s.items.size();
        if (sel) { br->SetColor(D2D1::ColorF(56 / 255.f, 117 / 255.f, 215 / 255.f)); dc->FillRectangle(D2D1::RectF(0, y, W, y + ROW), br); }
        else if (i % 2) { br->SetColor(D2D1::ColorF(237 / 255.f, 243 / 255.f, 254 / 255.f)); dc->FillRectangle(D2D1::RectF(0, y, W, y + ROW), br); }
        if (i >= (int)s.items.size()) continue;
        auto& it = s.items[i];
        D2D1_COLOR_F ink = sel ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0, 0, 0, 0.9f);
        // a pastinha azul ou a folha do documento, desenhadas (simples)
        if (it.dir) { br->SetColor(D2D1::ColorF(0.45f, 0.65f, 0.90f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, y + 5, 24, y + 15), 1.5f, 1.5f), br); dc->FillRectangle(D2D1::RectF(10, y + 3.5f, 16, y + 6), br); }
        else { br->SetColor(D2D1::ColorF(0.97f, 0.97f, 0.97f)); dc->FillRectangle(D2D1::RectF(12, y + 3, 22, y + 16), br); br->SetColor(D2D1::ColorF(0.55f, 0.55f, 0.55f)); dc->DrawRectangle(D2D1::RectF(12.5f, y + 3.5f, 21.5f, y + 15.5f), br, 1); }
        br->SetColor(ink);
        if (fr) dc->DrawTextW(it.name.c_str(), (UINT32)it.name.size(), fr, D2D1::RectF(30, y, W * 0.6f, y + ROW), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        std::wstring d = it.dir ? L"--" : DateText(it.mtime ? it.mtime : 1, true);
        if (!it.dir && s.t == 0) {  // agora: a data do arquivo de verdade
            d = DateText(it.mtime, true);
        }
        if (fr) dc->DrawTextW(d.c_str(), (UINT32)d.size(), fr, D2D1::RectF(W * 0.62f, y, W - 10, y + ROW), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (s.items.empty() && front) {
        static IDWriteTextFormat* fe = F(13, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));
        std::wstring e = Tr(L"Pasta vazia");
        if (fe) dc->DrawTextW(e.c_str(), (UINT32)e.size(), fe, D2D1::RectF(0, H / 2 - 10, W, H / 2 + 10), br);
    }
    dc->PopAxisAlignedClip();
    dc->SetTransform(old);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 5, 5), br, 1);
}

void Paint() {
    if (!g_open) return;
    float sc = Sc();
    RECT rc; GetClientRect(g_wnd, &rc);
    auto* dc = ui::GpuBegin(g_s, rc.right, rc.bottom, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    double now = anim::Now();
    float k = (float)std::clamp((now - g_at) / 450.0, 0.0, 1.0); k = anim::Ease(k);
    if (g_closing) k = 1 - k;
    ID2D1Layer* layer = nullptr; dc->CreateLayer(nullptr, &layer);
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), k), nullptr);
    // o espaço: azul-escuro quase preto, nebulosas e as estrelas piscando devagar
    Grad(dc, D2D1::RectF(0, 0, g_W, g_H), 0, { { 0, D2D1::ColorF(0.01f, 0.02f, 0.06f) }, { 1, D2D1::ColorF(0.04f, 0.03f, 0.10f) } });
    Nebula(dc, { g_W * 0.30f, g_H * 0.30f }, g_W * 0.42f, g_H * 0.38f, D2D1::ColorF(0.42f, 0.22f, 0.58f, 0.40f));
    Nebula(dc, { g_W * 0.72f, g_H * 0.22f }, g_W * 0.36f, g_H * 0.30f, D2D1::ColorF(0.18f, 0.32f, 0.70f, 0.35f));
    Nebula(dc, { g_W * 0.52f, g_H * 0.12f }, g_W * 0.20f, g_H * 0.14f, D2D1::ColorF(0.85f, 0.80f, 0.95f, 0.30f));  // o brilho no ponto de fuga
    unsigned seed = 12345;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 8) / 16777216.f; };
    for (int i = 0; i < 520; i++) {
        float x = rnd() * g_W, y = rnd() * g_H, s = 0.5f + rnd() * 1.4f, ph = rnd() * 6.28f;
        float a = 0.35f + 0.65f * (0.5f + 0.5f * std::sin((float)(now / 900.0) + ph));
        br->SetColor(D2D1::ColorF(0.9f, 0.92f, 1, a * (0.4f + 0.6f * rnd())));
        dc->FillEllipse(D2D1::Ellipse({ x, y }, s, s), br);
    }
    // a pilha de janelas: a escolhida na frente, as mais antigas para trás, rumo ao ponto de fuga
    auto f0 = FrontRect(); auto vp = VP();
    D2D1_POINT_2F c0{ (f0.left + f0.right) / 2, (f0.top + f0.bottom) / 2 };
    int first = std::max(0, (int)std::floor(g_pos)), last = std::min((int)g_snaps.size() - 1, first + 9);
    for (int i = last; i >= first; i--) {
        float d = i - g_pos;
        if (d < -0.95f) continue;
        float s = std::pow(0.84f, d) * (0.85f + 0.15f * k);
        D2D1_POINT_2F c{ vp.x + (c0.x - vp.x) * std::pow(0.80f, d), vp.y + (c0.y - vp.y) * std::pow(0.80f, d) };
        if (d < 0) { s = 1 + (-d) * 0.6f; c.y = c0.y + (-d) * g_H * 0.5f; }  // saindo para a frente (indo para o futuro)
        float w = (f0.right - f0.left) * s, h = (f0.bottom - f0.top) * s;
        float a = d < 0 ? 1 + d : std::clamp(1 - d * 0.10f, 0.f, 1.f);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
        DrawWindow(dc, br, g_snaps[i], D2D1::RectF(c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2), std::fabs(d) < 0.5f);
        dc->PopLayer();
    }
    // a linha do tempo na direita
    static IDWriteTextFormat *ftl = F(11, true, DWRITE_TEXT_ALIGNMENT_TRAILING), *fdate = F(16, true, DWRITE_TEXT_ALIGNMENT_CENTER), *fbtn = F(13, false, DWRITE_TEXT_ALIGNMENT_CENTER), *fmsg = F(13, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int i = 0; i < (int)g_snaps.size(); i++) {
        auto p = TickPt(i);
        bool on = i == g_sel, hv = i == g_hoverTick;
        br->SetColor(D2D1::ColorF(1, 1, 1, on ? 1.f : hv ? 0.85f : 0.45f));
        float hw = on ? 22 : hv ? 18 : 12;
        dc->FillRectangle(D2D1::RectF(p.x - hw / 2, p.y - 1.5f, p.x + hw / 2, p.y + 1.5f), br);
        if (on || hv || i == 0) {
            std::wstring t = DateText(g_snaps[i].t, false);
            br->SetColor(D2D1::ColorF(1, 1, 1, on ? 1.f : 0.8f));
            if (ftl) dc->DrawTextW(t.c_str(), (UINT32)t.size(), ftl, D2D1::RectF(p.x - 200, p.y - 9, p.x - 16, p.y + 9), br);
        }
    }
    // as setas (para trás = mais antigo; para a frente = mais novo)
    for (int up = 1; up >= 0; up--) {
        auto r = ArrowRect(up != 0);
        bool en = up ? g_sel + 1 < (int)g_snaps.size() : g_sel > 0;
        Grad(dc, r, 8, { { 0, D2D1::ColorF(0.55f, 0.55f, 0.58f, en ? 0.95f : 0.4f) }, { 1, D2D1::ColorF(0.20f, 0.20f, 0.22f, en ? 0.95f : 0.4f) } });
        br->SetColor(D2D1::ColorF(1, 1, 1, en ? 0.95f : 0.35f));
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        if (up) { dc->DrawLine({ cx - 12, cy + 6 }, { cx, cy - 6 }, br, 4); dc->DrawLine({ cx, cy - 6 }, { cx + 12, cy + 6 }, br, 4); }
        else { dc->DrawLine({ cx - 12, cy - 6 }, { cx, cy + 6 }, br, 4); dc->DrawLine({ cx, cy + 6 }, { cx + 12, cy - 6 }, br, 4); }
    }
    // a barra de baixo: Cancelar, a data da janela da frente e Restaurar
    Grad(dc, D2D1::RectF(0, g_H - 60, g_W, g_H), 0, { { 0, D2D1::ColorF(0.15f, 0.15f, 0.17f, 0.85f) }, { 1, D2D1::ColorF(0.05f, 0.05f, 0.06f, 0.95f) } });
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.18f)); dc->DrawLine({ 0, g_H - 59.5f }, { g_W, g_H - 59.5f }, br, 1);
    std::wstring dt = g_sel < (int)g_snaps.size() ? DateText(g_snaps[g_sel].t, true) : L"";
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
    if (fdate) dc->DrawTextW(dt.c_str(), (UINT32)dt.size(), fdate, D2D1::RectF(g_W / 2 - 170, g_H - 47, g_W / 2 + 170, g_H - 17), br);
    bool canRestore = g_sel > 0 && g_row >= 0 && g_row < (int)g_snaps[g_sel].items.size() && !g_snaps[g_sel].items[g_row].dir;
    for (int b = 0; b < 2; b++) {
        auto r = BtnRect(b);
        bool en = b == 0 || canRestore;
        float rad = (r.bottom - r.top) / 2;
        Grad(dc, r, rad, { { 0, D2D1::ColorF(1, 1, 1, en ? 1.f : 0.5f) }, { 0.5f, D2D1::ColorF(0.93f, 0.93f, 0.93f, en ? 1.f : 0.5f) }, { 0.5f, D2D1::ColorF(0.85f, 0.85f, 0.85f, en ? 1.f : 0.5f) }, { 1, D2D1::ColorF(0.97f, 0.97f, 0.97f, en ? 1.f : 0.5f) } });
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.5f)); dc->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br, 1);
        std::wstring t = b == 0 ? Tr(L"Cancelar") : Tr(L"Restaurar");
        br->SetColor(D2D1::ColorF(0, 0, 0, en ? 0.9f : 0.4f));
        if (fbtn) dc->DrawTextW(t.c_str(), (UINT32)t.size(), fbtn, r, br);
    }
    if (!g_msg.empty()) {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.85f));
        if (fmsg) dc->DrawTextW(g_msg.c_str(), (UINT32)g_msg.size(), fmsg, D2D1::RectF(0, f0.top - 40, g_W, f0.top - 14), br);
    }
    dc->PopLayer();
    layer->Release();
    br->Release();
    ui::GpuEnd(g_s);
}

void HideNow() { KillTimer(g_wnd, T_ANIM); ShowWindow(g_wnd, SW_HIDE); g_open = g_closing = false; g_snaps.clear(); }
void Close() { if (!g_open || g_closing) return; g_closing = true; g_at = anim::Now(); }

void Go(int to) {
    to = std::clamp(to, 0, (int)g_snaps.size() - 1);
    if (to == g_sel) return;
    g_sel = to; g_row = -1;
}

void Restore() {
    if (g_sel <= 0 || g_row < 0 || g_row >= (int)g_snaps[g_sel].items.size()) return;
    auto& it = g_snaps[g_sel].items[g_row];
    if (it.dir || it.src.empty()) return;
    std::wstring dst = g_folder + L"\\" + it.name;
    if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {  // mantém as duas: a restaurada ganha " (restaurado)"
        size_t dot = it.name.find_last_of(L'.');
        std::wstring base = dot == std::wstring::npos ? it.name : it.name.substr(0, dot), ext = dot == std::wstring::npos ? L"" : it.name.substr(dot);
        dst = g_folder + L"\\" + base + L" (" + Tr(L"restaurado") + L")" + ext;
        for (int n = 2; GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; n++)
            dst = g_folder + L"\\" + base + L" (" + Tr(L"restaurado") + L" " + std::to_wstring(n) + L")" + ext;
    }
    bool ok = CopyFileW(it.src.c_str(), dst.c_str(), TRUE) != FALSE;
    Log(L"time machine: restaurar %ls -> %ls: %ls", it.src.c_str(), dst.c_str(), ok ? L"ok" : L"falhou");
    Close();
}

D2D1_POINT_2F Pt(LPARAM l) { float sc = Sc(); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }
bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == T_ANIM) {
            double now = anim::Now(), dt = std::min(100.0, now - g_lastTick); g_lastTick = now;
            float diff = g_sel - g_pos;
            g_pos += diff * (1 - std::exp(-(float)dt / 110.f));
            if (std::fabs(g_sel - g_pos) < 0.002f) g_pos = (float)g_sel;
            Paint();
            if (g_closing && now - g_at > 470) HideNow();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = Pt(l);
        if (In(BtnRect(0), p)) { Close(); return 0; }
        if (In(BtnRect(1), p)) { Restore(); return 0; }
        if (In(ArrowRect(true), p)) { Go(g_sel + 1); return 0; }
        if (In(ArrowRect(false), p)) { Go(g_sel - 1); return 0; }
        for (int i = 0; i < (int)g_snaps.size(); i++) { auto t = TickPt(i); if (std::fabs(p.x - t.x) < 30 && std::fabs(p.y - t.y) < 8) { Go(i); return 0; } }
        auto f = FrontRect();
        if (In(f, p) && p.y > f.top + HEAD) { int r = (int)((p.y - f.top - HEAD) / ROW); g_row = r < (int)g_snaps[g_sel].items.size() ? r : -1; }
        return 0;
    }
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = Pt(l); int hv = -1;
        for (int i = 0; i < (int)g_snaps.size(); i++) { auto t = TickPt(i); if (std::fabs(p.x - t.x) < 30 && std::fabs(p.y - t.y) < 8) hv = i; }
        g_hoverTick = hv;
        return 0;
    }
    case WM_MOUSEWHEEL: Go(g_sel + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1)); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Close();
        else if (w == VK_UP) Go(g_sel + 1);
        else if (w == VK_DOWN) Go(g_sel - 1);
        else if (w == VK_RETURN) Restore();
        return 0;
    case WM_CLOSE: Close(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool tempo::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellTimeMachine"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Time Machine", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    apps::ours.push_back(g_wnd);
    g_s.hwnd = g_wnd;
    return true;
}

bool tempo::IsOpen() { return g_open && !g_closing; }

void tempo::Enter() {
    if (!g_wnd && !tempo::Create(shell::inst)) return;
    if (g_open) { if (!g_closing) Close(); return; }
    Load();
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r = mi.rcMonitor;
    SetWindowPos(g_wnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    float sc = Sc();
    g_W = (r.right - r.left) / sc; g_H = (r.bottom - r.top) / sc;
    g_open = true; g_closing = false; g_sel = 0; g_pos = 0; g_row = -1; g_hoverTick = -1;
    g_at = g_lastTick = anim::Now();
    Paint();
    ShowWindow(g_wnd, SW_SHOW);
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    if (att) AttachThreadInput(me, ft, FALSE);
    SetTimer(g_wnd, T_ANIM, 16, nullptr);
    Log(L"time machine: %zu versões de %ls (backup: %ls)", g_snaps.size(), g_folder.c_str(), g_root.empty() ? L"nenhum" : g_root.c_str());
}
