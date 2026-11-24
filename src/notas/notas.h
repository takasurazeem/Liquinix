// Notas — o app Notas do macOS Sequoia (shell próprio).
// Declarações compartilhadas: modelo (pastas, notas, parágrafos), gravação, Notas Matemáticas e o editor.
// Usa do Finder: gfx (Direct2D sobre DirectComposition, acrílico do DWM), shadow (sombra do Mac) e TextField.
#pragma once
#include "../finder/finder.h"
#include <dwrite.h>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <cstdint>

// ---- modelo
enum Style : uint8_t { S_TITLE, S_HEADING, S_SUBHEADING, S_BODY, S_MONO };
enum ListKind : uint8_t { L_NONE, L_BULLET, L_DASH, L_NUMBER, L_CHECK };
// atributos por caractere: bits 0-3 estilos, bits 4-6 cor do realce (0 = sem, 1..5)
enum : uint8_t { A_BOLD = 1, A_ITALIC = 2, A_UNDER = 4, A_STRIKE = 8 };
inline int HiOf(uint8_t a) { return (a >> 4) & 7; }
inline uint8_t WithHi(uint8_t a, int h) { return (uint8_t)((a & 0x0F) | ((h & 7) << 4)); }
const int HI_COUNT = 5;  // roxo, rosa, laranja, menta, azul (Sequoia)

struct Para {
    std::wstring text;
    std::vector<uint8_t> at;  // um por caractere
    uint8_t style = S_BODY, list = L_NONE, indent = 0;
    bool checked = false, collapsed = false;
};

struct Note {
    std::wstring id, folder, prevFolder;  // prevFolder: de onde veio (para recuperar de Apagadas)
    std::vector<Para> paras;
    ULONGLONG created = 0, modified = 0, deleted = 0;  // FILETIME (100 ns)
    ULONGLONG diskTime = 0;  // data do arquivo quando lido/gravado (para a Nota Rápida de outro processo)
    bool pinned = false;
    bool dirty = false;
    // desfazer
    struct Snap { std::vector<Para> paras; int cp, co, ap, ao; };
    std::vector<Snap> undo, redo;
    int lastEditKind = 0; ULONGLONG lastEditAt = 0;
    std::wstring Title() const;
    std::wstring Preview() const;  // primeira linha de texto depois do título
    bool Empty() const;
    std::wstring PlainText() const;
};

// pastas fixas: "math" (Notas Matemáticas, no topo), "todas" (Todo o iCloud, inteligente), "notas" (iCloud),
// "apagadas" (Apagadas Recentemente), "mac" (No Meu Mac); as do usuário ficam no iCloud
struct NFolder {
    std::wstring id, name;
    int sort = 0;          // 0 data de edição, 1 data de criação, 2 título
    bool group = true;     // agrupar por data
};

namespace store {
extern std::vector<std::unique_ptr<Note>> notes;
extern std::vector<NFolder> folders;  // só as do usuário + "notas" + "mac" + "math" (nomes editáveis das do usuário)
void Load();
void SaveNote(Note* n);          // grava agora (se sujo)
void SaveAllDirty();
void DeleteNoteFile(Note* n);
void SaveFolders();
bool Sync(const Note* keep);   // relê notas gravadas por outro processo; true se mudou algo
Note* NewNote(const std::wstring& folder);
void RemoveNote(Note* n);        // apaga de vez (arquivo + memória)
NFolder* FindFolder(const std::wstring& id);
std::wstring FolderName(const std::wstring& id);
ULONGLONG Now();
std::wstring Serialize(const std::vector<Para>& ps);
std::vector<Para> Deserialize(const std::wstring& s);
}

// ---- Notas Matemáticas: resultado depois de "=" e variáveis destacadas
struct MathLine {
    bool math = false;
    std::wstring result;                       // vazio: sem resultado
    std::vector<std::pair<int, int>> vars;     // (início, tamanho) das variáveis no texto
};
std::vector<MathLine> EvalMath(const std::vector<Para>& ps);

// ---- editor (editor.cpp)
namespace editor {
struct Pos { int p = 0, o = 0; };
void SetNote(Note* n);        // nota mostrada (nullptr: nenhuma)
Note* Current();
void Draw(ID2D1DeviceContext* dc, D2D1_RECT_F area);  // área de texto (abaixo da barra)
bool OnKey(UINT vk);          // true se tratou
void OnChar(wchar_t c);
void OnMouseDown(D2D1_POINT_2F p, bool shift, int clicks);
void OnMouseMove(D2D1_POINT_2F p, bool dragging);
bool OverChevronArea(D2D1_POINT_2F p);
void OnWheel(float dy);
void Focus(bool on);
bool Focused();
void BlinkReset();
void ToggleBlink();
void Invalidate();            // conteúdo mudou (recalcula o layout)
void SetFormats(const std::wstring& family, const std::wstring& mono);
void SetDark(bool d);
void SetSearch(const std::wstring& q);
// formatação a partir do painel Aa / atalhos
void ApplyStyle(int style);
void ToggleAttr(uint8_t flag);
void SetHighlight(int h);     // 0 tira
void ToggleList(int kind);
void Indent(int d);
void ToggleChecked();
int CurrentStyle();
int CurrentList();
uint8_t CurrentAttrs();
void SelectAll();
void PlaceCaretAtEnd();
void Undo();
void Redo();
void CaretScreenRect(D2D1_RECT_F& r);   // em DIPs (para o IME)
void Copy(bool cut);
void Paste();
bool HasSelection();
}

// ---- main.cpp (para o editor avisar)
void NoteEdited(Note* n);     // texto mudou: grava daqui a pouco, reordena a lista
void RequestPaint();
extern bool g_dark;
D2D1_COLOR_F Accent();        // amarelo do Notas
D2D1_COLOR_F AccentText();    // amarelo para texto (resultados, variáveis)
std::wstring LongDate(ULONGLONG ft);   // "7 de novembro de 2025 às 21:40"
