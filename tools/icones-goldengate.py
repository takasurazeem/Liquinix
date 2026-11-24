# Monta icones\goldengate com os ícones do macOS 27 Golden Gate (repositório zagnut531/macOS-27-Icons,
# exportados do Mac em 1024 px). Vale a versão mais recente de cada um: Beta 1 < 2 < 3 < 4 < 5 < 6 < 27.2.
# Os nomes viram os que o Liquinix procura (ex.: "System Settings" -> "Settings", "Apps" -> "Launchpad").
# Uso pessoal, como os outros pacotes de icones\ (ver icones\LEIA-ME.txt): não vai para o GitHub.
# python -I tools\icones-goldengate.py <pasta do repositório clonado>
import os, re, sys
from PIL import Image
repo = sys.argv[1]
out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'icones', 'goldengate')
os.makedirs(out, exist_ok=True)
ordem = ['Apple apps', 'Beta 1', 'Beta 2', 'Beta 3', 'Beta 4', 'Beta 5', 'Beta 6', 'Beta 7 (Xcode)', '27.2']
nomes = {'System Settings': 'Settings', 'Apps': 'Launchpad'}
final = {}
for pasta in ordem:
    base = os.path.join(repo, pasta)
    if not os.path.isdir(base): continue
    for raiz, _, arqs in os.walk(base):
        for a in arqs:
            if not a.lower().endswith('.png'): continue
            n = re.sub(r'\s*\((beta|Beta)[^)]*\)', '', a[:-4]).strip()
            final[nomes.get(n, n)] = os.path.join(raiz, a)
for n, p in sorted(final.items()):
    im = Image.open(p).convert('RGBA')
    if im.width > 512: im = im.resize((512, 512), Image.LANCZOS)
    im.save(os.path.join(out, n + '.png'), optimize=True)
with open(os.path.join(out, 'CREDITOS.txt'), 'w', encoding='utf-8') as f:
    f.write('Icones do macOS 27 Golden Gate exportados por zagnut531 (github.com/zagnut531/macOS-27-Icons).\n'
            'Copyright Apple. Uso pessoal: NAO distribuir junto com o projeto.\n')
print(len(final), 'ícones em', out)
