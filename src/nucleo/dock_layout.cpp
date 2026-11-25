// A disposição do Dock (ver dock_layout.h). Movida de Layout() em src/shell/dock.cpp com as mesmas contas, na mesma
// ordem: no Windows o resultado é igual, número por número.
#include "dock_layout.h"
#include "curvas.h"
#include <algorithm>
#include <cmath>

dock_layout::Resultado dock_layout::Calcular(std::vector<Item>& itens, const Parametros& p) {
    Resultado res;
    float W = p.eixo;
    float slot = p.passo;
    // largura base para achar os centros sem ampliação
    float base = p.margem;
    std::vector<float> c0;
    for (auto& it : itens) { float w = it.separador ? p.larguraSeparador : slot; c0.push_back(base + w / 2); base += w; }
    base += p.margem;
    float left0 = (W - base) / 2;
    // Tahoe (guia, seção 63): curva cos² com alcance de ~3,5 ícones para cada lado (medido 85/82/74/65 com 64 de base)
    float M = p.ampliado / p.icone, R = p.tahoe ? 3.5f : 2.6f;
    std::vector<float> wv;
    float total = p.margem * 2;
    for (size_t i = 0; i < itens.size(); i++) {
        float s = 1;
        float mx = p.mouse >= 0 ? p.mouse : p.ultimoMouse;
        if (p.ampliar && mx >= 0 && p.ampliacao > 0.001f && !itens[i].separador && !p.arrastando) {
            s = curvas::DockMagnify(std::fabs(mx - (left0 + c0[i])) / slot, R, M, p.ampliacao);
        }
        float sepS = 1;  // Tahoe (guia, seção 63): o separador também cresce perto do ponteiro (31 → 40 com 64)
        if (p.tahoe && p.ampliar && mx >= 0 && p.ampliacao > 0.001f && itens[i].separador && !p.arrastando) {
            sepS = curvas::DockMagnify(std::fabs(mx - (left0 + c0[i])) / slot, R, 40.f / 31.f, p.ampliacao);
        }
        itens[i].tamanho = itens[i].separador ? 0 : p.icone * s;
        wv.push_back(itens[i].separador ? p.larguraSeparador * sepS : slot * s * itens[i].fator);
        total += wv.back();
    }
    // Arrastando um app fixo: os outros abrem espaço ao vivo onde ele cairia (como no Mac); arrastado
    // para fora do Dock, o lugar dele fecha. destino guarda a posição de destino (índice na lista de apps).
    std::vector<int> order;
    for (int i = 0; i < (int)itens.size(); i++) order.push_back(i);
    int drag = p.arrastando && p.arrastado >= 0 && p.arrastado < (int)itens.size() && itens[p.arrastado].app ? p.arrastado : -1;
    if (drag >= 0) {
        order.erase(std::find(order.begin(), order.end(), drag));
        total -= wv[drag];
        if (!p.foraDoDock) {
            // centros sem o arrastado; o lugar novo fica entre os fixos, depois do Finder e do Launchpad
            float xx = (W - total - wv[drag]) / 2 + p.margem;
            int pos = -1, firstPinned = -1, lastPinned = -1;
            for (int k = 0; k < (int)order.size(); k++) {
                auto& it = itens[order[k]];
                bool pinned = it.app && it.fixoMovel;
                if (pinned) { if (firstPinned < 0) firstPinned = k; lastPinned = k; }
                float c = xx + wv[order[k]] / 2;
                if (pos < 0 && pinned && p.mouse < c) pos = k;
                xx += wv[order[k]];
            }
            if (firstPinned < 0) firstPinned = lastPinned = (int)order.size() - 1;
            if (pos < 0) pos = lastPinned + 1;
            pos = std::clamp(pos, firstPinned, lastPinned + 1);
            order.insert(order.begin() + pos, drag);
            total += wv[drag];
            int from = itens[drag].indiceApp;
            if (pos > firstPinned) { int prev = itens[order[pos - 1]].indiceApp; res.destino = prev + (prev < from ? 1 : 0); }
            else if (pos + 1 < (int)order.size() && itens[order[pos + 1]].app) res.destino = itens[order[pos + 1]].indiceApp - (itens[order[pos + 1]].indiceApp > from ? 1 : 0);
            else res.destino = from;
        }
    }
    float x = (W - total) / 2 + p.margem;
    for (int i : order) {
        float target = x + wv[i] / 2;
        // desliza até o lugar novo em vez de pular (só durante o arrasto; o relógio de quadros está ligado)
        itens[i].cx = p.arrastando && itens[i].cx > 0 ? itens[i].cx + (target - itens[i].cx) * 0.35f : target;
        x += wv[i];
    }
    if (drag >= 0 && p.foraDoDock) itens[drag].cx = -1000;
    res.total = total;
    return res;
}
