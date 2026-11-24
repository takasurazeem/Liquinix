// Arquivos — gerenciador de arquivos no estilo do Finder (shell próprio, fase 4).
// Declarações compartilhadas entre os arquivos de src/finder.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "../comum/idioma.h"

void Log(const wchar_t* fmt, ...);
// Caminho do LiquinixFinder.exe (no modo nativo, dentro do explorer.exe, GetModuleFileName(nullptr) seria o explorer)
void ExePath(wchar_t* out, DWORD n);
extern bool g_finderNativo;
extern bool g_finderEmbutido;  // Abrir/Salvar dentro do programa que pediu (MacDialogo.dll): a janela só se esconde no fim  // Finder dentro do explorer.exe (MacFinder.dll): uma janela só, com abas

// ---- gfx.cpp: Direct2D sobre DirectComposition. A janela não tem bitmap de redirecionamento;
// o conteúdo tem alfa real e o acrílico do DWM aparece onde desenhamos transparente.
namespace gfx {
extern IDWriteFactory* dw;
bool Init(HWND hwnd);
void Resize(UINT w, UINT h);
ID2D1DeviceContext* Begin(float dpi);  // já com BeginDraw; desenhe em DIPs
void End();                            // EndDraw + Present
ID2D1Bitmap* CreateBitmapBGRA(UINT w, UINT h, const void* pixels, UINT stride);  // pré-multiplicado
IUnknown* Device();                                 // ID3D11Device
ID2D1Bitmap* BitmapFromTexture(IUnknown* texture);   // ID3D11Texture2D BGRA -> bitmap (sem cópia)
}

// ---- thumbs.cpp: miniaturas e ícones (IShellItemImageFactory) em threads de fundo.
namespace thumbs {
enum Kind { KIND_IMAGE, KIND_DOCUMENT, KIND_FOLDER };  // documento sem miniatura / atalho para pasta
struct Result {
    UINT gen;
    std::wstring key;
    UINT w, h;
    std::vector<BYTE> bgra;  // BGRA pré-multiplicado, de cima para baixo (só KIND_IMAGE)
    Kind kind;
};
void Start(HWND notify, UINT msg);  // resultados chegam como PostMessage(notify, msg, 0, Result*)
void SetGeneration(UINT gen);       // descarta pedidos de pastas anteriores
void Request(UINT gen, const std::wstring& path, const std::wstring& key, int px);
void Stop();
}

// ---- textfield.cpp: campo de texto desenhado com DirectWrite (busca e renomear).
struct TextField {
    std::wstring text;
    int caret = 0, anchor = 0;  // seleção = [min(caret, anchor), max(...))
    float scrollX = 0;
    D2D1_RECT_F lastRect{};     // onde foi desenhado (para cliques)
    float lastOffset = 0;

    void SetText(const std::wstring& t, bool selectAll);
    void Select(int from, int to) { anchor = from; caret = to; }
    bool OnKey(UINT vk, bool shift, bool ctrl);  // true quando a tecla foi tratada
    void OnChar(wchar_t c);
    // center: centraliza quando o texto cabe (renomear); senão alinha à esquerda (busca)
    void Draw(ID2D1DeviceContext* dc, IDWriteTextFormat* tf, D2D1_RECT_F rc, ID2D1Brush* textBrush,
              ID2D1Brush* selBrush, ID2D1Brush* caretBrush, bool showCaret, bool center);
    void Click(IDWriteTextFormat* tf, D2D1_POINT_2F pt, bool shift);

private:
    int SelMin() const { return caret < anchor ? caret : anchor; }
    int SelMax() const { return caret > anchor ? caret : anchor; }
    void EraseSelection();
    void Insert(const std::wstring& s);
};

// ---- tags.cpp: etiquetas coloridas gravadas no próprio arquivo (fluxo alternativo do NTFS).
namespace tags {
const int COUNT = 7;
extern const wchar_t* names[COUNT];
extern UINT32 colors[COUNT];  // 0xRRGGBB (Tema troca para as cores do macOS 26)
void Tema(bool tahoe, bool dark);
unsigned Read(const std::wstring& path);              // máscara de bits (bit n = etiqueta n)
bool Write(const std::wstring& path, unsigned mask);  // preserva as datas do arquivo
std::vector<std::wstring> Tagged(int tag);            // todos os itens com a etiqueta (pelo índice)
}

// ---- shadow.cpp: sombra grande e difusa (estilo Mac) atrás da janela principal.
namespace shadow {
void Create(HWND main);
void SetActive(bool active);
void Update();  // chamar quando a janela mudar de posição, tamanho, ordem ou estado
void Destroy();
void SetRadius(float dip);  // cantos da janela (0 = retângulo, como no Sequoia; 26 no Tahoe)
}

// ---- tahoe.cpp: tema macOS Tahoe (Liquid Glass) quando liquid_glass=1 no ajustes.txt do Shell.
namespace tahoe {
float WindowR();               // cantos das janelas: 26 pt no Tahoe, 16 pt no Golden Gate (um raio só para todas)
int FolderColor();             // Aparência > Cor das pastas (índice de pastacor::colors; -1 = automática)
bool Golden();                 // tema macOS Golden Gate (liquid_glass=1 e golden_gate=1): o Tahoe com os ajustes do macOS 27
bool Leopard();                // tema Mac OS X Leopard (leopard=1 sem liquid_glass): o Sequoia com o metal e o Aqua de 2007
bool On();                     // tema Tahoe (liquid_glass=1)
bool Reduced();                // reduzir_transparencia=1: o vidro fica opaco
D2D1_COLOR_F Tint(D2D1_COLOR_F base);  // 10 % da cor do papel de parede (tingir_janelas), só no Tahoe
D2D1_COLOR_F Accent(bool dark);        // a cor de destaque escolhida no Ajustes (destaque=)
bool Reload();                 // relê o ajustes.txt se mudou; true quando On() mudou
void Frame(HWND h, bool on);   // janela de cantos grandes com o vidro da Composição (ou de volta ao DWM)
void Size(HWND h, float wpx, float hpx, bool dark);  // acompanha o tamanho (WM_SIZE) e o tema
bool IsDarkTheme();
void SetPanel(HWND h, D2D1_RECT_F dip, float radiusDip);  // onde o vidro aparece (a barra lateral flutuante)
float Radius(HWND h);          // raio atual da janela em DIPs (0 maximizada)
bool PushWindowClip(ID2D1DeviceContext* dc, D2D1_SIZE_F s, float radius);  // PopLayer depois, se true
ID2D1PathGeometry* Squircle(ID2D1Factory* f, D2D1_RECT_F r, float radius);  // canto contínuo da Apple (superelipse n = 5)
void WindowRim(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_SIZE_F s, float radius, bool dark);
// cápsula de vidro (radius < 0: meia altura); lift = força da sombra
void Capsule(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float radius, bool dark, float lift = 1);
void Panel(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float radius, bool dark);  // painel flutuante
// Borda de rolagem: desenhe a parte que rola num contexto fora da tela (SceneBegin..SceneEnd, mesmo DIP do
// principal) e componha com SceneDraw: nítida em `area` e, na faixa de cima (`band` DIPs), desfocada e sumindo.
ID2D1DeviceContext* SceneBegin(ID2D1DeviceContext* main, int slot, D2D1_SIZE_F dip, float dpi);  // nullptr: desenhe direto
void SceneEnd(int slot);
const wchar_t* SceneError();  // por que SceneBegin devolveu nullptr
void SceneDraw(ID2D1DeviceContext* dc, int slot, D2D1_RECT_F area, float band, D2D1_COLOR_F fade = D2D1::ColorF(0, 0, 0, 0));  // fade: cor do fundo para onde a faixa vai
}

// ---- drop.cpp: soltar arquivos arrastados de outros apps (e colar).
IDropTarget* CreateDropTarget(HWND hwnd);
HRESULT PasteInto(HWND hwnd, const std::wstring& folder);

// ---- main.cpp: usado pelo alvo de soltar.
struct DropSpot {
    enum Kind { NONE, FOLDER, SIDEBAR_INSERT, TAG } kind = NONE;
    std::wstring path;  // FOLDER: pasta que recebe os arquivos
    int index = -1;     // SIDEBAR_INSERT: posição nos favoritos; TAG: etiqueta
    int item = -1;      // item do conteúdo destacado
    int row = -1;       // linha da barra lateral destacada
};
DropSpot DropSpotAt(POINT clientPx);
void SetDropFeedback(const DropSpot& spot);
void DropCustom(const DropSpot& spot, const std::vector<std::wstring>& paths);  // inserir favorito / etiquetar
void SpringLoad(const std::wstring& path);  // arrastando parado sobre uma pasta: ela abre (pastas com mola do Mac)

// Busca: índice do Windows Search (folder vazio = tudo que está indexado) e varredura pelo nome.
// onPath devolve false para parar. Index devolve -1 se o índice não respondeu.
namespace search {
int Index(const std::wstring& q, const std::wstring& folder, int max, const std::function<bool(const std::wstring&)>& onPath);
int Walk(const std::wstring& q, const std::wstring& folder, int max, const std::function<bool(const std::wstring&)>& onPath);
// o índice do Spotlight do Shell (nomes de tudo, instantâneo); -1 se não houver índice
int Spotlight(const std::wstring& q, const std::wstring& folder, int max, const std::function<bool(const std::wstring&)>& onPath);
// "Buscar por" do Finder do Leopard (buscas prontas no índice, dentro da pasta do usuário, as mais novas primeiro):
// 0 Hoje, 1 Ontem, 2 Semana Passada, 3 Todas as Imagens, 4 Todos os Filmes, 5 Todos os Documentos
int Smart(int kind, int max, const std::function<bool(const std::wstring&)>& onPath);
}

// Arquivos como padrão para abrir pastas (HKCU, reversível; ver padrao.cpp).
namespace padrao {
bool Enabled();
bool Enable(FILE* log);
bool Disable(FILE* log);
}

// ---- media.cpp: áudio e vídeo do Quick Look (Media Foundation Media Engine).
namespace media {
void Init(HWND hwnd, UINT msg);   // eventos chegam como msg(wParam = evento)
bool Open(const std::wstring& path);
void Close();
void OnEvent(DWORD ev);
const std::wstring& Path();
bool Active();
bool Ready();
bool Failed();
bool HasVideo();
UINT VideoW();
UINT VideoH();
bool Paused();
bool Ended();
double Duration();
double Time();
void Seek(double t);
bool Muted();
void SetMuted(bool m);
void TogglePlay();
bool UpdateFrame();          // fora do BeginDraw; true se chegou quadro novo
ID2D1Bitmap* Frame();
}

// ---- info.cpp: janela "Obter Informações" (uma por item).
namespace info {
// admin: janela aberta como administrador (privilégios editáveis); px/py: posição (INT_MIN = ao lado do Finder)
void Show(const std::wstring& path, HWND anchor, const std::wstring& family, bool dark, bool admin = false, int px = -2147483647 - 1, int py = 0);
}
