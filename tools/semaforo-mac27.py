# Gera src/comum/semaforo_mac27.inc: os botões fechar/minimizar/zoom do macOS 27 tirados pixel a pixel das capturas 2x
# (512 Pixels), separados do fundo da janela. Cada um vira um quadrado de 36 × 36 px (o botão de 28 px e o brilho em
# volta) em BGRA pré-multiplicado, para o semaforo.h desenhar escalado:
#   0 fechar, 1 minimizar, 2 zoom (janela ativa, Finder-Home-Folder); 3 inativo claro (Finder-Get-Info);
#   4 inativo escuro (Apperance-Dark-Automatic)
# python -I tools\semaforo-mac27.py <pasta 512pixels>
import sys, os, math
import numpy as np
from PIL import Image
pasta = sys.argv[1]
N, R = 36, 14.0
SRC = [('Finder-Home-Folder.png', 164, 128, (237, 236, 235)), ('Finder-Home-Folder.png', 210, 128, (237, 236, 235)),
       ('Finder-Home-Folder.png', 256, 128, (237, 236, 235)), ('Finder-Get-Info.png', 788, 250, (242, 242, 242)),
       ('Apperance-Dark-Automatic.png', 1344, 182, (43, 42, 42))]
out = []
imgs = {}
for (name, cxx, cyy, bgc) in SRC:
    if name not in imgs: imgs[name] = np.array(Image.open(os.path.join(pasta, name)).convert('RGB')).astype(float)
    a = imgs[name]; bg = np.array(bgc, dtype=float)
    x0, y0 = cxx - N // 2, cyy - N // 2   # centro contínuo do botão no meio do quadrado
    c = N / 2 - 0.5
    px = a[y0:y0 + N, x0:x0 + N]
    rgba = np.zeros((N, N, 4))
    for y in range(N):
        for x in range(N):
            p = px[y, x]
            dx, dy = x - c, y - c
            d = math.hypot(dx, dy)
            if d <= R - 1.2:
                rgba[y, x] = [*p, 1.0]
                continue
            # cor de dentro na mesma direção (a borda), para achar quanto do pixel é botão
            k = (R - 1.6) / max(d, 1e-6)
            ix, iy = int(round(c + dx * k)), int(round(c + dy * k))
            fg = px[iy, ix] if d <= R + 0.8 else px[iy, ix] * 0.5 + bg * 0.5  # o brilho de fora: a cor do botão
            v = fg - bg
            al = float(np.clip(np.dot(p - bg, v) / max(np.dot(v, v), 1e-6), 0, 1))
            if al < 0.015: rgba[y, x] = [0, 0, 0, 0]; continue
            col = np.clip(bg + (p - bg) / al, 0, 255)  # a cor que, com essa opacidade, dá o pixel medido
            rgba[y, x] = [*col, al]
    out.append(rgba)
fn = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'comum', 'semaforo_mac27.inc')
with open(fn, 'w', encoding='utf-8') as f:
    f.write('// gerado por tools/semaforo-mac27.py: os botões do macOS 27 (captura 2x do Finder), 36 x 36 px BGRA pré-multiplicado\n')
    f.write('static const unsigned char MAC27_LIGHTS[5][36 * 36 * 4] = {\n')
    for rgba in out:
        vals = []
        for y in range(N):
            for x in range(N):
                r, g, b, al = rgba[y, x]
                vals += [int(round(b * al)), int(round(g * al)), int(round(r * al)), int(round(al * 255))]
        f.write('    { ' + ','.join(map(str, vals)) + ' },\n')
    f.write('};\n')
    # versão 1x (18 x 18), reduzida com Lanczos em luz linear sobre o alfa pré-multiplicado: sem serrilhado em telas 100 %
    f.write('static const unsigned char MAC27_LIGHTS_1X[5][18 * 18 * 4] = {\n')
    for rgba in out:
        pm = np.zeros((N, N, 4))
        pm[..., :3] = (rgba[..., :3] / 255.0) ** 2.2 * rgba[..., 3:]
        pm[..., 3] = rgba[..., 3]
        ch = [np.array(Image.fromarray(pm[..., k].astype(np.float32), 'F').resize((18, 18), Image.LANCZOS)) for k in range(4)]
        a1 = np.clip(ch[3], 0, 1)
        vals = []
        for y in range(18):
            for x in range(18):
                al = float(a1[y, x])
                lin = [float(np.clip(ch[k][y, x], 0, 1)) / al if al > 1e-4 else 0.0 for k in range(3)]
                s1 = [min(1.0, v) ** (1 / 2.2) for v in lin]
                vals += [int(round(s1[2] * al * 255)), int(round(s1[1] * al * 255)), int(round(s1[0] * al * 255)), int(round(al * 255))]
        f.write('    { ' + ','.join(map(str, vals)) + ' },\n')
    f.write('};\n')
# conferência: compõe de volta sobre o fundo e mede a diferença
for i, (rgba, (name, cxx, cyy, bgc)) in enumerate(zip(out, SRC)):
    a = imgs[name]; bg = np.array(bgc, dtype=float); x0, y0 = cxx - N // 2, cyy - N // 2
    comp = rgba[..., :3] * rgba[..., 3:] + bg * (1 - rgba[..., 3:])
    print('botão', i, 'erro médio de volta: %.2f' % np.abs(comp - a[y0:y0 + N, x0:x0 + N]).sum(2).mean())
print('gravado', fn)
