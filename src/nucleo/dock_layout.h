// A disposição do Dock ao longo da borda, igual em todas as plataformas (#19): onde fica o centro de cada item, o
// tamanho de cada ícone com a ampliação, o espaço que abre no arrasto e a largura total do fundo.
// Tudo em pontos, ao longo do eixo do Dock (em pé dos lados, a mesma conta).
#pragma once
#include <vector>

namespace dock_layout {

struct Item {
    bool separador = false;
    bool app = false;          // um app (os fixos podem ser arrastados para mudar de lugar)
    bool fixoMovel = false;    // app fixo que pode mudar de lugar (fixo e não é o Finder)
    int indiceApp = -1;        // posição do app na lista de apps (para dizer onde o arrastado cai)
    float fator = 1;           // 0..1: quanto do espaço o item ocupa (entrando ou saindo do Dock aos poucos)
    float cx = 0;              // entrada: centro atual (desliza a partir dele no arrasto); saída: centro novo
    float tamanho = 0;         // saída: tamanho do ícone (0 no separador)
};

struct Parametros {
    float eixo = 0;            // comprimento disponível ao longo do Dock
    float icone = 48;          // tamanho normal do ícone
    float ampliado = 48;       // tamanho máximo com a ampliação
    float passo = 0;           // espaço de um ícone (ícone + folga)
    float margem = 0;          // margem antes do primeiro e depois do último item
    float larguraSeparador = 0;
    bool ampliar = false;      // ampliação ligada nos ajustes
    bool tahoe = false;        // medidas do macOS 26: alcance maior e o separador também cresce
    float mouse = -1;          // ponteiro ao longo do Dock; -1 = fora
    float ultimoMouse = -1;    // última posição dentro (a ampliação encolhe dali ao sair)
    float ampliacao = 0;       // quanto da ampliação está aplicado (0..1, anima ao entrar e sair)
    bool arrastando = false;
    int arrastado = -1;        // índice (em itens) do app fixo sendo arrastado; -1 = nenhum
    bool foraDoDock = false;   // o arrastado saiu do Dock: o lugar dele fecha
};

struct Resultado {
    float total = 0;           // comprimento do fundo
    int destino = -1;          // arrastando: posição de destino na lista de apps; -1 = nenhuma
};

Resultado Calcular(std::vector<Item>& itens, const Parametros& p);

}  // namespace dock_layout
