// Cor e símbolo nas pastas (macOS 26 Tahoe: "Personalizar Pasta"). Botão direito numa pasta > Personalizar
// Pasta: uma cor (a pasta do tema é retingida: mantém a luz e a sombra do desenho e troca o matiz) e um emoji
// no meio. O Finder desenha na hora (pastacor::Draw); para a mesa, o Explorador e as janelas de abrir/salvar
// também mostrarem, é gerado um .ico em %APPDATA%\Shell\pastas e gravado como ícone da pasta (desktop.ini).
// Escolhas em %APPDATA%\Shell\pastas-cor.txt (caminho TAB cor TAB emoji).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include "pastacor.h"

namespace pastacor {

const COLORREF colors[COUNT] = { RGB(255, 69, 58), RGB(255, 159, 10), RGB(255, 214, 10), RGB(48, 209, 88), RGB(100, 210, 255), RGB(10, 132, 255),
                                 RGB(191, 90, 242), RGB(255, 55, 95), RGB(142, 142, 147) };
const wchar_t* names[COUNT] = { L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Menta", L"Azul", L"Roxo", L"Rosa", L"Cinza" };

namespace {

std::map<std::wstring, Info> g_map;  // caminho em minúsculas
bool g_loaded;
std::map<std::wstring, ID2D1Bitmap*> g_bmps;  // "tema|cor" -> pasta retingida
ID2D1DeviceContext* g_bmpDc;

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
std::wstring DataDir() {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = std::wstring(p) + L"\\Shell";
    CoTaskMemFree(p);
    return d;
}
std::wstring MapFile() { return DataDir() + L"\\pastas-cor.txt"; }

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    FILE* f = nullptr;
    if (_wfopen_s(&f, MapFile().c_str(), L"r, ccs=UTF-8") || !f) return;
    wchar_t line[2048];
    while (fgetws(line, 2048, f)) {
        std::wstring s = line; while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        size_t a = s.find(L'\t'), b = a == std::wstring::npos ? a : s.find(L'\t', a + 1);
        if (a == std::wstring::npos) continue;
        Info in; in.color = _wtoi(s.substr(a + 1, b - a - 1).c_str()); if (b != std::wstring::npos) in.emoji = s.substr(b + 1);
        g_map[Lower(s.substr(0, a))] = in;
    }
    fclose(f);
}
void SaveMap() {
    CreateDirectoryW(DataDir().c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, MapFile().c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& [k, v] : g_map) fwprintf(f, L"%ls\t%d\t%ls\n", k.c_str(), v.color, v.emoji.c_str());
    fclose(f);
}

std::wstring PackDir(bool tahoe) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe); PathRemoveFileSpecW(exe);
    return std::wstring(exe) + (tahoe ? L"\\icones\\tahoe\\" : L"\\icones\\sequoia\\");
}

IWICImagingFactory* Wic() {
    static IWICImagingFactory* f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}
bool LoadPng(const std::wstring& file, std::vector<BYTE>& px, UINT& w, UINT& h) {
    IWICImagingFactory* f = Wic();
    if (!f) return false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
              SUCCEEDED(f->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h));
    if (ok) { px.resize((size_t)w * h * 4); ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())); }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    return ok;
}

// troca o matiz e a saturação de cada pixel pelos da cor, mantendo a claridade (luz, sombra e dobra da pasta)
void Tint(std::vector<BYTE>& px, COLORREF c) {
    auto toHsl = [](float r, float g, float b, float& H, float& S, float& L) {
        float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }); L = (mx + mn) / 2;
        if (mx == mn) { H = S = 0; return; }
        float d = mx - mn; S = L > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r) H = (g - b) / d + (g < b ? 6 : 0); else if (mx == g) H = (b - r) / d + 2; else H = (r - g) / d + 4;
        H /= 6;
    };
    auto hue = [](float p, float q, float t) { if (t < 0) t += 1; if (t > 1) t -= 1; if (t < 1.f / 6) return p + (q - p) * 6 * t; if (t < 0.5f) return q; if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6; return p; };
    float th, ts, tl; toHsl(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, th, ts, tl);
    bool gray = ts < 0.05f;
    for (size_t i = 0; i < px.size(); i += 4) {
        float a = px[i + 3] / 255.f;
        if (a <= 0) continue;
        float b = px[i] / 255.f / a, g = px[i + 1] / 255.f / a, r = px[i + 2] / 255.f / a;
        float H, S, L; toHsl(r, g, b, H, S, L);
        if (S < 0.12f) continue;  // partes cinzas/brancas do desenho (papel, brilho) ficam
        float s2 = gray ? 0.04f : std::min(1.f, ts * (0.6f + S * 0.5f));
        float L2 = std::clamp(L + (tl - 0.55f) * 0.35f, 0.f, 1.f);
        float q = L2 < 0.5f ? L2 * (1 + s2) : L2 + s2 - L2 * s2, p = 2 * L2 - q;
        float R = hue(p, q, th + 1.f / 3), G = hue(p, q, th), B = hue(p, q, th - 1.f / 3);
        px[i] = (BYTE)std::lround(std::clamp(B, 0.f, 1.f) * a * 255);
        px[i + 1] = (BYTE)std::lround(std::clamp(G, 0.f, 1.f) * a * 255);
        px[i + 2] = (BYTE)std::lround(std::clamp(R, 0.f, 1.f) * a * 255);
    }
}

bool TintedPixels(bool tahoe, int color, std::vector<BYTE>& px, UINT& w, UINT& h) {
    if (!LoadPng(PackDir(tahoe) + L"folder default.png", px, w, h)) return false;
    if (color >= 0 && color < COUNT) Tint(px, colors[color]);
    return true;
}

IDWriteTextFormat* EmojiFormat(IDWriteFactory* dw, float size) {
    IDWriteTextFormat* tf = nullptr;
    dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &tf);
    if (tf) { tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    return tf;
}
IDWriteFactory* DW() { static IDWriteFactory* f; if (!f) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&f); return f; }

// o emoji fica no meio do corpo da pasta (um pouco abaixo do centro: a aba fica em cima)
D2D1_RECT_F EmojiBox(D2D1_RECT_F box) {
    float S = box.right - box.left, cx = (box.left + box.right) / 2, cy = box.top + S * 0.56f, e = S * 0.40f;
    return D2D1::RectF(cx - e, cy - e / 2 - S * 0.05f, cx + e, cy + e / 2 + S * 0.05f);
}

// .ico de 256 px (PNG dentro) com a pasta retingida e o emoji
bool WriteIco(const std::wstring& ico, bool tahoe, const Info& in) {
    std::vector<BYTE> px; UINT w = 0, h = 0;
    if (!TintedPixels(tahoe, in.color, px, w, h)) return false;
    IWICImagingFactory* f = Wic();
    ID2D1Factory* d2d = nullptr; D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    if (!f || !d2d) return false;
    const UINT S = 256;
    IWICBitmap* wb = nullptr; ID2D1RenderTarget* rt = nullptr; bool ok = false;
    if (SUCCEEDED(f->CreateBitmap(S, S, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wb)) &&
        SUCCEEDED(d2d->CreateWicBitmapRenderTarget(wb, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &rt))) {
        ID2D1Bitmap* b = nullptr;
        rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (b) rt->DrawBitmap(b, D2D1::RectF(0, 0, (float)S, (float)S));
        if (!in.emoji.empty()) {
            D2D1_RECT_F eb = EmojiBox(D2D1::RectF(0, 0, (float)S, (float)S));
            if (IDWriteTextFormat* tf = EmojiFormat(DW(), S * 0.30f)) {
                ID2D1SolidColorBrush* br = nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
                rt->DrawTextW(in.emoji.c_str(), (UINT32)in.emoji.size(), tf, eb, br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                if (br) br->Release(); tf->Release();
            }
        }
        ok = SUCCEEDED(rt->EndDraw());
        if (b) b->Release();
    }
    if (rt) rt->Release();
    d2d->Release();
    if (!ok) { if (wb) wb->Release(); return false; }
    // PNG na memória
    IStream* ms = nullptr; CreateStreamOnHGlobal(nullptr, TRUE, &ms);
    IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fe = nullptr;
    ok = ms && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(ms, WICBitmapEncoderNoCache)) &&
         SUCCEEDED(enc->CreateNewFrame(&fe, nullptr)) && SUCCEEDED(fe->Initialize(nullptr)) && SUCCEEDED(fe->WriteSource(wb, nullptr)) &&
         SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
    if (fe) fe->Release(); if (enc) enc->Release(); wb->Release();
    if (!ok) { if (ms) ms->Release(); return false; }
    STATSTG st{}; ms->Stat(&st, STATFLAG_NONAME);
    std::vector<BYTE> png((size_t)st.cbSize.QuadPart);
    LARGE_INTEGER z{}; ms->Seek(z, STREAM_SEEK_SET, nullptr);
    ULONG got = 0; ms->Read(png.data(), (ULONG)png.size(), &got);
    ms->Release();
    // .ico com uma imagem só (256 x 256, PNG): o Windows reduz para os outros tamanhos
    HANDLE fh = CreateFileW(ico.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    BYTE head[22] = { 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 32, 0 };
    DWORD sz = (DWORD)png.size(), off = 22;
    memcpy(head + 14, &sz, 4); memcpy(head + 18, &off, 4);
    DWORD wr; WriteFile(fh, head, 22, &wr, nullptr); WriteFile(fh, png.data(), sz, &wr, nullptr);
    CloseHandle(fh);
    return true;
}

void ApplySystemIcon(const std::wstring& folder, const Info* in, bool tahoe) {
    SHFOLDERCUSTOMSETTINGS fcs{ sizeof(fcs) };
    fcs.dwMask = FCSM_ICONFILE;
    std::wstring ico;
    if (in) {
        std::wstring dir = DataDir() + L"\\pastas";
        CreateDirectoryW(DataDir().c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
        wchar_t hash[32]; swprintf_s(hash, L"%08x", (unsigned)std::hash<std::wstring>{}(Lower(folder) + L"|" + std::to_wstring(in->color) + L"|" + in->emoji));
        ico = dir + L"\\" + hash + L".ico";
        if (!WriteIco(ico, tahoe, *in)) return;
        fcs.pszIconFile = ico.data(); fcs.cchIconFile = 0; fcs.iIconIndex = 0;
    } else { fcs.pszIconFile = nullptr; fcs.cchIconFile = 0; }
    SHGetSetFolderCustomSettings(&fcs, folder.c_str(), FCS_FORCEWRITE);
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, folder.c_str(), nullptr);
}

}  // namespace

bool Get(const std::wstring& path, Info& out) {
    Load();
    auto it = g_map.find(Lower(path));
    if (it == g_map.end()) return false;
    out = it->second;
    return true;
}

void Set(const std::wstring& path, int color, const std::wstring& emoji, bool tahoe) {
    Load();
    std::wstring k = Lower(path);
    if (color < 0 && emoji.empty()) { g_map.erase(k); SaveMap(); ApplySystemIcon(path, nullptr, tahoe); return; }
    Info in{ color, emoji };
    g_map[k] = in;
    SaveMap();
    ApplySystemIcon(path, &in, tahoe);
}

bool DrawColor(ID2D1DeviceContext* dc, int color, D2D1_RECT_F box, bool tahoe) {
    if (color < 0 || color >= COUNT) return false;
    Info in; in.color = color;
    return DrawInfo(dc, in, box, tahoe);
}

bool Draw(ID2D1DeviceContext* dc, const std::wstring& path, D2D1_RECT_F box, bool tahoe) {
    Info in;
    if (!Get(path, in)) return false;
    return DrawInfo(dc, in, box, tahoe);
}

bool DrawInfo(ID2D1DeviceContext* dc, const Info& in, D2D1_RECT_F box, bool tahoe) {
    if (g_bmpDc != dc) { for (auto& [k, b] : g_bmps) if (b) b->Release(); g_bmps.clear(); g_bmpDc = dc; }
    std::wstring key = std::wstring(tahoe ? L"t|" : L"s|") + std::to_wstring(in.color);
    auto it = g_bmps.find(key);
    if (it == g_bmps.end()) {
        std::vector<BYTE> px; UINT w = 0, h = 0;
        ID2D1Bitmap* b = nullptr;
        if (TintedPixels(tahoe, in.color, px, w, h))
            dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
        it = g_bmps.emplace(key, b).first;
    }
    if (!it->second) return false;
    float S = box.right - box.left, g = tahoe ? S * 0.05f : 0;
    dc->DrawBitmap(it->second, D2D1::RectF(box.left - g, box.top - g, box.right + g, box.bottom + g), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    if (!in.emoji.empty() && S >= 14) {
        static std::map<int, IDWriteTextFormat*> fmts;
        int sz = std::max(6, (int)std::lround(S * 0.30f));
        IDWriteTextFormat*& tf = fmts[sz];
        if (!tf) tf = EmojiFormat(DW(), (float)sz);
        if (tf) {
            ID2D1SolidColorBrush* br = nullptr; dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
            dc->DrawTextW(in.emoji.c_str(), (UINT32)in.emoji.size(), tf, EmojiBox(box), br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            if (br) br->Release();
        }
    }
    return true;
}

const std::vector<std::wstring>& Emojis() {
    static const std::vector<std::wstring> v = { L"⭐", L"❤️", L"🎵", L"🎧", L"🎬", L"📷", L"💼", L"📚", L"📝", L"💻", L"🎮", L"🏠", L"✈️", L"💡", L"🔒", L"🎨", L"💰", L"🧾", L"📦", L"⚙️" };
    return v;
}

}  // namespace pastacor
