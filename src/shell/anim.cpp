// Animações no jeito do macOS: relógio de alta precisão, a curva padrão do sistema e molas.
//  - Relógio: QueryPerformanceCounter (o GetTickCount anda de 15,6 em 15,6 ms e fazia quadros repetirem
//    ou pularem a 60 Hz — parte do "não fluido").
//  - Curva padrão do Core Animation: cubic-bezier(0.25, 0.1, 0.25, 1.0) (CAMediaTimingFunctionName.default).
//  - Molas como as do SwiftUI: resposta (s) e amortecimento; "suave" (sem quique) e "ágil" (um quique leve).
#include "shell.h"
#include <cmath>

double anim::Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// cubic-bezier (0,0)-(x1,y1)-(x2,y2)-(1,1): acha o parâmetro pelo x (Newton + bisseção) e devolve o y
float anim::Bezier(float t, float x1, float y1, float x2, float y2) {
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

float anim::Ease(float t) { return Bezier(t, 0.25f, 0.1f, 0.25f, 1.0f); }      // padrão do sistema
float anim::EaseInOut(float t) { return Bezier(t, 0.42f, 0.0f, 0.58f, 1.0f); }  // entra e sai devagar
float anim::EaseOut(float t) { return Bezier(t, 0.0f, 0.0f, 0.58f, 1.0f); }     // começa rápido

// Mola criticamente/sub-amortecida partindo de 0 e indo a 1. tSec: tempo em segundos.
// response: período da oscilação sem amortecimento (s); damping: 1 = sem quique, < 1 = quica.
float anim::Spring(float tSec, float response, float damping) {
    if (tSec <= 0) return 0;
    float w = 2 * 3.14159265f / response;
    if (damping >= 1) return 1 - (1 + w * tSec) * std::exp(-w * tSec);  // crítica: sem passar do ponto
    float wd = w * std::sqrt(1 - damping * damping);
    return 1 - std::exp(-damping * w * tSec) * (std::cos(wd * tSec) + damping * w / wd * std::sin(wd * tSec));
}

// Tahoe: gravado quadro a quadro no macOS 26 (guia macOS26-Theme-Reference, seções 44 e 53): o Spotlight (e os
// painéis de vidro) aparece com um fade de ~80 ms e cresce de 98 % a 100 % numa curva out-expo até ~300 ms; some
// com um fade de ~100 ms encolhendo de leve. A Central de Controle tem a dela (CCIn/CCOut).
static float OutExpo(float t) { return t >= 1 ? 1.f : 1 - std::pow(2.f, -10 * t); }

bool anim::reduceMotion = false;

anim::Panel anim::PanelIn(double ms) {
    float t = (float)(ms / 1000.0);
    Panel p;
    if (reduceMotion) return p;  // Reduzir Movimento: aparece na hora (o Spotlight do Mac também)
    if (liquid::On()) {
        p.fade = EaseOut(std::min(1.f, t / 0.08f));
        p.scale = 0.98f + 0.02f * OutExpo(std::min(1.f, t / 0.30f));
        p.done = ms >= 300;
        return p;
    }
    p.fade = Ease(std::min(1.f, t / 0.18f));
    p.scale = 0.96f + 0.04f * Spring(t, 0.30f, 0.78f);
    p.done = ms >= 520;
    return p;
}

anim::Panel anim::PanelOut(double ms) {
    if (reduceMotion) {  // fade curto, sem encolher
        Panel p;
        float u = std::min(1.f, (float)(ms / 120.0));
        p.fade = 1 - u; p.done = ms >= 120;
        return p;
    }
    if (liquid::On()) {
        Panel p;
        float u = std::min(1.f, (float)(ms / 100.0));
        p.fade = 1 - OutExpo(u);
        p.scale = 0.98f + 0.02f * (1 - u);
        p.done = ms >= 100;
        return p;
    }
    float t = 1 - std::min(1.f, (float)(ms / PANEL_OUT_MS));
    Panel p;
    p.fade = t * t;
    p.scale = 0.96f + 0.04f * p.fade;
    p.done = ms >= PANEL_OUT_MS;
    return p;
}

// Central de Controle do Tahoe (guia, seções 53 e 44): nasce colada na barra com ~95 % da largura e cresce em
// ~100 ms; os módulos aparecem com fade de ~250 ms. Fechando: fade linear de ~220 ms, encolhendo para ~88 %.
anim::Panel anim::CCIn(double ms) {
    float t = (float)(ms / 1000.0);
    Panel p;
    p.fade = EaseOut(std::min(1.f, t / 0.25f));
    p.scale = reduceMotion ? 1.f : 0.95f + 0.05f * Bezier(std::min(1.f, t / 0.10f), 0.2f, 0.9f, 0.3f, 1);  // Reduzir Movimento: só o fade
    p.done = ms >= 260;
    return p;
}
anim::Panel anim::CCOut(double ms) {
    Panel p;
    float D = reduceMotion ? 270.f : 220.f;  // Reduzir Movimento: fade de ~0,27 s, sem encolher (medido)
    float u = std::min(1.f, (float)(ms / D));
    p.fade = 1 - u;
    p.scale = reduceMotion ? 1.f : 1 - 0.12f * EaseInOut(u);
    p.done = ms >= D;
    return p;
}

// quanto tempo a mola leva para assentar (|1 - x| < 0,1%)
float anim::SpringDuration(float response, float damping) {
    for (float t = 0.05f; t < 4; t += 0.01f) {
        bool settled = true;
        for (float u = t; u < t + 0.3f; u += 0.02f) if (std::fabs(1 - Spring(u, response, damping)) > 0.001f) { settled = false; break; }
        if (settled) return t;
    }
    return 4;
}

// mesma base do GetTickCount (tempo de interrupção), mas com fração de milissegundo: serve para comparar
// com marcas gravadas por GetTickCount (ex.: App::launchedAt) sem os degraus de 15,6 ms
double anim::Tick() {
    using Fn = void(WINAPI*)(PULONGLONG);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryInterruptTimePrecise");
    if (!fn) return (double)GetTickCount();
    ULONGLONG t = 0; fn(&t);
    static double offset = (double)t / 10000.0 - (double)GetTickCount();  // alinha com o GetTickCount uma vez
    return (double)t / 10000.0 - offset;
}
