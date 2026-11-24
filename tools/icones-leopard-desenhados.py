# Ícones do tema Leopard desenhados aqui (não vêm do pacote): apps que não existiam em 2007 ganham um desenho no estilo
# da época, feito a partir da forma do logo de hoje (icones\extra / icones\sequoia).
#   Chrome: o ícone de 2008 — a "esfera" 3D com os três gomos em relevo (vermelho, amarelo, verde), o anel branco e o
#           centro azul de vidro, com o brilho forte em cima e a sombra embaixo.
#   Claude: a estrela laranja com volume (degradê de cima para baixo), relevo nas bordas e brilho.
# Saída no formato de icones\leopard (quadro 25 % maior que o desenho, sombra projetada). Chamado por
# tools\icones-leopard.py depois do "aquificar" (sobrescreve as versões aquificadas desses apps).
#   python tools\icones-leopard-desenhados.py [destino]
import os, sys, colorsys
import numpy as np
from PIL import Image, ImageFilter, ImageDraw

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
from aqua_icone import recorta, tira_placa

RAIZ = os.path.join(AQUI, '..', 'icones')
DST = sys.argv[1] if len(sys.argv) > 1 else os.path.join(RAIZ, 'leopard')
LADO = 320


def fonte(*nomes):
    for sub in ('extra', 'sequoia'):
        for n in nomes:
            p = os.path.join(RAIZ, sub, n + '.png')
            if os.path.isfile(p): return p
    return None


def logo_solto(path):
    im = recorta(Image.open(path).convert('RGBA'))
    s = tira_placa(im)
    return recorta(s if s else im)


def quadro(art):
    """centraliza o desenho (80 % do quadro) com a sombra projetada embaixo"""
    s = int(LADO * 0.8)
    k = s / max(art.size)
    art = art.convert('RGBa').resize((max(1, round(art.width * k)), max(1, round(art.height * k))), Image.LANCZOS).convert('RGBA')  # pré-multiplicado: sem halo claro na borda
    q = Image.new('RGBA', (LADO, LADO), (0, 0, 0, 0))
    x0, y0 = (LADO - art.width) // 2, (LADO - art.height) // 2 - int(LADO * 0.01)
    sa = Image.new('L', (LADO, LADO), 0)
    sa.paste(art.getchannel('A').point(lambda v: int(v * 0.5)), (x0, y0 + int(LADO * 0.025)))
    sh = Image.new('RGBA', (LADO, LADO), (0, 0, 0, 0)); sh.putalpha(sa.filter(ImageFilter.GaussianBlur(LADO * 0.022)))
    q = Image.alpha_composite(q, sh)
    q.alpha_composite(art, (x0, y0))
    return q


def chrome():
    src = fonte('Google Chrome', 'chrome', 'google chrome')
    if not src: return None
    im = logo_solto(src)
    N = 512
    k = N / max(im.size)
    im = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32) / 255
    h, w = a.shape[:2]
    rgb, al = a[..., :3], a[..., 3]
    # classifica cada pixel pela cor do logo de hoje (a geometria dos gomos é a mesma de 2008)
    mx, mn = rgb.max(2), rgb.min(2)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    hue = np.zeros_like(r)
    d = np.maximum(mx - mn, 1e-6)
    hue = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) * 60
    lab = np.full((h, w), -1, np.int8)  # 0 vermelho, 1 amarelo, 2 verde, 3 azul, 4 branco
    on = al > 0.5
    lab[on & (sat < 0.25) & (mx > 0.75)] = 4
    col = on & (sat >= 0.25)
    lab[col & ((hue < 25) | (hue >= 330))] = 0
    lab[col & (hue >= 25) & (hue < 75)] = 1
    lab[col & (hue >= 75) & (hue < 180)] = 2
    lab[col & (hue >= 180) & (hue < 330)] = 3
    # o centro: mantém o círculo (anel branco e miolo azul) medindo pelo próprio logo
    ys, xs = np.nonzero(on)
    cy, cx = ys.mean(), xs.mean()
    R = max(ys.max() - ys.min(), xs.max() - xs.min()) / 2
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dist = np.hypot(yy - cy, xx - cx) / R
    blue = lab == 3
    rb = np.percentile(dist[blue], 98) if blue.any() else 0.36
    rw = rb * 1.25
    # cores de 2008: mais vivas e com volume (claras em cima, escuras embaixo e na borda)
    base = {0: (0.86, 0.13, 0.11), 1: (1.0, 0.80, 0.10), 2: (0.16, 0.62, 0.20)}
    out = np.zeros((h, w, 4), np.float32)
    luz = 1.18 - 0.45 * ((yy - (cy - R)) / (2 * R))          # luz de cima
    borda = 1 - 0.38 * np.clip((dist - 0.55) / 0.45, 0, 1) ** 2  # escurece perto do aro (esfera)
    for kk, c in base.items():
        m = lab == kk
        for i in range(3):
            out[..., i] = np.where(m, np.clip(c[i] * luz * borda, 0, 1), out[..., i])
    # anel branco (perolado) e o miolo azul de vidro
    ring = (dist <= rw) & (dist > rb) & on
    pr = np.clip(1.0 - 0.18 * ((yy - cy) / (rw * R) + 1) / 2, 0, 1)
    for i in range(3): out[..., i] = np.where(ring, pr * (0.98, 0.98, 1.0)[i], out[..., i])
    core = (dist <= rb) & on
    t = np.clip(((yy - cy) / (rb * R) + 1) / 2, 0, 1)  # 0 em cima, 1 embaixo
    cb = [(0.45 + 0.20 * (1 - t)), (0.68 + 0.18 * (1 - t)), (0.98 + 0.0 * t)]
    cb = [np.clip(cb[0] - 0.25 * t, 0, 1), np.clip(cb[1] - 0.30 * t, 0, 1), np.clip(cb[2] - 0.15 * t, 0, 1)]
    for i in range(3): out[..., i] = np.where(core, cb[i], out[..., i])
    # o que sobrou sem rótulo dentro do logo (antisserrilhado): cor original
    rest = on & (lab == -1)
    for i in range(3): out[..., i] = np.where(rest, rgb[..., i], out[..., i])
    out[..., 3] = al
    img = Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8), 'RGBA')
    # divisões em relevo entre os gomos (fio escuro) e o aro escuro de fora
    lab_img = Image.fromarray(((lab.astype(np.int16) + 1) * 40).astype(np.uint8), 'L')
    ed = np.asarray(lab_img.filter(ImageFilter.FIND_EDGES)).astype(np.float32)
    ed = (ed > 10).astype(np.float32) * on
    ed = np.asarray(Image.fromarray((ed * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.6))).astype(np.float32) / 255
    dark = Image.new('RGBA', (w, h), (0, 0, 0, 0)); dark.putalpha(Image.fromarray((np.clip(ed * 0.55, 0, 1) * 255).astype(np.uint8)))
    img = Image.alpha_composite(img, dark)
    am = Image.fromarray((al * 255).astype(np.uint8))
    aro = np.asarray(am).astype(np.float32) - np.asarray(am.filter(ImageFilter.MinFilter(7))).astype(np.float32)
    rimg = Image.new('RGBA', (w, h), (40, 10, 10, 0)); rimg.putalpha(Image.fromarray((np.clip(aro, 0, 255) * 0.5).astype(np.uint8)))
    img = Image.alpha_composite(img, rimg)
    # o brilho de vidro: elipse branca na metade de cima, sumindo para baixo
    gl = Image.new('L', (w, h), 0)
    ImageDraw.Draw(gl).ellipse((cx - R * 0.80, cy - R * 0.97, cx + R * 0.80, cy + R * 0.05), fill=255)
    grad = np.clip(1 - (yy - (cy - R)) / (R * 1.0), 0, 1) * 0.45
    gla = np.asarray(gl.filter(ImageFilter.GaussianBlur(2))).astype(np.float32) / 255 * grad * al
    white = Image.new('RGBA', (w, h), (255, 255, 255, 0)); white.putalpha(Image.fromarray((gla * 255).astype(np.uint8)))
    img = Image.alpha_composite(img, white)
    # brilhinho no miolo azul
    sp = Image.new('L', (w, h), 0)
    ImageDraw.Draw(sp).ellipse((cx - rb * R * 0.6, cy - rb * R * 0.85, cx + rb * R * 0.6, cy - rb * R * 0.15), fill=170)
    spw = Image.new('RGBA', (w, h), (255, 255, 255, 0)); spw.putalpha(sp.filter(ImageFilter.GaussianBlur(3)))
    img = Image.alpha_composite(img, spw)
    return quadro(img)


def claude():
    src = fonte('Claude')
    if not src: return None
    im = logo_solto(src)
    N = 512
    k = N / max(im.size)
    im = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
    w, h = im.size
    # forma suavizada (o recorte da placa deixa a borda serrilhada): desfoca e aperta de novo
    a0 = np.asarray(im.getchannel('A').filter(ImageFilter.GaussianBlur(2.2))).astype(np.float32) / 255
    al = np.clip((a0 - 0.5) * 4 + 0.5, 0, 1)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # o laranja do Claude (#D97757) com volume: mais claro em cima, terracota embaixo
    t = yy / h
    cor = np.stack([0.93 - 0.14 * t, 0.56 - 0.20 * t, 0.42 - 0.18 * t], -1)
    # relevo: a luz vem de cima à esquerda (derivada do alfa desfocado)
    ab = np.asarray(Image.fromarray((al * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(5))).astype(np.float32) / 255
    gy, gx = np.gradient(ab)
    rel = -(gx * 0.6 + gy * 0.8) * 9
    cor = np.clip(cor + np.clip(rel, -0.6, 0.6)[..., None] * 0.12, 0, 1)
    out = np.dstack([cor, al])
    img = Image.fromarray((out * 255).astype(np.uint8), 'RGBA')
    # contorno fino mais escuro (como os ícones da época)
    am = img.getchannel('A')
    aro = np.asarray(am).astype(np.float32) - np.asarray(am.filter(ImageFilter.MinFilter(5))).astype(np.float32)
    rimg = Image.new('RGBA', (w, h), (120, 45, 20, 0)); rimg.putalpha(Image.fromarray((np.clip(aro, 0, 255) * 0.35).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)))
    img = Image.alpha_composite(img, rimg)
    # brilho: faixa branca na metade de cima, só dentro da forma
    grad = np.clip(1 - yy / (h * 0.55), 0, 1) * 0.40
    gla = grad * np.asarray(am.filter(ImageFilter.MinFilter(9))).astype(np.float32) / 255
    white = Image.new('RGBA', (w, h), (255, 255, 255, 0)); white.putalpha(Image.fromarray((gla * 255).astype(np.uint8)))
    img = Image.alpha_composite(img, white)
    return quadro(img)


if __name__ == '__main__':
    os.makedirs(DST, exist_ok=True)
    feitos = []
    c = chrome()
    if c:
        for n in ('Google Chrome', 'chrome', 'google chrome', 'Google_Chrome 2'):
            c.save(os.path.join(DST, n + '.png'))
        feitos.append('Chrome')
    c = claude()
    if c:
        c.save(os.path.join(DST, 'Claude.png')); feitos.append('Claude')
    print('desenhados:', ', '.join(feitos))
