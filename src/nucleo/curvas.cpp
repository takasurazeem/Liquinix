// Curvas de animação do macOS (ver curvas.h). Movidas de src/shell/anim.cpp sem mudar as contas.
//  - Curva padrão do Core Animation: cubic-bezier(0.25, 0.1, 0.25, 1.0) (CAMediaTimingFunctionName.default).
//  - Molas como as do SwiftUI: resposta (s) e amortecimento; "suave" (sem quique) e "ágil" (um quique leve).
#include "curvas.h"
#include <cmath>

float curvas::Bezier(float t, float x1, float y1, float x2, float y2) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    auto bx = [&](float u) { float v = 1 - u; return 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u; };
    auto by = [&](float u) { float v = 1 - u; return 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u; };
    auto dx = [&](float u) { float v = 1 - u; return 3 * v * v * x1 + 6 * v * u * (x2 - x1) + 3 * u * u * (1 - x2); };
    float u = t;
    for (int i = 0; i < 6; i++) { float e = bx(u) - t, d = dx(u); if (std::fabs(e) < 1e-4f) break; if (std::fabs(d) < 1e-6f) break; u -= e / d; }
    if (u < 0 || u > 1 || std::fabs(bx(u) - t) > 1e-3f) {  // garantia: bisseção
        float lo = 0, hi = 1; u = t;
        for (int i = 0; i < 30; i++) { float x = bx(u); if (std::fabs(x - t) < 1e-5f) break; if (x < t) lo = u; else hi = u; u = (lo + hi) / 2; }
    }
    return by(u);
}

float curvas::Ease(float t) { return Bezier(t, 0.25f, 0.1f, 0.25f, 1.0f); }
float curvas::EaseInOut(float t) { return Bezier(t, 0.42f, 0.0f, 0.58f, 1.0f); }
float curvas::EaseOut(float t) { return Bezier(t, 0.0f, 0.0f, 0.58f, 1.0f); }
float curvas::OutExpo(float t) { return t >= 1 ? 1.f : 1 - std::pow(2.f, -10 * t); }

float curvas::Spring(float tSec, float response, float damping) {
    if (tSec <= 0) return 0;
    float w = 2 * 3.14159265f / response;
    if (damping >= 1) return 1 - (1 + w * tSec) * std::exp(-w * tSec);  // crítica: sem passar do ponto
    float wd = w * std::sqrt(1 - damping * damping);
    return 1 - std::exp(-damping * w * tSec) * (std::cos(wd * tSec) + damping * w / wd * std::sin(wd * tSec));
}

float curvas::SpringDuration(float response, float damping) {
    for (float t = 0.05f; t < 4; t += 0.01f) {
        bool settled = true;
        for (float u = t; u < t + 0.3f; u += 0.02f) if (std::fabs(1 - Spring(u, response, damping)) > 0.001f) { settled = false; break; }
        if (settled) return t;
    }
    return 4;
}

float curvas::DockMagnify(float dist, float reach, float maxScale, float amount) {
    if (dist >= reach) return 1;
    float k = std::cos(dist / reach * 3.14159f / 2);
    return 1 + (maxScale - 1) * k * k * amount;
}
