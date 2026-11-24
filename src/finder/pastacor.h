#pragma once
// Cor e símbolo nas pastas (pastacor.cpp)
#include <d2d1_1.h>
#include <string>
#include <vector>

namespace pastacor {
const int COUNT = 9;
extern const COLORREF colors[COUNT];
extern const wchar_t* names[COUNT];
struct Info { int color = -1; std::wstring emoji; };
bool Get(const std::wstring& path, Info& out);
// color -1 e emoji vazio = volta ao normal. tahoe: qual desenho de pasta usar no ícone do sistema
void Set(const std::wstring& path, int color, const std::wstring& emoji, bool tahoe);
// desenha a pasta personalizada (false: a pasta não tem personalização)
bool Draw(ID2D1DeviceContext* dc, const std::wstring& path, D2D1_RECT_F box, bool tahoe);
bool DrawInfo(ID2D1DeviceContext* dc, const Info& in, D2D1_RECT_F box, bool tahoe);
// pasta comum retingida numa das cores (Aparência > Cor das pastas, macOS 27)
bool DrawColor(ID2D1DeviceContext* dc, int color, D2D1_RECT_F box, bool tahoe);
const std::vector<std::wstring>& Emojis();
}
