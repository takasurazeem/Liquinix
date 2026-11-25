// Desenho 2D sem plataforma (#19): o código compartilhado (Dock, emblemas...) desenha por esta interface, e cada
// plataforma traz o seu pintor: Direct2D no Windows (src/plataforma/win/pintor_d2d), QPainter no Linux
// (src/plataforma/linux/pintor_qt). Coordenadas em pontos (DIP: 1/96 de polegada), y para baixo, como no Direct2D.
// A interface cresce conforme o código vem para cá: só o que alguém já usa.
#pragma once
#include <string>

namespace desenho {

struct Cor { float r = 0, g = 0, b = 0, a = 1; };     // 0..1, sem pré-multiplicar
struct Ret { float esq = 0, topo = 0, dir = 0, base = 0; };
enum class Peso { Normal = 400, Medio = 500, Semi = 600, Negrito = 700 };  // os números do CSS/DirectWrite/Qt
enum class Alinhar { Esquerda, Centro, Direita };

class Pintor {
public:
    virtual ~Pintor() = default;
    // retângulo preenchido com os cantos arredondados (raio igual nos dois eixos)
    virtual void RetArredondado(const Ret& r, float raio, const Cor& c) = 0;
    // largura do texto numa linha, na fonte da interface; < 0 se não houver fonte (aí não dá para desenhar texto)
    virtual float LarguraTexto(const std::wstring& s, float tamanho, Peso p) = 0;
    // texto numa linha dentro de r: alinhado na horizontal como pedido e centrado na vertical
    virtual void Texto(const std::wstring& s, const Ret& r, float tamanho, Peso p, Alinhar a, const Cor& c) = 0;
};

}  // namespace desenho
