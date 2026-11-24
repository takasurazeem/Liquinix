# Gera src/relogio/mapa_mundi.inc: a máscara dos continentes do Relógio Mundial do macOS 27, tirada da captura
# Clock (512 Pixels, 2x). O mapa é equirretangular: largura = 360° (de -180 à esquerda), altura = do polo norte
# (90°) até ~-85,8° (a Antártica cortada embaixo, como no Mac). Texto, pontos das cidades e a curva dia/noite
# da captura saem com abertura/fechamento morfológico.
# python -I tools\mapa-mundi.py <pasta 512pixels>
import sys, os
import numpy as np
from PIL import Image, ImageFilter
pasta = sys.argv[1]
a = np.array(Image.open(os.path.join(pasta, 'Clock.png')).convert('L')).astype(int)
w = a[76:1612, 112:2160][104:1105]          # a faixa do mapa (2x): 2048 x 1001
land = ((w >= 20) & (w < 120)).astype(np.uint8) * 255   # terra: 63 de dia, 25 na parte da noite; oceano 0
im = Image.fromarray(land)
im = im.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3))   # tira linhas finas (curva, meridianos)
im = im.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.MinFilter(7))   # fecha os buracos do texto e da curva
W, H = 768, 375
small = np.array(im.resize((W, H), Image.BOX)) > 127
fn = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'relogio', 'mapa_mundi.inc')
bits = np.packbits(small, axis=1)            # 96 bytes por linha
with open(fn, 'w', encoding='utf-8') as f:
    f.write('// gerado por tools/mapa-mundi.py: continentes (1 = terra), %d x %d, 1 bit por ponto, da esquerda (bit 7)\n' % (W, H))
    f.write('static const int MAPA_W = %d, MAPA_H = %d;\n' % (W, H))
    f.write('static const double MAPA_LAT_BAIXO = %.3f;  // latitude da última linha\n' % (90 - 180 * (1001 / 1024)))
    f.write('static const unsigned char MAPA_BITS[%d] = {\n' % bits.size)
    flat = bits.ravel()
    for i in range(0, flat.size, 32):
        f.write('    ' + ','.join('0x%02x' % v for v in flat[i:i + 32]) + ',\n')
    f.write('};\n')
Image.fromarray((small * 255).astype(np.uint8)).save(os.path.join(os.environ.get('TEMP', '.'), 'mapa-mundi-teste.png'))
print('gravado', fn, small.mean())
