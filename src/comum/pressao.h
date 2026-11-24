#pragma once
// Pressão de memória como a do macOS (Monitor de Atividade > Memória): não é "quanto da RAM está ocupada"
// (o Windows, como o Mac, usa a RAM livre de cache, então ela vive cheia), e sim quanto o sistema ainda
// consegue entregar sem ir para o disco. Conta a memória disponível (livre + cache em espera, que o
// Windows devolve na hora) e o quanto da memória prometida (commit) já chegou ao limite (RAM + arquivo
// de paginação). Verde = normal; amarelo = o sistema já está comprimindo e tirando páginas; vermelho =
// faltando memória (o Mac mostra "Seu sistema ficou sem memória para aplicativos").
// Usada pelo Shell (memoria.cpp), pelo Monitor de Atividade e pela janela Forçar Encerrar.
#include <windows.h>
#include <psapi.h>
#include <algorithm>

struct PressaoMemoria {
    float valor = 0;  // 0..1, a altura do gráfico
    int nivel = 0;    // 0 normal (verde), 1 aviso (amarelo), 2 crítica (vermelho)
    double livre = 1, commit = 0;  // fração disponível da RAM; fração usada do limite de commit
};

inline PressaoMemoria MedirPressao() {
    PressaoMemoria p;
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (!GlobalMemoryStatusEx(&ms) || !ms.ullTotalPhys) return p;
    PERFORMANCE_INFORMATION pi{ sizeof(pi) };
    if (GetPerformanceInfo(&pi, sizeof(pi)) && pi.CommitLimit) p.commit = (double)pi.CommitTotal / (double)pi.CommitLimit;
    p.livre = (double)ms.ullAvailPhys / (double)ms.ullTotalPhys;
    p.nivel = (p.livre < 0.05 || p.commit > 0.95) ? 2 : (p.livre < 0.12 || p.commit > 0.88) ? 1 : 0;
    float v = (float)std::max((0.40 - p.livre) / 0.40, (p.commit - 0.55) / 0.45);
    v = std::clamp(v, 0.06f, 1.f);  // o Mac sempre mostra uma faixa verde baixa
    if (p.nivel == 1) v = std::max(v, 0.62f);
    if (p.nivel == 2) v = std::max(v, 0.86f);
    if (p.nivel == 0) v = std::min(v, 0.58f);
    p.valor = v;
    return p;
}
