# Cursor do tema Mac OS X Leopard (cursor\Leopard) a partir do pacote "Mac OS X Cursors" (RaZcaLinSIDe), que o
# usuário baixou em Downloads\mac_os_x_cursor_pack_by_razcalinside_dgcsg6. Uso pessoal: fora do git (.gitignore).
# Só os 15 arquivos que o Shell aplica (ponteiro.cpp, ponteiro::TemaLeopard): a seta, a mãozinha, o I-beam etc. e o
# cata-vento colorido de espera do Leopard ("RainbowSwirl"). O .inf do pacote copiaria para C:\Windows\Cursors (pasta
# do sistema); aqui nada vai para o Windows: o Shell grava o esquema só em HKCU e devolve o anterior.
#   python tools\cursor-leopard.py [pasta do pacote] [destino]
import os, shutil, sys

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expandvars(r'%USERPROFILE%\Downloads\mac_os_x_cursor_pack_by_razcalinside_dgcsg6')
DST = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'cursor', 'Leopard')
ARQUIVOS = ['Normal Select.cur', 'Help Select.cur', 'RainbowSwirl-Background.ani', 'RainbowSwirl-Busy.ani',
            'Precision Select.cur', 'Text Select.cur', 'Handwriting.cur', 'Unavailable.cur', 'Vertical Resize.cur',
            'Horizontal Resize.cur', 'Diagonal Resize 1.cur', 'Diagonal Resize 2.cur', 'Move.cur', 'Alternate Select.cur',
            'Link Select.cur']
os.makedirs(DST, exist_ok=True)
falta = [a for a in ARQUIVOS if not os.path.isfile(os.path.join(SRC, a))]
for a in ARQUIVOS:
    if a not in falta: shutil.copyfile(os.path.join(SRC, a), os.path.join(DST, a))
for f in ('Readme.txt',):
    if os.path.isfile(os.path.join(SRC, f)): shutil.copyfile(os.path.join(SRC, f), os.path.join(DST, f))
print(f'{len(ARQUIVOS) - len(falta)} cursores em {os.path.abspath(DST)}' + (f' (faltando: {falta})' if falta else ''))
