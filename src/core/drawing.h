// Platform-free 2D drawing (#19): shared code (the Dock, badges...) draws through this interface, and each platform
// brings its own painter: Direct2D on Windows (src/platform/win/painter_d2d), QPainter on Linux
// (src/platform/linux/painter_qt). Coordinates in points (DIP: 1/96 inch), y down, as in Direct2D.
// The interface grows as code moves here: only what something already uses.
#pragma once
#include <string>

namespace drawing {

struct Color { float r = 0, g = 0, b = 0, a = 1; };  // 0..1, not premultiplied
struct Rect { float left = 0, top = 0, right = 0, bottom = 0; };
enum class Weight { Normal = 400, Medium = 500, Semibold = 600, Bold = 700 };  // the CSS/DirectWrite/Qt numbers
enum class Align { Leading, Center, Trailing };

class Painter {
public:
    virtual ~Painter() = default;
    // a filled rectangle with rounded corners (the same radius on both axes)
    virtual void FillRoundedRect(const Rect& r, float radius, const Color& c) = 0;
    // width of a single line of text in the interface font; < 0 if there is no font (then text can't be drawn)
    virtual float TextWidth(const std::wstring& s, float size, Weight w) = 0;
    // a single line of text inside r: aligned horizontally as asked and centred vertically
    virtual void Text(const std::wstring& s, const Rect& r, float size, Weight w, Align a, const Color& c) = 0;
};

}  // namespace drawing
