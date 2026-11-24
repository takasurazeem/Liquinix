// Ajustes do Sistema — o app de ajustes no visual do macOS Sequoia (shell próprio).
// Reaproveita do Finder a janela sem moldura, o desenho (gfx), a sombra e o campo de texto.
#pragma once
#include "../finder/finder.h"
#include <map>
#include <memory>

namespace aj {

// Uma linha dentro de um grupo (caixa arredondada), como nas listas do Ajustes do Mac.
enum class Kind { Value, Toggle, Popup, Slider, Link, Button, Custom };

struct Ctx;  // cores, fontes e pincel do quadro atual (main.cpp)

struct Row {
    Kind kind = Kind::Value;
    std::wstring label, detail;          // detail: texto cinza, menor, embaixo do rótulo
    wchar_t icon = 0;                    // ícone em quadradinho colorido à esquerda (opcional)
    UINT32 iconColor = 0;                // 0xRRGGBB do quadradinho
    bool enabled = true;
    std::function<std::wstring()> value;                                   // Value/Popup/Link: texto à direita
    std::function<bool()> get; std::function<void(bool)> set;              // Toggle
    std::vector<std::wstring> options;                                     // Popup
    std::function<int()> getIdx; std::function<void(int)> setIdx;          // Popup
    float min = 0, max = 1;                                                // Slider
    std::function<float()> getF; std::function<void(float)> setF;          // Slider (setF ao soltar e ao arrastar)
    std::wstring minGlyph, maxGlyph;                                       // Slider: rótulos/ícones nas pontas
    bool hasDef = false; float def = 0;                                    // Slider: valor padrão (marca "Padrão" embaixo e ímã ao arrastar)
    std::function<void()> click;                                           // Link/Button
    std::wstring button;                                                   // Button: texto do botão à direita
    float height = 0;                                                      // Custom: altura (pt); nas outras, > 0 fixa a altura
    bool noLine = false;                                                   // sem a divisória em cima (continua a linha de cima)
    bool gg = false;                                                       // Slider/Popup no desenho do macOS 27 (Aparência)
    std::function<D2D1_COLOR_F(int)> dot;                                  // Popup do macOS 27: bolinha da opção (a < 0: degradê "automático")
    std::function<void(Ctx&, D2D1_RECT_F)> draw;                           // Custom
    std::function<void(D2D1_POINT_2F, D2D1_RECT_F)> clickAt;               // Custom: clique (ponto, área da linha)
};

struct Group {
    std::wstring title, subtitle;  // título em negrito acima da caixa (opcional)
    std::vector<Row> rows;
    bool box = true;               // false: sem caixa (ex.: a grade de imagens de fundo)
};

struct Page {
    std::wstring title;
    std::vector<Group> groups;
};

using Builder = std::function<Page()>;

struct Ctx {
    ID2D1DeviceContext* dc;
    ID2D1SolidColorBrush* br;
    bool dark;
    D2D1_COLOR_F text, text2, text3, accent, line, group, control;
    IDWriteTextFormat *body, *note, *bold, *title, *glyph, *glyphSmall, *center, *noteCenter;
    IDWriteTextFormat* noteBoldCenter = nullptr;  // 11 pt em negrito, centrado (rótulo escolhido das miniaturas do macOS 27)
};

void Push(const std::wstring& title, Builder b);  // abre uma subpágina (com Voltar)
void Rebuild();                                   // reconstrói a página atual (valores mudaram)
void Redraw();
HWND Window();
bool Dark();
D2D1_COLOR_F Accent();
void OpenUri(const std::wstring& uri);            // ms-settings:..., arquivos, URLs

// Desenho reaproveitado pelas páginas (main.cpp)
void Text(Ctx& c, const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* tf, D2D1_COLOR_F col);
void Tile(Ctx& c, D2D1_RECT_F r, wchar_t glyph, UINT32 color, const std::wstring& symbol = L"");  // quadradinho colorido com ícone branco (SF Symbol se houver)
ID2D1Bitmap* BitmapFromFile(const std::wstring& path, UINT maxPx);  // miniatura (cache); nullptr se ainda não carregou
int Menu(D2D1_RECT_F anchor, const std::vector<std::wstring>& opts, int cur);  // menu do Mac embaixo de anchor (DIP); índice ou -1
void PopupBadge(Ctx& c, float cx, float cy);  // ⌃⌄ (no Tahoe, num círculo cinza), como nos menus das linhas
void ButtonFace(Ctx& c, D2D1_RECT_F b, const std::wstring& label);  // botão comum (no Tahoe, cápsula cinza)

// Ajustes do Shell (Dock, barra de menus...): %APPDATA%\Shell\ajustes.txt
std::wstring Setting(const wchar_t* key, const wchar_t* def);
void SetSetting(const wchar_t* key, const std::wstring& v);  // grava e avisa o Shell para recarregar

}  // namespace aj

// ---- paginas.cpp: a barra lateral e o conteúdo de cada página
struct SideItem {
    std::wstring name;
    wchar_t glyph;
    UINT32 color;
    aj::Builder build;
    int group;              // itens com o mesmo número ficam juntos (espaço entre grupos)
    std::wstring keywords;  // busca
    std::wstring id;        // --pagina=<id> e links ms-settings
};
std::vector<SideItem>& SidebarItems();
int SidebarIndexFor(const std::wstring& idOrUri);  // -1 se não houver
void OpenUpdatePage();  // Geral > Atualização de Software (--pagina=atualizacao)
std::wstring SymbolFor(const std::wstring& label);  // SF Symbol (icones\simbolos) do item, "" se não houver

// ---- sistema.cpp: o que cada página lê e muda no Windows
namespace sys {
// Som
float Volume();               // 0..1 (saída padrão)
void SetVolume(float v);
bool Muted();
void SetMuted(bool m);
float InputVolume();
void SetInputVolume(float v);
struct AudioDev { std::wstring id, name; bool isDefault; };
std::vector<AudioDev> AudioDevices(bool output);
bool SetDefaultAudio(const std::wstring& id);

// Wi-Fi (wlanapi)
struct WifiNet { std::wstring ssid; int signal; bool secure, known, connected; };
bool WifiPresent();
std::wstring WifiCurrent();   // "" se desconectado
std::vector<WifiNet> WifiNetworks();  // da última varredura (pede outra em segundo plano)
bool WifiConnect(const std::wstring& ssid, const std::wstring& password, bool secure);
void WifiDisconnect();

// Rádios (Windows.Devices.Radios): Wi-Fi e Bluetooth ligados/desligados
enum Radio { RADIO_WIFI, RADIO_BT };
int RadioState(Radio r);      // 1 ligado, 0 desligado, -1 não existe (ainda carregando: -2)
void SetRadio(Radio r, bool on);

// Bluetooth
struct BtDev { std::wstring name; bool connected; ULONG cls; };
std::vector<BtDev> BluetoothDevices();

// Rede
struct NetIf { std::wstring name, kind, ip; bool up; };
std::vector<NetIf> Interfaces();

// Energia
int PowerMode();              // 0 economia, 1 equilibrado, 2 desempenho
void SetPowerMode(int m);
int DisplayOffMinutes();      // 0 = nunca
void SetDisplayOffMinutes(int m);
int SleepMinutes();
void SetSleepMinutes(int m);
bool HasBattery(int* percent, bool* charging);

// Telas
float Brightness();           // -1 se o monitor não aceita (DDC/CI)
void SetBrightness(float v);
struct Mode { int w, h; };
std::vector<Mode> Resolutions();
Mode CurrentResolution();
bool SetResolution(Mode m);
int RefreshRate();
std::vector<int> RefreshRates();
bool SetRefreshRate(int hz);
float ScaleFactor();          // 1.0, 1.25...

// Imagem de fundo
std::wstring Wallpaper();
bool SetWallpaper(const std::wstring& path);
int WallpaperFit();               // DESKTOP_WALLPAPER_POSITION (sem o Explorador: lido do registro)
void SetWallpaperFit(int pos);
std::vector<std::wstring> WallpaperChoices();

// Sobre
struct Info { std::wstring computer, cpu, gpu, windows, build, model, ram, user, fullName; };
const Info& About();
struct Disk { std::wstring root, label; ULONGLONG total, free; };
std::vector<Disk> Disks();

// Itens de início (HKCU Run + StartupApproved, como no Gerenciador de Tarefas)
struct Startup { std::wstring name, command; bool enabled; };
std::vector<Startup> StartupItems();
void SetStartupEnabled(const std::wstring& name, bool on);

// Aparência
int Theme();                  // 0 claro, 1 escuro
void SetTheme(int t);

// Teclado e mouse
int KeyRepeat();              // 0..31
void SetKeyRepeat(int v);
int KeyDelay();               // 0..3
void SetKeyDelay(int v);
int MouseSpeed();             // 1..20
void SetMouseSpeed(int v);
int DoubleClickMs();
void SetDoubleClickMs(int ms);
bool ButtonsSwapped();
void SetButtonsSwapped(bool s);
int WheelLines();
void SetWheelLines(int n);
bool SnapEnabled();
void SetSnapEnabled(bool on);

// Usuários e impressoras
std::vector<std::wstring> LocalUsers();
std::vector<std::wstring> Printers();
std::wstring DefaultPrinter();
void SetPrinterDefault(const std::wstring& name);

ID2D1Bitmap* AccountPicture();  // foto da conta (nullptr se não houver)
}
