// desenho::Pintor sobre QPainter (Linux, Qt 6). Desenha num QPainter já aberto; a fonte da interface vem de quem usa
// (por enquanto a do sistema). Coordenadas em pontos: o QPainter já está na escala do dispositivo.
#pragma once
#include "../../nucleo/desenho.h"
#include <QFont>
#include <QPainter>

class PintorQt : public desenho::Pintor {
public:
    explicit PintorQt(QPainter& p, const QString& familia = QString()) : p_(p), familia_(familia) {}

    void RetArredondado(const desenho::Ret& r, float raio, const desenho::Cor& c) override;
    float LarguraTexto(const std::wstring& s, float tamanho, desenho::Peso p) override;
    void Texto(const std::wstring& s, const desenho::Ret& r, float tamanho, desenho::Peso p, desenho::Alinhar a, const desenho::Cor& c) override;

private:
    QFont Fonte(float tamanho, desenho::Peso p) const;
    QPainter& p_;
    QString familia_;
};
