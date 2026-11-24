# Gera sequoia-lockscreen.reg: o mod local "sequoia-lockscreen" do Windhawk (LockApp.exe) e os estilos do
# Sequoia (medidos no vídeo de referência, ver src/shell/bloqueio.cpp). Nomes dos elementos tirados do
# MainPage.xbf do LockApp (resources.pri): TimeAndDatePanel, TimePanel, TimeText, DateText, WidgetCanvasPanel...
# Uso: python estilos.py <arquivo da dll instalada>   (o instalar.ps1 chama sozinho)
import sys, time

dll = sys.argv[1] if len(sys.argv) > 1 else 'local@sequoia-lockscreen_1.0.dll'
KEY = r'HKEY_LOCAL_MACHINE\SOFTWARE\Windhawk\Engine\Mods\local@sequoia-lockscreen'

FONT = 'SF Pro Display'
# relógio: 133 pt Semibold (algarismos de y=154 a 248 no vídeo); data: 28 pt Semibold, ACIMA do relógio
STYLES = [
    ('Grid#LockRootGrid', []),  # (placeholder: nada)
    ('#TimeAndDatePanel', ['Margin=0,78,0,0', 'HorizontalAlignment=Center']),
    ('TextBlock#TimeText', [f'FontFamily={FONT}', 'FontWeight=SemiBold', 'FontSize=133', 'Foreground=#EBFFFFFF',
                            'LineHeight=150', 'TextLineBounds=Tight']),
    ('TextBlock#DateText', [f'FontFamily={FONT}', 'FontWeight=SemiBold', 'FontSize=28', 'Foreground=#E0FFFFFF',
                            'TextLineBounds=Tight', 'Margin=0,0,0,0',
                            'RenderTransform:=<TranslateTransform Y="-138"/>']),  # sobe para cima do relógio
    ('#TimePanel', ['RenderTransform:=<TranslateTransform Y="42"/>']),        # desce o relógio
    # o que o Sequoia não tem: widgets, clima, "Saiba mais sobre esta imagem", busca, status no canto
    ('#WidgetCanvasPanel', ['Visibility=Collapsed']),
    ('#WidgetCanvasContainer', ['Visibility=Collapsed']),
    ('#LockCreativeOverlay', ['Visibility=Collapsed']),
    ('#LockBingSearchExperience', ['Visibility=Collapsed']),
    ('#StatusProviderPanel', ['Visibility=Collapsed']),
    ('#StatusProviderPanelV2', ['Visibility=Collapsed']),
    # papel de parede nítido, sem o escurecimento do Windows
    ('#DimmingOverlayNoPassword', ['Opacity=0']),
    ('#DimmingOverlayPassword', ['Opacity=0']),
]

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

out = ['Windows Registry Editor Version 5.00', '',
       f'[-{KEY}]', '', f'[{KEY}]',
       f'"LibraryFileName"={q(dll)}', '"Disabled"=dword:00000000', '"Include"="LockApp.exe"', '"Exclude"=""',
       '"Architecture"="x86-64"', '"Version"="1.0"', f'"SettingsChangeTime"=dword:{int(time.time()) & 0xFFFFFFFF:08x}', '',
       f'[{KEY}\\Settings]', '"theme"=""', '"styleConstants[0]"=""', '"themeResourceVariables[0]"=""']
# o tipo de cada elemento não está no .xbf de um jeito fácil de ler: "#Nome" sozinho vira uma regra para cada
# tipo provável (as que não casam não fazem nada)
TIPOS = ['Grid', 'StackPanel', 'Border', 'Canvas', 'ContentControl', 'ContentPresenter', 'Rectangle', 'Image',
         'LockApp.CreativeOverlay', 'LockApp.BingSearch', 'LockApp.PrivateLockStackPanel', 'UserControl']
expanded = []
for target, styles in STYLES:
    if target.startswith('#'):
        expanded += [(t + target, styles) for t in TIPOS]
    else:
        expanded.append((target, styles))
STYLES = expanded

i = 0
for target, styles in STYLES:
    if not styles:
        continue
    out.append(f'"controlStyles[{i}].target"={q(target)}')
    for j, st in enumerate(styles):
        out.append(f'"controlStyles[{i}].styles[{j}]"={q(st)}')
    i += 1
open(__file__.replace('estilos.py', 'sequoia-lockscreen.reg'), 'w', encoding='utf-16').write('\r\n'.join(out) + '\r\n')
print('ok', i, 'alvos')
