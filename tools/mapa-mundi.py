# Gera src/relogio/mapa_mundi.inc: a máscara dos continentes do Relógio Mundial, a partir dos polígonos de terra
# do Natural Earth (domínio público, https://www.naturalearthdata.com/).
#
# Generates the World Clock continent mask from Natural Earth's 1:50m land polygons (public domain). Only the
# Python standard library is needed.
#
#   1. Download ne_50m_land.geojson from Natural Earth, version 5.1.2:
#      https://raw.githubusercontent.com/nvkelso/natural-earth-vector/v5.1.2/geojson/ne_50m_land.geojson
#      (sha256 e874b27a51d146452be360cafb3cc50c86001074a67d534113e6534682f9826b)
#   2. python -I tools/mapa-mundi.py <ne_50m_land.geojson> [preview.pgm]
#
# Equirectangular projection: width = 360° (from -180° on the left), height = from the north pole (90°) down to
# MAPA_LAT_BAIXO (~-85.96°, Antarctica cut at the bottom). src/relogio/main.cpp maps latitude linearly between
# those two, so MAPA_W, MAPA_H and MAPA_LAT_BAIXO must stay in step with it.
import json
import os
import sys

W, H = 768, 375                     # pixels; 96 bytes per row
LAT_BAIXO = 90 - 180 * (1001 / 1024)  # latitude of the bottom edge of the last row (-85.957)
SS = 4                              # SS x SS samples per pixel; a pixel is land when at least half of them are

if len(sys.argv) < 2:
    sys.exit('uso: python -I tools/mapa-mundi.py <ne_50m_land.geojson> [preview.pgm]')

with open(sys.argv[1], encoding='utf-8') as f:
    data = json.load(f)

# Every ring (outer rings and holes) as a list of points in sample units. Land polygons don't overlap, so the
# even-odd rule over all rings fills land and leaves lakes such as the Caspian Sea empty.
SW, SH = W * SS, H * SS
def sx(lon): return (lon + 180) / 360 * SW
def sy(lat): return (90 - lat) / (90 - LAT_BAIXO) * SH

edges_by_row = [[] for _ in range(SH)]
for feat in data['features']:
    g = feat['geometry']
    polys = [g['coordinates']] if g['type'] == 'Polygon' else g['coordinates'] if g['type'] == 'MultiPolygon' else []
    for poly in polys:
        for ring in poly:
            pts = [(sx(p[0]), sy(p[1])) for p in ring]
            for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
                if y0 == y1:
                    continue
                if y0 > y1:
                    x0, y0, x1, y1 = x1, y1, x0, y0
                # sample rows r whose centre r + 0.5 lies in [y0, y1)
                r0 = max(0, int(-(-(y0 - 0.5) // 1)))
                r1 = min(SH - 1, int(-(-(y1 - 0.5) // 1)) - 1)
                if r0 > r1:
                    continue
                k = (x1 - x0) / (y1 - y0)
                for r in range(r0, r1 + 1):
                    edges_by_row[r].append(x0 + (r + 0.5 - y0) * k)

# Count land samples per pixel.
count = [[0] * W for _ in range(H)]
for r in range(SH):
    xs = sorted(edges_by_row[r])
    row = count[r // SS]
    for a, b in zip(xs[0::2], xs[1::2]):
        # sample columns c whose centre c + 0.5 lies in [a, b)
        c0 = max(0, int(-(-(a - 0.5) // 1)))
        c1 = min(SW - 1, int(-(-(b - 0.5) // 1)) - 1)
        for c in range(c0, c1 + 1):
            row[c // SS] += 1

half = SS * SS // 2
land = [[1 if v >= half else 0 for v in row] for row in count]

bits = bytearray()
for row in land:
    for i in range(0, W, 8):
        v = 0
        for b in range(8):
            v = (v << 1) | row[i + b]
        bits.append(v)

fn = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'relogio', 'mapa_mundi.inc')
with open(fn, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// gerado por tools/mapa-mundi.py a partir do Natural Earth 1:50m land v5.1.2 (domínio público)\n')
    f.write('// continentes (1 = terra), %d x %d, 1 bit por ponto, da esquerda (bit 7)\n' % (W, H))
    f.write('static const int MAPA_W = %d, MAPA_H = %d;\n' % (W, H))
    f.write('static const double MAPA_LAT_BAIXO = %.3f;  // latitude da última linha\n' % LAT_BAIXO)
    f.write('static const unsigned char MAPA_BITS[%d] = {\n' % len(bits))
    for i in range(0, len(bits), 32):
        f.write('    ' + ','.join('0x%02x' % v for v in bits[i:i + 32]) + ',\n')
    f.write('};\n')

if len(sys.argv) > 2:  # optional preview as a plain PGM image (white = land)
    with open(sys.argv[2], 'wb') as f:
        f.write(b'P5 %d %d 255\n' % (W, H))
        f.write(bytes(255 * v for row in land for v in row))

print('gravado', fn, '- terra: %.1f%%' % (100 * sum(map(sum, land)) / (W * H)))
