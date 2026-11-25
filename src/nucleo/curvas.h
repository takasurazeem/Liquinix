// Curvas de animação do macOS, sem nada do Windows: o Shell (anim::) e, depois, o lado Linux usam as mesmas.
// Só C++ padrão aqui (nada de <windows.h>, Direct2D ou Qt): é código compartilhado entre as plataformas (#19).
#pragma once

namespace curvas {

// cubic-bezier (0,0)-(x1,y1)-(x2,y2)-(1,1) avaliada em t (0..1): acha o parâmetro pelo x e devolve o y
float Bezier(float t, float x1, float y1, float x2, float y2);
float Ease(float t);       // curva padrão do Core Animation (0.25, 0.1, 0.25, 1)
float EaseInOut(float t);  // (0.42, 0, 0.58, 1)
float EaseOut(float t);    // (0, 0, 0.58, 1)
float OutExpo(float t);    // 1 - 2^(-10 t)

// Mola como a do SwiftUI, de 0 a 1. tSec em segundos; response = período sem amortecimento (s);
// damping 1 = sem quique, < 1 = quica.
float Spring(float tSec, float response = 0.55f, float damping = 0.825f);
float SpringDuration(float response, float damping);  // quanto a mola leva para assentar (|1 - x| < 0,1%)

// Ampliação do Dock: escala de um ícone a `dist` passos (slots) do ponteiro. Curva cos² com alcance `reach` passos
// para cada lado; `maxScale` = tamanho ampliado / tamanho normal; `amount` = quanto da ampliação está aplicado (0..1).
// Fora do alcance devolve 1.
float DockMagnify(float dist, float reach, float maxScale, float amount);

}  // namespace curvas
