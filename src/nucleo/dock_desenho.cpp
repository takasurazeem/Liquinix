// Desenho do Dock compartilhado entre as plataformas (ver dock_desenho.h). Movido de src/shell/dock.cpp sem mudar as
// contas: no Windows o resultado é o mesmo, pixel a pixel.
#include "dock_desenho.h"
#include <algorithm>

void dock_desenho::Emblema(desenho::Pintor& p, const desenho::Ret& ib, int n) {
    using namespace desenho;
    float body = ib.dir - ib.esq;
    float cx = (ib.esq + ib.dir) / 2, cy = (ib.topo + ib.base) / 2;
    // medidas da referência do Mac (ícone do Mensagens): círculo de ~44% do ícone, passando ~13% da borda
    // para a direita e ~10% para cima; número fino, ~55% do círculo
    float d = body * 0.44f;
    std::wstring t = n > 99 ? L"99+" : std::to_wstring(n);
    float tw = p.LarguraTexto(t, d * 0.60f, Peso::Normal);
    bool fonte = tw >= 0;
    if (!fonte) tw = d * 0.5f;
    float w = std::max(d, tw + d * 0.55f);
    float bx = cx + body / 2 + body * 0.13f - w / 2, by = cy - body / 2 - body * 0.10f + d / 2;  // borda direita e topo passando do ícone
    Ret r{ bx - w / 2, by - d / 2, bx + w / 2, by + d / 2 };
    p.RetArredondado(Ret{ r.esq, r.topo + d * 0.04f, r.dir, r.base + d * 0.06f }, d / 2, Cor{ 0, 0, 0, 0.18f });  // sombrinha
    p.RetArredondado(r, d / 2, Cor{ 1.0f, 0.231f, 0.188f, 1 });  // vermelho do sistema (#FF3B30)
    if (fonte) p.Texto(t, Ret{ r.esq, r.topo - d * 0.04f, r.dir, r.base - d * 0.04f }, d * 0.60f, Peso::Normal, Alinhar::Centro, Cor{ 1, 1, 1, 1 });
}
