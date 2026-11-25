// Serviços que mudam de uma plataforma para outra, atrás de uma interface pequena (#19). O código dos apps chama
// estas funções e não sabe em qual sistema roda. Cada backend implementa todas:
//   win/plataforma_win.cpp      Windows (o que o Shell sempre usou)
//   linux/plataforma_linux.cpp  Linux (compilado pelo CMake do #18)
// O #ifdef fica só na escolha do backend (no build), nunca no código dos apps.
#pragma once
#include <string>

namespace plataforma {

// Relógio monotônico em milissegundos, com fração (Windows: QueryPerformanceCounter; Linux: steady_clock).
// Para animações: passo pelo tempo real, nunca por quadro.
double AgoraMs();

// Pasta dos ajustes (ajustes.txt e os outros .txt), sem a barra no fim; vazio se não deu para saber.
// Windows: %APPDATA%\Shell. Linux: $XDG_CONFIG_HOME/liquinix (ou ~/.config/liquinix). Não cria a pasta.
std::wstring PastaAjustes();

}  // namespace plataforma
