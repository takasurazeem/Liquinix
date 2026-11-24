// Acesso ao vidro do backdrop.cpp para quem monta peças de vidro próprias na Composição (ex.: os módulos
// da Central de Controle no Tahoe): o mesmo compositor e o mesmo fosco (lente + desfoque do Liquid Glass).
#pragma once
#include <winrt/Windows.UI.Composition.h>

namespace backdrop {
winrt::Windows::UI::Composition::Compositor Compositor();     // nullptr antes do primeiro Attach
winrt::Windows::UI::Composition::CompositionBrush Frost();    // pincel novo: fundo da janela com o fosco do Tahoe
winrt::Windows::UI::Composition::CompositionBrush FrostFor(winrt::Windows::UI::Composition::Compositor const& c);  // idem, noutro compositor
}
