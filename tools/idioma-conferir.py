# Confere os dicionários src/comum/idioma_<código>.inc contra idioma_en.inc (a lista de chaves de referência):
# chaves que faltam (saem em inglês até alguém traduzir) e traduções cujo %d/%ls não bate com o inglês.
#   python tools/idioma-conferir.py            -> resumo por idioma
#   python tools/idioma-conferir.py fr         -> lista as chaves que faltam em francês
import glob, os, re, sys

RAIZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'comum')
PAR = re.compile(r'\{ L"(.*)", L"(.*)" \},?\s*$')
FMT = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(ll|l|z|h|)([a-zA-Z%])')


def ler(caminho):
    d = {}
    for linha in open(caminho, encoding='utf-8'):
        m = PAR.match(linha)
        if m:
            d[m.group(1)] = m.group(2)
    return d


def formato(s):
    return [m.groups() for m in FMT.finditer(s) if m.group(2) != '%']


en = ler(os.path.join(RAIZ, 'idioma_en.inc'))
so = sys.argv[1] if len(sys.argv) > 1 else None
erros = 0
for caminho in sorted(glob.glob(os.path.join(RAIZ, 'idioma_*.inc'))):
    cod = os.path.basename(caminho)[7:-4]
    if cod == 'en' or (so and cod != so):
        continue
    d = ler(caminho)
    falta = [k for k in en if k not in d]
    ruins = [k for k in en if k in d and formato(d[k]) != formato(en[k])]
    erros += len(ruins)
    print(f'{cod}: {len(en) - len(falta)}/{len(en)} traduzidas, {len(falta)} faltando, {len(ruins)} com formato diferente')
    for k in ruins:
        print('   FORMATO', repr(k), '->', repr(d[k]))
    if so:
        for k in falta:
            print('   FALTA', repr(k), '(en:', repr(en[k]) + ')')
sys.exit(1 if erros else 0)
