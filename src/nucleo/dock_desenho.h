// Partes do desenho do Dock que são iguais em todas as plataformas (desenham por desenho::Pintor).
#pragma once
#include "desenho.h"

namespace dock_desenho {

// Emblema vermelho com o número de avisos de um app, no canto de cima à direita do ícone `icone` (o corpo do
// ícone, em pontos). n > 99 aparece como "99+".
void Emblema(desenho::Pintor& p, const desenho::Ret& icone, int n);

}  // namespace dock_desenho
