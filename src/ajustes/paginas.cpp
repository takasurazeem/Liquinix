// As páginas do Ajustes do Sistema, na ordem e com os nomes do macOS Sequoia (pt-BR). Cada uma
// mostra e muda o equivalente do Windows; o que não tem como ser feito aqui abre a página do Windows.
#include "ajustes.h"
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include "../comum/simbolos.h"  // símbolos SF da prévia do Liquid Glass (Golden Gate)
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

void AskText(const std::wstring& title, const std::wstring& message, const std::wstring& okLabel, bool password,
             std::function<void(const std::wstring&)> ok);

using namespace aj;

Page Armazenamento();

namespace {

// cores dos quadradinhos (as do Ajustes do Mac)
const UINT32 C_BLUE = 0x0A84FF, C_GREEN = 0x34C759, C_GRAY = 0x8E8E93, C_DARK = 0x2C2C2E, C_RED = 0xFF3B30, C_PINK = 0xFF2D55,
             C_INDIGO = 0x5856D6, C_PURPLE = 0xAF52DE, C_CYAN = 0x32ADE6, C_ORANGE = 0xFF9500, C_TEAL = 0x30B0C7;

std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list a; va_start(a, f); vswprintf_s(buf, Tr(f), a); va_end(a);  // o formato no idioma escolhido (mesmos %d/%ls)
    return buf;
}

std::wstring Size(ULONGLONG b) {
    double gb = b / 1e9;
    return gb >= 1000 ? Fmt(L"%.2f TB", gb / 1000) : gb >= 10 ? Fmt(L"%.0f GB", gb) : Fmt(L"%.1f GB", gb);
}

Row Value(const std::wstring& label, std::function<std::wstring()> v) { Row r; r.kind = Kind::Value; r.label = label; r.value = v; return r; }
Row Value(const std::wstring& label, const std::wstring& v) { return Value(label, [v] { return v; }); }
Row Toggle(const std::wstring& label, std::function<bool()> get, std::function<void(bool)> set, const std::wstring& detail = L"") {
    Row r; r.kind = Kind::Toggle; r.label = label; r.get = get; r.set = set; r.detail = detail; return r;
}
Row Popup(const std::wstring& label, std::vector<std::wstring> opts, std::function<int()> get, std::function<void(int)> set) {
    Row r; r.kind = Kind::Popup; r.label = label; r.options = opts; r.getIdx = get; r.setIdx = set; return r;
}
Row Slider(const std::wstring& label, float mn, float mx, std::function<float()> get, std::function<void(float)> set,
           const std::wstring& minG = L"", const std::wstring& maxG = L"") {
    Row r; r.kind = Kind::Slider; r.label = label; r.min = mn; r.max = mx; r.getF = get; r.setF = set; r.minGlyph = minG; r.maxGlyph = maxG; return r;
}
Row Link(const std::wstring& label, std::function<void()> click, wchar_t icon = 0, UINT32 color = 0, std::function<std::wstring()> v = nullptr) {
    Row r; r.kind = Kind::Link; r.label = label; r.click = click; r.icon = icon; r.iconColor = color; r.value = v; return r;
}
Row Uri(const std::wstring& label, const std::wstring& uri, wchar_t icon = 0, UINT32 color = 0) {
    return Link(label, [uri] { OpenUri(uri); }, icon, color);
}
Row Button(const std::wstring& label, const std::wstring& button, std::function<void()> click, const std::wstring& detail = L"") {
    Row r; r.kind = Kind::Button; r.label = label; r.button = button; r.click = click; r.detail = detail; return r;
}
Row Note(const std::wstring& text) {  // linha só de texto cinza (explicação)
    Row r; r.kind = Kind::Custom;
    r.height = 0;
    r.draw = [text](Ctx& c, D2D1_RECT_F rr) {
        c.note->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        Text(c, text, D2D1::RectF(rr.left + 10, rr.top + 9, rr.right - 10, rr.bottom), c.note, c.text2);
        c.note->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    r.height = 38;
    return r;
}
Group G(std::vector<Row> rows, const std::wstring& title = L"", const std::wstring& sub = L"") { Group g; g.rows = std::move(rows); g.title = title; g.subtitle = sub; return g; }

// botões alinhados à direita, fora das caixas (ex.: "Avançado…", "Relatório do Sistema…")
Row Buttons(std::vector<std::pair<std::wstring, std::function<void()>>> bs) {
    Row r; r.kind = Kind::Custom; r.height = 34;
    r.draw = [bs](Ctx& c, D2D1_RECT_F rr) {
        float right = rr.right;
        for (int i = (int)bs.size() - 1; i >= 0; i--) {
            float w = 0;
            IDWriteTextLayout* tl = nullptr;
            if (SUCCEEDED(gfx::dw->CreateTextLayout(bs[i].first.c_str(), (UINT32)bs[i].first.size(), c.center, 400, 30, &tl))) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); w = m.width; tl->Release(); }
            w = std::max(w + (tahoe::On() ? 28 : 22), 60.f);
            D2D1_RECT_F b = tahoe::On() ? D2D1::RectF(right - w, rr.top + 4, right, rr.top + 30) : D2D1::RectF(right - w, rr.top + 6, right, rr.top + 28);
            ButtonFace(c, b, bs[i].first);
            right = b.left - 10;
        }
    };
    r.clickAt = [bs](D2D1_POINT_2F p, D2D1_RECT_F rr) {
        float right = rr.right;
        for (int i = (int)bs.size() - 1; i >= 0; i--) {
            float w = 0;
            IDWriteTextLayout* tl = nullptr;
            IDWriteTextFormat* tf = nullptr;
            gfx::dw->CreateTextFormat(L"SF Pro Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"pt-br", &tf);
            if (tf && SUCCEEDED(gfx::dw->CreateTextLayout(bs[i].first.c_str(), (UINT32)bs[i].first.size(), tf, 400, 30, &tl))) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); w = m.width; tl->Release(); }
            if (tf) tf->Release();
            w = std::max(w + (tahoe::On() ? 28 : 22), 60.f);
            if (p.x >= right - w && p.x <= right && p.y >= rr.top + 4 && p.y <= rr.top + 30) { bs[i].second(); return; }
            right -= w + 10;
        }
    };
    return r;
}
Group Plain(Row r) { Group g; g.box = false; g.rows.push_back(r); return g; }

// cabeçalho grande no topo de páginas como Wi-Fi e Bluetooth (ícone + título + descrição)
Row Hero(wchar_t glyph, UINT32 color, const std::wstring& title, const std::wstring& text) {
    Row r; r.kind = Kind::Custom; r.height = 112;
    r.draw = [=](Ctx& c, D2D1_RECT_F rr) {
        float cx = (rr.left + rr.right) / 2;
        Tile(c, D2D1::RectF(cx - 24, rr.top + 14, cx + 24, rr.top + 62), glyph, color, SymbolFor(title));
        IDWriteTextFormat* tf = nullptr;
        gfx::dw->CreateTextFormat(L"SF Pro Display", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15, L"pt-br", &tf);
        if (tf) { tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); Text(c, title, D2D1::RectF(rr.left + 10, rr.top + 66, rr.right - 10, rr.top + 86), tf, c.text); tf->Release(); }
        c.note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        c.note->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        Text(c, text, D2D1::RectF(rr.left + 40, rr.top + 87, rr.right - 40, rr.bottom), c.note, c.text2);
        c.note->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    return r;
}

// ---- reaproveitados por várias páginas
const std::vector<int> MINUTES = { 1, 2, 3, 5, 10, 15, 20, 30, 45, 60, 120, 180, 300, 0 };
std::vector<std::wstring> MinuteLabels(const wchar_t* prefix) {
    std::vector<std::wstring> r;
    for (int m : MINUTES) r.push_back(m == 0 ? L"Nunca" : m < 60 ? Fmt(L"%ls%d minuto%ls", Tr(prefix), m, m > 1 ? L"s" : L"") : Fmt(L"%ls%d hora%ls", Tr(prefix), m / 60, m >= 120 ? L"s" : L""));
    return r;
}
int MinuteIndex(int m) {
    if (m == 0) return (int)MINUTES.size() - 1;
    for (int i = 0; i < (int)MINUTES.size() - 1; i++) if (MINUTES[i] >= m) return i;
    return (int)MINUTES.size() - 2;
}

// ================= Conta
Page Conta() {
    const sys::Info& inf = sys::About();
    Page p;
    Row head; head.kind = Kind::Custom; head.height = 150;
    head.draw = [&inf](Ctx& c, D2D1_RECT_F rr) {
        float cx = (rr.left + rr.right) / 2;
        D2D1_POINT_2F m{ cx, rr.top + 52 };
        if (ID2D1Bitmap* pic = sys::AccountPicture()) {
            ID2D1Factory* f = nullptr; c.dc->GetFactory(&f);
            ID2D1EllipseGeometry* eg = nullptr; f->CreateEllipseGeometry(D2D1::Ellipse(m, 40, 40), &eg);
            c.dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), eg), nullptr);
            c.dc->DrawBitmap(pic, D2D1::RectF(m.x - 40, m.y - 40, m.x + 40, m.y + 40), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            c.dc->PopLayer();
            eg->Release(); f->Release();
        } else {
            c.br->SetColor(D2D1::ColorF(0.6f, 0.6f, 0.63f, 1));
            c.dc->FillEllipse(D2D1::Ellipse(m, 40, 40), c.br);
        }
        IDWriteTextFormat* tf = nullptr;
        gfx::dw->CreateTextFormat(L"SF Pro Display", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"pt-br", &tf);
        if (tf) { tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); Text(c, inf.fullName, D2D1::RectF(rr.left, rr.top + 100, rr.right, rr.top + 126), tf, c.text); tf->Release(); }
        c.note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(c, inf.user + L" · " + inf.computer, D2D1::RectF(rr.left, rr.top + 126, rr.right, rr.top + 144), c.note, c.text2);
        c.note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    p.groups.push_back(Plain(head));
    p.groups.push_back(G({ Uri(L"Informações Pessoais", L"ms-settings:yourinfo", 0xE77B, C_GRAY),
                           Uri(L"Início de Sessão e Segurança", L"ms-settings:signinoptions", 0xE72E, C_GRAY),
                           Uri(L"E-mail e Contas", L"ms-settings:emailandaccounts", L'@', C_BLUE) }));
    p.groups.push_back(G({ Uri(L"Família", L"ms-settings:family-group", 0xE716, C_BLUE),
                           Uri(L"Backup do Windows (OneDrive)", L"ms-settings:backup", 0xE753, C_CYAN) }));
    return p;
}

// ================= Wi-Fi
Page WiFi() {
    Page p;
    int st = sys::RadioState(sys::RADIO_WIFI);
    if (!sys::WifiPresent() && st == -1) {
        p.groups.push_back(Plain(Hero(0xE701, C_BLUE, L"Wi‑Fi", L"Nenhum adaptador de Wi‑Fi foi encontrado neste computador. A conexão de rede está em Rede.")));
        p.groups.push_back(G({ Link(L"Rede", [] { int i = SidebarIndexFor(L"rede"); (void)i; OpenUri(L"ms-settings:network"); }, 0xE774, C_BLUE) }));
        return p;
    }
    // como no macOS 26: ícone, título com a explicação embaixo e a chave à direita
    Row t = Toggle(L"Wi‑Fi", [] { return sys::RadioState(sys::RADIO_WIFI) == 1; }, [](bool on) { sys::SetRadio(sys::RADIO_WIFI, on); },
                   L"Configure o Wi‑Fi para conectar este computador à internet sem fio. Ligue o Wi‑Fi e escolha uma rede.");
    t.icon = 0xE701; t.iconColor = C_BLUE;
    t.height = 64;
    std::wstring cur = sys::WifiCurrent();
    Group top = G({ t });
    if (!cur.empty() && st != 0) {  // rede atual: nome, "● Conectado" em verde, cadeado, sinal e Detalhes…
        int sig = 100; bool sec = true;
        for (auto& n : sys::WifiNetworks()) if (n.connected) { sig = n.signal; sec = n.secure; }
        Row r; r.kind = Kind::Custom; r.height = 54;
        r.draw = [cur, sig, sec](Ctx& c, D2D1_RECT_F rr) {
            Text(c, cur, D2D1::RectF(rr.left + 10, rr.top + 7, rr.right - 160, rr.top + 27), c.body, c.text);
            c.br->SetColor(D2D1::ColorF(0.20f, 0.78f, 0.35f, 1));
            c.dc->FillEllipse(D2D1::Ellipse({ rr.left + 15, rr.top + 37 }, 4.5f, 4.5f), c.br);
            Text(c, L"Conectado", D2D1::RectF(rr.left + 25, rr.top + 27, rr.right - 160, rr.top + 47), c.note, c.text2);
            D2D1_RECT_F b = D2D1::RectF(rr.right - 10 - 96, (rr.top + rr.bottom) / 2 - 13, rr.right - 10, (rr.top + rr.bottom) / 2 + 13);
            ButtonFace(c, b, L"Detalhes…");
            float x = b.left - 12;
            D2D1_COLOR_F w = c.text; w.a *= sig > 66 ? 1 : sig > 33 ? 0.75f : 0.5f;
            Text(c, std::wstring(1, (wchar_t)0xE701), D2D1::RectF(x - 18, rr.top, x, rr.bottom), c.glyphSmall, w);
            if (sec) Text(c, std::wstring(1, (wchar_t)0xE72E), D2D1::RectF(x - 40, rr.top, x - 22, rr.bottom), c.glyphSmall, c.text);
        };
        r.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) { if (pt.x > rr.right - 110) OpenUri(L"ms-settings:network-wifi"); };
        top.rows.push_back(r);
    }
    p.groups.push_back(top);
    if (st == 0) return p;
    auto nets = sys::WifiNetworks();
    Group known = G({}, L"Redes Conhecidas"), other = G({}, L"Outras Redes");
    for (auto& n : nets) {
        if (n.connected) continue;
        Row r; r.kind = Kind::Custom; r.height = 42;
        std::wstring ssid = n.ssid; bool secure = n.secure, isKnown = n.known; int signal = n.signal;
        r.draw = [ssid, secure, signal](Ctx& c, D2D1_RECT_F rr) {
            Text(c, ssid, D2D1::RectF(rr.left + 30, rr.top, rr.right - 80, rr.bottom), c.body, c.text);
            float x = rr.right - 12;
            D2D1_COLOR_F weak = c.text; weak.a *= signal > 66 ? 1 : signal > 33 ? 0.7f : 0.45f;
            Text(c, std::wstring(1, (wchar_t)0xE701), D2D1::RectF(x - 18, rr.top, x, rr.bottom), c.glyphSmall, weak);
            if (secure) Text(c, std::wstring(1, (wchar_t)0xE72E), D2D1::RectF(x - 40, rr.top, x - 22, rr.bottom), c.glyphSmall, c.text);
        };
        r.clickAt = [ssid, secure, isKnown](D2D1_POINT_2F, D2D1_RECT_F) {
            if (isKnown || !secure) { sys::WifiConnect(ssid, L"", secure); Rebuild(); return; }
            AskText(Tr(L"A rede Wi‑Fi “") + ssid + Tr(L"” requer uma senha."), L"", L"Conectar", true,
                    [ssid](const std::wstring& pw) { sys::WifiConnect(ssid, pw, true); Rebuild(); });
        };
        (isKnown ? known : other).rows.push_back(r);
    }
    if (!known.rows.empty()) p.groups.push_back(known);
    if (!other.rows.empty()) p.groups.push_back(other);
    if (!cur.empty()) p.groups.push_back(G({ Button(Tr(L"Desconectar de “") + cur + L"”", L"Desconectar", [] { sys::WifiDisconnect(); Rebuild(); }) }));
    p.groups.push_back(Plain(Buttons({ { L"Avançado…", [] { OpenUri(L"ms-settings:network-wifisettings"); } } })));
    return p;
}

// ================= Bluetooth
Page Bluetooth() {
    Page p;
    int st = sys::RadioState(sys::RADIO_BT);
    if (st == -1) {
        p.groups.push_back(Plain(Hero(0xE702, C_BLUE, L"Bluetooth", L"Nenhum adaptador Bluetooth foi encontrado neste computador.")));
        return p;
    }
    wchar_t name[64]; DWORD n = 64; GetComputerNameW(name, &n);
    Row t = Toggle(L"Bluetooth", [] { return sys::RadioState(sys::RADIO_BT) == 1; }, [](bool on) { sys::SetRadio(sys::RADIO_BT, on); },
                   L"Conecte acessórios para ouvir música, digitar e jogar.");
    t.icon = 0xE702; t.iconColor = C_BLUE;
    Group top = G({ t });
    if (st == 1) top.rows.push_back(Note(std::wstring(Tr(L"Este computador está detectável como “")) + name + Tr(L"” enquanto os ajustes do Bluetooth estão abertos.")));
    p.groups.push_back(top);
    if (st == 0) return p;
    Group mine = G({}, L"Meus Dispositivos");
    for (auto& d : sys::BluetoothDevices()) {
        // ícone pelo tipo (classe Bluetooth: periférico = mouse/teclado, áudio = fone), nome e o estado embaixo, (i) à direita
        Row r; r.kind = Kind::Custom; r.height = 52;
        std::wstring nm = d.name; bool con = d.connected; ULONG cls = d.cls;
        r.draw = [nm, con, cls](Ctx& c, D2D1_RECT_F rr) {
            ULONG major = (cls >> 8) & 0x1F, minor = (cls >> 2) & 0x3F;
            wchar_t g = major == 4 ? 0xE7F6 /*fone*/ : major == 5 ? ((minor & 0x10) ? 0xE765 /*teclado*/ : 0xE962 /*mouse*/) : major == 2 ? 0xE8EA /*celular*/ : 0xE702;
            Text(c, std::wstring(1, g), D2D1::RectF(rr.left + 10, rr.top, rr.left + 36, rr.bottom), c.glyph, c.text2);
            Text(c, nm, D2D1::RectF(rr.left + 46, rr.top + 7, rr.right - 50, rr.top + 27), c.body, c.text);
            Text(c, con ? L"Conectado" : L"Não Conectado", D2D1::RectF(rr.left + 46, rr.top + 27, rr.right - 50, rr.top + 45), c.note, c.text2);
            float cx = rr.right - 22, cy = (rr.top + rr.bottom) / 2;  // (i)
            c.br->SetColor(c.text2);
            c.dc->DrawEllipse(D2D1::Ellipse({ cx, cy }, 9, 9), c.br, 1.2f);
            Text(c, L"i", D2D1::RectF(cx - 9, cy - 9, cx + 9, cy + 9), c.center, c.text2);
        };
        r.clickAt = [](D2D1_POINT_2F, D2D1_RECT_F) { OpenUri(L"ms-settings:bluetooth"); };
        mine.rows.push_back(r);
    }
    if (mine.rows.empty()) mine.rows.push_back(Note(L"Nenhum dispositivo emparelhado."));
    p.groups.push_back(mine);
    p.groups.push_back(G({ Button(L"Novo dispositivo", L"Adicionar…", [] {
        wchar_t sysdir[MAX_PATH]; GetSystemDirectoryW(sysdir, MAX_PATH);
        ShellExecuteW(nullptr, L"open", (std::wstring(sysdir) + L"\\DevicePairingWizard.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }) }, L"Dispositivos Próximos"));
    p.groups.push_back(Plain(Buttons({ { L"Avançado…", [] { OpenUri(L"ms-settings:bluetooth"); } } })));
    return p;
}

// ================= Rede
Page RedeDetalhe(sys::NetIf n) {
    Page p;
    p.groups.push_back(G({ Value(L"Status", n.up ? L"Conectado" : L"Não conectado"), Value(L"Tipo", n.kind), Value(L"Endereço IP", n.ip.empty() ? L"—" : n.ip) }));
    p.groups.push_back(Plain(Buttons({ { L"Ajustes do Adaptador…", [] { OpenUri(L"ms-settings:network-advancedsettings"); } } })));
    return p;
}

Page Rede() {
    Page p;
    Group ifs = G({});
    for (auto& n : sys::Interfaces()) {
        Row r = Link(n.name, [n] { Push(n.name, [n] { return RedeDetalhe(n); }); }, n.kind == L"Ethernet" ? (wchar_t)0xE839 : (wchar_t)0xE701, C_BLUE,
                     [up = n.up] { return std::wstring(up ? L"Conectado" : L"Não conectado"); });
        ifs.rows.push_back(r);
    }
    if (ifs.rows.empty()) ifs.rows.push_back(Note(L"Nenhum adaptador de rede encontrado."));
    p.groups.push_back(ifs);
    p.groups.push_back(G({ Uri(L"Firewall", L"windowsdefender://network", 0xE83D, C_RED),
                           Uri(L"VPN", L"ms-settings:network-vpn", 0xE705, C_BLUE),
                           Uri(L"Proxies", L"ms-settings:network-proxy", 0xE774, C_GRAY) }, L"Outros Serviços"));
    return p;
}

// ================= Energia / Bateria
Page Energia() {
    Page p;
    int pct = 0; bool charging = false;
    if (sys::HasBattery(&pct, &charging))
        p.groups.push_back(G({ Value(L"Nível da Bateria", Fmt(L"%d%%%ls", pct, charging ? Tr(L" (carregando)") : L"")) }));
    p.groups.push_back(G({ Popup(L"Modo de Energia", { L"Economia de Energia", L"Equilibrado", L"Melhor Desempenho" }, [] { return sys::PowerMode(); },
                                 [](int i) { sys::SetPowerMode(i); }) }));
    p.groups.push_back(G({ Popup(L"Desligar a tela quando inativo", MinuteLabels(L"Após "), [] { return MinuteIndex(sys::DisplayOffMinutes()); },
                                 [](int i) { sys::SetDisplayOffMinutes(MINUTES[i]); }),
                           Popup(L"Repousar quando inativo", MinuteLabels(L"Após "), [] { return MinuteIndex(sys::SleepMinutes()); },
                                 [](int i) { sys::SetSleepMinutes(MINUTES[i]); }) }));
    // App Nap do Mac (shell\memoria.cpp): o Shell põe no modo eficiência do Windows quem está fora de vista
    p.groups.push_back(G({ Toggle(L"Soneca de Apps", [] { return Setting(L"app_nap", L"1") != L"0"; },
                                  [](bool v) { SetSetting(L"app_nap", v ? L"1" : L"0"); },
                                  L"Programas com todas as janelas escondidas, minimizadas ou cobertas por 30 segundos passam a gastar menos processador, como o App Nap do Mac. Voltam ao normal assim que aparecem. Não vale para o que está tocando ou gravando som.") }));
    // Modo Jogo do Mac (shell\jogo.cpp): jogo na frente → o Shell desliga o que mais pesa até o jogo fechar
    p.groups.push_back(G({ Toggle(L"Modo Jogo", [] { return Setting(L"modo_jogo", L"1") != L"0"; },
                                  [](bool v) { SetSetting(L"modo_jogo", v ? L"1" : L"0"); },
                                  L"Quando um jogo vem para a frente, o Liquinix desliga o vidro real, os efeitos que acompanham o mouse e os Cantos Ativos e passa a usar menos processador, até o jogo fechar. Jogos que não forem reconhecidos podem ser postos em jogos.txt, na pasta de ajustes.") }));
    p.groups.push_back(Plain(Buttons({ { L"Opções de Energia…", [] { OpenUri(L"ms-settings:powersleep"); } } })));
    return p;
}

// ================= Geral
Page Sobre() {
    const sys::Info& i = sys::About();
    Page p;
    Row head; head.kind = Kind::Custom; head.height = 150;
    std::wstring model = i.model.empty() ? L"PC" : i.model;
    head.draw = [model](Ctx& c, D2D1_RECT_F rr) {
        float cx = (rr.left + rr.right) / 2;
        // monitor desenhado (no lugar da foto do Mac)
        D2D1_RECT_F scr = D2D1::RectF(cx - 52, rr.top + 14, cx + 52, rr.top + 80);
        c.br->SetColor(D2D1::ColorF(0.12f, 0.12f, 0.13f, 1));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(scr, 5, 5), c.br);
        ID2D1GradientStopCollection* st = nullptr;
        D2D1_GRADIENT_STOP gs[2] = { { 0, D2D1::ColorF(0.24f, 0.55f, 0.95f, 1) }, { 1, D2D1::ColorF(0.55f, 0.32f, 0.85f, 1) } };
        c.dc->CreateGradientStopCollection(gs, 2, &st);
        ID2D1LinearGradientBrush* lg = nullptr;
        if (st) c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ scr.left, scr.top }, { scr.right, scr.bottom }), st, &lg);
        if (lg) { c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(scr.left + 3, scr.top + 3, scr.right - 3, scr.bottom - 3), 3, 3), lg); lg->Release(); }
        if (st) st->Release();
        c.br->SetColor(c.dark ? D2D1::ColorF(0.6f, 0.6f, 0.62f, 1) : D2D1::ColorF(0.72f, 0.72f, 0.74f, 1));
        c.dc->FillRectangle(D2D1::RectF(cx - 8, scr.bottom, cx + 8, scr.bottom + 12), c.br);
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 24, scr.bottom + 11, cx + 24, scr.bottom + 15), 2, 2), c.br);
        IDWriteTextFormat* tf = nullptr;
        gfx::dw->CreateTextFormat(L"SF Pro Display", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"pt-br", &tf);
        if (tf) { tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); Text(c, model, D2D1::RectF(rr.left, rr.top + 102, rr.right, rr.top + 128), tf, c.text); tf->Release(); }
    };
    p.groups.push_back(Plain(head));
    p.groups.push_back(G({ Value(L"Nome", i.computer), Value(L"Processador", i.cpu), Value(L"Memória", i.ram), Value(L"Gráficos", i.gpu) }));
    p.groups.push_back(G({ Value(L"Windows", i.windows), Value(L"Compilação", i.build) }, L"Sistema"));
    auto mode = sys::CurrentResolution();
    p.groups.push_back(G({ Button(L"Tela", L"Ajustes de Telas…", [] { int k = SidebarIndexFor(L"telas"); if (k >= 0) { HWND w = Window(); COPYDATASTRUCT cd{ 0x414A5354, 10, (void*)L"telas" }; SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cd); } },
                                  Fmt(L"%d × %d", mode.w, mode.h)) }, L"Telas"));
    std::vector<Row> disks;
    for (auto& d : sys::Disks())
        disks.push_back(Button(d.label.empty() ? d.root : d.label + L" (" + d.root.substr(0, 2) + L")", L"Ajustes de Armazenamento…",
                               [] { Push(L"Armazenamento", Armazenamento); },
                               Size(d.free) + Tr(L" disponíveis de ") + Size(d.total)));
    if (!disks.empty()) p.groups.push_back(G(disks, L"Armazenamento"));
    p.groups.push_back(Plain(Buttons({ { L"Relatório do Sistema…", [] { ShellExecuteW(nullptr, L"open", L"msinfo32.exe", nullptr, nullptr, SW_SHOWNORMAL); } } })));
    return p;
}

}  // namespace

Page Armazenamento() {
    Page p;
    for (auto& d : sys::Disks()) {
        Row r; r.kind = Kind::Custom; r.height = 84;
        std::wstring name = (d.label.empty() ? std::wstring(Tr(L"Disco Local")) : d.label) + L" (" + d.root.substr(0, 2) + L")";
        ULONGLONG total = d.total, freeB = d.free;
        r.draw = [name, total, freeB](Ctx& c, D2D1_RECT_F rr) {
            Text(c, name, D2D1::RectF(rr.left + 12, rr.top + 10, rr.right - 12, rr.top + 28), c.bold, c.text);
            Text(c, Size(total - freeB) + Tr(L" usados de ") + Size(total), D2D1::RectF(rr.left + 12, rr.top + 28, rr.right - 12, rr.top + 44), c.note, c.text2);
            D2D1_RECT_F bar = D2D1::RectF(rr.left + 12, rr.top + 52, rr.right - 12, rr.top + 66);
            c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.07f));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 4, 4), c.br);
            float k = total ? (float)(total - freeB) / total : 0;
            c.br->SetColor(D2D1::ColorF(1.0f, 0.62f, 0.04f, 1));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * k, bar.bottom), 4, 4), c.br);
        };
        p.groups.push_back(G({ r }));
    }
    p.groups.push_back(Plain(Buttons({ { L"Recomendações…", [] { OpenUri(L"ms-settings:storagesense"); } } })));
    return p;
}

namespace {

// ---- Atualização do Liquinix pelo GitHub (tools\atualizar-github.ps1). Ao abrir a página o Ajustes confere sozinho,
// numa thread, a versão instalada (bin\versao.txt), a mais nova publicada e os commits que faltam; o botão só
// aparece quando há o que instalar e roda o mesmo script, que baixa e troca só o que mudou.
static std::wstring BinDir();
struct Upd { int state = 0; std::wstring atual, novo, data, erro; std::vector<std::wstring> faltam; };  // 0 nada, 1 procurando, 2 pronto
static Upd g_upd;
static std::mutex g_updMu;
static std::wstring UpdScript() {
    std::wstring root = BinDir(); root = root.substr(0, root.find_last_of(L'\\'));
    return root + L"\\tools\\atualizar-github.ps1";
}
static void UpdCheck() {
    { std::lock_guard<std::mutex> lk(g_updMu); if (g_upd.state == 1) return; g_upd.state = 1; }
    std::thread([] {
        Upd u; u.state = 2;
        wchar_t tmp[MAX_PATH], f[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); GetTempFileNameW(tmp, L"wac", 0, f);
        std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + UpdScript() + L"\" -Verificar -Saida \"" + f + L"\" -Bin \"" + BinDir() + L"\"";
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 45000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        FILE* fp = nullptr;
        if (!_wfopen_s(&fp, f, L"r, ccs=UTF-8") && fp) {
            wchar_t line[1024];
            while (fgetws(line, 1024, fp)) {
                std::wstring l = line;
                while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
                if (!l.empty() && l[0] == 0xFEFF) l.erase(0, 1);
                auto val = [&](const wchar_t* k) { size_t n = wcslen(k); return l.compare(0, n, k) == 0 ? l.substr(n) : std::wstring(1, 0); };
                if (auto v = val(L"atual="); v != std::wstring(1, 0)) u.atual = v;
                else if (auto v2 = val(L"novo="); v2 != std::wstring(1, 0)) u.novo = v2;
                else if (auto v3 = val(L"data="); v3 != std::wstring(1, 0)) u.data = v3;
                else if (auto v4 = val(L"erro="); v4 != std::wstring(1, 0)) u.erro = v4;
                else if (auto v5 = val(L"commit="); v5 != std::wstring(1, 0)) u.faltam.push_back(v5);
            }
            fclose(fp);
        } else u.erro = Tr(L"não deu para conferir agora");
        DeleteFileW(f);
        if (u.novo.empty() && u.erro.empty()) u.erro = Tr(L"não deu para conferir agora");
        { std::lock_guard<std::mutex> lk(g_updMu); g_upd = u; }
        aj::Rebuild();
    }).detach();
}

Page Atualizacao() {
    const sys::Info& i = sys::About();
    Page p;
    Upd u;
    { std::lock_guard<std::mutex> lk(g_updMu); u = g_upd; }
    if (u.state == 0) { UpdCheck(); u.state = 1; }
    bool tem = u.state == 2 && !u.novo.empty() && u.novo != u.atual;
    std::vector<Row> rows;
    rows.push_back(Value(L"Versão instalada", u.atual.empty() ? (u.state == 2 ? Tr(L"desconhecida") : Tr(L"Procurando…")) : u.atual));
    rows.push_back(Value(L"Versão mais nova", u.state == 1 ? Tr(L"Procurando…") : !u.erro.empty() ? u.erro : u.novo + (u.data.empty() ? L"" : L" (" + u.data + L")")));
    if (tem) {
        std::wstring label = u.faltam.empty() ? Tr(L"Há uma versão nova do Liquinix.")
                           : std::to_wstring(u.faltam.size()) + (u.faltam.size() == 1 ? Tr(L" mudança para instalar.") : Tr(L" mudanças para instalar."));
        rows.push_back(Button(label, L"Atualizar Agora", [] {
            // o instalador do Liquinix (LiquinixSetup.exe, na pasta da instalação): baixa, confere e instala, com janela própria
            std::wstring setup = BinDir().substr(0, BinDir().find_last_of(L'\\')) + L"\\LiquinixSetup.exe";
            if (GetFileAttributesW(setup.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ShellExecuteW(nullptr, L"open", setup.c_str(), L"--atualizar", nullptr, SW_SHOWNORMAL);
                return;
            }
            std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + UpdScript() + L"\"";
            STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        }));
    } else if (u.state == 2 && u.erro.empty()) rows.push_back(Note(Tr(L"O Liquinix está atualizado.")));
    rows.push_back(Button(L"Confere de novo no GitHub.", L"Procurar Atualizações", [] { { std::lock_guard<std::mutex> lk(g_updMu); g_upd.state = 0; } aj::Rebuild(); }));
    p.groups.push_back(G(rows, L"Liquinix"));
    p.groups.push_back(G({ Toggle(L"Avisar quando houver atualização", [] { return Setting(L"avisar_atualizacao", L"1") != L"0"; },
                                  [](bool v) { SetSetting(L"avisar_atualizacao", v ? L"1" : L"0"); },
                                  L"O Liquinix confere o GitHub ao entrar e a cada 6 horas e mostra uma notificação quando sai uma versão nova.") }));
    if (tem && !u.faltam.empty()) {
        std::vector<Row> ch;
        for (size_t k = 0; k < u.faltam.size() && k < 15; k++) ch.push_back(Note(L"• " + u.faltam[k]));
        if (u.faltam.size() > 15) ch.push_back(Note(L"… +" + std::to_wstring(u.faltam.size() - 15)));
        p.groups.push_back(G(ch, L"O que falta"));
    }
    p.groups.push_back(G({ Button(L"O Windows Update cuida das atualizações deste computador.", L"Abrir o Windows Update…", [] { OpenUri(L"ms-settings:windowsupdate"); }) }, L"Windows"));
    p.groups.push_back(G({ Value(L"Instalado", i.windows + L" (" + i.build + L")"), Link(L"Atualizações Automáticas", [] { OpenUri(L"ms-settings:windowsupdate-options"); }) }));
    return p;
}

Page Inicio() {
    Page p;
    Group g = G({}, L"Abrir ao Iniciar Sessão", L"Estes itens abrem automaticamente quando você inicia a sessão.");
    for (auto& s : sys::StartupItems()) {
        std::wstring name = s.name;
        g.rows.push_back(Toggle(name, [name] { for (auto& x : sys::StartupItems()) if (x.name == name) return x.enabled; return false; },
                                [name](bool on) { sys::SetStartupEnabled(name, on); }));
    }
    if (g.rows.empty()) g.rows.push_back(Note(L"Nenhum item."));
    p.groups.push_back(g);
    p.groups.push_back(Plain(Buttons({ { L"Mais Itens…", [] { OpenUri(L"ms-settings:startupapps"); } } })));
    return p;
}

Page DataHora() {
    Page p;
    TIME_ZONE_INFORMATION tz{};
    GetTimeZoneInformation(&tz);
    std::wstring zone = tz.StandardName;
    p.groups.push_back(G({
        Value(L"Data e hora", [] {
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t d[64], t[32];
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, d, 64, nullptr);
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 32);
            return std::wstring(d) + L", " + t;
        }),
        Value(L"Fuso horário", zone),
    }));
    p.groups.push_back(Plain(Buttons({ { L"Ajustar…", [] { OpenUri(L"ms-settings:dateandtime"); } } })));
    return p;
}

// ================= O próprio Liquinix (Geral): abrir ao entrar no Windows, pastas no Finder, desligar
// A tarefa agendada "Liquinix" (a mesma do instalador e do docs\iniciar-com-windows.ps1) abre o Shell no login.
static std::wstring BinDir() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    return d.substr(0, d.find_last_of(L'\\'));
}
static DWORD RunHidden(std::wstring cmd, DWORD waitMs = 15000) {
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return code;
    WaitForSingleObject(pi.hProcess, waitMs);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}
static bool ShellAutoStart() { return RunHidden(L"schtasks.exe /Query /TN Liquinix", 5000) == 0; }
// A chave "Abrir ao entrar no Windows" lia o estado rodando o schtasks.exe A CADA DESENHO da linha (13-33 ms por
// processo, medido em 20/11/2026): rolar a página Geral até o fim, onde ela fica, travava a rolagem (cada quadro
// esperava um processo). Agora o estado é lido uma vez, numa thread, e guardado; o clique muda o guardado na hora.
static std::atomic<int> g_autoStart{ -1 };  // -1 não lido, -2 lendo, 0/1
static bool ShellAutoStartCached() {
    int v = g_autoStart.load();
    if (v == -1 && g_autoStart.compare_exchange_strong(v, -2))
        std::thread([] { g_autoStart = ShellAutoStart() ? 1 : 0; aj::Rebuild(); }).detach();
    return g_autoStart.load() == 1;
}
static void SetShellAutoStart(bool on);
static void SetShellAutoStartAsync(bool on) {
    g_autoStart = on ? 1 : 0;
    std::thread([on] { SetShellAutoStart(on); g_autoStart = ShellAutoStart() ? 1 : 0; aj::Rebuild(); }).detach();
}
static void SetShellAutoStart(bool on) {
    if (!on) { RunHidden(L"schtasks.exe /Delete /TN Liquinix /F"); return; }
    std::wstring exe = BinDir() + L"\\LiquinixShell.exe";
    // ao fazer logon, só para este usuário, sem administrador (o schtasks /SC ONLOGON pediria administrador)
    std::wstring ps = L"$u=\"$env:USERDOMAIN\\$env:USERNAME\"; Register-ScheduledTask -TaskName Liquinix -Force "
                      L"-Action (New-ScheduledTaskAction -Execute '" + exe + L"' -WorkingDirectory '" + BinDir() + L"') "
                      L"-Trigger (New-ScheduledTaskTrigger -AtLogOn -User $u) "
                      L"-Settings (New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -Priority 4 -MultipleInstances IgnoreNew) "
                      L"-Principal (New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Limited) | Out-Null";
    RunHidden(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"" + ps + L"\"", 30000);
}
static void TurnShellOff() {
    int r = MessageBoxW(GetActiveWindow(),
        Tr(L"O Liquinix vai fechar agora e não vai mais abrir ao entrar no Windows. A barra de tarefas e o "
           L"Explorador do Windows voltam como antes, e as pastas voltam a abrir no Explorador.\n\n"
           L"Para usar de novo: abra \"Liquinix\" no Menu Iniciar."),
        Tr(L"Desligar o Liquinix?"), MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (r != IDOK) return;
    SetShellAutoStart(false);
    g_autoStart = 0;
    RunHidden(L"\"" + BinDir() + L"\\LiquinixFinder.exe\" --padrao off --quieto");  // pastas voltam para o Explorador
    SetSetting(L"finder_padrao", L"0");
    SetSetting(L"finder_volta", L"1");  // o Shell, ao abrir de novo, religa o Finder (issue #1: o Win+E ficava no Explorador)
    if (HWND h = FindWindowW(L"ShellMensagens", nullptr)) PostMessageW(h, WM_HOTKEY, 1, 0);  // o mesmo do "sair do Shell"
}

Page Geral() {
    Page p;
    if (Setting(L"liquid_glass", L"0") == L"1" && Setting(L"golden_gate", L"0") == L"1") {
        // macOS 27 (captura Settings-General, 2x): cartão de 163,5 no alto com o ícone de 50 (centro a 50), "Geral" em
        // negrito de 22 (centro a 99) e a descrição cinza de 13 centrada embaixo
        Row hero; hero.kind = Kind::Custom; hero.height = 163.5f;
        hero.draw = [](Ctx& c, D2D1_RECT_F rr) {
            float cx = (rr.left + rr.right) / 2;
            Tile(c, D2D1::RectF(cx - 25, rr.top + 25, cx + 25, rr.top + 75), 0xE713, C_GRAY, L"gearshape.fill");
            static IDWriteTextFormat* tfBig = nullptr;
            if (!tfBig) { IDWriteFactory* dw = nullptr; DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
                if (dw) { dw->CreateTextFormat(L"SF Pro Display", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22, L"", &tfBig); dw->Release(); }
                if (tfBig) { tfBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); tfBig->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); } }
            if (tfBig) Text(c, L"Geral", D2D1::RectF(rr.left, rr.top + 85, rr.right, rr.top + 113), tfBig, c.text);
            DWRITE_WORD_WRAPPING ww = c.noteCenter->GetWordWrapping();
            c.noteCenter->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            Text(c, L"Gerencie a configuração geral e as preferências do computador, como atualizações de software, idioma, compartilhamento e mais.",
                 D2D1::RectF(rr.left + 30, rr.top + 106, rr.right - 30, rr.top + 146), c.noteCenter, c.text2);
            c.noteCenter->SetWordWrapping(ww);
        };
        p.groups.push_back(G({ hero }));
    }
    p.groups.push_back(G({ Link(L"Sobre", [] { Push(L"Sobre", Sobre); }, 0xE7F4, C_GRAY),
                           Link(L"Atualização de Software", [] { Push(L"Atualização de Software", Atualizacao); }, 0xE895, C_GRAY) }));
    p.groups.push_back(G({ Link(L"Armazenamento", [] { Push(L"Armazenamento", Armazenamento); }, 0xEDA2, C_GRAY) }));
    p.groups.push_back(G({ Uri(L"Compartilhamento por Proximidade", L"ms-settings:crossdevice", 0xE72D, C_BLUE),
                           Link(L"Itens de Início", [] { Push(L"Itens de Início", Inicio); }, 0xE8FD, C_GRAY) }));
    // Idioma do Liquinix (idioma=<código> no ajustes.txt, idioma.h): o Shell troca na hora; este app também;
    // os outros apps abertos leem o idioma ao abrir. Cada idioma aparece com o nome na própria língua.
    std::vector<std::wstring> langs;
    for (int l : idioma::kMenuOrder) langs.push_back(idioma::kInfo[l].native);
    Row lang = Popup(L"Idioma", langs, [] {
                         int cur = idioma::FromCode(Setting(L"idioma", L"en"));
                         for (int i = 0; i < idioma::COUNT; i++) if (idioma::kMenuOrder[i] == cur) return i;
                         return 0;
                     },
                     [](int i) {
                         int l = idioma::kMenuOrder[std::clamp(i, 0, idioma::COUNT - 1)];
                         SetSetting(L"idioma", idioma::kInfo[l].code);
                         idioma::Set(l);
                         SetWindowTextW(Window(), TrPick(L"Ajustes do Sistema", L"System Settings"));
                         Rebuild();
                     });
    lang.icon = 0xE774; lang.iconColor = C_BLUE;
    p.groups.push_back(G({ lang, Note(L"O Shell muda na hora. Os apps abertos (Finder, Notas, Terminal…) mudam ao serem reabertos.") },
                         L"Idioma do Liquinix"));
    p.groups.push_back(G({ Uri(L"Idioma e Região", L"ms-settings:regionlanguage", 0xE774, C_BLUE),
                           Link(L"Data e Hora", [] { Push(L"Data e Hora", DataHora); }, 0xE787, C_BLUE) }));
    p.groups.push_back(G({ Uri(L"Compartilhamento", L"ms-settings:sharedexperiences", 0xE72D, C_GRAY),
                           Uri(L"Histórico de Arquivos", L"control.exe /name Microsoft.FileHistory", 0xE81C, C_GREEN),
                           Uri(L"Transferir ou Redefinir", L"ms-settings:recovery", 0xE777, C_GRAY) }));
    p.groups.push_back(G({ Toggle(L"Abrir ao entrar no Windows", [] { return ShellAutoStartCached(); }, [](bool on) { SetShellAutoStartAsync(on); }),
                           Toggle(L"Abrir pastas no Finder", [] { return Setting(L"finder_padrao", L"1") == L"1"; },
                                  [](bool on) {
                                      SetSetting(L"finder_padrao", on ? L"1" : L"0");
                                      RunHidden(L"\"" + BinDir() + L"\\LiquinixFinder.exe\" --quieto --padrao " + (on ? L"on" : L"off"));
                                  }, L"Downloads do navegador, \"Mostrar na pasta\" e o Win+E também"),
                           // reinicia só o Shell (barra, Dock, mesa, vidro): o mesmo do "ShellReiniciar" que a atualização usa
                           Button(L"Reiniciar o Liquinix", L"Reiniciar", [] {
                               static UINT msg = RegisterWindowMessageW(L"ShellReiniciar");
                               HWND sh = FindWindowExW(HWND_MESSAGE, nullptr, L"ShellMensagens", nullptr);
                               if (!sh) sh = FindWindowW(L"ShellMensagens", nullptr);
                               if (sh) PostMessageW(sh, msg, 0, 0);
                               else ShellExecuteW(nullptr, L"open", (BinDir() + L"\\LiquinixShell.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);  // o Shell não está rodando: abre
                           }, L"Fecha e abre de novo a barra de menus, o Dock, a mesa e o vidro, sem sair do Windows. Os seus apps continuam abertos."),
                           Button(L"Parar de usar o Liquinix", L"Desligar…", [] { TurnShellOff(); },
                                  L"Fecha o Shell e devolve a barra de tarefas e o Explorador do Windows") }, L"Liquinix"));
    return p;
}

// ================= Acessibilidade
Page Acessibilidade() {
    Page p;
    p.groups.push_back(G({ Uri(L"Narrador", L"ms-settings:easeofaccess-narrator", 0xE7F5, C_DARK),
                           Uri(L"Lupa", L"ms-settings:easeofaccess-magnifier", 0xE71E, C_DARK),
                           Uri(L"Tela", L"ms-settings:easeofaccess-display", 0xE7F4, C_BLUE),
                           Uri(L"Tamanho do Texto", L"ms-settings:easeofaccess-display", 0xE8D2, C_BLUE),
                           Uri(L"Filtros de Cor", L"ms-settings:easeofaccess-colorfilter", 0xE790, C_ORANGE) }, L"Visão"));
    p.groups.push_back(G({ Uri(L"Áudio", L"ms-settings:easeofaccess-audio", 0xE767, C_PINK),
                           Uri(L"Legendas", L"ms-settings:easeofaccess-closedcaptioning", 0xE7F0, C_GRAY) }, L"Audição"));
    p.groups.push_back(G({ Uri(L"Controle por Voz", L"ms-settings:easeofaccess-speechrecognition", 0xE720, C_BLUE),
                           Uri(L"Teclado", L"ms-settings:easeofaccess-keyboard", 0xE765, C_GRAY),
                           Uri(L"Ponteiro do Mouse", L"ms-settings:easeofaccess-mousepointer", 0xE962, C_GRAY) }, L"Motor"));
    // Reduzir Movimento do Mac (Acessibilidade > Tela): janelas, Spotlight e Central de Notificações aparecem na hora,
    // a Central de Controle só com fade (medido no macOS 26; Shell: anim::reduceMotion)
    p.groups.push_back(G({ Toggle(L"Reduzir movimento", [] { return Setting(L"reduzir_movimento", L"0") == L"1"; },
                                  [](bool v) { SetSetting(L"reduzir_movimento", v ? L"1" : L"0"); Rebuild(); },
                                  L"Troca os movimentos e as mudanças de tamanho por cortes secos ou fades curtos.") }, L"Movimento"));
    return p;
}

// ================= Aparência
const std::vector<std::pair<std::wstring, UINT32>> ACCENTS = {
    { L"multicor", 0 }, { L"azul", 0x007AFF }, { L"roxo", 0x953D96 }, { L"rosa", 0xF74F9E }, { L"vermelho", 0xE0383E },
    { L"laranja", 0xF7821B }, { L"amarelo", 0xFCB827 }, { L"verde", 0x62BA46 }, { L"grafite", 0x8C8C8C } };
const wchar_t* ACCENT_NAMES[] = { L"Multicor", L"Azul", L"Roxo", L"Rosa", L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Grafite" };

Page TransparenciaEfeitos();

Row TemaRow() {
    // Tema: o vidro do Sequoia, o Liquid Glass do macOS Tahoe ou o macOS Golden Gate (macOS 27: o Tahoe com botões
    // Aqua de vidro, um canto só e menor em todas as janelas e o controle de transparência). Golden Gate =
    // liquid_glass=1 + golden_gate=1, então tudo o que só conhece o Tahoe continua funcionando. Detalhes do vidro em
    // Transparência e Efeitos.
    // Mac OS X Leopard (10.5, 2007) = leopard=1 SEM o liquid_glass: o Sequoia por baixo (tudo o que não conhece o
    // Leopard continua no visual do Sequoia) e, por cima, o Aqua de 2007: barra de menus clara com a maçã grafite, Dock
    // em prateleira espelhada, botões de gel e barras cinza metálico. Ligar o Liquid Glass em outro lugar desliga o
    // Leopard sozinho (leopard só vale com liquid_glass=0).
    // Lançado em 20/11/2026 (antes só aparecia com leopard_previa=1): o pacote leva icones\leopard e cursor\Leopard.
    // ordem do usuário: o mais novo primeiro (0 Golden Gate, 1 Tahoe, 2 Sequoia, 3 Leopard)
    std::vector<std::wstring> temas = { L"macOS Golden Gate", L"macOS Tahoe", L"macOS Sequoia", L"Mac OS X Leopard" };
    return Popup(L"Tema", temas,
                     [] {
                         if (Setting(L"liquid_glass", L"0") != L"1") return Setting(L"leopard", L"0") == L"1" ? 3 : 2;
                         return Setting(L"golden_gate", L"0") == L"1" ? 0 : 1;
                     },
                     [](int i) {
                         if (i == 0) SetSetting(L"liquid_estilo", L"nitido");  // o vidro do Golden Gate é o controle deslizante
                         SetSetting(L"leopard", i == 3 ? L"1" : L"0");
                         SetSetting(L"golden_gate", i == 0 ? L"1" : L"0");
                         SetSetting(L"liquid_glass", i == 0 || i == 1 ? L"1" : L"0");
                         // o papel de parede do usuário nunca muda ao trocar de tema (o usuário pediu)
                         Rebuild();
                     });
}

Row GlassEstiloRow(bool title = true) {  // title = false: embaixo da foto da página nova, que já tem o título
    Row glass; glass.kind = Kind::Custom; glass.height = 96;
    glass.enabled = Setting(L"reduzir_transparencia", L"0") != L"1";
    glass.draw = [title](Ctx& c, D2D1_RECT_F rr) {
        if (title) Text(c, L"Liquid Glass", D2D1::RectF(rr.left + 10, rr.top + 8, rr.left + 160, rr.top + 28), c.body, c.text);
        const wchar_t* names[2] = { L"Transparente", L"Tingido" };
        int cur = Setting(L"liquid_estilo", L"nitido") == L"tingido" ? 1 : 0;
        float w = 66, h = 44, gap = 16, x = rr.right - 10 - 2 * w - gap;
        for (int k = 0; k < 2; k++, x += w + gap) {
            D2D1_RECT_F t = D2D1::RectF(x, rr.top + 12, x + w, rr.top + 12 + h);
            // papel de parede colorido atrás e uma placa de vidro por cima: clara (transparente) ou fosca (tingida)
            ID2D1GradientStopCollection* gs = nullptr;
            D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0.30f, 0.55f, 0.98f, 1) }, { 1, D2D1::ColorF(0.95f, 0.50f, 0.55f, 1) } };
            c.dc->CreateGradientStopCollection(st, 2, &gs);
            ID2D1LinearGradientBrush* lg = nullptr;
            if (gs) c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ t.left, t.top }, { t.right, t.bottom }), gs, &lg);
            if (lg) { c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 6, 6), lg); lg->Release(); }
            if (gs) gs->Release();
            D2D1_RECT_F pl = D2D1::RectF(t.left + 9, t.top + 9, t.right - 9, t.bottom - 9);
            c.br->SetColor(k == 0 ? D2D1::ColorF(1, 1, 1, 0.18f) : (c.dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.82f) : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.85f)));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(pl, 8, 8), c.br);
            c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.75f));
            c.dc->DrawRoundedRectangle(D2D1::RoundedRect(pl, 8, 8), c.br, 0.8f);
            if (k == cur) { c.br->SetColor(c.accent); c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left - 3, t.top - 3, t.right + 3, t.bottom + 3), 8, 8), c.br, 2.5f); }
            Text(c, names[k], D2D1::RectF(t.left - 12, t.bottom + 6, t.right + 12, t.bottom + 24), c.noteCenter, k == cur ? c.text : c.text2);
        }
    };
    glass.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        float w = 66, gap = 16, x = rr.right - 10 - 2 * w - gap;
        for (int k = 0; k < 2; k++, x += w + gap)
            if (pt.x >= x - 6 && pt.x <= x + w + 6) { SetSetting(L"liquid_estilo", k == 0 ? L"nitido" : L"tingido"); Rebuild(); }
    };
    return glass;
}

Page AparenciaGolden();
Page AparenciaLeopard();
Page Aparencia() {
    // Mac OS X Leopard: o painel Aparência do 10.5 (pop-ups Aqua em vez das miniaturas do macOS moderno)
    if (Setting(L"leopard", L"0") == L"1" && Setting(L"liquid_glass", L"0") != L"1") return AparenciaLeopard();
    // a página do macOS 27 vale para os três temas (o usuário pediu): no Tahoe a foto ganha a escolha Transparente /
    // Tingido no lugar do controle; no Sequoia (sem Liquid Glass) só Aparência, Tema e o resto. A antiga fica de reserva.
    if (true) return AparenciaGolden();
    Page p;
    Row modes; modes.kind = Kind::Custom; modes.height = 108;
    auto modeIdx = [] { return Setting(L"aparencia_auto", L"0") == L"1" ? 0 : sys::Theme() == 0 ? 1 : 2; };
    modes.draw = [modeIdx](Ctx& c, D2D1_RECT_F rr) {
        Text(c, L"Aparência", D2D1::RectF(rr.left + 10, rr.top + 8, rr.left + 140, rr.top + 28), c.body, c.text);
        const wchar_t* names[3] = { L"Automático", L"Claro", L"Escuro" };
        int cur = modeIdx();
        float w = 66, h = 44, gap = 16, x = rr.right - 10 - 3 * w - 2 * gap;
        for (int k = 0; k < 3; k++) {
            D2D1_RECT_F t = D2D1::RectF(x, rr.top + 14, x + w, rr.top + 14 + h);
            // miniatura: fundo + janelinha clara/escura (automático: metade de cada)
            auto mini = [&](bool dark, D2D1_RECT_F clip) {
                c.dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                c.br->SetColor(dark ? D2D1::ColorF(0.12f, 0.2f, 0.42f, 1) : D2D1::ColorF(0.45f, 0.68f, 0.95f, 1));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 6, 6), c.br);
                D2D1_RECT_F win = D2D1::RectF(t.left + 10, t.top + 8, t.right - 10, t.bottom - 6);
                c.br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 1) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 1));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(win, 3, 3), c.br);
                c.br->SetColor(dark ? D2D1::ColorF(0.3f, 0.3f, 0.32f, 1) : D2D1::ColorF(0.85f, 0.85f, 0.86f, 1));
                c.dc->FillRectangle(D2D1::RectF(win.left, win.top + 3, win.left + 12, win.bottom - 1), c.br);
                c.br->SetColor(c.accent);
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(win.left + 16, win.top + 6, win.left + 32, win.top + 10), 2, 2), c.br);
                c.dc->PopAxisAlignedClip();
            };
            if (k == 0) { mini(false, D2D1::RectF(t.left, t.top, (t.left + t.right) / 2, t.bottom)); mini(true, D2D1::RectF((t.left + t.right) / 2, t.top, t.right, t.bottom)); }
            else mini(k == 2, t);
            if (k == cur) { c.br->SetColor(c.accent); c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left - 3, t.top - 3, t.right + 3, t.bottom + 3), 8, 8), c.br, 2.5f); }
            Text(c, names[k], D2D1::RectF(t.left - 10, t.bottom + 6, t.right + 10, t.bottom + 24), c.noteCenter, k == cur ? c.text : c.text2);
            x += w + gap;
        }
    };
    modes.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        float w = 66, gap = 16, x = rr.right - 10 - 3 * w - 2 * gap;
        for (int k = 0; k < 3; k++, x += w + gap) {
            if (pt.x < x - 6 || pt.x > x + w + 6) continue;
            if (k == 0) {  // automático: claro de dia, escuro à noite (o Shell troca na hora certa)
                SetSetting(L"aparencia_auto", L"1");
                SYSTEMTIME st; GetLocalTime(&st);
                sys::SetTheme(st.wHour >= 7 && st.wHour < 19 ? 0 : 1);
            } else {
                SetSetting(L"aparencia_auto", L"0");
                sys::SetTheme(k == 1 ? 0 : 1);
            }
            SetSetting(L"tema", L"auto");
            Rebuild();
        }
    };
    Row acc; acc.kind = Kind::Custom; acc.height = 56;
    acc.draw = [](Ctx& c, D2D1_RECT_F rr) {
        Text(c, L"Cor de destaque", D2D1::RectF(rr.left + 10, rr.top, rr.left + 160, rr.top + 38), c.body, c.text);
        std::wstring cur = Setting(L"destaque", L"azul");
        float x = rr.right - 10 - 9 * 22 + 8;
        for (int k = 0; k < 9; k++, x += 22) {
            D2D1_POINT_2F m{ x, rr.top + 19 };
            if (k == 0) {  // multicor: arco-íris
                const UINT32 cols[6] = { 0xE0383E, 0xF7821B, 0xFCB827, 0x62BA46, 0x007AFF, 0x953D96 };
                for (int s = 0; s < 6; s++) {
                    c.br->SetColor(D2D1::ColorF(((cols[s] >> 16) & 255) / 255.f, ((cols[s] >> 8) & 255) / 255.f, (cols[s] & 255) / 255.f, 1));
                    c.dc->PushAxisAlignedClip(D2D1::RectF(m.x - 8 + s * 16 / 6.f, m.y - 8, m.x - 8 + (s + 1) * 16 / 6.f, m.y + 8), D2D1_ANTIALIAS_MODE_ALIASED);
                    c.dc->FillEllipse(D2D1::Ellipse(m, 8, 8), c.br);
                    c.dc->PopAxisAlignedClip();
                }
            } else {
                UINT32 col = ACCENTS[k].second;
                c.br->SetColor(D2D1::ColorF(((col >> 16) & 255) / 255.f, ((col >> 8) & 255) / 255.f, (col & 255) / 255.f, 1));
                c.dc->FillEllipse(D2D1::Ellipse(m, 8, 8), c.br);
            }
            c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.12f));
            c.dc->DrawEllipse(D2D1::Ellipse(m, 8, 8), c.br, 0.6f);
            if (ACCENTS[k].first == cur) { c.br->SetColor(D2D1::ColorF(1, 1, 1, 1)); c.dc->FillEllipse(D2D1::Ellipse(m, 3, 3), c.br); }
        }
        std::wstring name = L"Azul";
        for (int k = 0; k < 9; k++) if (ACCENTS[k].first == cur) name = ACCENT_NAMES[k];
        c.note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        Text(c, name, D2D1::RectF(rr.left, rr.top + 30, rr.right - 10, rr.top + 48), c.note, c.text2);
        c.note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    acc.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        float x = rr.right - 10 - 9 * 22 + 8;
        for (int k = 0; k < 9; k++, x += 22)
            if (std::fabs(pt.x - x) <= 10 && std::fabs(pt.y - (rr.top + 19)) <= 10) { SetSetting(L"destaque", k == 0 ? L"azul" : ACCENTS[k].first); Rebuild(); }
    };
    Row tema = TemaRow();
    // o Tema fica sempre livre: com "Reduzir transparência" o vidro só fica opaco (como no Mac)
    bool tahoeOn = Setting(L"liquid_glass", L"0") == L"1";
    if (!tahoeOn) { p.groups.push_back(G({ tema, modes, acc })); }
    else {
        // macOS 26 (Aparência): "Liquid Glass" com a escolha Transparente / Tingido (26.1) e "Estilo de ícones e widgets"
        // em miniaturas — Padrão, Escuro, Transparente (o modo translúcido) e Tingido. A transparência fina continua em
        // Transparência e Efeitos.
        Row glass = GlassEstiloRow();
        p.groups.push_back(G({ tema, modes, glass }));
        static const wchar_t* ESTILOS[] = { L"padrao", L"escuro", L"transparente", L"tingido" };
        Row icons; icons.kind = Kind::Custom; icons.height = 96;
        icons.draw = [](Ctx& c, D2D1_RECT_F rr) {
            Text(c, L"Estilo de ícones e widgets", D2D1::RectF(rr.left + 10, rr.top + 8, rr.left + 200, rr.top + 28), c.body, c.text);
            const wchar_t* names[4] = { L"Padrão", L"Escuro", L"Transparente", L"Tingido" };
            std::wstring v = Setting(L"estilo_icones", L"padrao");
            int cur = 0;
            for (int k = 0; k < 4; k++) if (v == ESTILOS[k]) cur = k;
            float w = 52, h = 44, gap = 14, x = rr.right - 10 - 4 * w - 3 * gap;
            for (int k = 0; k < 4; k++, x += w + gap) {
                D2D1_RECT_F t = D2D1::RectF(x, rr.top + 12, x + w, rr.top + 12 + h);
                c.br->SetColor(c.dark ? D2D1::ColorF(0.22f, 0.22f, 0.24f, 1) : D2D1::ColorF(0.88f, 0.89f, 0.92f, 1));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 6, 6), c.br);
                // o ícone de exemplo (quadrado arredondado com uma folha) em cada estilo
                float s = 28, cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
                D2D1_RECT_F ic = D2D1::RectF(cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2);
                D2D1_COLOR_F bg, fg;
                if (k == 0) { bg = D2D1::ColorF(1, 1, 1, 1); fg = D2D1::ColorF(0.20f, 0.70f, 0.30f, 1); }
                else if (k == 1) { bg = D2D1::ColorF(0.13f, 0.13f, 0.14f, 1); fg = D2D1::ColorF(0.30f, 0.82f, 0.42f, 1); }
                else if (k == 2) { bg = D2D1::ColorF(1, 1, 1, c.dark ? 0.16f : 0.45f); fg = D2D1::ColorF(1, 1, 1, 0.92f); }
                else { bg = D2D1::ColorF(c.accent.r * 0.35f, c.accent.g * 0.35f, c.accent.b * 0.35f, 1); fg = c.accent; }
                c.br->SetColor(bg);
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(ic, 7, 7), c.br);
                if (k == 2) { c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.8f)); c.dc->DrawRoundedRectangle(D2D1::RoundedRect(ic, 7, 7), c.br, 0.8f); }
                c.br->SetColor(fg);
                c.dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 7, 7), c.br);
                if (k == cur) { c.br->SetColor(c.accent); c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left - 3, t.top - 3, t.right + 3, t.bottom + 3), 8, 8), c.br, 2.5f); }
                Text(c, names[k], D2D1::RectF(t.left - 12, t.bottom + 6, t.right + 12, t.bottom + 24), c.noteCenter, k == cur ? c.text : c.text2);
            }
        };
        icons.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
            float w = 52, gap = 14, x = rr.right - 10 - 4 * w - 3 * gap;
            for (int k = 0; k < 4; k++, x += w + gap)
                if (pt.x >= x - 5 && pt.x <= x + w + 5) { SetSetting(L"estilo_icones", ESTILOS[k]); Rebuild(); }
        };
        p.groups.push_back(G({ acc, icons }, L"Tema"));
    }
    // barras de rolagem: "Sempre" = barras do Windows sempre visíveis (DynamicScrollbars = 0)
    p.groups.push_back(G({ Popup(L"Mostrar barras de rolagem", { L"Ao rolar", L"Sempre" },
                                 [] { DWORD v = 1, cb = sizeof(v); RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", RRF_RT_REG_DWORD, nullptr, &v, &cb); return v ? 0 : 1; },
                                 [](int i) {
                                     DWORD v = i == 0 ? 1 : 0;
                                     RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", REG_DWORD, &v, sizeof(v));
                                     SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Accessibility", SMTO_ABORTIFHUNG, 200, nullptr);
                                 }) }));
    if (Setting(L"liquid_glass", L"0") == L"1") p.groups.back().title = L"Janelas";
    p.groups.push_back(G({ Link(L"Transparência e Efeitos", [] { Push(L"Transparência e Efeitos", TransparenciaEfeitos); }, 0xE790, C_BLUE) }));
    return p;
}

// ================= Aparência no macOS 27 Golden Gate
// Copiada da captura 2x dos Ajustes do macOS 27 (512 Pixels, Settings-Appearance.png), medidas em pt a partir da
// caixa (ver Documents\macOS27-Theme-Reference\MEDIDAS.md):
//   caixa 1: Aparência (85,5) com miniaturas 71,5 x 48,5 de passo 82 (Claro, Escuro, Automático), a última a 11 da
//            borda, 10,5 do alto, rótulos centrados a 69; Liquid Glass (196 + 36) com a foto 267,5 x 179,5 a 10 da
//            borda e 11 do alto, e o controle (trilho a 213 / 33,5 das bordas)
//   "Tema" (57,5 entre as caixas); caixa 2: Cor (61) com bolinhas de 23,5, passo 36, a última a 21,75 da borda e o
//            centro a 21,25 do alto, nome da escolhida embaixo; Cor do realce do texto (37,5)
//   caixa 3: Estilo de ícones e widgets (87,5) com quadrinhos de 29,5, passo 51,5, a 13 da borda e 24 do alto, rótulos
//            a 54; Cor das pastas (40). Menus: bolinha de 16, texto a 8,5 dela e o botão redondo a 13,5 da borda.
static D2D1_COLOR_F GGRgb(UINT32 c, float a = 1) { return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }

// Cor do realce do texto: a cor de seleção do Windows (HKCU\Control Panel\Colors\Hilight + SetSysColors, vale na hora
// para os programas clássicos). A original é guardada em realce_original na 1ª troca e volta com "Automática"
// (registrado em docs\registro-mudancas.txt).
static const UINT32 GG_REALCE[9] = { 0, 0x007AFF, 0x953D96, 0xF74F9E, 0xE0383E, 0xF7821B, 0xFFC726, 0x62BA46, 0x989898 };
static void GGAplicaRealce(int i) {
    wchar_t cur[64] = {}; DWORD cb = sizeof(cur);
    RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Hilight", RRF_RT_REG_SZ, nullptr, cur, &cb);
    std::wstring orig = Setting(L"realce_original", L"");
    if (i <= 0) {
        if (orig.empty()) { SetSetting(L"realce_texto", L"0"); return; }
        int r = 0, g = 0, b = 0;
        if (swscanf_s(orig.c_str(), L"%d %d %d", &r, &g, &b) == 3) {
            RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Hilight", REG_SZ, orig.c_str(), (DWORD)((orig.size() + 1) * sizeof(wchar_t)));
            INT el = COLOR_HIGHLIGHT; COLORREF cr = RGB(r, g, b); SetSysColors(1, &el, &cr);
        }
        SetSetting(L"realce_original", L"");
        SetSetting(L"realce_texto", L"0");
        return;
    }
    if (orig.empty() && cur[0]) SetSetting(L"realce_original", cur);
    UINT32 c = GG_REALCE[i];
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    wchar_t v[32]; swprintf_s(v, L"%d %d %d", r, g, b);
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Hilight", REG_SZ, v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
    INT el = COLOR_HIGHLIGHT; COLORREF cr = RGB(r, g, b); SetSysColors(1, &el, &cr);
    SetSetting(L"realce_texto", std::to_wstring(i));
}

Page AparenciaGolden() {
    Page p;
    // ---- Aparência: Claro, Escuro, Automático
    Row modes; modes.kind = Kind::Custom; modes.height = 85.5f;
    auto modeIdx = [] { return Setting(L"aparencia_auto", L"0") == L"1" ? 2 : sys::Theme() == 0 ? 0 : 1; };
    modes.draw = [modeIdx](Ctx& c, D2D1_RECT_F rr) {
        Text(c, L"Aparência", D2D1::RectF(rr.left + 10, rr.top + 8, rr.left + 160, rr.top + 28), c.body, c.text);
        const wchar_t* names[3] = { L"Claro", L"Escuro", L"Automático" };
        int cur = modeIdx();
        float w = 71.5f, h = 48.5f, pitch = 82, x = rr.right - 11 - w - 2 * pitch;
        for (int k = 0; k < 3; k++, x += pitch) {
            D2D1_RECT_F t = D2D1::RectF(x, rr.top + 10.5f, x + w, rr.top + 10.5f + h);
            ID2D1Factory* fac = nullptr; c.dc->GetFactory(&fac);
            ID2D1RoundedRectangleGeometry* rg = nullptr; fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(t, 7, 7), &rg);
            c.dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), nullptr);
            auto mini = [&](bool dark, D2D1_RECT_F clip) {
                c.dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                // o papel de parede azul das miniaturas do macOS 27 (claro: céu pálido -> azul; escuro: anil profundo)
                D2D1_GRADIENT_STOP st[3] = { { 0, dark ? D2D1::ColorF(0.33f, 0.33f, 0.72f, 1) : D2D1::ColorF(0.80f, 0.82f, 0.86f, 1) },
                                             { 0.55f, dark ? D2D1::ColorF(0.14f, 0.16f, 0.52f, 1) : D2D1::ColorF(0.35f, 0.58f, 0.92f, 1) },
                                             { 1, dark ? D2D1::ColorF(0.05f, 0.07f, 0.28f, 1) : D2D1::ColorF(0.10f, 0.36f, 0.85f, 1) } };
                ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
                if (SUCCEEDED(c.dc->CreateGradientStopCollection(st, 3, &gs)) &&
                    SUCCEEDED(c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ t.left, t.top }, { t.right, t.bottom }), gs, &lb)))
                    c.dc->FillRectangle(t, lb);
                if (lb) lb->Release();
                if (gs) gs->Release();
                // janelinha de cima à esquerda com a barra na cor de destaque
                D2D1_RECT_F top = D2D1::RectF(t.left + 3, t.top + 3, t.left + 44, t.top + 21);
                c.br->SetColor(dark ? D2D1::ColorF(0.10f, 0.10f, 0.22f, 0.85f) : D2D1::ColorF(1, 1, 1, 0.72f));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(top, 3.5f, 3.5f), c.br);
                c.br->SetColor(c.accent);
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(top.left + 3, top.top + 8, top.right - 3, top.top + 14), 1.5f, 1.5f), c.br);
                // janela de baixo à direita com os três botões
                D2D1_RECT_F win = D2D1::RectF(t.left + 19, t.top + 26, t.right + 6, t.bottom + 6);
                c.br->SetColor(dark ? D2D1::ColorF(0.07f, 0.07f, 0.09f, 1) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 1));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(win, 5, 5), c.br);
                const UINT32 lights[3] = { 0xFF6159, 0xFEBC2E, 0x28C840 };
                for (int d = 0; d < 3; d++) {
                    c.br->SetColor(GGRgb(lights[d]));
                    c.dc->FillEllipse(D2D1::Ellipse({ win.left + 11 + d * 7.5f, win.top + 11 }, 2.6f, 2.6f), c.br);
                }
                c.dc->PopAxisAlignedClip();
            };
            if (k == 2) { mini(false, D2D1::RectF(t.left, t.top, (t.left + t.right) / 2, t.bottom)); mini(true, D2D1::RectF((t.left + t.right) / 2, t.top, t.right, t.bottom)); }
            else mini(k == 1, t);
            c.dc->PopLayer();
            if (rg) rg->Release();
            if (fac) fac->Release();
            c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.10f));
            c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left + 0.25f, t.top + 0.25f, t.right - 0.25f, t.bottom - 0.25f), 7, 7), c.br, 0.5f);
            if (k == cur) { c.br->SetColor(c.accent); c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left - 2, t.top - 2, t.right + 2, t.bottom + 2), 9, 9), c.br, 3); }
            // rótulo de 11 pt (escolhido em negrito), centrado a 69 do alto da linha
            Text(c, names[k], D2D1::RectF(t.left - 20, rr.top + 61, t.right + 20, rr.top + 77), k == cur && c.noteBoldCenter ? c.noteBoldCenter : c.noteCenter, k == cur ? c.text : c.text2);
        }
    };
    modes.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        float w = 71.5f, pitch = 82, x = rr.right - 11 - w - 2 * pitch;
        for (int k = 0; k < 3; k++, x += pitch) {
            if (pt.x < x - 4 || pt.x > x + w + 4) continue;
            if (k == 2) {  // automático: claro de dia, escuro à noite (o Shell troca na hora certa)
                SetSetting(L"aparencia_auto", L"1");
                SYSTEMTIME st; GetLocalTime(&st);
                sys::SetTheme(st.wHour >= 7 && st.wHour < 19 ? 0 : 1);
            } else {
                SetSetting(L"aparencia_auto", L"0");
                sys::SetTheme(k == 0 ? 0 : 1);
            }
            SetSetting(L"tema", L"auto");
            Rebuild();
        }
    };
    // ---- Liquid Glass: a foto com a barra de vidro de exemplo e o controle embaixo
    bool reduce = Setting(L"reduzir_transparencia", L"0") == L"1";
    const bool liquidOn = Setting(L"liquid_glass", L"0") == L"1", goldenOn = liquidOn && Setting(L"golden_gate", L"0") == L"1";
    Row prev; prev.kind = Kind::Custom; prev.height = 196;
    prev.enabled = !reduce;
    prev.draw = [](Ctx& c, D2D1_RECT_F rr) {
        Text(c, L"Liquid Glass", D2D1::RectF(rr.left + 10, rr.top + 9.5f, rr.left + 160, rr.top + 29.5f), c.body, c.text);
        float w = 267.5f, h = 179.5f;
        D2D1_RECT_F t = D2D1::RectF(rr.right - 10 - w, rr.top + 11, rr.right - 10, rr.top + 11 + h);
        ID2D1Factory* fac = nullptr; c.dc->GetFactory(&fac);
        ID2D1RoundedRectangleGeometry* rg = nullptr; fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(t, 14, 14), &rg);
        c.dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), nullptr);
        c.br->SetColor(D2D1::ColorF(0.45f, 0.62f, 0.80f, 1));
        c.dc->FillRectangle(t, c.br);
        D2D1_RECT_F card = D2D1::RectF(t.left, t.bottom - 34.5f, t.right, t.bottom);
        std::wstring photo;
        wchar_t prof[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) photo = std::wstring(prof) + L"\\Pictures\\Wallpapers macOS\\macOS 27\\macos-27-golden-4096x2160-27635.jpg";
        if (photo.empty() || GetFileAttributesW(photo.c_str()) == INVALID_FILE_ATTRIBUTES) photo = sys::Wallpaper();
        if (ID2D1Bitmap* b = BitmapFromFile(photo, 640)) {  // a foto ocupa o que fica acima do cartão
            auto s2 = b->GetSize();
            float ph = card.top - t.top, k = std::max(w / s2.width, ph / s2.height);
            float bw = s2.width * k, bh = s2.height * k, cx = (t.left + t.right) / 2, cy = t.top + ph / 2;
            c.dc->PushAxisAlignedClip(D2D1::RectF(t.left, t.top, t.right, card.top), D2D1_ANTIALIAS_MODE_ALIASED);
            c.dc->DrawBitmap(b, D2D1::RectF(cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            c.dc->PopAxisAlignedClip();
        }
        // cartão de baixo (medido: 234,230,224 no claro) com o título em negrito, cortado pela borda como na referência
        c.br->SetColor(c.dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 1) : GGRgb(0xEAE6E0));
        c.dc->FillRectangle(card, c.br);
        Text(c, L"Golden Gate Bridge", D2D1::RectF(card.left + 16.5f, card.top + 9, card.right - 10, card.top + 42), c.title, c.dark ? c.text : D2D1::ColorF(0, 0, 0, 0.85f));
        // barra de vidro: quanto mais para a direita o controle, mais opaco o vidro
        // Golden Gate: o controle; Tahoe: Transparente (quase nada) ou Tingido (fosco)
        float v = Setting(L"golden_gate", L"0") == L"1" ? std::clamp((float)_wtof(Setting(L"vidro_tinta", L"0.5").c_str()), 0.f, 1.f)
                                                        : Setting(L"liquid_estilo", L"nitido") == L"tingido" ? 0.85f : 0.15f;
        D2D1_COLOR_F fill = c.dark ? D2D1::ColorF(0.20f, 0.20f, 0.21f, 0.18f + 0.72f * v) : D2D1::ColorF(1, 1, 1, 0.28f + 0.66f * v);
        D2D1_COLOR_F ink = c.dark ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(0, 0, 0, 0.82f);
        auto pill = [&](D2D1_RECT_F q) {
            float rad = (q.bottom - q.top) / 2;
            c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.08f));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(q.left, q.top + 1, q.right, q.bottom + 1.5f), rad, rad), c.br);
            c.br->SetColor(fill);
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(q, rad, rad), c.br);
            c.br->SetColor(D2D1::ColorF(1, 1, 1, c.dark ? 0.25f : 0.8f));
            c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(q.left + 0.5f, q.top + 0.5f, q.right - 0.5f, q.bottom - 0.5f), rad - 0.5f, rad - 0.5f), c.br, 0.8f);
        };
        float y0 = t.top + 8, hh = 36;
        D2D1_RECT_F g1 = D2D1::RectF(t.left + 8, y0, t.left + 8 + 73.5f, y0 + hh);
        D2D1_RECT_F g2 = D2D1::RectF(g1.right + 7.5f, y0, g1.right + 7.5f + hh, y0 + hh);
        D2D1_RECT_F g3 = D2D1::RectF(g2.right + 7.5f, y0, t.right - 8.5f, y0 + hh);
        pill(g1); pill(g2); pill(g3);
        c.br->SetColor(ink);
        simbolo::Draw(c.dc, L"square.and.arrow.up", D2D1::RectF(g1.left + 11, y0 + 8, g1.left + 29, y0 + 28), c.br);
        for (int k = 0; k < 3; k++) c.dc->FillEllipse(D2D1::Ellipse({ g1.left + 49.5f + k * 5.5f, y0 + hh / 2 }, 1.5f, 1.5f), c.br);
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g2.left + 13, y0 + 13, g2.right - 8, y0 + 27), 3, 3), c.br, 1.5f);
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g2.left + 9, y0 + 9, g2.right - 12, y0 + 23), 3, 3), c.br, 1.5f);
        simbolo::Draw(c.dc, L"magnifyingglass", D2D1::RectF(g3.left + 10, y0 + 11, g3.left + 24, y0 + 25), c.br);
        Text(c, L"Buscar", D2D1::RectF(g3.left + 32, y0, g3.right - 6, y0 + hh), c.body, c.dark ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(0, 0, 0, 0.5f));
        c.dc->PopLayer();
        if (rg) rg->Release();
        if (fac) fac->Release();
    };
    Row gg = Slider(L"", 0, 1, [] { return (float)_wtof(Setting(L"vidro_tinta", L"0.5").c_str()); },
                    [](float v) { wchar_t b[16]; swprintf_s(b, L"%.2f", v); SetSetting(L"vidro_tinta", b); Redraw(); }, L"", L"");
    gg.height = 36; gg.noLine = true; gg.gg = true; gg.enabled = !reduce;
    if (goldenOn) p.groups.push_back(G({ modes, prev, gg }));
    else if (liquidOn) { Row ge = GlassEstiloRow(false); ge.noLine = true; p.groups.push_back(G({ modes, prev, ge })); }
    else p.groups.push_back(G({ modes }));

    // ---- Tema: cor de destaque (Multicor + 10 cores; as do macOS 27 lidas na captura) e cor do realce do texto
    Row color; color.kind = Kind::Custom; color.height = 61;
    static const UINT32 GG_COLS[10] = { 0, 0x007AFF, 0x953D96, 0xF74F9E, 0xE0383E, 0xF7821B, 0xFFC726, 0x62BA46, 0x989898, 0x98A8D9 };
    static const wchar_t* GG_KEYS[10] = { L"multicor", L"azul", L"roxo", L"rosa", L"vermelho", L"laranja", L"amarelo", L"verde", L"grafite", L"lavanda" };
    static const wchar_t* GG_NAMES[10] = { L"Multicor", L"Azul", L"Roxo", L"Rosa", L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Grafite", L"Lavanda" };
    color.draw = [](Ctx& c, D2D1_RECT_F rr) {
        Text(c, L"Cor", D2D1::RectF(rr.left + 10, rr.top + 8, rr.left + 120, rr.top + 28), c.body, c.text);
        std::wstring cur = Setting(L"destaque", L"azul");
        int sel = 1;
        for (int k = 0; k < 10; k++) if (cur == GG_KEYS[k]) sel = k;
        ID2D1Factory* f = nullptr; c.dc->GetFactory(&f);
        for (int k = 0; k < 10; k++) {
            D2D1_POINT_2F m{ rr.right - 21.75f - (9 - k) * 36.f, rr.top + 21.25f };
            if (k == 0) {  // multicor: disco de cores feito em fatias
                for (int s = 0; s < 36 && f; s++) {
                    float a0 = s * 10.f * 3.14159f / 180, a1 = (s + 1.2f) * 10.f * 3.14159f / 180;
                    float hue = s / 36.f;
                    float r = std::clamp(std::fabs(hue * 6 - 3) - 1, 0.f, 1.f), g = std::clamp(2 - std::fabs(hue * 6 - 2), 0.f, 1.f), b = std::clamp(2 - std::fabs(hue * 6 - 4), 0.f, 1.f);
                    ID2D1PathGeometry* pg = nullptr;
                    if (SUCCEEDED(f->CreatePathGeometry(&pg))) {
                        ID2D1GeometrySink* sk = nullptr;
                        if (SUCCEEDED(pg->Open(&sk))) {
                            sk->BeginFigure(m, D2D1_FIGURE_BEGIN_FILLED);
                            sk->AddLine({ m.x + 11.75f * std::cos(a0), m.y + 11.75f * std::sin(a0) });
                            sk->AddLine({ m.x + 11.75f * std::cos(a1), m.y + 11.75f * std::sin(a1) });
                            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sk->Close(); sk->Release();
                        }
                        c.br->SetColor(D2D1::ColorF(r, g, b, 1));
                        c.dc->FillGeometry(pg, c.br);
                        pg->Release();
                    }
                }
            } else {
                c.br->SetColor(GGRgb(GG_COLS[k]));
                c.dc->FillEllipse(D2D1::Ellipse(m, 11.75f, 11.75f), c.br);
            }
            if (k == sel) {  // anel na cor de destaque, com um respiro da cor da caixa
                c.br->SetColor(c.dark ? D2D1::ColorF(0.19f, 0.19f, 0.19f, 1) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 1));
                c.dc->DrawEllipse(D2D1::Ellipse(m, 12.9f, 12.9f), c.br, 1.5f);
                c.br->SetColor(c.accent);
                c.dc->DrawEllipse(D2D1::Ellipse(m, 14.5f, 14.5f), c.br, 2.5f);
                Text(c, GG_NAMES[k], D2D1::RectF(m.x - 50, rr.top + 36, m.x + 50, rr.top + 52), c.noteCenter, c.text2);  // 11 pt, a 44 do alto
            }
        }
        if (f) f->Release();
    };
    color.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        for (int k = 0; k < 10; k++) {
            float x = rr.right - 21.75f - (9 - k) * 36.f;
            if (std::fabs(pt.x - x) <= 15 && std::fabs(pt.y - (rr.top + 21.25f)) <= 15) { SetSetting(L"destaque", k == 0 ? L"azul" : GG_KEYS[k]); Rebuild(); }
        }
    };
    Row hl = Popup(L"Cor do realce do texto", { L"Automática", L"Azul", L"Roxo", L"Rosa", L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Grafite" },
                   [] { return std::clamp(_wtoi(Setting(L"realce_texto", L"0").c_str()), 0, 8); }, [](int i) { GGAplicaRealce(i); Rebuild(); });
    hl.gg = true; hl.height = 37.5f;
    hl.dot = [](int i) { return i <= 0 ? D2D1::ColorF(0, 0, 0, -1) : GGRgb(GG_REALCE[std::clamp(i, 0, 8)]); };
    p.groups.push_back(G({ color, hl }, L"Tema"));

    // ---- Estilo de ícones e widgets (o símbolo do Tempo em cada estilo) e cor das pastas
    static const wchar_t* ESTILOS[] = { L"padrao", L"escuro", L"transparente", L"tingido" };
    Row icons; icons.kind = Kind::Custom;
    // com Escuro escolhido o macOS 27 mostra embaixo "Sempre / Auto" (Auto: escuro só no modo escuro) e a linha cresce
    icons.height = Setting(L"estilo_icones", L"padrao") == L"escuro" ? 102.5f : 87.5f;
    icons.draw = [](Ctx& c, D2D1_RECT_F rr) {
        Text(c, L"Estilo de ícones e widgets", D2D1::RectF(rr.left + 10, rr.top + 8, rr.left + 220, rr.top + 28), c.body, c.text);
        const wchar_t* names[4] = { L"Padrão", L"Escuro", L"Transparente", L"Tingido" };
        std::wstring v = Setting(L"estilo_icones", L"padrao");
        int cur = 0;
        for (int k = 0; k < 4; k++) if (v == ESTILOS[k]) cur = k;
        const float s = 29.5f, pitch = 51.5f;
        for (int k = 0; k < 4; k++) {
            float x = rr.right - 13 - s - (3 - k) * pitch;
            D2D1_RECT_F t = D2D1::RectF(x, rr.top + 13, x + s, rr.top + 13 + s);  // medido: 13 do alto da linha
            D2D1_COLOR_F top, bot;
            if (k == 0) { top = GGRgb(0x5AB8F5); bot = GGRgb(0x1E7AE6); }
            else if (k == 1) { top = GGRgb(0x2A2A2C); bot = GGRgb(0x0E0E10); }
            else if (k == 2) { top = GGRgb(0xA9A9AE); bot = GGRgb(0x7E7E84); }
            else { top = D2D1::ColorF(std::min(1.f, c.accent.r + 0.25f), std::min(1.f, c.accent.g + 0.25f), std::min(1.f, c.accent.b + 0.25f), 1); bot = c.accent; }
            D2D1_GRADIENT_STOP st[2] = { { 0, top }, { 1, bot } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(c.dc->CreateGradientStopCollection(st, 2, &gs)) &&
                SUCCEEDED(c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ t.left, t.top }, { t.left, t.bottom }), gs, &lb)))
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 7, 7), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            // nuvem com o sol atrás (o ícone do Tempo da referência)
            float cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
            if (k <= 1) { c.br->SetColor(GGRgb(0xFFC52E)); c.dc->FillEllipse(D2D1::Ellipse({ cx + 4.5f, cy - 4 }, 5.5f, 5.5f), c.br); }
            c.br->SetColor(k == 2 ? D2D1::ColorF(0.93f, 0.93f, 0.95f, 1) : D2D1::ColorF(1, 1, 1, 1));
            c.dc->FillEllipse(D2D1::Ellipse({ cx - 4, cy + 2.5f }, 5, 4.5f), c.br);
            c.dc->FillEllipse(D2D1::Ellipse({ cx + 1, cy }, 5.5f, 5.5f), c.br);
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 9, cy + 1.5f, cx + 9, cy + 7.5f), 3, 3), c.br);
            if (k == cur) { c.br->SetColor(c.accent); c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left - 2.5f, t.top - 2.5f, t.right + 2.5f, t.bottom + 2.5f), 9, 9), c.br, 3); }
            Text(c, names[k], D2D1::RectF(t.left - 22, rr.top + 46, t.right + 22, rr.top + 62), k == cur && c.noteBoldCenter ? c.noteBoldCenter : c.noteCenter, k == cur ? c.text : c.text2);
        }
        if (cur == 1) {  // medido: bolinhas a 123,5 e 52,5 da borda, textos a 13 delas, centro a 85,5 do alto
            bool autoOn = Setting(L"estilo_escuro_auto", L"0") == L"1";
            float cy = rr.top + 85.5f;
            const wchar_t* labels[2] = { L"Sempre", L"Automático" };
            float xs[2] = { rr.right - 123.5f, rr.right - 52.5f };
            for (int k = 0; k < 2; k++) {
                bool on = (k == 1) == autoOn;
                D2D1_POINT_2F m{ xs[k], cy };
                if (on) {
                    c.br->SetColor(c.accent); c.dc->FillEllipse(D2D1::Ellipse(m, 7, 7), c.br);
                    c.br->SetColor(D2D1::ColorF(1, 1, 1, 1)); c.dc->FillEllipse(D2D1::Ellipse(m, 2.8f, 2.8f), c.br);
                } else {
                    c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.08f)); c.dc->FillEllipse(D2D1::Ellipse(m, 7, 7), c.br);
                }
                Text(c, labels[k], D2D1::RectF(m.x + 13, cy - 10, m.x + 80, cy + 10), c.body, c.text);
            }
        }
    };
    icons.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        const float s = 29.5f, pitch = 51.5f;
        for (int k = 0; k < 4; k++) {
            float x = rr.right - 13 - s - (3 - k) * pitch;
            if (pt.x >= x - 10 && pt.x <= x + s + 10 && pt.y >= rr.top + 8 && pt.y <= rr.top + 64) { SetSetting(L"estilo_icones", ESTILOS[k]); Rebuild(); }
        }
        if (Setting(L"estilo_icones", L"padrao") == L"escuro" && std::fabs(pt.y - (rr.top + 85.5f)) <= 10) {
            if (pt.x >= rr.right - 132 && pt.x < rr.right - 60) { SetSetting(L"estilo_escuro_auto", L"0"); Rebuild(); }
            else if (pt.x >= rr.right - 60) { SetSetting(L"estilo_escuro_auto", L"1"); Rebuild(); }
        }
    };
    // Cor das pastas: "Automática" segue a cor de destaque; as outras retingem as pastas comuns do Finder (as mesmas cores
    // de Personalizar Pasta, finder\pastacor.cpp), gravado em cor_pastas (-1 = automática)
    static const UINT32 GG_PASTAS[9] = { 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x64D2FF, 0x0A84FF, 0xBF5AF2, 0xFF375F, 0x8E8E93 };
    Row folder = Popup(L"Cor das pastas", { L"Automática", L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Menta", L"Azul", L"Roxo", L"Rosa", L"Cinza" },
                       [] { return std::clamp(_wtoi(Setting(L"cor_pastas", L"-1").c_str()) + 1, 0, 9); },
                       [](int i) { SetSetting(L"cor_pastas", std::to_wstring(i - 1)); Rebuild(); });
    folder.gg = true; folder.height = 40;
    folder.dot = [](int i) { return i <= 0 ? Accent() : GGRgb(GG_PASTAS[std::clamp(i - 1, 0, 8)]); };
    p.groups.push_back(G({ icons, folder }));

    // ---- Janelas (barras de rolagem), Transparência e Efeitos e o tema do Liquinix
    p.groups.push_back(G({ Popup(L"Mostrar barras de rolagem", { L"Ao rolar", L"Sempre" },
                                 [] { DWORD v = 1, cb = sizeof(v); RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", RRF_RT_REG_DWORD, nullptr, &v, &cb); return v ? 0 : 1; },
                                 [](int i) {
                                     DWORD v = i == 0 ? 1 : 0;
                                     RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", REG_DWORD, &v, sizeof(v));
                                     SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Accessibility", SMTO_ABORTIFHUNG, 200, nullptr);
                                 }) }, L"Janelas"));
    p.groups.push_back(G({ Link(L"Transparência e Efeitos", [] { Push(L"Transparência e Efeitos", TransparenciaEfeitos); }, 0xE790, C_BLUE) }));
    Row tema = TemaRow();
    tema.label = L"Estilo do Liquinix";
    if (Setting(L"leopard", L"0") == L"1" && Setting(L"liquid_glass", L"0") != L"1") {
        // Leopard: o Aurora (o papel de parede padrão do 10.5) só como sugestão; trocar de tema nunca muda o papel de
        // parede do usuário (pedido dele), quem troca é este botão
        Row aurora = Button(L"Papel de parede do Leopard", L"Usar o Aurora", [] {
            wchar_t prof[MAX_PATH] = {};
            if (!GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) return;
            std::wstring f = std::wstring(prof) + L"\\Pictures\\Wallpapers macOS\\Leopard\\os-x-leopard-aurora-5120x3200-24048.jpg";
            if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) sys::SetWallpaper(f);
            Rebuild();
        }, L"Aurora, o papel de parede original do Mac OS X Leopard. Trocar de tema não muda o seu papel de parede.");
        p.groups.push_back(G({ tema, aurora }));
    } else p.groups.push_back(G({ tema }));
    return p;
}

// ================= Aparência no Mac OS X Leopard
// O painel "Aparência" das Preferências do Sistema do 10.5: Aparência (Azul / Grafite: botões, menus e janelas), Cor
// de realce (a seleção do texto), barras de rolagem, a barra de menus translúcida (10.5.2) e o tema do Liquinix.
Page AparenciaLeopard() {
    Page p;
    p.groups.push_back(G({
        Popup(L"Aparência", { L"Azul", L"Grafite" }, [] { return Setting(L"destaque", L"azul") == L"grafite" ? 1 : 0; },
              [](int i) { SetSetting(L"destaque", i == 1 ? L"grafite" : L"azul"); Rebuild(); }),
        Popup(L"Cor de realce", { L"Automática", L"Azul", L"Roxo", L"Rosa", L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Grafite" },
              [] { return std::clamp(_wtoi(Setting(L"realce_texto", L"0").c_str()), 0, 8); }, [](int i) { GGAplicaRealce(i); Rebuild(); }),
        Note(L"A aparência vale para os botões, menus e janelas."),
    }));
    p.groups.push_back(G({
        Popup(L"Mostrar barras de rolagem", { L"Ao rolar", L"Sempre" },
              [] { DWORD v = 1, cb = sizeof(v); RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", RRF_RT_REG_DWORD, nullptr, &v, &cb); return v ? 0 : 1; },
              [](int i) {
                  DWORD v = i == 0 ? 1 : 0;
                  RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", REG_DWORD, &v, sizeof(v));
                  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Accessibility", SMTO_ABORTIFHUNG, 200, nullptr);
              }),
        Toggle(L"Barra de menus translúcida", [] { return Setting(L"barra_translucida", L"1") == L"1"; },
               [](bool v) { SetSetting(L"barra_translucida", v ? L"1" : L"0"); Rebuild(); }),
    }));
    Row tema = TemaRow();
    tema.label = L"Estilo do Liquinix";
    Row aurora = Button(L"Papel de parede do Leopard", L"Usar o Aurora", [] {
        wchar_t prof[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) return;
        std::wstring f = std::wstring(prof) + L"\\Pictures\\Wallpapers macOS\\Leopard\\os-x-leopard-aurora-5120x3200-24048.jpg";
        if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) sys::SetWallpaper(f);
        Rebuild();
    }, L"Aurora, o papel de parede original do Mac OS X Leopard. Trocar de tema não muda o seu papel de parede.");
    p.groups.push_back(G({ tema, aurora }));
    return p;
}

// ================= Aparência > Transparência e Efeitos
// O vidro do Shell (Dock, barra de menus, menus, Central de Controle, Spotlight e painéis): o do
// Sequoia, ou o Liquid Glass do macOS Tahoe (lente nas bordas, brilho especular, cantos redondos)
// em "Nítido" ou "Tingido", como no 26.1. Tudo vai para %APPDATA%\Shell\ajustes.txt e o Shell
// reaplica na hora (mensagem ShellAjustesMudaram).
Page TransparenciaEfeitos() {
    Page p;
    auto on = [](const wchar_t* k, const wchar_t* d) { return Setting(k, d) == L"1"; };
    auto num = [](const wchar_t* k, const wchar_t* d) { return (float)_wtof(Setting(k, d).c_str()); };
    bool liquid = on(L"liquid_glass", L"0"), reduce = on(L"reduzir_transparencia", L"0");

    // material: três miniaturas (papel de parede com um Dock de vidro em cima)
    Row mat; mat.kind = Kind::Custom; mat.height = 112;
    auto current = [] {
        if (Setting(L"liquid_glass", L"0") != L"1") return 0;
        return Setting(L"liquid_estilo", L"nitido") == L"tingido" ? 2 : 1;
    };
    mat.draw = [current](Ctx& c, D2D1_RECT_F rr) {
        Text(c, L"Material", D2D1::RectF(rr.left + 10, rr.top, rr.left + 160, rr.top + 38), c.body, c.text);
        const wchar_t* names[3] = { L"Sequoia", L"Nítido", L"Tingido" };
        int cur = current();
        float w = 92, h = 58, gap = 16, x = rr.right - 10 - 3 * w - 2 * gap;
        for (int k = 0; k < 3; k++, x += w + gap) {
            D2D1_RECT_F t = D2D1::RectF(x, rr.top + 12, x + w, rr.top + 12 + h);
            // papel de parede: degradê com duas manchas de cor
            ID2D1GradientStopCollection* gs = nullptr;
            D2D1_GRADIENT_STOP st[3] = { { 0, c.dark ? D2D1::ColorF(0.10f, 0.16f, 0.38f, 1) : D2D1::ColorF(0.38f, 0.62f, 0.98f, 1) },
                                         { 0.55f, c.dark ? D2D1::ColorF(0.30f, 0.12f, 0.40f, 1) : D2D1::ColorF(0.78f, 0.52f, 0.95f, 1) },
                                         { 1, c.dark ? D2D1::ColorF(0.55f, 0.20f, 0.25f, 1) : D2D1::ColorF(1.0f, 0.62f, 0.45f, 1) } };
            c.dc->CreateGradientStopCollection(st, 3, &gs);
            ID2D1LinearGradientBrush* lg = nullptr;
            if (gs) c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ t.left, t.top }, { t.right, t.bottom }), gs, &lg);
            if (lg) { c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 6, 6), lg); lg->Release(); }
            if (gs) gs->Release();
            c.br->SetColor(D2D1::ColorF(1.0f, 0.85f, 0.35f, 0.85f));
            c.dc->FillEllipse(D2D1::Ellipse({ t.left + 30, t.bottom - 14 }, 10, 10), c.br);
            c.br->SetColor(D2D1::ColorF(0.2f, 0.85f, 0.75f, 0.8f));
            c.dc->FillEllipse(D2D1::Ellipse({ t.right - 24, t.bottom - 20 }, 8, 8), c.br);
            // "Dock" de vidro
            D2D1_RECT_F d = D2D1::RectF(t.left + 12, t.bottom - 22, t.right - 12, t.bottom - 7);
            float rad = k == 0 ? 3.5f : 7.5f;
            float a = k == 0 ? 0.42f : k == 1 ? 0.16f : 0.62f;
            c.br->SetColor(c.dark ? D2D1::ColorF(0.13f, 0.14f, 0.15f, a + 0.1f) : D2D1::ColorF(1, 1, 1, a));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(d, rad, rad), c.br);
            if (k == 0) {
                c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.5f));
                c.dc->DrawRoundedRectangle(D2D1::RoundedRect(d, rad, rad), c.br, 0.6f);
            } else {  // contorno especular (claro em cima à esquerda e embaixo à direita)
                D2D1_GRADIENT_STOP rs[3] = { { 0, D2D1::ColorF(1, 1, 1, 0.95f) }, { 0.5f, D2D1::ColorF(1, 1, 1, 0.15f) }, { 1, D2D1::ColorF(1, 1, 1, 0.7f) } };
                gs = nullptr; lg = nullptr;
                c.dc->CreateGradientStopCollection(rs, 3, &gs);
                if (gs) c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ d.left, d.top }, { d.right, d.bottom }), gs, &lg);
                if (lg) { c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(d.left + 0.5f, d.top + 0.5f, d.right - 0.5f, d.bottom - 0.5f), rad - 0.5f, rad - 0.5f), lg, 1.0f); lg->Release(); }
                if (gs) gs->Release();
                c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.22f));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(d.left + 3, d.top + 1.5f, d.right - 3, d.top + 4.5f), 1.5f, 1.5f), c.br);
            }
            const UINT32 col[4] = { 0x0A84FF, 0x34C759, 0xFF9500, 0xFF2D55 };
            for (int i = 0; i < 4; i++) {  // ícones
                c.br->SetColor(D2D1::ColorF(((col[i] >> 16) & 255) / 255.f, ((col[i] >> 8) & 255) / 255.f, (col[i] & 255) / 255.f, 1));
                float ix = d.left + 7 + i * ((d.right - d.left - 14 - 9) / 3);
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ix, d.top + 3, ix + 9, d.top + 12), 2.2f, 2.2f), c.br);
            }
            if (k == cur) { c.br->SetColor(c.accent); c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left - 3, t.top - 3, t.right + 3, t.bottom + 3), 8, 8), c.br, 2.5f); }
            Text(c, names[k], D2D1::RectF(t.left - 10, t.bottom + 6, t.right + 10, t.bottom + 24), c.noteCenter, k == cur ? c.text : c.text2);
        }
    };
    mat.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        float w = 92, gap = 16, x = rr.right - 10 - 3 * w - 2 * gap;
        for (int k = 0; k < 3; k++, x += w + gap) {
            if (pt.x < x - 6 || pt.x > x + w + 6) continue;
            if (k == 0) { SetSetting(L"liquid_glass", L"0"); SetSetting(L"golden_gate", L"0"); SetSetting(L"leopard", L"0"); }  // Sequoia: sai do Golden Gate e do Leopard também
            else { SetSetting(L"liquid_estilo", k == 1 ? L"nitido" : L"tingido"); SetSetting(L"liquid_glass", L"1"); }
            Rebuild();
        }
    };
    Row lg = Toggle(L"Liquid Glass", [] { return Setting(L"liquid_glass", L"0") == L"1"; },
                    [](bool v) { SetSetting(L"liquid_glass", v ? L"1" : L"0"); if (!v) SetSetting(L"golden_gate", L"0"); else SetSetting(L"leopard", L"0"); Rebuild(); },
                    L"O vidro do macOS Tahoe: pouco fosco e mais vivo, com brilho nas bordas e cantos mais redondos.");
    lg.enabled = !reduce;
    // vidro real: a tela por baixo, com refração de verdade na borda (como o Tahoe); desligado, o vidro
    // fosco (aparece em gravações de outros programas, que não veem as peças do vidro real)
    Row real = Toggle(L"Vidro real (refração)", [] { return Setting(L"vidro_real", L"1") == L"1"; },
                      [](bool v) { SetSetting(L"vidro_real", v ? L"1" : L"0"); Rebuild(); },
                      L"O fundo aparece através do vidro e dobra nas bordas, com o brilho da luz na borda, como no Tahoe. Desligado, o vidro fica fosco.");
    real.enabled = liquid && !reduce;
    Row refr = Slider(L"Refração nas bordas", 0, 1, [num] { return num(L"liquid_refracao", L"0.6"); },
                      [](float v) { wchar_t b[16]; swprintf_s(b, L"%.2f", v); SetSetting(L"liquid_refracao", b); }, L"Nenhum", L"Forte");
    refr.enabled = liquid && !reduce;
    refr.hasDef = true; refr.def = 0.6f;
    // (o vidro real é montado pelas janelas de trás e aparece nas gravações: não há mais o que trocar para gravar)
    Row redo = Button(L"Se o vidro ficar fosco, preto ou parado, refaz o Liquid Glass sem reiniciar o Shell.", L"Reiniciar o Liquid Glass", [] {
        static UINT msg = RegisterWindowMessageW(L"ShellReiniciarVidro");
        HWND sh = FindWindowExW(HWND_MESSAGE, nullptr, L"ShellMensagens", nullptr);
        if (!sh) sh = FindWindowW(L"ShellMensagens", nullptr);
        if (sh) PostMessageW(sh, msg, 0, 0);
    });
    redo.enabled = liquid && !reduce;
    p.groups.push_back(G({ mat, lg, real, refr, redo }, L"Vidro", L"Vale para o Dock, a barra de menus, os menus, a Central de Controle, o Spotlight e os painéis do Shell."));

    Row red = Toggle(L"Reduzir transparência", [] { return Setting(L"reduzir_transparencia", L"0") == L"1"; },
                     [](bool v) { SetSetting(L"reduzir_transparencia", v ? L"1" : L"0"); Rebuild(); },
                     L"Deixa o vidro quase opaco (e desliga o Liquid Glass).");
    Row tint = Slider(L"Transparência do vidro", 0, 1, [num] { return num(L"vidro_tinta", L"0.5"); },
                      [](float v) { wchar_t b[16]; swprintf_s(b, L"%.2f", v); SetSetting(L"vidro_tinta", b); }, L"Transparente", L"Opaco");
    tint.enabled = !reduce;
    tint.hasDef = true; tint.def = 0.5f;
    Row bar = Toggle(L"Mostrar fundo da barra de menus", [] { return Setting(L"barra_fundo", L"1") != L"0"; },
                     [](bool v) { SetSetting(L"barra_fundo", v ? L"1" : L"0"); },
                     L"Desligado, a barra fica sem fundo, só com o papel de parede atrás (como no Tahoe).");
    bar.enabled = !reduce && !liquid;  // no Tahoe a barra nunca tem fundo, como no macOS 26
    // macOS 26: "Permitir tingir janelas com o papel de parede" (a barra lateral dos apps pega a cor dele)
    Row wtint = Toggle(L"Permitir tingir janelas com o papel de parede", [] { return Setting(L"tingir_janelas", L"1") != L"0"; },
                       [](bool v) { SetSetting(L"tingir_janelas", v ? L"1" : L"0"); },
                       L"A barra lateral dos apps (Lembretes, Calendário, Contatos, Fotos, Música) ganha um pouco da cor do papel de parede. Vale ao abrir o app de novo.");
    p.groups.push_back(G({ red, tint, bar, wtint }, L"Transparência"));

    Row genie = Toggle(L"Efeito gênio ao minimizar", [] { return Setting(L"genio", L"1") == L"1"; },
                       [](bool v) { SetSetting(L"genio", v ? L"1" : L"0"); });
    Row anim = Toggle(L"Animar aplicativos ao abrir", [] { return Setting(L"animar_abertura", L"1") == L"1"; },
                      [](bool v) { SetSetting(L"animar_abertura", v ? L"1" : L"0"); });
    p.groups.push_back(G({ genie, anim }, L"Efeitos"));
    return p;
}

// ================= Apple Intelligence e Siri → Copilot e Fala
Page Inteligencia() {
    Page p;
    p.groups.push_back(Plain(Hero(0xE720, C_PURPLE, L"Assistente e Fala", L"No Windows, o assistente é o Copilot e o reconhecimento de fala fica nos ajustes de Fala.")));
    p.groups.push_back(G({ Uri(L"Copilot", L"ms-copilot:", 0xE99A, C_PURPLE), Uri(L"Fala", L"ms-settings:speech", 0xE720, C_GRAY) }));
    return p;
}

// ================= Central de Controle (a nossa barra de menus)
Page CentralControle() {
    Page p;
    auto showHide = [](const wchar_t* key, const wchar_t* def) {
        std::wstring k = key, d = def;
        return std::make_pair(std::function<int()>([k, d] { return Setting(k.c_str(), d.c_str()) == L"1" ? 0 : 1; }),
                              std::function<void(int)>([k](int i) { SetSetting(k.c_str(), i == 0 ? L"1" : L"0"); }));
    };
    auto wifi = showHide(L"barra_wifi", L"1"), spot = showHide(L"barra_spotlight", L"1");
    Row w = Popup(L"Wi‑Fi", { L"Mostrar na Barra de Menus", L"Não Mostrar na Barra de Menus" }, wifi.first, wifi.second); w.icon = 0xE701; w.iconColor = C_BLUE;
    Row s = Popup(L"Spotlight", { L"Mostrar na Barra de Menus", L"Não Mostrar na Barra de Menus" }, spot.first, spot.second); s.icon = 0xE721; s.iconColor = C_GRAY;
    // som e Bluetooth: fora da barra por padrão, como no Mac
    auto som = showHide(L"barra_som", L"0"), bt = showHide(L"barra_bluetooth", L"0");
    Row so = Popup(L"Som", { L"Mostrar na Barra de Menus", L"Não Mostrar na Barra de Menus" }, som.first, som.second); so.icon = 0xE767; so.iconColor = C_PURPLE;
    Row b = Popup(L"Bluetooth", { L"Mostrar na Barra de Menus", L"Não Mostrar na Barra de Menus" }, bt.first, bt.second); b.icon = 0xE702; b.iconColor = C_BLUE;
    p.groups.push_back(G({ w, b, so, s }, L"Módulos da Central de Controle", L"Estes módulos ficam na Central de Controle. Você pode escolher quando eles também aparecem na barra de menus."));
    // ícones da bandeja: guardados (lista no botão direito da Central de Controle, como o Mac sem setinha),
    // na setinha » ou todos na barra
    Row tray = Popup(L"Ícones dos aplicativos", { L"Guardados (botão direito na Central de Controle)", L"Agrupados no menu »", L"Sempre na barra de menus" },
                     [] { return Setting(L"barra_icones_todos", L"0") == L"1" ? 2 : Setting(L"barra_seta", L"0") == L"1" ? 1 : 0; },
                     [](int i) { SetSetting(L"barra_icones_todos", i == 2 ? L"1" : L"0"); SetSetting(L"barra_seta", i == 1 ? L"1" : L"0"); });
    tray.icon = 0xE71D; tray.iconColor = C_GRAY;
    p.groups.push_back(G({ tray }, L"Barra de Menus", L"Ícones que os aplicativos põem na área de notificação do Windows."));
    Row clock = Popup(L"Relógio", { L"Dia, data e hora", L"Só a hora" }, [] { return Setting(L"relogio_curto", L"0") == L"1" ? 1 : 0; },
                      [](int i) { SetSetting(L"relogio_curto", i == 1 ? L"1" : L"0"); });
    clock.icon = 0xE787; clock.iconColor = C_GRAY;
    p.groups.push_back(G({ clock }));
    // Ilha Dinâmica (src/shell/ilha.cpp): desligada por padrão; as outras opções só valem com ela ligada
    auto on = [](const wchar_t* k, const wchar_t* d) { std::wstring kk = k, dd = d; return std::function<bool()>([kk, dd] { return Setting(kk.c_str(), dd.c_str()) == L"1"; }); };
    auto set = [](const wchar_t* k) { std::wstring kk = k; return std::function<void(bool)>([kk](bool v) { SetSetting(kk.c_str(), v ? L"1" : L"0"); Rebuild(); }); };
    std::vector<Row> ilha = { Toggle(L"Ilha Dinâmica", on(L"ilha", L"0"), set(L"ilha"),
                                     L"Mostra no topo da tela a música tocando, timers, notificações, downloads, o volume e uma estante de arquivos. Passe o ponteiro sobre ela para abrir.") };
    if (Setting(L"ilha", L"0") == L"1") {
        // estilo: notch (entalhe grudado no topo), ilha (pílula sempre na barra) ou só quando acontece algo
        Row est = Popup(L"Estilo", { L"Notch", L"Ilha", L"Só quando houver algo" },
                        [] { std::wstring v = Setting(L"ilha_estilo", L"notch"); return v == L"ilha" ? 1 : v == L"notificacao" ? 2 : 0; },
                        [](int i) { SetSetting(L"ilha_estilo", i == 1 ? L"ilha" : i == 2 ? L"notificacao" : L"notch"); });
        ilha.push_back(est);
        ilha.push_back(Toggle(L"Abrir ao passar o ponteiro", on(L"ilha_passar", L"1"), set(L"ilha_passar")));
        ilha.push_back(Toggle(L"Mostrar notificações na ilha", on(L"ilha_notificacoes", L"1"), set(L"ilha_notificacoes")));
        ilha.push_back(Toggle(L"Mostrar o volume na ilha", on(L"ilha_volume", L"1"), set(L"ilha_volume")));
    }
    p.groups.push_back(G(ilha, L"Ilha Dinâmica"));
    return p;
}

// ================= Personalizar o Dock / a Mesa (pedido de 20/11/2026, na linha do MyDockFinder)
// O Shell lê cada chave (dock.cpp ReadSettings, mesa.cpp LerMedidas, widgets.cpp SettingsChanged) assim que o
// ajustes.txt muda. Chave vazia = o padrão medido do Mac em cada tema.
static float NumSetting(const wchar_t* k, float def) { std::wstring v = Setting(k, L""); return v.empty() ? def : (float)_wtof(v.c_str()); }
static std::function<void(float)> SetNum(const wchar_t* k) { std::wstring kk = k; return [kk](float v) { SetSetting(kk.c_str(), std::to_wstring((int)std::lround(v))); }; }
static Row Flag(const std::wstring& label, const wchar_t* k, const wchar_t* def, const std::wstring& detail = L"") {
    std::wstring kk = k, dd = def;
    return Toggle(label, [kk, dd] { return Setting(kk.c_str(), dd.c_str()) != L"0"; }, [kk](bool on) { SetSetting(kk.c_str(), on ? L"1" : L"0"); }, detail);
}
static std::wstring IconesDockDir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell\\icones-dock"; CoTaskMemFree(p); }
    return d;
}

Page PersonalizarDock() {
    Page p;
    bool tahoe = Setting(L"liquid_glass", L"0") == L"1";
    Row esp = Slider(L"Espaço entre os ícones", 0, 32, [] { return NumSetting(L"dock_espaco", 0); }, SetNum(L"dock_espaco"), L"Juntos", L"Separados");
    esp.hasDef = true; esp.def = 0;
    Row marg = Slider(L"Distância da borda da tela", 0, 40, [] { return NumSetting(L"dock_margem", 5); }, SetNum(L"dock_margem"), L"Encostado", L"Longe");
    marg.hasDef = true; marg.def = 5;
    p.groups.push_back(G({ esp, marg }, L"Tamanho e Espaço"));
    float raioDef = tahoe ? 61.f : 43.f, opacDef = tahoe ? 39.f : 45.f;
    Row raio = Slider(L"Arredondamento dos cantos", 0, 100, [raioDef] { return NumSetting(L"dock_raio", raioDef); }, SetNum(L"dock_raio"), L"Reto", L"Pílula");
    raio.hasDef = true; raio.def = raioDef;
    static const wchar_t* cores[] = { L"auto", L"clara", L"escura", L"destaque" };
    Row cor = Popup(L"Cor do vidro", { L"Automática", L"Clara", L"Escura", L"Cor de destaque" },
                    [] { std::wstring v = Setting(L"dock_cor", L"auto"); for (int i = 0; i < 4; i++) if (v == cores[i]) return i; return 0; },
                    [](int i) { SetSetting(L"dock_cor", cores[std::clamp(i, 0, 3)]); });
    Row opac = Slider(L"Opacidade do vidro", 0, 100, [opacDef] { return NumSetting(L"dock_opacidade", opacDef); }, SetNum(L"dock_opacidade"), L"Transparente", L"Sólido");
    opac.hasDef = true; opac.def = opacDef;
    p.groups.push_back(G({ raio, cor, opac }, L"Fundo"));
    Row ind = Popup(L"Indicador de app aberto", { L"Ponto", L"Traço" }, [] { return Setting(L"dock_indicador", L"ponto") == L"traco" ? 1 : 0; },
                    [](int i) { SetSetting(L"dock_indicador", i == 1 ? L"traco" : L"ponto"); });
    p.groups.push_back(G({ Flag(L"Mostrar o Launchpad", L"dock_launchpad", L"1"), Flag(L"Mostrar a divisória", L"dock_separador", L"1"),
                           Flag(L"Mostrar o nome ao passar o ponteiro", L"dock_rotulos", L"1"), ind,
                           Flag(L"Mostrar indicadores de aplicativos abertos", L"indicadores", L"1"),
                           Flag(L"Animar aplicativos ao abrir", L"animar_abertura", L"1"),
                           Flag(L"Pular quando chega uma notificação", L"pulo_notificacao", L"1") }, L"Itens e Animações"));
    p.groups.push_back(G({ Button(L"Ícones próprios", L"Abrir a Pasta…", [] {
                               std::wstring d = IconesDockDir();
                               if (d.empty()) return;
                               SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
                               ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                           }, L"Ponha um PNG com o nome do .exe (chrome.png, discord.png) ou do app (Google Chrome.png) nesta pasta para trocar o ícone dele no Dock. Imagens de 512 × 512 com a margem do Mac ficam melhores.") }, L"Ícones"));
    p.groups.push_back(Plain(Buttons({ { L"Restaurar o Padrão", [] {
        for (auto k : { L"dock_espaco", L"dock_margem", L"dock_raio", L"dock_opacidade" }) SetSetting(k, L"");
        SetSetting(L"dock_cor", L"auto"); SetSetting(L"dock_indicador", L"ponto");
        for (auto k : { L"dock_launchpad", L"dock_separador", L"dock_rotulos" }) SetSetting(k, L"1");
        Rebuild();
    } } })));
    return p;
}

Page PersonalizarMesa() {
    Page p;
    Row tam = Slider(L"Tamanho dos ícones", 16, 128, [] { return NumSetting(L"mesa_icone_tamanho", 64); }, SetNum(L"mesa_icone_tamanho"), L"Pequeno", L"Grande");
    tam.hasDef = true; tam.def = 64;
    Row esp = Slider(L"Espaço da grade", -20, 80, [] { return NumSetting(L"mesa_espaco", 0); }, SetNum(L"mesa_espaco"), L"Junto", L"Separado");
    esp.hasDef = true; esp.def = 0;
    Row txt = Slider(L"Tamanho do texto", 10, 16, [] { return NumSetting(L"mesa_texto", 12); }, SetNum(L"mesa_texto"), L"10", L"16");
    txt.hasDef = true; txt.def = 12;
    p.groups.push_back(G({ Flag(L"Mostrar ícones na mesa", L"mesa_icones", L"1"), tam, esp, txt,
                           Toggle(L"Alinhar à grade", [] { return Setting(L"mesa_grade", L"0") == L"1"; }, [](bool on) { SetSetting(L"mesa_grade", on ? L"1" : L"0"); }),
                           Toggle(L"Usar Pilhas", [] { return Setting(L"mesa_pilhas", L"0") == L"1"; }, [](bool on) { SetSetting(L"mesa_pilhas", on ? L"1" : L"0"); },
                                  L"Agrupa os arquivos da mesa por tipo.") }, L"Ícones da Mesa"));
    p.groups.push_back(G({ Flag(L"Tempo", L"widget.tempo.on", L"1"), Flag(L"Relógio", L"widget.relogio.on", L"1"), Flag(L"Calendário", L"widget.calendario.on", L"1"),
                           Flag(L"Relógio Mundial", L"widget.mundial.on", L"1"), Flag(L"Nascer do Sol", L"widget.sol.on", L"1") }, L"Widgets na Mesa"));
    p.groups.push_back(Plain(Buttons({ { L"Restaurar o Padrão", [] {
        for (auto k : { L"mesa_icone_tamanho", L"mesa_espaco", L"mesa_texto" }) SetSetting(k, L"");
        Rebuild();
    } } })));
    return p;
}

// ================= Mesa e Dock
Page MesaDock() {
    Page p;
    auto num = [](const wchar_t* k, const wchar_t* d) { return (float)_wtof(Setting(k, d).c_str()); };
    auto flag = [](const wchar_t* k, const wchar_t* d) { std::wstring kk = k, dd = d; return std::function<bool()>([kk, dd] { return Setting(kk.c_str(), dd.c_str()) == L"1"; }); };
    auto setFlag = [](const wchar_t* k) { std::wstring kk = k; return std::function<void(bool)>([kk](bool on) { SetSetting(kk.c_str(), on ? L"1" : L"0"); }); };
    Row size = Slider(L"Tamanho", 16, 128, [num] { return num(L"tamanho", L"48"); }, [](float v) { SetSetting(L"tamanho", std::to_wstring((int)std::lround(v))); }, L"Pequeno", L"Grande");
    Row mag = Toggle(L"Ampliação", flag(L"ampliacao", L"0"), setFlag(L"ampliacao"));
    Row magSize = Slider(L"Tamanho da ampliação", 16, 256, [num] { return num(L"ampliacao_tamanho", L"128"); },
                         [](float v) { SetSetting(L"ampliacao_tamanho", std::to_wstring((int)std::lround(v))); }, L"Pequeno", L"Grande");
    magSize.enabled = Setting(L"ampliacao", L"0") == L"1";
    size.hasDef = true; size.def = 48;
    magSize.hasDef = true; magSize.def = 128;
    // dock_posicao = baixo | esquerda | direita (o Dock do Leopard, a prateleira em perspectiva, fica sempre embaixo)
    Row pos = Popup(L"Posição na tela", { L"Esquerda", L"Embaixo", L"Direita" }, [] {
        std::wstring v = Setting(L"dock_posicao", L"baixo");
        return v == L"esquerda" ? 0 : v == L"direita" ? 2 : 1;
    }, [](int i) { SetSetting(L"dock_posicao", i == 0 ? L"esquerda" : i == 2 ? L"direita" : L"baixo"); });
    // Monitor do Dock (dock_monitor = nome do monitor, ex. \\.\DISPLAY2; vazio = o principal). A barra de
    // menus fica em todos; o Dock só neste.
    std::vector<std::wstring> monDev{ L"" }, monName{ L"Tela principal" };
    {
        struct Ctx { std::vector<std::wstring>*dev, *name; } ctx{ &monDev, &monName };
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM p) -> BOOL {
            auto* c = (Ctx*)p;
            MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hm, &mi) || (mi.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
            DISPLAY_DEVICEW dd{ sizeof(dd) };
            std::wstring name = mi.szDevice;
            if (name.rfind(L"\\\\.\\", 0) == 0) name = name.substr(4);  // \\.\DISPLAY2 -> DISPLAY2
            if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0]) name += std::wstring(L" (") + dd.DeviceString + L")";
            c->dev->push_back(mi.szDevice);
            c->name->push_back(name);
            return TRUE;
        }, (LPARAM)&ctx);
    }
    Row monRow = Popup(L"Monitor do Dock", monName,
                       [monDev] { std::wstring v = Setting(L"dock_monitor", L""); for (int i = 0; i < (int)monDev.size(); i++) if (!_wcsicmp(v.c_str(), monDev[i].c_str())) return i; return 0; },
                       [monDev](int i) { SetSetting(L"dock_monitor", monDev[std::clamp(i, 0, (int)monDev.size() - 1)]); });
    monRow.enabled = monDev.size() > 1;
    // Inteligente: só esconde quando uma janela encosta no Dock (pedido de um usuário no Reddit)
    Row hide = Popup(L"Ocultar o Dock", { L"Nunca", L"Automaticamente", L"Quando uma janela encostar nele" },
                     [] { std::wstring v = Setting(L"ocultar", L"0"); return v == L"1" ? 1 : v == L"inteligente" ? 2 : 0; },
                     [](int i) { SetSetting(L"ocultar", i == 1 ? L"1" : i == 2 ? L"inteligente" : L"0"); });
    Row anim = Toggle(L"Animar aplicativos ao abrir", flag(L"animar_abertura", L"1"), setFlag(L"animar_abertura"));
    Row ind = Toggle(L"Mostrar indicadores de aplicativos abertos", flag(L"indicadores", L"1"), setFlag(L"indicadores"));
    Row dl = Toggle(L"Mostrar a pasta Downloads no Dock", flag(L"dock_downloads", L"1"), setFlag(L"dock_downloads"));
    Row pers = Link(L"Personalizar o Dock…", [] { Push(L"Personalizar o Dock", PersonalizarDock); });
    p.groups.push_back(G({ size, mag, magSize, pos, monRow, hide, anim, ind, dl, pers }, L"Dock"));
    // como no macOS 15/26 (Mesa e Dock > Área de Trabalho e Organizador Visual)
    // macOS 14+: "Mostrar Itens: Na Mesa" (o mesmo que o menu da mesa "Ocultar Ícones da Mesa")
    Row icons = Toggle(L"Mostrar ícones na mesa", [] { return Setting(L"mesa_icones", L"1") != L"0"; }, [](bool v) { SetSetting(L"mesa_icones", v ? L"1" : L"0"); });
    Row persMesa = Link(L"Personalizar a Mesa…", [] { Push(L"Personalizar a Mesa", PersonalizarMesa); });
    p.groups.push_back(G({ icons, persMesa, Popup(L"Clicar no papel de parede para mostrar a mesa", { L"Sempre", L"Só no Organizador Visual", L"Nunca" },
                                 [] { std::wstring v = Setting(L"clique_mostra_mesa", L"palco"); return v == L"1" ? 0 : v == L"0" ? 2 : 1; },
                                 [](int i) { SetSetting(L"clique_mostra_mesa", i == 0 ? L"1" : i == 2 ? L"0" : L"palco"); }) }, L"Área de Trabalho"));
    p.groups.push_back(G({ Toggle(L"Arrastar janelas para as bordas da tela para organizar", [] { return sys::SnapEnabled(); }, [](bool on) { sys::SetSnapEnabled(on); }),
                           Uri(L"Navegador padrão", L"ms-settings:defaultapps") }, L"Janelas"));
    // Organizador Visual (Stage Manager): só o app da frente na tela, os outros numa faixa à esquerda
    p.groups.push_back(G({ Toggle(L"Organizador Visual", flag(L"organizador", L"0"), setFlag(L"organizador")) }, L"",
                         L"Os outros apps ficam numa faixa à esquerda; clique numa miniatura para trazer o app."));
    // Cantos Ativos (como no Mac): uma ação por canto da tela; o Shell lê canto_se/sd/ie/id
    {
        static const wchar_t* vals[] = { L"nada", L"mission", L"janelas", L"mesa", L"launchpad", L"notificacoes", L"central", L"spotlight", L"bloquear", L"tela", L"nota" };
        static const std::vector<std::wstring> names = { L"Nada", L"Mission Control", L"Janelas do Aplicativo", L"Mesa", L"Launchpad", L"Central de Notificações",
                                                         L"Central de Controle", L"Spotlight", L"Bloquear Tela", L"Descansar a Tela", L"Nota Rápida" };
        const int NV = (int)(sizeof(vals) / sizeof(vals[0]));
        auto corner = [](const wchar_t* label, const wchar_t* key, const wchar_t* def) {
            std::wstring k = key, d = def;
            return Popup(label, names,
                         [k, d] { std::wstring v = Setting(k.c_str(), d.c_str()); for (int i = 0; i < NV; i++) if (v == vals[i]) return i; return 0; },
                         [k](int i) { SetSetting(k.c_str(), vals[std::clamp(i, 0, NV - 1)]); });
        };
        p.groups.push_back(G({ corner(L"Canto superior esquerdo", L"canto_se", L"nada"), corner(L"Canto superior direito", L"canto_sd", L"nada"),
                               corner(L"Canto inferior esquerdo", L"canto_ie", L"nada"), corner(L"Canto inferior direito", L"canto_id", L"nota"),
                               Note(L"Leve o ponteiro até um canto da tela para fazer a ação. Não funciona arrastando nem com um app em tela cheia.") },
                             L"Cantos Ativos"));
    }
    return p;
}

// ================= Telas
static bool g_allRes;  // Telas: "Mostrar todas as resoluções"

Page Telas() {
    Page p;
    DISPLAY_DEVICEW dd{ sizeof(dd) }, mon{ sizeof(mon) };
    std::wstring name = L"Tela";
    for (DWORD k = 0; EnumDisplayDevicesW(nullptr, k, &dd, 0); k++)
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { if (EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0) && mon.DeviceString[0]) name = mon.DeviceString; break; }
    auto mode = sys::CurrentResolution();
    std::wstring wall = sys::Wallpaper();
    // monitor com a imagem de fundo na tela e o pé, como no macOS 26; nome embaixo
    Row head; head.kind = Kind::Custom; head.height = 150;
    head.draw = [name, mode, wall](Ctx& c, D2D1_RECT_F rr) {
        float cx = (rr.left + rr.right) / 2;
        float ar = mode.h ? (float)mode.w / mode.h : 1.6f;
        float h = 78, w = std::min(220.f, h * ar);
        D2D1_RECT_F scr = D2D1::RectF(cx - w / 2, rr.top + 14, cx + w / 2, rr.top + 14 + h);
        c.br->SetColor(D2D1::ColorF(0.08f, 0.08f, 0.09f, 1));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(scr, 5, 5), c.br);
        D2D1_RECT_F in = D2D1::RectF(scr.left + 3, scr.top + 3, scr.right - 3, scr.bottom - 3);
        if (ID2D1Bitmap* b = BitmapFromFile(wall, 480)) {
            auto s2 = b->GetSize();
            float k = std::max((in.right - in.left) / s2.width, (in.bottom - in.top) / s2.height);
            float dw = s2.width * k, dh = s2.height * k, mx = (in.left + in.right) / 2, my = (in.top + in.bottom) / 2;
            c.dc->PushAxisAlignedClip(in, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            c.dc->DrawBitmap(b, D2D1::RectF(mx - dw / 2, my - dh / 2, mx + dw / 2, my + dh / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            c.dc->PopAxisAlignedClip();
        } else { c.br->SetColor(c.accent); c.dc->FillRectangle(in, c.br); }
        c.br->SetColor(D2D1::ColorF(0.55f, 0.55f, 0.57f, 1));  // pé
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 22, scr.bottom, cx + 22, scr.bottom + 6), 1.5f, 1.5f), c.br);
        Text(c, name, D2D1::RectF(rr.left, scr.bottom + 14, rr.right, scr.bottom + 34), c.center, c.text);
        // setinha embaixo do nome (a tela escolhida), como no Mac
        c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.08f));
        c.dc->FillRectangle(D2D1::RectF(rr.left - 20, rr.bottom - 1, rr.right + 20, rr.bottom), c.br);
    };
    p.groups.push_back(Plain(head));

    // resoluções numa lista dentro da caixa (a atual destacada); "Mostrar todas" abre as de outras proporções
    auto res = sys::Resolutions();
    std::vector<sys::Mode> shown;
    float curAr = mode.h ? (float)mode.w / mode.h : 0;
    for (auto& m : res) {
        bool dup = false;
        for (auto& o : shown) if (o.w == m.w && o.h == m.h) dup = true;
        if (dup) continue;
        if (!g_allRes && m.h && std::fabs((float)m.w / m.h - curAr) > 0.02f) continue;
        shown.push_back(m);
    }
    std::sort(shown.begin(), shown.end(), [](const sys::Mode& a, const sys::Mode& b) { return a.w * a.h > b.w * b.h; });
    if (!g_allRes && shown.size() > 6) {  // as 6 em volta da atual
        int ci = 0;
        for (int k = 0; k < (int)shown.size(); k++) if (shown[k].w == mode.w && shown[k].h == mode.h) ci = k;
        int from = std::clamp(ci - 2, 0, (int)shown.size() - 6);
        shown = std::vector<sys::Mode>(shown.begin() + from, shown.begin() + from + 6);
    }
    Group rg = G({});
    if (!shown.empty()) {
        Row list; list.kind = Kind::Custom; list.height = 8 + shown.size() * 34.f;
        list.draw = [shown, mode](Ctx& c, D2D1_RECT_F rr) {
            for (size_t k = 0; k < shown.size(); k++) {
                D2D1_RECT_F r = D2D1::RectF(rr.left + 8, rr.top + 4 + k * 34, rr.right - 8, rr.top + 4 + (k + 1) * 34);
                bool cur = shown[k].w == mode.w && shown[k].h == mode.h;
                if (cur) { c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.08f)); c.dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), c.br); }
                else if (k > 0 && !(shown[k - 1].w == mode.w && shown[k - 1].h == mode.h)) {
                    c.br->SetColor(c.line); c.dc->FillRectangle(D2D1::RectF(r.left + 10, r.top - 0.5f, r.right - 10, r.top + 0.5f), c.br);
                }
                Text(c, std::to_wstring(shown[k].w) + L" × " + std::to_wstring(shown[k].h), D2D1::RectF(r.left + 10, r.top, r.right - 10, r.bottom), c.body, c.text);
            }
        };
        list.clickAt = [shown](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
            int k = (int)((pt.y - rr.top - 4) / 34);
            if (k >= 0 && k < (int)shown.size()) { sys::SetResolution(shown[k]); Rebuild(); }
        };
        rg.rows.push_back(list);
    }
    rg.rows.push_back(Toggle(L"Mostrar todas as resoluções", [] { return g_allRes; }, [](bool on) { g_allRes = on; Rebuild(); }));
    p.groups.push_back(rg);

    float b = sys::Brightness();
    if (b >= 0) p.groups.push_back(G({ Slider(L"Brilho", 0, 1, [] { return std::max(0.f, sys::Brightness()); }, [](float v) { sys::SetBrightness(v); },
                                              std::wstring(1, (wchar_t)0xE706), std::wstring(1, (wchar_t)0xE706)) }));
    auto rates = sys::RefreshRates();
    if (rates.size() > 1) {
        std::vector<std::wstring> ol;
        int cr = sys::RefreshRate(), ci = 0;
        for (int k = 0; k < (int)rates.size(); k++) { ol.push_back(std::to_wstring(rates[k]) + L" Hertz"); if (rates[k] == cr) ci = k; }
        p.groups.push_back(G({ Popup(L"Taxa de atualização", ol, [ci] { return ci; }, [rates](int k) { sys::SetRefreshRate(rates[k]); }) }));
    }
    p.groups.push_back(G({ Link(L"Escala", [] { OpenUri(L"ms-settings:display"); }, 0, 0, [] { return Fmt(L"%d%%", (int)std::lround(sys::ScaleFactor() * 100)); }) }));
    p.groups.push_back(Plain(Buttons({ { L"Avançado…", [] { OpenUri(L"ms-settings:display-advanced"); } }, { L"Night Shift…", [] { OpenUri(L"ms-settings:nightlight"); } } })));
    return p;
}

// ================= Protetor de Tela
const wchar_t* SAVER_FILES[] = { L"", L"scrnsave.scr", L"Bubbles.scr", L"Ribbons.scr", L"Mystify.scr", L"PhotoScreensaver.scr", L"ssText3d.scr" };
const wchar_t* SAVER_NAMES[] = { L"Nenhum", L"Em Branco", L"Bolhas", L"Fitas", L"Mistificar", L"Fotos", L"Texto 3D" };

Page Protetor() {
    Page p;
    std::vector<std::wstring> names(std::begin(SAVER_NAMES), std::end(SAVER_NAMES));
    p.groups.push_back(G({
        Popup(L"Protetor de tela", names,
              [] {
                  wchar_t buf[MAX_PATH] = {}; DWORD cb = sizeof(buf);
                  RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"SCRNSAVE.EXE", RRF_RT_REG_SZ, nullptr, buf, &cb);
                  std::wstring f = PathFindFileNameW(buf);
                  for (int i = 1; i < 7; i++) if (_wcsicmp(f.c_str(), SAVER_FILES[i]) == 0) return i;
                  return 0;
              },
              [](int i) {
                  wchar_t sysdir[MAX_PATH]; GetSystemDirectoryW(sysdir, MAX_PATH);
                  if (i == 0) { RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"SCRNSAVE.EXE"); SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); return; }
                  std::wstring path = std::wstring(sysdir) + L"\\" + SAVER_FILES[i];
                  RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"SCRNSAVE.EXE", REG_SZ, path.c_str(), (DWORD)((path.size() + 1) * 2));
                  SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
              }),
        Popup(L"Iniciar o protetor de tela quando inativo", MinuteLabels(L"Após "),
              [] { UINT s = 0; SystemParametersInfoW(SPI_GETSCREENSAVETIMEOUT, 0, &s, 0); BOOL on = FALSE; SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &on, 0); return MinuteIndex(on ? (int)(s / 60) : 0); },
              [](int i) { if (MINUTES[i]) SystemParametersInfoW(SPI_SETSCREENSAVETIMEOUT, MINUTES[i] * 60, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
                          else SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE); }),
    }));
    p.groups.push_back(Plain(Buttons({ { L"Pré-visualizar", [] {
        wchar_t buf[MAX_PATH] = {}; DWORD cb = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"SCRNSAVE.EXE", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS)
            ShellExecuteW(nullptr, L"open", buf, L"/s", nullptr, SW_SHOWNORMAL);
    } } })));
    return p;
}

// ================= Spotlight
Page Spotlight() {
    Page p;
    p.groups.push_back(G({ Uri(L"Permissões de Pesquisa", L"ms-settings:search-permissions", 0xE721, C_GRAY),
                           Uri(L"Pesquisa no Windows (Indexação)", L"ms-settings:cortana-windowssearch", 0xE8B7, C_BLUE) },
                         L"Resultados da Pesquisa", L"O Spotlight usa o índice de pesquisa do Windows para achar arquivos."));
    return p;
}

// ================= Imagem de Fundo
const wchar_t* FIT_NAMES[] = { L"Centralizar", L"Lado a Lado", L"Esticar", L"Ajustar à Tela", L"Preencher Tela", L"Estender" };

static bool g_allPhotos;  // Imagem de Fundo: "Mostrar Todas"

void AddPhoto() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC spec[] = { { L"Imagens", L"*.jpg;*.jpeg;*.png;*.bmp" } };
    d->SetFileTypes(1, spec);
    if (SUCCEEDED(d->Show(Window()))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) { sys::SetWallpaper(path); CoTaskMemFree(path); }
            it->Release();
        }
    }
    d->Release();
    Rebuild();
}

Page Fundo() {
    Page p;
    std::wstring cur = sys::Wallpaper();
    // como no macOS 26: a imagem atual à esquerda; à direita, numa caixa, o nome e o ajuste (menu), e embaixo
    // os botões
    Row head; head.kind = Kind::Custom; head.height = 128;
    auto layout = [](D2D1_RECT_F rr, D2D1_RECT_F& t, D2D1_RECT_F& box, D2D1_RECT_F& b1, D2D1_RECT_F& b2) {
        t = D2D1::RectF(rr.left, rr.top + 6, rr.left + 190, rr.top + 6 + 112);
        box = D2D1::RectF(t.right + 16, rr.top + 6, rr.right, rr.top + 6 + 50);
        // botões embaixo da caixa, alinhados à direita dela (sem passar por cima da imagem em janela estreita)
        float bw = std::min(146.f, (box.right - box.left - 10) / 2);
        b2 = D2D1::RectF(box.right - bw, box.bottom + 12, box.right, box.bottom + 38);
        b1 = D2D1::RectF(b2.left - 10 - bw, box.bottom + 12, b2.left - 10, box.bottom + 38);
    };
    head.draw = [cur, layout](Ctx& c, D2D1_RECT_F rr) {
        D2D1_RECT_F t, box, b1, b2; layout(rr, t, box, b1, b2);
        c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0, 0, 0, 0.06f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 8, 8), c.br);
        if (ID2D1Bitmap* b = BitmapFromFile(cur, 400)) {
            auto s2 = b->GetSize();
            float k = std::max((t.right - t.left) / s2.width, (t.bottom - t.top) / s2.height);
            float w = s2.width * k, h = s2.height * k, cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
            ID2D1Factory* f = nullptr; c.dc->GetFactory(&f);
            ID2D1RoundedRectangleGeometry* rg = nullptr; f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(t, 8, 8), &rg);
            c.dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), nullptr);
            c.dc->DrawBitmap(b, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            c.dc->PopLayer();
            rg->Release(); f->Release();
        }
        c.br->SetColor(c.group);
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(box, 12, 12), c.br);
        std::wstring n = PathFindFileNameW(cur.c_str());
        size_t dot = n.rfind(L'.'); if (dot != std::wstring::npos) n = n.substr(0, dot);
        std::wstring fit = FIT_NAMES[std::clamp(sys::WallpaperFit(), 0, 5)];
        float fw = 0;
        IDWriteTextLayout* tl = nullptr;
        if (SUCCEEDED(gfx::dw->CreateTextLayout(fit.c_str(), (UINT32)fit.size(), c.body, 400, 30, &tl))) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); fw = m.width; tl->Release(); }
        Text(c, n.empty() ? L"Imagem de fundo" : n, D2D1::RectF(box.left + 12, box.top, box.right - fw - 50, box.bottom), c.body, c.text);
        Text(c, fit, D2D1::RectF(box.right - 38 - fw, box.top, box.right - 30, box.bottom), c.body, c.text);
        PopupBadge(c, box.right - 18, (box.top + box.bottom) / 2);
        ButtonFace(c, b1, L"Protetor de Tela…");
        ButtonFace(c, b2, L"Adicionar Foto…");
    };
    head.clickAt = [layout](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
        D2D1_RECT_F t, box, b1, b2; layout(rr, t, box, b1, b2);
        auto in = [&](D2D1_RECT_F r) { return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom; };
        if (in(box)) {
            int k = Menu(D2D1::RectF(box.right - 160, box.top, box.right, box.bottom), std::vector<std::wstring>(std::begin(FIT_NAMES), std::end(FIT_NAMES)), sys::WallpaperFit());
            if (k >= 0) { sys::SetWallpaperFit(k); Rebuild(); }
        } else if (in(b1)) Push(L"Protetor de Tela", Protetor);
        else if (in(b2)) AddPhoto();
    };
    p.groups.push_back(Plain(head));
    // seções como no Ajustes do macOS 26: Papéis de Parede Dinâmicos (um por versão do sistema, do mais novo
    // ao mais antigo; "Mostrar Todas" abre todos), Suas Fotos (com "Adicionar Foto…" primeiro), os Aerials por
    // categoria (Paisagem, Cidades, Debaixo d'Água, Terra) e Mac. Os do sistema ficam em
    // Pictures\Wallpapers macOS\<versão> e os Aerials em Pictures\Wallpapers macOS\Aerials\<categoria>.
    struct Item { std::wstring path, label; };
    struct Sec { std::wstring title; std::vector<Item> items; };
    static std::vector<Sec> secs;
    static DWORD at = 0;
    if (secs.empty() || GetTickCount() - at > 30000) {
        secs.clear();
        at = GetTickCount();
        // na pasta Pictures do perfil (fora do OneDrive: são ~2 GB que não precisam ir para a nuvem); se não
        // existir, na pasta Imagens do Windows
        std::wstring macDir;
        wchar_t prof[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) {
            std::wstring d = std::wstring(prof) + L"\\Pictures\\Wallpapers macOS";
            if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) macDir = d;
        }
        PWSTR pics = nullptr;
        if (macDir.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) { macDir = std::wstring(pics) + L"\\Wallpapers macOS"; CoTaskMemFree(pics); }
        auto list = [](const std::wstring& dir) {
            std::vector<std::wstring> files;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    std::wstring ext = PathFindExtensionW(fd.cFileName);
                    for (auto& ch : ext) ch = towlower(ch);
                    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png") files.push_back(dir + L"\\" + fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            std::sort(files.begin(), files.end());
            return files;
        };
        auto stem = [](const std::wstring& f) {
            std::wstring n = PathFindFileNameW(f.c_str());
            size_t dot = n.rfind(L'.');
            return dot == std::wstring::npos ? n : n.substr(0, dot);
        };
        // dinâmicos: o primeiro de cada versão vem antes (a fileira fechada mostra Tahoe, Sequoia, Sonoma…)
        static const wchar_t* VERS[] = { L"Tahoe", L"macOS 27", L"Sequoia", L"Sonoma", L"Ventura", L"Monterey", L"Big Sur", L"Catalina", L"Mojave",
                                         L"High Sierra", L"Sierra", L"El Capitan", L"Yosemite", L"Mavericks", L"Mountain Lion", L"Lion",
                                         L"Snow Leopard", L"Leopard", L"Tiger", L"Panther", L"Jaguar", L"Cheetah" };
        Sec dyn{ L"Papéis de Parede Dinâmicos", {} };
        std::vector<Item> rest;
        for (auto* v : VERS) {
            auto files = list(macDir + L"\\" + v);
            for (size_t k = 0; k < files.size(); k++) (k == 0 ? dyn.items : rest).push_back({ files[k], v });
        }
        dyn.items.insert(dyn.items.end(), rest.begin(), rest.end());
        if (!dyn.items.empty()) secs.push_back(dyn);
        // suas fotos: as outras pastas de papel de parede (sem as do macOS); "Adicionar Foto…" é o primeiro quadro
        Sec mine{ L"Suas Fotos", { { L"*adicionar*", L"Adicionar Foto…" } } };
        for (auto& f : sys::WallpaperChoices())
            if (macDir.empty() || _wcsnicmp(f.c_str(), macDir.c_str(), macDir.size())) mine.items.push_back({ f, stem(f) });
        secs.push_back(mine);
        for (auto* c : { L"Paisagem", L"Cidades", L"Debaixo d'Água", L"Terra" }) {
            Sec a{ c, {} };
            for (auto& f : list(macDir + L"\\Aerials\\" + c)) a.items.push_back({ f, stem(f) });
            if (!a.items.empty()) secs.push_back(a);
        }
        Sec mac{ L"Mac", {} };
        for (auto& f : list(macDir + L"\\Mac")) {  // nome do arquivo do site sem resolução e número
            std::wstring n = stem(f), nice;
            std::vector<std::wstring> parts; std::wstring cur2;
            for (wchar_t ch : n) { if (ch == L'-' || ch == L'_') { if (!cur2.empty()) parts.push_back(cur2); cur2.clear(); } else cur2 += ch; }
            if (!cur2.empty()) parts.push_back(cur2);
            while (!parts.empty() && iswdigit(parts.back()[0])) parts.pop_back();
            for (size_t k = 0; k < parts.size() && k < 3; k++) {
                std::wstring w2 = parts[k];
                if (w2 == L"macos") w2 = L"macOS"; else if (w2 == L"imac") w2 = L"iMac"; else if (w2 == L"macbook") w2 = L"MacBook"; else if (!w2.empty()) w2[0] = towupper(w2[0]);
                nice += (nice.empty() ? L"" : L" ") + w2;
            }
            mac.items.push_back({ f, nice });
        }
        if (!mac.items.empty()) secs.push_back(mac);
    }
    static std::set<std::wstring> openSecs;  // seções abertas ("Mostrar Todas")
    const float TW = 104, TH = 64, LABEL = 20, GAP = 12, HEAD = 30;
    for (size_t si = 0; si < secs.size(); si++) {
        std::wstring title = secs[si].title;
        const std::vector<Item>* items = &secs[si].items;
        int perRow = 4;
        bool open = openSecs.count(title) > 0;
        size_t count = open ? items->size() : std::min<size_t>(items->size(), perRow);
        Row grid; grid.kind = Kind::Custom;
        grid.height = HEAD + ((float)((count + perRow - 1) / perRow)) * (TH + LABEL + GAP) + 10;
        grid.draw = [cur, TW, TH, LABEL, GAP, HEAD, count, title, items, open](Ctx& c, D2D1_RECT_F rr) {
            Text(c, title, D2D1::RectF(rr.left + 2, rr.top, rr.right - 160, rr.top + 24), c.bold, c.text);
            if (items->size() > 4) {
                c.body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                Text(c, open ? std::wstring(L"Mostrar Menos") : Fmt(L"Mostrar Todas (%d)", (int)items->size()), D2D1::RectF(rr.right - 200, rr.top, rr.right, rr.top + 24), c.body, c.text2);
                c.body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            rr.top += HEAD;
            int perRow = std::max(1, (int)((rr.right - rr.left + GAP) / (TW + GAP)));
            float tw = (rr.right - rr.left - (perRow - 1) * GAP) / perRow, th = tw * TH / TW;
            for (size_t i = 0; i < count; i++) {
                float x = rr.left + (i % perRow) * (tw + GAP), y = rr.top + (i / perRow) * (th + LABEL + GAP);
                D2D1_RECT_F t = D2D1::RectF(x, y, x + tw, y + th);
                c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0, 0, 0, 0.05f));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 6, 6), c.br);
                const Item& it = (*items)[i];
                if (it.path == L"*adicionar*") {  // quadro "Adicionar Foto…": foto com um + no meio
                    float cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
                    c.br->SetColor(c.text2);
                    c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 13, cy - 10, cx + 11, cy + 8), 3, 3), c.br, 1.5f);
                    c.dc->FillEllipse(D2D1::Ellipse({ cx + 11, cy + 8 }, 7, 7), c.br);
                    c.br->SetColor(c.dark ? D2D1::ColorF(0.2f, 0.2f, 0.21f, 1) : D2D1::ColorF(1, 1, 1, 1));
                    c.dc->FillRectangle(D2D1::RectF(cx + 10.2f, cy + 4.5f, cx + 11.8f, cy + 11.5f), c.br);
                    c.dc->FillRectangle(D2D1::RectF(cx + 7.5f, cy + 7.2f, cx + 14.5f, cy + 8.8f), c.br);
                    Text(c, it.label, D2D1::RectF(t.left, t.bottom + 3, t.right, t.bottom + LABEL), c.noteCenter, c.text);
                    continue;
                }
                if (ID2D1Bitmap* b = BitmapFromFile(it.path, 256)) {
                    auto s2 = b->GetSize();
                    float k = std::max(tw / s2.width, th / s2.height);
                    float w = s2.width * k, h = s2.height * k, cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
                    ID2D1Factory* fac = nullptr; c.dc->GetFactory(&fac);
                    ID2D1RoundedRectangleGeometry* rg = nullptr; fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(t, 6, 6), &rg);
                    c.dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), nullptr);
                    c.dc->DrawBitmap(b, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    c.dc->PopLayer();
                    rg->Release(); fac->Release();
                }
                if (_wcsicmp(it.path.c_str(), cur.c_str()) == 0) {
                    c.br->SetColor(c.accent);
                    c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left - 2.5f, t.top - 2.5f, t.right + 2.5f, t.bottom + 2.5f), 8, 8), c.br, 2.5f);
                }
                Text(c, it.label, D2D1::RectF(t.left, t.bottom + 3, t.right, t.bottom + LABEL), c.noteCenter, c.text);
            }
        };
        grid.clickAt = [TW, TH, LABEL, GAP, HEAD, count, title, items](D2D1_POINT_2F pt, D2D1_RECT_F rr) {
            if (pt.y < rr.top + HEAD) {
                if (pt.x > rr.right - 200 && items->size() > 4) { if (openSecs.count(title)) openSecs.erase(title); else openSecs.insert(title); Rebuild(); }
                return;
            }
            rr.top += HEAD;
            int perRow = std::max(1, (int)((rr.right - rr.left + GAP) / (TW + GAP)));
            float tw = (rr.right - rr.left - (perRow - 1) * GAP) / perRow, th = tw * TH / TW;
            int col = (int)((pt.x - rr.left) / (tw + GAP)), row = (int)((pt.y - rr.top) / (th + LABEL + GAP));
            size_t i = (size_t)row * perRow + col;
            if (col < 0 || col >= perRow || i >= count) return;
            if ((*items)[i].path == L"*adicionar*") { AddPhoto(); return; }
            sys::SetWallpaper((*items)[i].path);
            Rebuild();
        };
        Group g; g.box = false; g.rows.push_back(grid);
        p.groups.push_back(g);
    }
    return p;
}

// ================= Notificações
Page Notificacoes() {
    Page p;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
    p.groups.push_back(G({ Toggle(L"Permitir notificações",
                                  [key] { DWORD v = 1, cb = sizeof(v); RegGetValueW(HKEY_CURRENT_USER, key, L"ToastEnabled", RRF_RT_REG_DWORD, nullptr, &v, &cb); return v != 0; },
                                  [key](bool on) { DWORD v = on; RegSetKeyValueW(HKEY_CURRENT_USER, key, L"ToastEnabled", REG_DWORD, &v, sizeof(v)); },
                                  L"Desligado, os aplicativos não mostram avisos (pode ser preciso reiniciar a sessão).") }));
    p.groups.push_back(G({ Uri(L"Notificações por Aplicativo", L"ms-settings:notifications", 0xEA8F, C_RED) }, L"Notificações dos Aplicativos"));
    // banners e Central de Notificações do Liquinix (notificacoes.cpp)
    Row dur = Slider(L"Tempo do banner na tela", 2, 30, [] { return NumSetting(L"notif_duracao", 5); }, SetNum(L"notif_duracao"), L"2 s", L"30 s");
    dur.hasDef = true; dur.def = 5;
    p.groups.push_back(G({ Flag(L"Banners do Mac", L"banners_mac", L"1", L"Os avisos dos aplicativos aparecem no canto de cima, no visual do Mac."), dur,
                           Flag(L"Calendário na Central de Notificações", L"central_calendario", L"1"),
                           Flag(L"Relógio na Central de Notificações", L"central_relogio", L"1") }, L"Central de Notificações"));
    return p;
}

// ================= Som
int g_soundTab = 0;
Page Som() {
    Page p;
    p.groups.push_back(G({ Link(L"Sons de alerta", [] { ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl,,2", nullptr, SW_SHOWNORMAL); }) }, L"Efeitos Sonoros"));
    Row tabs; tabs.kind = Kind::Custom; tabs.height = 44;
    tabs.draw = [](Ctx& c, D2D1_RECT_F rr) {
        float w = 160, cx = (rr.left + rr.right) / 2;
        D2D1_RECT_F seg = D2D1::RectF(cx - w / 2, rr.top + 10, cx + w / 2, rr.top + 34);
        c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0, 0, 0, 0.06f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(seg, 6, 6), c.br);
        D2D1_RECT_F on = g_soundTab == 0 ? D2D1::RectF(seg.left + 2, seg.top + 2, cx - 1, seg.bottom - 2) : D2D1::RectF(cx + 1, seg.top + 2, seg.right - 2, seg.bottom - 2);
        c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(1, 1, 1, 1));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(on, 5, 5), c.br);
        Text(c, L"Saída", D2D1::RectF(seg.left, seg.top, cx, seg.bottom), c.center, c.text);
        Text(c, L"Entrada", D2D1::RectF(cx, seg.top, seg.right, seg.bottom), c.center, c.text);
    };
    tabs.clickAt = [](D2D1_POINT_2F pt, D2D1_RECT_F rr) { g_soundTab = pt.x < (rr.left + rr.right) / 2 ? 0 : 1; Rebuild(); };
    Group io = G({ tabs }, L"Saída e Entrada");
    bool out = g_soundTab == 0;
    for (auto& d : sys::AudioDevices(out)) {
        Row r; r.kind = Kind::Custom; r.height = 34;
        std::wstring id = d.id, name = d.name; bool def = d.isDefault;
        r.draw = [name, def](Ctx& c, D2D1_RECT_F rr) {
            D2D1_RECT_F b = D2D1::RectF(rr.left + 6, rr.top + 2, rr.right - 6, rr.bottom - 2);
            if (def) { c.br->SetColor(c.accent); c.dc->FillRoundedRectangle(D2D1::RoundedRect(b, 5, 5), c.br); }
            Text(c, name, D2D1::RectF(b.left + 10, b.top, b.right - 10, b.bottom), c.body, def ? D2D1::ColorF(1, 1, 1, 1) : c.text);
        };
        r.clickAt = [id](D2D1_POINT_2F, D2D1_RECT_F) { sys::SetDefaultAudio(id); Rebuild(); };
        io.rows.push_back(r);
    }
    if (out) {
        Row vol = Slider(L"Volume de saída", 0, 1, [] { return sys::Volume(); }, [](float v) { sys::SetVolume(v); }, std::wstring(1, (wchar_t)0xE993), std::wstring(1, (wchar_t)0xE995));
        Row mute = Toggle(L"Mudo", [] { return sys::Muted(); }, [](bool m) { sys::SetMuted(m); });
        io.rows.push_back(vol);
        io.rows.push_back(mute);
    } else if (sys::InputVolume() >= 0) {
        io.rows.push_back(Slider(L"Volume de entrada", 0, 1, [] { return std::max(0.f, sys::InputVolume()); }, [](float v) { sys::SetInputVolume(v); },
                                 std::wstring(1, (wchar_t)0xE720), std::wstring(1, (wchar_t)0xE720)));
    }
    p.groups.push_back(io);
    p.groups.push_back(Plain(Buttons({ { L"Mais Ajustes de Som…", [] { OpenUri(L"ms-settings:sound"); } } })));
    return p;
}

// ================= Foco, Tempo de Uso
Page Foco() {
    Page p;
    p.groups.push_back(Plain(Hero(0xE708, C_INDIGO, L"Foco", L"O Foco silencia notificações enquanto você trabalha. No Windows ele se chama Não Incomodar e Sessões de Foco.")));
    p.groups.push_back(G({ Uri(L"Não Incomodar", L"ms-settings:notifications", 0xE708, C_INDIGO), Uri(L"Sessões de Foco", L"ms-clock:", 0xE916, C_INDIGO) }));
    return p;
}
Page TempoUso() {
    Page p;
    p.groups.push_back(Plain(Hero(0xE916, C_INDIGO, L"Tempo de Uso", L"O tempo de uso e os limites ficam no Microsoft Family Safety.")));
    p.groups.push_back(G({ Uri(L"Família e Limites", L"ms-settings:family-group", 0xE716, C_INDIGO), Uri(L"Histórico de Atividades", L"ms-settings:privacy-activityhistory", 0xE81C, C_GRAY) }));
    return p;
}

// ================= Tela Bloqueada
// ---- Fundo da tela de bloqueio: imagem ou vídeo escolhido aqui.
// Guardado em %APPDATA%\Shell\bloqueio\fundo.jpg (+ fundo.mp4 no vídeo; o .jpg é o 1º quadro, usado no vidro do
// relógio e enquanto o vídeo carrega). Ajustes bloqueio_fundo / bloqueio_video. Quando a tela de logon do Windows
// é a do projeto (AuthUX, pasta C:\Windows\Web\Screen\macOS), os dois vão também para lá como
// personalizado.jpg / personalizado.mp4: a pasta é só de administradores, então o Windows pede a confirmação.
namespace lockbg {
const wchar_t* AUTHUX = L"C:\\Windows\\Web\\Screen\\macOS";
std::wstring Dir() {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell\\bloqueio"; CoTaskMemFree(p); }
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    return d;
}
bool Exists(const std::wstring& f) { return !f.empty() && GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

// grava um HBITMAP ou um arquivo de imagem como JPEG (WIC)
bool SaveJpeg(IWICImagingFactory* wic, IWICBitmapSource* src, const std::wstring& out) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* bag = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &bag))) {
        PROPBAG2 opt{}; wchar_t nome[] = L"ImageQuality"; opt.pstrName = nome;
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.92f;
        bag->Write(1, &opt, &v);
        if (SUCCEEDED(fr->Initialize(bag)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(src, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            UINT w = 0, h = 0; cv->GetSize(&w, &h);
            WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
            ok = SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&pf)) && SUCCEEDED(fr->WriteSource(cv, nullptr)) &&
                 SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        }
    }
    for (IUnknown* u : { (IUnknown*)cv, (IUnknown*)bag, (IUnknown*)fr, (IUnknown*)enc, (IUnknown*)st }) if (u) u->Release();
    if (!ok) DeleteFileW(out.c_str());
    return ok;
}
bool ImageToJpeg(const std::wstring& in, const std::wstring& out) {
    IWICImagingFactory* wic = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromFilename(in.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SaveJpeg(wic, fr, out);
    for (IUnknown* u : { (IUnknown*)fr, (IUnknown*)dec, (IUnknown*)wic }) if (u) u->Release();
    return ok;
}
// Um quadro do vídeo (a 1 s do começo), decodificado do MESMO jeito que a tela de logon toca o vídeo
// (Media Foundation, SourceReader convertendo para RGB32). Se aqui não decodifica (HEVC sem a extensão, AV1, arquivo
// quebrado...), lá a tela ficaria preta: o vídeo é recusado. A miniatura do Windows não serve para isso (ela existe
// mesmo quando o vídeo não toca, e o primeiro quadro costuma ser preto).
bool VideoFrameToJpeg(const std::wstring& in, const std::wstring& out) {
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    IMFAttributes* at = nullptr; IMFSourceReader* rd = nullptr; IMFMediaType* mt = nullptr; IMFMediaType* cur = nullptr;
    IMFSample* smp = nullptr; IMFMediaBuffer* buf = nullptr; IMF2DBuffer* b2 = nullptr;
    IWICImagingFactory* wic = nullptr; IWICBitmap* bmp = nullptr;
    bool ok = false;
    const DWORD V = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    do {
        if (FAILED(MFCreateAttributes(&at, 1)) || FAILED(at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))) break;
        if (FAILED(MFCreateSourceReaderFromURL(in.c_str(), at, &rd))) break;
        if (FAILED(MFCreateMediaType(&mt)) || FAILED(mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) || FAILED(mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32))) break;
        if (FAILED(rd->SetCurrentMediaType(V, nullptr, mt))) break;  // sem decodificador para este vídeo
        UINT32 w = 0, h = 0;
        if (FAILED(rd->GetCurrentMediaType(V, &cur)) || FAILED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) break;
        PROPVARIANT pos; PropVariantInit(&pos); pos.vt = VT_I8; pos.hVal.QuadPart = 10000000;  // 1 s
        rd->SetCurrentPosition(GUID_NULL, pos);
        for (int n = 0; n < 60 && !smp; n++) {
            DWORD flags = 0; LONGLONG ts = 0;
            if (FAILED(rd->ReadSample(V, 0, nullptr, &flags, &ts, &smp))) break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // vídeo com menos de 1 s: do começo
                if (smp) break;
                pos.hVal.QuadPart = 0; rd->SetCurrentPosition(GUID_NULL, pos);
            }
        }
        if (!smp || FAILED(smp->ConvertToContiguousBuffer(&buf))) break;
        std::vector<BYTE> px((size_t)w * h * 4);
        BYTE* scan = nullptr; LONG pitch = 0;
        if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan, &pitch))) {
            for (UINT32 y = 0; y < h; y++) memcpy(&px[(size_t)y * w * 4], scan + (LONG_PTR)y * pitch, (size_t)w * 4);
            b2->Unlock2D();
        } else {
            BYTE* d = nullptr; DWORD len = 0;
            if (FAILED(buf->Lock(&d, nullptr, &len)) || len < px.size()) break;
            memcpy(px.data(), d, px.size());
            buf->Unlock();
        }
        bool algo = false;  // quadro todo preto = não decodificou de verdade
        for (size_t k = 0; k < px.size() && !algo; k += 4 * 97) algo = px[k] > 12 || px[k + 1] > 12 || px[k + 2] > 12;
        if (!algo) break;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) break;
        if (FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, (UINT)px.size(), px.data(), &bmp))) break;
        ok = SaveJpeg(wic, bmp, out);
    } while (false);
    for (IUnknown* u : { (IUnknown*)bmp, (IUnknown*)wic, (IUnknown*)b2, (IUnknown*)buf, (IUnknown*)smp, (IUnknown*)cur, (IUnknown*)mt, (IUnknown*)rd, (IUnknown*)at }) if (u) u->Release();
    MFShutdown();
    return ok;
}
// leva (ou tira) o fundo personalizado da pasta da tela de logon do projeto; pede administrador (UAC)
void Publish(const std::wstring& jpg, const std::wstring& mp4) {
    if (!Exists(AUTHUX)) return;
    std::wstring d = AUTHUX, cmd = L"/c ";
    if (jpg.empty()) cmd += L"del /q \"" + d + L"\\personalizado.jpg\" \"" + d + L"\\personalizado.mp4\"";
    else {
        cmd += L"copy /y \"" + jpg + L"\" \"" + d + L"\\personalizado.jpg\"";
        if (mp4.empty()) cmd += L" & del /q \"" + d + L"\\personalizado.mp4\"";
        else cmd += L" & copy /y \"" + mp4 + L"\" \"" + d + L"\\personalizado.mp4\"";
    }
    ShellExecuteW(Window(), L"runas", L"cmd.exe", cmd.c_str(), nullptr, SW_HIDE);
}
void Default() {
    SetSetting(L"bloqueio_fundo", L"");
    SetSetting(L"bloqueio_video", L"");
    Publish(L"", L"");
    Rebuild();
}
void Pick(bool video) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC img[] = { { L"Imagens", L"*.jpg;*.jpeg;*.png;*.bmp;*.heic;*.webp" } };
    COMDLG_FILTERSPEC vid[] = { { L"Vídeos", L"*.mp4;*.mov;*.m4v" } };
    d->SetFileTypes(1, video ? vid : img);
    std::wstring in;
    if (SUCCEEDED(d->Show(Window()))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) { in = path; CoTaskMemFree(path); }
            it->Release();
        }
    }
    d->Release();
    if (in.empty()) return;
    // nome novo a cada escolha (a miniatura da página é guardada pelo caminho); as antigas saem
    std::wstring dir = Dir(), jpg = dir + L"\\fundo-" + std::to_wstring(GetTickCount64()) + L".jpg", mp4 = dir + L"\\fundo.mp4";

    bool ok;
    // o vídeo só é aceito se decodifica aqui (senão a tela de bloqueio ficaria preta); nada é trocado em caso de erro
    if (video) ok = VideoFrameToJpeg(in, jpg) && CopyFileW(in.c_str(), mp4.c_str(), FALSE);
    else { ok = ImageToJpeg(in, jpg); if (ok) DeleteFileW(mp4.c_str()); }
    if (!ok) {
        DeleteFileW(jpg.c_str());
        MessageBoxW(Window(), video ? Tr(L"Este vídeo não pode ser usado: o Windows não consegue reproduzir o formato dele. Use um vídeo MP4 em H.264.") : Tr(L"Não foi possível usar esta imagem."), Tr(L"Tela Bloqueada"), MB_ICONWARNING);
        return;
    }
    {   // as imagens antigas saem (menos a nova)
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\fundo-*.jpg").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { std::wstring f = dir + L"\\" + fd.cFileName; if (_wcsicmp(f.c_str(), jpg.c_str())) DeleteFileW(f.c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }
    }
    SetSetting(L"bloqueio_fundo", jpg);
    SetSetting(L"bloqueio_video", video ? mp4 : L"");
    SetSetting(L"bloqueio_fundo_nome", PathFindFileNameW(in.c_str()));
    Publish(jpg, video ? mp4 : L"");
    Rebuild();
}
}  // namespace lockbg

Page TelaBloqueada() {
    Page p;
    {   // a imagem atual à esquerda; à direita o nome e o tipo; embaixo os botões
        std::wstring jpg = Setting(L"bloqueio_fundo", L""), mp4 = Setting(L"bloqueio_video", L"");
        bool custom = lockbg::Exists(jpg);
        bool video = custom && lockbg::Exists(mp4);
        std::wstring shown = custom ? jpg : sys::Wallpaper();
        std::wstring name = custom ? Setting(L"bloqueio_fundo_nome", L"") : Tr(L"Padrão (a imagem de fundo da mesa)");
        std::wstring kind = !custom ? L"" : video ? Tr(L"Vídeo") : Tr(L"Imagem");
        Row head; head.kind = Kind::Custom; head.height = 124;
        head.draw = [shown, name, kind](Ctx& c, D2D1_RECT_F rr) {
            D2D1_RECT_F t = D2D1::RectF(rr.left, rr.top + 6, rr.left + 190, rr.top + 6 + 112);
            c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0, 0, 0, 0.06f));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 8, 8), c.br);
            if (ID2D1Bitmap* b = BitmapFromFile(shown, 400)) {
                auto s2 = b->GetSize();
                float k = std::max((t.right - t.left) / s2.width, (t.bottom - t.top) / s2.height);
                float w = s2.width * k, h = s2.height * k, cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
                ID2D1Factory* f = nullptr; c.dc->GetFactory(&f);
                ID2D1RoundedRectangleGeometry* rg = nullptr; f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(t, 8, 8), &rg);
                if (rg) {
                    c.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), rg), nullptr);
                    c.dc->DrawBitmap(b, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    c.dc->PopLayer();
                    rg->Release();
                }
                if (f) f->Release();
            }
            c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.86f));
            c.dc->DrawTextW(name.c_str(), (UINT32)name.size(), c.body, D2D1::RectF(t.right + 16, rr.top + 40, rr.right, rr.top + 60), c.br);
            if (!kind.empty()) {
                c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.5f));
                c.dc->DrawTextW(kind.c_str(), (UINT32)kind.size(), c.note, D2D1::RectF(t.right + 16, rr.top + 62, rr.right, rr.top + 80), c.br);
            }
        };
        p.groups.push_back(G({ head }, L"Imagem de Fundo da Tela Bloqueada"));
        std::vector<std::pair<std::wstring, std::function<void()>>> bs = {
            { L"Escolher Imagem…", [] { lockbg::Pick(false); } }, { L"Escolher Vídeo…", [] { lockbg::Pick(true); } } };
        if (custom) bs.insert(bs.begin(), { L"Usar o Padrão", [] { lockbg::Default(); } });
        p.groups.push_back(Plain(Buttons(bs)));
    }
    p.groups.push_back(G({
        Popup(L"Desligar a tela quando inativo", MinuteLabels(L"Após "), [] { return MinuteIndex(sys::DisplayOffMinutes()); }, [](int i) { sys::SetDisplayOffMinutes(MINUTES[i]); }),
        Toggle(L"Exigir senha ao sair do protetor de tela",
               [] { wchar_t v[8] = L"0"; DWORD cb = sizeof(v); RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaverIsSecure", RRF_RT_REG_SZ, nullptr, v, &cb); return v[0] == L'1'; },
               [](bool on) { const wchar_t* v = on ? L"1" : L"0"; RegSetKeyValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaverIsSecure", REG_SZ, v, 4); }),
    }));
    return p;
}

// ================= Privacidade e Segurança
Page Privacidade() {
    Page p;
    p.groups.push_back(G({ Uri(L"Serviços de Localização", L"ms-settings:privacy-location", 0xE707, C_BLUE),
                           Uri(L"Câmera", L"ms-settings:privacy-webcam", 0xE722, C_GRAY),
                           Uri(L"Microfone", L"ms-settings:privacy-microphone", 0xE720, C_ORANGE),
                           Uri(L"Contatos", L"ms-settings:privacy-contacts", 0xE77B, C_GRAY),
                           Uri(L"Calendários", L"ms-settings:privacy-calendar", 0xE787, C_RED),
                           Uri(L"Fotos", L"ms-settings:privacy-pictures", 0xE8B9, C_PINK),
                           Uri(L"Arquivos e Pastas", L"ms-settings:privacy-documents", 0xE8B7, C_BLUE),
                           Uri(L"Acesso Total ao Disco", L"ms-settings:privacy-broadfilesystemaccess", 0xEDA2, C_GRAY) }, L"Privacidade"));
    p.groups.push_back(G({ Uri(L"Análise e Melhorias", L"ms-settings:privacy-feedback", 0xE9D9, C_BLUE),
                           Uri(L"Publicidade", L"ms-settings:privacy-general", 0xE789, C_BLUE) }));
    p.groups.push_back(G({ Uri(L"Segurança do Windows", L"windowsdefender:", 0xE83D, C_BLUE),
                           Uri(L"Encontrar Meu Dispositivo", L"ms-settings:findmydevice", 0xE707, C_GREEN) }, L"Segurança"));
    return p;
}

// ================= Touch ID e Senha → Windows Hello
Page Senha() {
    Page p;
    p.groups.push_back(Plain(Hero(0xE928, C_PINK, L"Windows Hello e Senha", L"Digital, rosto, PIN e a senha de início de sessão.")));
    p.groups.push_back(G({ Uri(L"Opções de Início de Sessão", L"ms-settings:signinoptions", 0xE928, C_PINK),
                           Uri(L"Configurar Digital", L"ms-settings:signinoptions-launchfingerprintenrollment", 0xE928, C_PINK),
                           Uri(L"Configurar Rosto", L"ms-settings:signinoptions-launchfaceenrollment", 0xE77B, C_GRAY) }));
    return p;
}

// ================= Usuários e Grupos
Page Usuarios() {
    Page p;
    const sys::Info& inf = sys::About();
    Group g = G({});
    for (auto& u : sys::LocalUsers()) {
        Row r = Value(u, _wcsicmp(u.c_str(), inf.user.c_str()) == 0 ? L"Você" : L"");
        r.icon = 0xE77B; r.iconColor = C_GRAY;
        g.rows.push_back(r);
    }
    if (g.rows.empty()) g.rows.push_back(Value(inf.fullName, L"Você"));
    p.groups.push_back(g);
    p.groups.push_back(Plain(Buttons({ { L"Adicionar Usuário…", [] { OpenUri(L"ms-settings:otherusers"); } } })));
    return p;
}

Page ContasInternet() {
    Page p;
    p.groups.push_back(G({ Uri(L"E-mail e Contas", L"ms-settings:emailandaccounts", L'@', C_BLUE), Uri(L"Contas de Trabalho ou Escola", L"ms-settings:workplace", 0xE821, C_GRAY) }));
    return p;
}
Page Jogos() {
    Page p;
    p.groups.push_back(G({ Uri(L"Barra de Jogo", L"ms-settings:gaming-gamebar", 0xE7FC, C_GREEN), Uri(L"Capturas", L"ms-settings:gaming-gamedvr", 0xE714, C_GRAY),
                           Uri(L"Modo de Jogo", L"ms-settings:gaming-gamemode", 0xE945, C_GREEN) }));
    return p;
}
Page Nuvem() {
    Page p;
    p.groups.push_back(Plain(Hero(0xE753, C_CYAN, L"OneDrive", L"No Windows, a nuvem dos seus arquivos e fotos é o OneDrive.")));
    p.groups.push_back(G({ Uri(L"Backup do Windows", L"ms-settings:backup", 0xE753, C_CYAN) }));
    return p;
}

// ================= Teclado, Mouse, Trackpad, Impressoras
Page Teclado() {
    Page p;
    p.groups.push_back(G({
        Slider(L"Repetição de tecla", 0, 31, [] { return (float)sys::KeyRepeat(); }, [](float v) { sys::SetKeyRepeat((int)std::lround(v)); }, L"Lenta", L"Rápida"),
        Slider(L"Atraso até repetir", 0, 3, [] { return (float)(3 - sys::KeyDelay()); }, [](float v) { sys::SetKeyDelay(3 - (int)std::lround(v)); }, L"Longo", L"Curto"),
    }));
    p.groups.push_back(G({ Uri(L"Fontes de Entrada", L"ms-settings:keyboard", 0xE765, C_GRAY), Uri(L"Ditado", L"ms-settings:speech", 0xE720, C_GRAY),
                           Uri(L"Digitação", L"ms-settings:typing", 0xE765, C_GRAY) }, L"Entrada de Texto"));
    p.groups.push_back(G({ Toggle(L"Atalhos do Mac para apps", [] { return Setting(L"atalhos_mac", L"1") == L"1"; },
                                  [](bool on) { SetSetting(L"atalhos_mac", on ? L"1" : L"0"); }),
                           Note(L"Win+Q encerra o app, Win+H oculta o app e Win+W fecha a janela, como Command no Mac. Desligado, esses atalhos voltam ao Windows.") },
                         L"Atalhos"));
    return p;
}
Page Mouse() {
    Page p;
    p.groups.push_back(G({
        Slider(L"Velocidade de rastreamento", 1, 20, [] { return (float)sys::MouseSpeed(); }, [](float v) { sys::SetMouseSpeed((int)std::lround(v)); }, L"Lenta", L"Rápida"),
        Slider(L"Velocidade do clique duplo", 200, 900, [] { return (float)(1100 - sys::DoubleClickMs()); }, [](float v) { sys::SetDoubleClickMs(1100 - (int)std::lround(v)); }, L"Lenta", L"Rápida"),
        Slider(L"Velocidade de rolagem", 1, 20, [] { return (float)sys::WheelLines(); }, [](float v) { sys::SetWheelLines((int)std::lround(v)); }, L"Lenta", L"Rápida"),
        Popup(L"Clique secundário", { L"Clicar no Lado Direito", L"Clicar no Lado Esquerdo" }, [] { return sys::ButtonsSwapped() ? 1 : 0; }, [](int i) { sys::SetButtonsSwapped(i == 1); }),
    }));
    p.groups.push_back(Plain(Buttons({ { L"Avançado…", [] { OpenUri(L"ms-settings:mousetouchpad"); } } })));
    return p;
}
Page Trackpad() {
    Page p;
    p.groups.push_back(G({ Uri(L"Toque e Gestos", L"ms-settings:devices-touchpad", 0xEFA5, C_GRAY) }));
    return p;
}
Page Impressoras() {
    Page p;
    auto list = sys::Printers();
    if (!list.empty()) {
        std::vector<std::wstring> opts = list;
        p.groups.push_back(G({ Popup(L"Impressora padrão", opts, [list] { auto d = sys::DefaultPrinter(); for (int i = 0; i < (int)list.size(); i++) if (list[i] == d) return i; return -1; },
                                     [list](int i) { sys::SetPrinterDefault(list[i]); }) }));
        Group g = G({}, L"Impressoras");
        for (auto& n : list) { Row r = Value(n, L""); r.icon = 0xE749; r.iconColor = C_GRAY; g.rows.push_back(r); }
        p.groups.push_back(g);
    } else p.groups.push_back(G({ Note(L"Nenhuma impressora.") }));
    p.groups.push_back(Plain(Buttons({ { L"Adicionar Impressora, Scanner ou Fax…", [] { OpenUri(L"ms-settings:printers"); } } })));
    return p;
}

}  // namespace

std::vector<SideItem>& SidebarItems() {
    static std::vector<SideItem> items;
    if (!items.empty()) return items;
    bool battery = sys::HasBattery(nullptr, nullptr);
    items = {
        { L"Wi‑Fi", 0xE701, C_BLUE, WiFi, 0, L"wifi rede sem fio internet", L"wifi" },
        { L"Bluetooth", 0xE702, C_BLUE, Bluetooth, 0, L"fone mouse teclado dispositivos", L"bluetooth" },
        { L"Rede", 0xE774, C_BLUE, Rede, 0, L"ethernet internet ip firewall vpn proxy", L"rede" },
        { battery ? L"Bateria" : L"Energia", (wchar_t)(battery ? 0xE83F : 0xE945), C_GREEN, Energia, 0, L"energia bateria repouso suspender modo desempenho", L"energia" },
        { L"Geral", 0xE713, C_GRAY, Geral, 1, L"sobre atualização armazenamento data hora idioma início", L"geral" },
        { L"Acessibilidade", 0xE776, C_BLUE, Acessibilidade, 1, L"narrador lupa legendas", L"acessibilidade" },
        { L"Aparência", 1, 0x1C1C1E, Aparencia, 1, L"tema escuro claro destaque cor", L"aparencia" },
        { L"Apple Intelligence e Siri", 0xE720, C_PURPLE, Inteligencia, 1, L"copilot fala voz assistente", L"inteligencia" },
        { L"Central de Controle", 0xE9E9, C_GRAY, CentralControle, 1, L"barra de menus relógio ícones", L"central" },
        { L"Mesa e Dock", 0xE7F4, 0x1C1C1E, MesaDock, 1, L"dock tamanho ampliação ocultar janelas", L"dock" },
        { L"Telas", 0xE706, C_BLUE, Telas, 1, L"monitor brilho resolução escala night shift", L"telas" },
        { L"Protetor de Tela", 0xE8B9, C_CYAN, Protetor, 1, L"proteção de tela", L"protetor" },
        { L"Spotlight", 0xE721, C_GRAY, Spotlight, 1, L"pesquisa busca índice", L"spotlight" },
        { L"Imagem de Fundo", 0xE771, C_TEAL, Fundo, 1, L"papel de parede wallpaper", L"fundo" },
        { L"Notificações", 0xEA8F, C_RED, Notificacoes, 2, L"avisos alertas", L"notificacoes" },
        { L"Som", 0xE767, C_PINK, Som, 2, L"volume saída entrada alto-falante microfone", L"som" },
        { L"Foco", 0xE708, C_INDIGO, Foco, 2, L"não incomodar", L"foco" },
        { L"Tempo de Uso", 0xE916, C_INDIGO, TempoUso, 2, L"família limites", L"tempo" },
        { L"Tela Bloqueada", 0xE72E, 0x1C1C1E, TelaBloqueada, 3, L"bloqueio senha protetor", L"bloqueio" },
        { L"Privacidade e Segurança", 0xE8D7, C_BLUE, Privacidade, 3, L"localização câmera microfone permissões segurança", L"privacidade" },
        { L"Touch ID e Senha", 0xE928, C_PINK, Senha, 3, L"windows hello digital pin senha", L"senha" },
        { L"Usuários e Grupos", 0xE716, C_BLUE, Usuarios, 3, L"contas usuários", L"usuarios" },
        { L"Contas de Internet", L'@', C_BLUE, ContasInternet, 4, L"email e-mail contas", L"contas" },
        { L"Game Center", 0xE7FC, C_GREEN, Jogos, 4, L"jogos xbox", L"jogos" },
        { L"iCloud", 0xE753, C_CYAN, Nuvem, 4, L"onedrive nuvem backup", L"nuvem" },
        { L"Teclado", 0xE765, C_GRAY, Teclado, 5, L"repetição teclas digitação", L"teclado" },
        { L"Mouse", 0xE962, C_GRAY, Mouse, 5, L"rastreamento clique rolagem", L"mouse" },
        { L"Trackpad", 0xEFA5, C_GRAY, Trackpad, 5, L"touchpad gestos", L"trackpad" },
        { L"Impressoras e Scanners", 0xE749, C_GRAY, Impressoras, 5, L"imprimir scanner", L"impressoras" },
        { L"Conta", 0xE77B, C_GRAY, Conta, 9, L"conta usuário perfil", L"conta" },
    };
    return items;
}

// SF Symbol usado no Ajustes do Sequoia para cada item (nome do .svg em icones\simbolos)
std::wstring SymbolFor(const std::wstring& label) {
    static const std::map<std::wstring, std::wstring> m = {
        { L"Wi‑Fi", L"wifi" }, { L"Rede", L"globe" }, { L"Energia", L"bolt.fill" }, { L"Bateria", L"battery.100percent" },
        { L"Geral", L"gearshape.fill" }, { L"Acessibilidade", L"accessibility" }, { L"Aparência", L"circle.lefthalf.filled" },
        { L"Apple Intelligence e Siri", L"apple.intelligence" }, { L"Assistente e Fala", L"apple.intelligence" }, { L"Central de Controle", L"switch.2" },
        { L"Mesa e Dock", L"dock.rectangle" }, { L"Telas", L"sun.max.fill" }, { L"Protetor de Tela", L"photo.on.rectangle.angled" },
        { L"Spotlight", L"magnifyingglass" }, { L"Imagem de Fundo", L"camera.macro" }, { L"Notificações", L"bell.badge.fill" },
        { L"Som", L"speaker.wave.3.fill" }, { L"Foco", L"moon.fill" }, { L"Tempo de Uso", L"hourglass" }, { L"Tela Bloqueada", L"lock.fill" },
        { L"Privacidade e Segurança", L"hand.raised.fill" }, { L"Touch ID e Senha", L"touchid" }, { L"Windows Hello e Senha", L"touchid" },
        { L"Usuários e Grupos", L"person.2.fill" }, { L"Contas de Internet", L"at" }, { L"Game Center", L"gamecontroller.fill" },
        { L"iCloud", L"icloud.fill" }, { L"OneDrive", L"icloud.fill" }, { L"Teclado", L"keyboard.fill" }, { L"Mouse", L"computermouse.fill" },
        { L"Trackpad", L"rectangle.and.hand.point.up.left.fill" }, { L"Impressoras e Scanners", L"printer.fill" },
        // linhas das páginas
        { L"Sobre", L"desktopcomputer" }, { L"Atualização de Software", L"gear.badge" }, { L"Armazenamento", L"internaldrive.fill" },
        { L"Compartilhamento por Proximidade", L"airplay.audio" }, { L"Itens de Início", L"list.bullet" }, { L"Idioma e Região", L"globe" },
        { L"Data e Hora", L"calendar.badge.clock" }, { L"Compartilhamento", L"square.and.arrow.up.fill" },
        { L"Histórico de Arquivos", L"clock.arrow.trianglehead.counterclockwise.rotate.90" }, { L"Transferir ou Redefinir", L"arrow.counterclockwise" },
        { L"Narrador", L"person.wave.2.fill" }, { L"Lupa", L"plus.magnifyingglass" }, { L"Tela", L"display" }, { L"Tamanho do Texto", L"textformat.size" },
        { L"Filtros de Cor", L"camera.filters" }, { L"Áudio", L"speaker.wave.2.fill" }, { L"Legendas", L"captions.bubble.fill" },
        { L"Controle por Voz", L"microphone.fill" }, { L"Ponteiro do Mouse", L"cursorarrow.rays" },
        { L"Transparência e Efeitos", L"sparkles" }, { L"Copilot", L"apple.intelligence" }, { L"Fala", L"microphone.fill" },
        { L"Spotlight ", L"magnifyingglass" }, { L"Ícones dos aplicativos", L"square.grid.2x2.fill" }, { L"Relógio", L"clock.arrow.trianglehead.counterclockwise.rotate.90" },
        { L"Firewall", L"shield.lefthalf.filled" }, { L"VPN", L"network" }, { L"Proxies", L"network" },
        { L"Informações Pessoais", L"person.fill" }, { L"Início de Sessão e Segurança", L"lock.shield.fill" }, { L"E-mail e Contas", L"at" },
        { L"Família", L"person.2.fill" }, { L"Backup do Windows (OneDrive)", L"icloud.fill" }, { L"Backup do Windows", L"icloud.fill" },
        { L"Serviços de Localização", L"location.fill" }, { L"Câmera", L"camera.fill" }, { L"Microfone", L"microphone.fill" },
        { L"Contatos", L"person.crop.circle" }, { L"Calendários", L"calendar" }, { L"Fotos", L"photo.fill" }, { L"Arquivos e Pastas", L"folder.fill" },
        { L"Acesso Total ao Disco", L"externaldrive.fill" }, { L"Análise e Melhorias", L"chart.bar.fill" }, { L"Publicidade", L"megaphone.fill" },
        { L"Segurança do Windows", L"lock.shield.fill" }, { L"Encontrar Meu Dispositivo", L"location.circle.fill" },
        { L"Opções de Início de Sessão", L"touchid" }, { L"Configurar Digital", L"touchid" }, { L"Configurar Rosto", L"faceid" },
        { L"Imagem e Mensagem da Tela de Bloqueio", L"lock.rectangle.fill" }, { L"Notificações por Aplicativo", L"bell.badge.fill" },
        { L"Não Incomodar", L"moon.fill" }, { L"Sessões de Foco", L"timer" }, { L"Família e Limites", L"hourglass" },
        { L"Histórico de Atividades", L"clock.arrow.trianglehead.counterclockwise.rotate.90" },
        { L"Contas de Trabalho ou Escola", L"briefcase.fill" }, { L"Barra de Jogo", L"gamecontroller.fill" }, { L"Capturas", L"record.circle.fill" },
        { L"Modo de Jogo", L"bolt.fill" }, { L"Fontes de Entrada", L"keyboard.fill" }, { L"Ditado", L"microphone.fill" }, { L"Digitação", L"keyboard" },
        { L"Toque e Gestos", L"rectangle.and.hand.point.up.left.fill" },
    };
    auto it = m.find(label);
    if (it != m.end()) return it->second;
    return L"";
}

// --pagina=atualizacao (o aviso de versão nova do Shell): Geral > Atualização de Software
void OpenUpdatePage() { aj::Push(L"Atualização de Software", Atualizacao); }

int SidebarIndexFor(const std::wstring& idOrUri) {
    auto& items = SidebarItems();
    std::wstring id = idOrUri;
    // links ms-settings: → página equivalente
    static const std::pair<const wchar_t*, const wchar_t*> uris[] = {
        { L"ms-settings:network-wifi", L"wifi" }, { L"ms-settings:bluetooth", L"bluetooth" }, { L"ms-settings:network", L"rede" },
        { L"ms-settings:powersleep", L"energia" }, { L"ms-settings:batterysaver", L"energia" }, { L"ms-settings:about", L"geral" },
        { L"ms-settings:easeofaccess", L"acessibilidade" }, { L"ms-settings:colors", L"aparencia" }, { L"ms-settings:personalization-colors", L"aparencia" },
        { L"ms-settings:taskbar", L"dock" }, { L"ms-settings:display", L"telas" }, { L"ms-settings:lockscreen", L"bloqueio" },
        { L"ms-settings:personalization-background", L"fundo" }, { L"ms-settings:personalization", L"fundo" }, { L"ms-settings:notifications", L"notificacoes" },
        { L"ms-settings:sound", L"som" }, { L"ms-settings:quiethours", L"foco" }, { L"ms-settings:privacy", L"privacidade" },
        { L"ms-settings:signinoptions", L"senha" }, { L"ms-settings:otherusers", L"usuarios" }, { L"ms-settings:yourinfo", L"conta" },
        { L"ms-settings:emailandaccounts", L"contas" }, { L"ms-settings:gaming", L"jogos" }, { L"ms-settings:typing", L"teclado" },
        { L"ms-settings:keyboard", L"teclado" }, { L"ms-settings:mousetouchpad", L"mouse" }, { L"ms-settings:devices-touchpad", L"trackpad" },
        { L"ms-settings:printers", L"impressoras" }, { L"ms-settings:dateandtime", L"geral" }, { L"ms-settings:windowsupdate", L"geral" },
        { L"ms-settings:storagesense", L"geral" }, { L"ms-settings:search", L"spotlight" } };
    if (id.rfind(L"ms-settings:", 0) == 0) {
        std::wstring best;
        size_t len = 0;
        for (auto& [u, pid] : uris) { size_t n = wcslen(u); if (id.compare(0, n, u) == 0 && n > len) { len = n; best = pid; } }
        id = best.empty() ? L"geral" : best;
    }
    for (int i = 0; i < (int)items.size(); i++) if (items[i].id == id) return i;
    return -1;
}
