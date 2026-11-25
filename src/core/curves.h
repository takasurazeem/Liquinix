// macOS animation curves, with nothing Windows-specific: the shell (anim::) and the Linux side use the same ones.
// Standard C++ only here (no <windows.h>, Direct2D or Qt): this code is shared between platforms (#19).
#pragma once

namespace curves {

// cubic-bezier (0,0)-(x1,y1)-(x2,y2)-(1,1) evaluated at t (0..1): finds the parameter for x and returns y
float Bezier(float t, float x1, float y1, float x2, float y2);
float Ease(float t);       // Core Animation's default curve (0.25, 0.1, 0.25, 1)
float EaseInOut(float t);  // (0.42, 0, 0.58, 1)
float EaseOut(float t);    // (0, 0, 0.58, 1)
float OutExpo(float t);    // 1 - 2^(-10 t)

// A SwiftUI-style spring from 0 to 1. tSec in seconds; response = undamped period (s);
// damping 1 = no bounce, < 1 = bounces.
float Spring(float tSec, float response = 0.55f, float damping = 0.825f);
float SpringDuration(float response, float damping);  // how long the spring takes to settle (|1 - x| < 0.1%)

// Dock magnification: the scale of an icon `dist` steps (slots) away from the pointer. A cos² curve reaching `reach`
// steps to each side; `maxScale` = magnified size / normal size; `amount` = how much of the magnification is applied
// (0..1). Returns 1 out of reach.
float DockMagnify(float dist, float reach, float maxScale, float amount);

// A Dock icon's hop (the launch bounce): 0 -> 1 -> 0 as a parabola, repeating every `periodMs`. `ms` = time since it
// started. Callers scale it by the hop height.
float DockHop(double ms, float periodMs);

}  // namespace curves
