// Menu de contexto próprio: desenha um menu no estilo macOS (acrílico, cantos arredondados,
// no máximo 3 grupos, submenus de um nível) sobre as extensões de shell reais do Windows
// (IContextMenu), e executa o comando escolhido pela extensão original.
#pragma once
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace ctxmenu {

struct Options {
    bool demo = false;   // abre já com um item destacado e ignora a perda de foco (captura de tela)
    int fadeMs = 160;    // duração do fade-out ao fechar
};

// Uma vez no início, depois de OleInitialize. `log` pode ser nullptr.
void Init(HINSTANCE inst, FILE* log, Options opt = {});

// A janela dona repassa WM_INITMENUPOPUP, WM_DRAWITEM, WM_MEASUREITEM e WM_MENUCHAR.
// Devolve true quando a extensão tratou a mensagem (use *res como retorno).
bool ForwardOwnerMsg(UINT msg, WPARAM wp, LPARAM lp, LRESULT* res);

// Carrega as extensões de menu de contexto antes do primeiro uso (o primeiro menu fica rápido).
void Warmup(HWND owner, const std::wstring& folder, const std::wstring& file);
void WarmupAsync(const std::wstring& folder, const std::wstring& file);  // o mesmo, sem travar a janela

// Vidro dos menus (o Shell repassa Ajustes > Transparência e Efeitos). alpha: opacidade final da
// tinta a partir da original (nullptr = sem mudança).
struct Glass {
    bool liquid = false;   // Liquid Glass (Tahoe): reflexo e contorno especular
    bool golden = false;   // macOS 27 Golden Gate: medidas do menu do macOS 27 e quase sem símbolos
    bool leopard = false;  // Mac OS X Leopard: branco translúcido e destaque azul em degradê de ponta a ponta
    float (*alpha)(float) = nullptr;
};
void SetGlass(const Glass& g);
// Ícone do Tahoe (glifo da Segoe Fluent Icons) pelo nome de um item de menu; 0 = nenhum
wchar_t AutoIcon(const std::wstring& label);
// O próximo menu sai sem ícones automáticos (o menu do Dock, que no Mac não tem)
void PlainNext();

bool IsOpen();
// Fecha o menu aberto (ex.: a janela dona recebeu WM_ACTIVATEAPP com wParam = FALSE).
void Close();

struct Result {
    bool chosen = false;   // um comando foi escolhido
    bool rename = false;   // escolheu Renomear: a janela dona faz a edição no lugar
    std::wstring verb;     // verbo canônico ("copy", "delete", ...) quando a extensão informa
    HRESULT hr = S_OK;
    int extra = 0;         // id do item do app escolhido (ExtraNext); 0 = nenhum
};

// Menu dos itens `paths` (todos da mesma pasta) ou, com `paths` vazio, do fundo de `folder`.
// full = todas as opções na ordem do Windows (Shift+clique direito).
Result Show(HWND owner, POINT screenPt, const std::wstring& folder,
            const std::vector<std::wstring>& paths, bool full);

// Menu com itens do próprio app (Agrupar por, Ações...), no mesmo visual. Devolve o id
// escolhido ou 0. Submenus de um nível.
// Fileira de bolinhas coloridas (as etiquetas do Finder): cada uma devolve o seu id quando clicada.
struct Dot { int id = 0; UINT32 rgb = 0; bool on = false; };  // rgb = 0xRRGGBB; on = marcada (✓)
struct Item {
    std::wstring label;
    int id = 0;
    bool checked = false, disabled = false, separator = false;
    std::wstring shortcut;  // atalho mostrado à direita, cinza (ex.: "Ctrl+C")
    std::vector<Item> sub;
    wchar_t icon = 0;       // ícone à esquerda (glifo da Segoe Fluent Icons), como os símbolos do menu do Tahoe
    std::vector<Dot> dots;  // não vazio: a linha é a fileira de bolinhas (sem texto)
};
int ShowItems(HWND owner, POINT screenPt, const std::vector<Item>& items);
// O próximo Show ganha estes itens do app (label, id > 0, icon) logo após o primeiro grupo; escolhido um
// deles, Show devolve chosen = true e extra = id, sem executar nada.
void ExtraNext(const std::vector<Item>& items);

// Objeto de interface do shell para vários itens da mesma pasta (IDataObject para arrastar,
// IDropTarget para soltar sobre uma pasta etc.).
HRESULT GetUIObject(HWND owner, const std::vector<std::wstring>& paths, REFIID riid, void** out);

// Executa um verbo canônico ("copy", "cut", "delete", "properties"...) sem mostrar menu.
HRESULT InvokeVerb(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& paths, const char* verb);

// Grava no log a árvore original e a reorganizada (diagnóstico).
bool DumpToLog(HWND owner, const std::wstring& path, bool background);

}  // namespace ctxmenu
