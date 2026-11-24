# "Aquifica" um ícone moderno (o quadrado arredondado chapado do macOS 11+) no estilo dos ícones do Mac OS X Leopard
# (comparado com os ícones do 10.5 na Dock): o canto bem menos redondo dos ícones de 2007 (o "squircle" de hoje vira um
# quadrado de canto 14 %, com os cantos preenchidos pela cor da borda do próprio desenho), o brilho de gel do Aqua
# (reflexo branco forte na metade de cima, com a borda de baixo nítida), o volume (base mais escura), o contorno
# escuro fino, o fio de luz no alto e a sombra projetada embaixo.
# Saída no formato do pacote do Leopard do projeto: quadro 25 % maior que o desenho (10 % de margem de cada lado).
# Usado por tools/icones-leopard.py para os apps que não existiam em 2007 (Chrome, Discord, WhatsApp...).
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageEnhance


def recorta(im):
    a = im.getchannel('A').point(lambda v: 255 if v > 24 else 0)
    box = a.getbbox()
    return im.crop(box) if box else im


def mascara_quadrada(w, h, raio):
    m = Image.new('L', (w * 4, h * 4), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w * 4 - 1, h * 4 - 1), radius=raio * 4, fill=255)
    return m.resize((w, h), Image.LANCZOS)


def tira_placa(im):
    """Se o ícone é um desenho sobre uma placa clara e lisa, devolve só o desenho (a placa vira transparente; o
    branco de dentro do desenho, que não encosta na placa, fica). Senão, None."""
    w, h = im.size
    px = im.load()
    amostra = []
    for i in range(60):
        t = i / 60
        for x, y in ((t * w, h * 0.07), (t * w, h * 0.93), (w * 0.07, t * h), (w * 0.93, t * h)):
            x, y = min(w - 1, int(x)), min(h - 1, int(y))
            if w * 0.18 < x < w * 0.82 or h * 0.18 < y < h * 0.82: amostra.append(px[x, y])
    if not amostra: return None
    lum = [0.3 * r + 0.59 * g + 0.11 * b for r, g, b, a in amostra]
    med = sum(lum) / len(lum)
    if med < 195 or max(lum) - min(lum) > 75: return None
    # a placa pode ter degradê de cima para baixo: a cor de referência de cada linha vem das bordas dela
    linhas = []
    for y in range(h):
        cs = [px[x, y] for x in (int(w * 0.07), int(w * 0.93), int(w * 0.10), int(w * 0.90)) if px[x, y][3] > 250]
        linhas.append(tuple(sum(c[i] for c in cs) // len(cs) for i in range(3)) if cs else None)
    ult = next((c for c in linhas if c), None)
    for y in range(h):
        if linhas[y]: ult = linhas[y]
        else: linhas[y] = ult
    dist = Image.new('L', (w, h), 0)
    dp = dist.load()
    for y in range(h):
        cor = linhas[y]
        for x in range(w):
            r, g, b, a = px[x, y]
            d = max(abs(r - cor[0]), abs(g - cor[1]), abs(b - cor[2]))
            dp[x, y] = 0 if a < 250 else min(255, d)  # o meio-transparente (sombra da placa) é fundo
    # a faixa de 7 % junto da borda é sempre placa (o aro sombreado do "squircle" moderno)
    m = int(min(w, h) * 0.07)
    ImageDraw.Draw(dist).rectangle((0, 0, w - 1, h - 1), outline=0, width=m)
    desenho = dist.point(lambda v: 255 if v > 30 else 0)
    # sobras pequenas soltas (pedaços do aro, sombra embaixo) saem: só ficam as partes com 3 % do desenho ou mais
    marca = desenho.copy()
    mp = marca.load()
    partes = []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if mp[x, y] == 255:
                antes = marca.histogram()[255]
                ImageDraw.floodfill(marca, (x, y), 128 - len(partes) % 100)
                partes.append(((x, y), antes - marca.histogram()[255]))
    total = sum(n for _, n in partes) or 1
    for (x, y), n in partes:
        if n < 0.03 * total: ImageDraw.floodfill(desenho, (x, y), 0)
    # o fundo é o que é placa e encosta na borda (enchente a partir da borda)
    fundo = desenho.point(lambda v: 0 if v else 100)
    for x0, y0 in ((w // 2, 2), (w // 2, h - 3), (2, h // 2), (w - 3, h // 2), (w // 5, h // 5)):
        if fundo.getpixel((x0, y0)) == 100: ImageDraw.floodfill(fundo, (x0, y0), 255)
    placa = fundo.point(lambda v: 255 if v == 255 else 0)
    if sum(placa.histogram()[255:]) < 0.25 * w * h: return None
    # borda macia: perto do desenho a transparência acompanha a distância de cor
    suave = dist.point(lambda v: min(255, max(0, (v - 6) * 12)))
    perto = placa.filter(ImageFilter.MinFilter(5))
    alpha = ImageChops.subtract(Image.new('L', (w, h), 255), perto)
    borda = ImageChops.subtract(placa, perto)
    alpha = ImageChops.lighter(ImageChops.subtract(alpha, borda), ImageChops.multiply(borda, suave))
    alpha = ImageChops.multiply(alpha, im.getchannel('A'))
    out = im.copy(); out.putalpha(alpha)
    if not alpha.point(lambda v: 255 if v > 24 else 0).getbbox(): return None
    return out


def aquifica(src, dst, lado=320):
    im = recorta(Image.open(src).convert('RGBA'))
    s = int(lado * 0.8)
    k = s / max(im.size)
    im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    im = ImageEnhance.Color(im).enhance(1.15)  # as cores do Aqua eram mais saturadas
    w, h = im.size
    orig_a = im.getchannel('A')
    cheio = orig_a.point(lambda v: 255 if v > 200 else 0)
    # só vale trocar o contorno de quem é um quadrado arredondado cheio (a maior parte do quadro é opaca)
    quadrado = sum(cheio.histogram()[255:]) > 0.80 * w * h
    solto = quadrado and tira_placa(im)
    if solto:
        # placa clara (Chrome, Claude, Edge...): em 2007 o ícone era o desenho solto, sem a placa
        im = recorta(solto)
        k = s / max(im.size)
        im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
        w, h = im.size
        alpha = im.getchannel('A')
    elif quadrado:
        # cantos: o próprio ícone ampliado e desfocado por baixo preenche o que o "squircle" deixava vazio
        fundo = im.resize((round(w * 1.18), round(h * 1.18)), Image.LANCZOS).filter(ImageFilter.GaussianBlur(w * 0.03))
        base = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        base.alpha_composite(fundo, (-(fundo.width - w) // 2, -(fundo.height - h) // 2))
        base.alpha_composite(im)
        alpha = mascara_quadrada(w, h, round(w * 0.14))
        base.putalpha(alpha)
        im = base
    else:
        alpha = orig_a
    # volume: a base escurece (o Aqua desenhava os ícones com luz de cima)
    sombra = Image.new('L', (1, h), 0)
    for y in range(h):
        t = y / h
        sombra.putpixel((0, y), int(255 * 0.28 * max(0.0, (t - 0.55) / 0.45)) if t > 0.55 else 0)
    sombra = ImageChops.multiply(sombra.resize((w, h)), alpha)
    preto = Image.new('RGBA', (w, h), (0, 0, 0, 0)); preto.putalpha(sombra)
    out = Image.alpha_composite(im, preto)
    # o gel: reflexo branco forte em cima que termina numa curva nítida pouco acima do meio
    gel = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(gel)
    d.ellipse((-w * 0.30, -h * 0.80, w * 1.30, h * 0.50), fill=255)
    grad = Image.new('L', (1, h), 0)
    for y in range(h):
        t = y / h
        grad.putpixel((0, y), int(255 * (0.62 - 0.50 * min(1.0, t / 0.50))) if t < 0.5 else 0)
    gel = ImageChops.multiply(gel.filter(ImageFilter.GaussianBlur(w * 0.004)), grad.resize((w, h)))
    gel = ImageChops.multiply(gel, alpha.filter(ImageFilter.MinFilter(5)))
    branco = Image.new('RGBA', (w, h), (255, 255, 255, 0)); branco.putalpha(gel)
    out = Image.alpha_composite(out, branco)
    # contorno escuro fino e o fio de luz no alto da borda
    er = alpha.filter(ImageFilter.MinFilter(3))
    aro = ImageChops.subtract(alpha, er)
    escuro = Image.new('RGBA', (w, h), (0, 0, 0, 0)); escuro.putalpha(aro.point(lambda v: int(v * 0.45)))
    out = Image.alpha_composite(out, escuro)
    er2 = alpha.filter(ImageFilter.MinFilter(7))
    luz = ImageChops.subtract(er, er2)
    topo = Image.new('L', (1, h), 0)
    for y in range(h):
        topo.putpixel((0, y), int(255 * 0.65 * max(0.0, 1 - y / (h * 0.35))))
    luz = ImageChops.multiply(luz, topo.resize((w, h)))
    lb = Image.new('RGBA', (w, h), (255, 255, 255, 0)); lb.putalpha(luz)
    out = Image.alpha_composite(out, lb)
    # quadro com a margem do pacote e a sombra projetada (mais forte embaixo)
    q = Image.new('RGBA', (lado, lado), (0, 0, 0, 0))
    x0, y0 = (lado - w) // 2, (lado - h) // 2 - int(lado * 0.01)
    sh = Image.new('RGBA', (lado, lado), (0, 0, 0, 0))
    sa = Image.new('L', (lado, lado), 0)
    sa.paste(alpha.point(lambda v: int(v * 0.55)), (x0, y0 + int(lado * 0.025)))
    sh.putalpha(sa.filter(ImageFilter.GaussianBlur(lado * 0.022)))
    q = Image.alpha_composite(q, sh)
    q.alpha_composite(out, (x0, y0))
    q.save(dst)


if __name__ == '__main__':
    import sys
    aquifica(sys.argv[1], sys.argv[2])
