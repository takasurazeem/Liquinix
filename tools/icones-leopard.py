# Ícones do tema Mac OS X Leopard (icones\leopard) a partir do pacote "Leopard Icons" (650 ícones no padrão do
# Linux/GNOME, 128 px; itransform.freehostia.com), que o usuário baixou em Downloads\leopard-huge-iconpack-650.
# Uso pessoal: a pasta icones\leopard fica fora do git (.gitignore), como as dos outros pacotes.
# O pacote usa nomes do freedesktop (nautilus.png, user-trash.png...); aqui cada um ganha o nome que o WacOS
# procura (o nome do app do Mac, como em icones\goldengate, ou o .exe do programa do Windows). O que faltar segue
# o pacote do Sequoia (apps.cpp / ui.cpp).
#   python tools\icones-leopard.py [pasta do pacote] [destino]
import os, sys
from PIL import Image  # pip install pillow

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expandvars(r'%USERPROFILE%\Downloads\leopard-huge-iconpack-650\Leopard Icons')
DST = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'icones', 'leopard')

MAPA = {
    # apps do sistema (nomes do Mac, os mesmos de icones\goldengate)
    'Finder': 'apps/nautilus.png',
    'Safari': 'apps/web-browser.png',
    'Settings': 'apps/gnome-control-center.png',          # Preferências do Sistema do 10.5
    'System Preferences': 'apps/gnome-control-center.png',
    'Activity Monitor': 'apps/utilities-system-monitor.png',
    'Terminal': 'apps/gnome-terminal.png',
    'Calculator': 'apps/calc.png',
    'Notes': 'apps/tomboy.png',                            # o Leopard tinha o Stickies (Notas só no 10.8)
    'Stickies': 'apps/tomboy.png',
    'TextEdit': 'apps/text-editor.png',
    'Clock': 'apps/clock.png',
    'Preview': 'apps/eog.png',
    'Contacts': 'apps/addressbook.png',
    'Address Book': 'apps/addressbook.png',
    'Time Machine': 'apps/timemachine.png',
    'Front Row': 'apps/frontrow.png',
    'Dictionary': 'apps/gnome-dictionary.png',
    'Spotlight': 'apps/system-search.png',
    'Launchpad': 'Folder/folder2.png',                     # o Leopard não tinha Launchpad: a pasta Aplicativos
    'Force Quit': 'apps/gnome-panel-force-quit.png',
    'Screenshot': 'apps/applets-screenshooter.png',        # o Grab
    'QuickTime Player': 'More/QuickTimePlayer.png',
    'Photos': 'apps/f-spot.png',                           # o iPhoto
    'Music': 'More/iTunes.png',
    'iTunes': 'More/iTunes.png',
    'Calendar': 'More/iCal.png',
    'iCal': 'More/iCal.png',
    'Mail': 'apps/evolution.png',
    'Messages': 'apps/gaim.png',                           # o iChat
    'Automator': 'More/Automator.png',
    'GarageBand': 'More/Garage Band.png',
    'Keynote': 'More/Keynote.png',
    'Tips': 'apps/gnome-help.png',
    'Help Viewer': 'apps/gnome-help.png',
    'System Information': 'places/gnome-fs-client.png',
    'Boot Camp Assistant': 'apps/bootcamp.png',
    'Dashcode': 'apps/dashcode.png',
    'Mission Control': 'apps/multidesktop.png',            # no 10.5, o Spaces (os quatro quadros)
    'Spaces': 'apps/multidesktop.png',
    'Reminders': 'apps/evolution-tasks.png',               # o 10.5 não tinha Lembretes: a prancheta de tarefas
    'Keychain Access': 'apps/stock_keyring.png',
    'Console': 'apps/logviewer.png',
    'Disk Utility': 'More/hdd.png',
    'Script Editor': 'More/AppleScript.png',
    'DVD Player': 'apps/DvD AppIcon.png',
    'Print Center': 'apps/printer.png',
    'Software Update': 'apps/software-properties.png',
    'Users & Groups': 'apps/system-users.png',
    'Wallpaper': 'apps/preferences-desktop-wallpaper.png',
    'Screen Saver': 'apps/screensaver.png',
    'World Clock': 'apps/gworldclock.png',
    'Sherlock': 'More/Sherlock.png',
    'X11': 'More/X11.png',
    # programas do Windows pelo .exe (apps.cpp compara o nome e o .exe, normalizados)
    'firefox': 'apps/firefox.png', 'vlc': 'apps/vlc.png', 'skype': 'apps/skype.png', 'opera': 'apps/opera.png',
    'blender': 'apps/blender.png', 'audacity': 'apps/audacity.png', 'python': 'apps/python.png', 'java': 'apps/java.png',
    'inkscape': 'apps/inkscape.png', 'eclipse': 'apps/eclipse.png', 'virtualbox': 'apps/virtualbox.png',
    'vmware': 'apps/vmware.png', '7zip': 'apps/7zip.png', 'photoshop': 'More/Photoshop CS2.png',
    'illustrator': 'More/Illustrator CS2.png', 'acrobat': 'More/Adobe Acrobat.png', 'googleearth': 'More/Google Earth.png',
    'wmplayer': 'More/WMP.png', 'mspaint': 'apps/gnome-graphics.png',
    # painéis das Preferências do Sistema do 10.5 (Ajustes no tema Leopard: "pref <id da página>")
    'pref aparencia': 'apps/gnome-settings-theme.png', 'pref fundo': 'apps/preferences-desktop-wallpaper.png',
    'pref protetor': 'apps/screensaver.png', 'pref dock': 'apps/avant-window-navigator.png',
    'pref central': 'apps/multidesktop.png', 'pref spotlight': 'apps/system-search.png', 'pref geral': 'apps/gnome-settings.png',
    'pref telas': 'apps/display-capplet.png', 'pref som': 'apps/preferences-desktop-sound.png',
    'pref energia': 'apps/gnome-power-manager.png', 'pref teclado': 'devices/input-keyboard.png', 'pref mouse': 'devices/input-mouse.png',
    'pref trackpad': 'devices/gvm-dev-tablet.png', 'pref impressoras': 'apps/printer.png', 'pref bluetooth': 'apps/bluetooth.png',
    'pref rede': 'places/gnome-fs-network.png', 'pref wifi': 'devices/network-wireless.png', 'pref contas': 'apps/evolution.png',
    'pref nuvem': 'apps/gnome-globe.png', 'pref jogos': 'devices/input-gaming.png', 'pref usuarios': 'apps/system-users.png',
    'pref privacidade': 'apps/system-config-securitylevel.png', 'pref senha': 'apps/stock_keyring.png',
    'pref bloqueio': 'apps/gnome-lockscreen.png', 'pref acessibilidade': 'apps/preferences-desktop-accessibility.png',
    'pref inteligencia': 'apps/orca.png', 'pref notificacoes': 'More/Alert Note.png', 'pref foco': 'apps/gnome-session-hibernate.png',
    'pref tempo': 'apps/users.png', 'pref data': 'apps/config-date.png', 'pref atualizacao': 'apps/software-properties.png',
    # Finder: ícones da barra lateral do 10.5 ("side <item>") e dos documentos por tipo ("doc <tipo>")
    'side recentes': 'places/document-open-recent.png', 'side aplicativos': 'Folder/folder2.png',
    'side mesa': 'places/desktop.png', 'side documentos': 'places/gnome-fs-regular.png',
    'side videos': 'mimetypes/video-x-generic.png', 'side musica': 'mimetypes/audio-x-generic.png',
    'side imagens': 'apps/Pictures.png', 'side downloads': 'apps/gwget.png', 'side casa': 'places/user-home.png',
    'side computador': 'places/gnome-fs-client.png', 'side disco': 'places/gnome-dev-harddisk.png',
    'side removivel': 'devices/drive-removable-media.png', 'side rede': 'places/gnome-fs-network.png',
    'side pasta': 'Folder/gnome-fs-directory.png', 'side inteligente': 'Folder/folder17.png',
    'doc texto': 'mimetypes/text-x-generic.png', 'doc pdf': 'mimetypes/application-pdf.png',
    'doc word': 'mimetypes/application-msword.png', 'doc rich': 'mimetypes/text-richtext.png',
    'doc planilha': 'mimetypes/application-vnd.ms-excel.png',
    'doc apresentacao': 'mimetypes/application-vnd.oasis.opendocument.presentation.png',
    'doc zip': 'mimetypes/application-zip.png', 'doc audio': 'mimetypes/audio-x-generic.png',
    'doc exe': 'mimetypes/application-x-executable.png', 'doc html': 'mimetypes/text-html.png',
    'doc script': 'mimetypes/text-x-script.png', 'doc python': 'mimetypes/text-x-python.png',
    'doc c': 'mimetypes/text-x-c.png', 'doc cpp': 'mimetypes/text-x-c++.png', 'doc java': 'mimetypes/text-x-java.png',
    'doc disco': 'mimetypes/application-x-cd-image.png', 'doc fonte': 'mimetypes/font-x-generic.png',
    'doc log': 'mimetypes/text-x-log.png', 'doc xml': 'mimetypes/application-xml.png',
    'doc generico': 'places/gnome-fs-regular.png',
    # pastas e Lixeira (nomes do pacote do Sequoia que o Dock pede)
    'folder downloads': 'Folder/folder7.png',
    'folder default': 'Folder/gnome-fs-directory.png',
    'folder applications': 'Folder/folder2.png',
    'folder documents': 'Folder/folder5.png',
    'folder docs': 'Folder/folder5.png',                   # nomes que o Finder pede (os mesmos do Tahoe)
    'folder videos': 'Folder/folder10.png',
    'folder desktop': 'Folder/gnome-fs-directory.png',
    'folder cloud': 'Folder/gnome-fs-directory.png',
    'folder music': 'Folder/folder11.png',
    'folder movies': 'Folder/folder10.png',
    'folder pictures': 'Folder/folder12.png',
    'folder public': 'Folder/folder8.png',
    'folder sites': 'Folder/folder6.png',
    'folder library': 'Folder/folder9.png',
    'folder utilities': 'Folder/folder4.png',
    'folder home': 'places/user-home.png',
    'Alert Note': 'More/Alert Note.png',                 # o ícone dos alertas do 10.5 (alertas.cpp)
    'trashempty@2x': 'places/user-trash.png',
    'trashfull@2x': 'places/user-trash-full.png',
}

os.makedirs(DST, exist_ok=True)
ok = falta = 0
for nome, rel in MAPA.items():
    src = os.path.join(SRC, *rel.split('/'))
    if not os.path.isfile(src):
        print('falta no pacote:', rel); falta += 1; continue
    # os PNGs do pacote do Sequoia trazem a margem do Mac (o desenho ocupa ~80 % do quadro) e o Dock os desenha
    # assim; os do Leopard vêm sem margem: centraliza num quadro 25 % maior para ficarem do mesmo tamanho
    im = Image.open(src).convert('RGBA')
    # antes, corta a margem transparente que cada PNG do pacote traz (diferente em cada um): sem isso o Finder e as
    # Preferências saíam menores que os outros ícones na Dock
    caixa = im.getchannel('A').point(lambda v: 255 if v > 24 else 0).getbbox()
    if caixa: im = im.crop(caixa)
    lado = round(max(im.size) * 1.25)
    q = Image.new('RGBA', (lado, lado), (0, 0, 0, 0))
    q.paste(im, ((lado - im.width) // 2, (lado - im.height) // 2))
    q.save(os.path.join(DST, nome + '.png')); ok += 1
# Os apps que não existiam em 2007 (ou que o pacote não tem): o ícone moderno do projeto (icones\extra, depois
# icones\sequoia) "aquificado" (tools/aqua_icone.py: brilho de gel, fio de luz e a sombra projetada do Leopard).
# Pastas e Lixeira ficam só as do 10.5 (MAPA).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aqua_icone import aquifica
def norm(n): return ''.join(c for c in n.lower() if c.isalnum())
feitos = {norm(n) for n in MAPA}
raiz = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'icones')
aq = 0
for sub in ('extra', 'sequoia'):
    pasta = os.path.join(raiz, sub)
    if not os.path.isdir(pasta): continue
    for arq in sorted(os.listdir(pasta)):
        nome, ext = os.path.splitext(arq)
        if ext.lower() != '.png' or nome.lower().startswith(('folder', 'trash')) or norm(nome) in feitos: continue
        feitos.add(norm(nome))
        try: aquifica(os.path.join(pasta, arq), os.path.join(DST, arq)); aq += 1
        except Exception as e: print('não deu para aquificar', arq, e)
print(f'{aq} ícones modernos no estilo Aqua')
# os desenhados à mão para o Leopard (Chrome de 2008, Claude com volume) por cima dos aquificados
import subprocess
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icones-leopard-desenhados.py'), DST], check=False)
with open(os.path.join(DST, 'CREDITOS.txt'), 'w', encoding='utf-8') as f:
    f.write('Pacote "Leopard Icons" (650 ícones, itransform.freehostia.com), baixado pelo usuário. Uso pessoal; não distribuir.\n'
            'Gerado por tools\\icones-leopard.py.\n')
print(f'{ok} ícones em {os.path.abspath(DST)} ({falta} faltando)')
