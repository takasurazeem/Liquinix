# Gera os .ico do Mac usados para trocar os ícones do próprio Windows (tools\icones-sistema.ps1).
# Saída: bin\icones-sistema\*.ico. Pastas/Lixeira do tema Tahoe; HD, CD, Mac e documentos do Sequoia.
# Documentos: a folha branca com o canto dobrado do macOS, o ícone do app no meio e a extensão embaixo.
import os, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TA = os.path.join(ROOT, 'icones', 'tahoe')
SQ = os.path.join(ROOT, 'icones', 'sequoia')
OUT = os.path.join(ROOT, 'bin', 'icones-sistema')
os.makedirs(OUT, exist_ok=True)
SIZES = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (96, 96), (128, 128), (256, 256)]


def load(path):
    im = Image.open(path).convert('RGBA')
    bb = im.getbbox()  # tira a borda vazia; recentra num quadrado
    if bb: im = im.crop(bb)
    s = max(im.size)
    sq = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    sq.paste(im, ((s - im.width) // 2, (s - im.height) // 2))
    return sq


def pad(im, frac):  # deixa uma margem (o Mac não encosta o desenho na borda)
    s = im.width
    n = int(round(s / (1 - 2 * frac)))
    c = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    c.paste(im, ((n - s) // 2, (n - s) // 2))
    return c.resize((512, 512), Image.LANCZOS)


def save(im, name):
    im.save(os.path.join(OUT, name + '.ico'), sizes=SIZES)
    print('ok', name)


def font(sz):
    for f in ('C:/Windows/Fonts/seguisb.ttf', 'C:/Windows/Fonts/segoeuib.ttf', 'C:/Windows/Fonts/arial.ttf'):
        if os.path.exists(f): return ImageFont.truetype(f, sz)
    return ImageFont.load_default()


def document(badge, label):
    S = 1024  # desenha em 1024 e reduz (bordas suaves)
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    L, T, R, B = 200, 70, 824, 954  # folha em pé (proporção do macOS)
    fold = 170
    page = [(L, T), (R - fold, T), (R, T + fold), (R, B), (L, B)]
    sh = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).polygon([(x, y + 10) for x, y in page], fill=(0, 0, 0, 70))
    im = Image.alpha_composite(im, sh.filter(ImageFilter.GaussianBlur(14)))
    d = ImageDraw.Draw(im)
    d.polygon(page, fill=(252, 252, 252, 255), outline=(196, 196, 200, 255), width=6)
    d.polygon([(R - fold, T), (R - fold, T + fold), (R, T + fold)], fill=(226, 226, 230, 255), outline=(196, 196, 200, 255), width=6)
    if badge:
        b = load(badge).resize((400, 400), Image.LANCZOS)
        im.alpha_composite(b, ((L + R) // 2 - 200, 330))
    if label:
        f = font(118)
        w = d.textlength(label, font=f)
        d.text(((L + R - w) / 2, 770), label, font=f, fill=(128, 128, 134, 255))
    return im.resize((512, 512), Image.LANCZOS)


def plain(path, frac=0.04):
    return pad(load(path), frac)


# pastas, lixeira, Mac, discos
save(plain(os.path.join(TA, 'folder default.png')), 'pasta')
for k in ['desktop', 'docs', 'downloads', 'pictures', 'music', 'videos', 'home', 'cloud']:
    save(plain(os.path.join(TA, f'folder {k}.png')), 'pasta-' + k)
save(plain(os.path.join(TA, 'trashempty@2x.png')), 'lixeira-vazia')
save(plain(os.path.join(TA, 'trashfull@2x.png')), 'lixeira-cheia')
save(plain(os.path.join(SQ, 'macstudio.png'), 0.06), 'computador')
save(plain(os.path.join(SQ, 'HD.png'), 0.06), 'disco')
save(plain(os.path.join(SQ, 'CD.png'), 0.06), 'cd')
save(plain(os.path.join(SQ, 'zip.png'), 0.02), 'doc-zip')

# documentos (extensão -> app no meio)
te, pv = os.path.join(TA, 'TextEdit.png'), os.path.join(TA, 'Preview.png')
mu, qt = os.path.join(TA, 'Music.png'), os.path.join(SQ, 'Quicktime.png')
code, web = os.path.join(TA, 'Visual Studio Code.png'), os.path.join(TA, 'Safari.png')
DOCS = {
    'txt': (te, 'TXT'), 'log': (te, 'LOG'), 'ini': (te, 'INI'), 'md': (te, 'MD'), 'rtf': (te, 'RTF'),
    'pdf': (pv, 'PDF'), 'png': (pv, 'PNG'), 'jpg': (pv, 'JPEG'), 'gif': (pv, 'GIF'), 'bmp': (pv, 'BMP'),
    'webp': (pv, 'WEBP'), 'heic': (pv, 'HEIC'), 'ico': (pv, 'ICO'), 'svg': (pv, 'SVG'),
    'mp3': (mu, 'MP3'), 'wav': (mu, 'WAV'), 'flac': (mu, 'FLAC'), 'm4a': (mu, 'M4A'), 'ogg': (mu, 'OGG'), 'aif': (mu, 'AIFF'),
    'mp4': (qt, 'MP4'), 'mkv': (qt, 'MKV'), 'mov': (qt, 'MOV'), 'avi': (qt, 'AVI'), 'webm': (qt, 'WEBM'),
    'html': (web, 'HTML'),
    'json': (code, 'JSON'), 'js': (code, 'JS'), 'py': (code, 'PY'), 'cpp': (code, 'CPP'), 'h': (code, 'H'),
    'css': (code, 'CSS'), 'xml': (code, 'XML'), 'ps1': (code, 'PS1'), 'bat': (code, 'BAT'), 'cmd': (code, 'CMD'),
    'docx': (os.path.join(SQ, 'word.png'), 'DOCX'), 'xlsx': (os.path.join(SQ, 'excel.png'), 'XLSX'),
    'pptx': (os.path.join(SQ, 'powerpoint.png'), 'PPTX'), 'csv': (os.path.join(SQ, 'excel.png'), 'CSV'),
    '_': (None, None),  # documento genérico (tipo sem ícone)
    # grupos: vários tipos com o mesmo ProgID (ex.: todas as imagens nas Fotos) ficam sem a extensão escrita
    'imagem': (pv, None), 'audio': (mu, None), 'video': (qt, None), 'midia': (qt, None),
    'texto': (te, None), 'codigo': (code, None), 'web': (web, None),
}
for ext, (badge, label) in DOCS.items():
    save(document(badge, label), 'doc-' + ('generico' if ext == '_' else ext))
