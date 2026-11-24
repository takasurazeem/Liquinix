// Campo de texto de uma linha desenhado com DirectWrite (sem controles EDIT do Win32,
// que não aparecem numa janela sem bitmap de redirecionamento).
#include "finder.h"
#include <algorithm>
#include <cwctype>

static bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

void TextField::SetText(const std::wstring& t, bool selectAll) {
    text = t;
    caret = (int)t.size();
    anchor = selectAll ? 0 : caret;
    scrollX = 0;
}

void TextField::EraseSelection() {
    int a = SelMin(), b = SelMax();
    text.erase(a, b - a);
    caret = anchor = a;
}

void TextField::Insert(const std::wstring& s) {
    EraseSelection();
    text.insert(caret, s);
    caret += (int)s.size();
    anchor = caret;
}

static int PrevWord(const std::wstring& t, int i) {
    while (i > 0 && !IsWordChar(t[i - 1])) i--;
    while (i > 0 && IsWordChar(t[i - 1])) i--;
    return i;
}

static int NextWord(const std::wstring& t, int i) {
    int n = (int)t.size();
    while (i < n && !IsWordChar(t[i])) i++;
    while (i < n && IsWordChar(t[i])) i++;
    return i;
}

static void CopyToClipboard(const std::wstring& s) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

static std::wstring PasteFromClipboard() {
    std::wstring s;
    if (!OpenClipboard(nullptr)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) { s = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c == L'\r' || c == L'\n' || c == L'\t'; }), s.end());
    return s;
}

bool TextField::OnKey(UINT vk, bool shift, bool ctrl) {
    int n = (int)text.size();
    switch (vk) {
    case VK_LEFT:
        if (!shift && caret != anchor) caret = anchor = SelMin();
        else { caret = ctrl ? PrevWord(text, caret) : std::max(0, caret - 1); if (!shift) anchor = caret; }
        return true;
    case VK_RIGHT:
        if (!shift && caret != anchor) caret = anchor = SelMax();
        else { caret = ctrl ? NextWord(text, caret) : std::min(n, caret + 1); if (!shift) anchor = caret; }
        return true;
    case VK_HOME: caret = 0; if (!shift) anchor = caret; return true;
    case VK_END: caret = n; if (!shift) anchor = caret; return true;
    case VK_BACK:
        if (caret != anchor) EraseSelection();
        else if (caret > 0) { int from = ctrl ? PrevWord(text, caret) : caret - 1; text.erase(from, caret - from); caret = anchor = from; }
        return true;
    case VK_DELETE:
        if (caret != anchor) EraseSelection();
        else if (caret < n) { int to = ctrl ? NextWord(text, caret) : caret + 1; text.erase(caret, to - caret); }
        return true;
    case 'A': if (ctrl) { anchor = 0; caret = n; return true; } break;
    case 'C': if (ctrl) { if (caret != anchor) CopyToClipboard(text.substr(SelMin(), SelMax() - SelMin())); return true; } break;
    case 'X': if (ctrl) { if (caret != anchor) { CopyToClipboard(text.substr(SelMin(), SelMax() - SelMin())); EraseSelection(); } return true; } break;
    case 'V': if (ctrl) { Insert(PasteFromClipboard()); return true; } break;
    }
    return false;
}

void TextField::OnChar(wchar_t c) {
    if (c < 32 || c == 127) return;
    Insert(std::wstring(1, c));
}

void TextField::Draw(ID2D1DeviceContext* dc, IDWriteTextFormat* tf, D2D1_RECT_F rc, ID2D1Brush* textBrush,
                     ID2D1Brush* selBrush, ID2D1Brush* caretBrush, bool showCaret, bool center) {
    float w = rc.right - rc.left, h = rc.bottom - rc.top;
    IDWriteTextLayout* tl = nullptr;
    if (FAILED(gfx::dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), tf, 100000, h, &tl))) return;
    DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm);
    float cx, cy; DWRITE_HIT_TEST_METRICS hm;
    tl->HitTestTextPosition((UINT32)caret, FALSE, &cx, &cy, &hm);

    float textW = tm.widthIncludingTrailingWhitespace;
    if (center && textW <= w - 4) { scrollX = 0; lastOffset = (w - textW) / 2; }
    else {
        if (cx - scrollX > w - 4) scrollX = cx - (w - 4);   // mantém o cursor visível
        if (cx - scrollX < 0) scrollX = cx;
        scrollX = std::max(0.f, std::min(scrollX, std::max(0.f, textW - (w - 4))));
        lastOffset = 2 - scrollX;
    }
    lastRect = rc;

    dc->PushAxisAlignedClip(rc, D2D1_ANTIALIAS_MODE_ALIASED);
    if (caret != anchor) {
        UINT32 count = 0;
        tl->HitTestTextRange(SelMin(), SelMax() - SelMin(), 0, 0, nullptr, 0, &count);
        std::vector<DWRITE_HIT_TEST_METRICS> ms(count);
        tl->HitTestTextRange(SelMin(), SelMax() - SelMin(), 0, 0, ms.data(), count, &count);
        for (auto& m : ms)
            dc->FillRectangle(D2D1::RectF(rc.left + lastOffset + m.left, rc.top + m.top,
                                          rc.left + lastOffset + m.left + m.width, rc.top + m.top + m.height), selBrush);
    }
    dc->DrawTextLayout(D2D1::Point2F(rc.left + lastOffset, rc.top), tl, textBrush);
    if (showCaret)
        dc->FillRectangle(D2D1::RectF(rc.left + lastOffset + cx, rc.top + hm.top + 1,
                                      rc.left + lastOffset + cx + 1.2f, rc.top + hm.top + hm.height - 1), caretBrush);
    dc->PopAxisAlignedClip();
    tl->Release();
}

void TextField::Click(IDWriteTextFormat* tf, D2D1_POINT_2F pt, bool shift) {
    IDWriteTextLayout* tl = nullptr;
    float h = lastRect.bottom - lastRect.top;
    if (FAILED(gfx::dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), tf, 100000, h, &tl))) return;
    BOOL trailing, inside; DWRITE_HIT_TEST_METRICS hm;
    tl->HitTestPoint(pt.x - lastRect.left - lastOffset, pt.y - lastRect.top, &trailing, &inside, &hm);
    caret = (int)hm.textPosition + (trailing ? 1 : 0);
    caret = std::min(caret, (int)text.size());
    if (!shift) anchor = caret;
    tl->Release();
}
