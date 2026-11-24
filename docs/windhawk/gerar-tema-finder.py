# Gera tema-finder.reg: o visual do Arquivos (Finder do Sequoia) aplicado ao Explorador via
# Windhawk (mods "Windows 11 File Explorer Styler" e "Explorer Font Changer").
# Desfazer: importar os backup-*.reg desta pasta (como administrador).
import io, os

MOD = r'HKEY_LOCAL_MACHINE\SOFTWARE\Windhawk\Engine\Mods'

BAR = '#FF303232'      # barra de ferramentas do Finder (escuro), igual ao Arquivos
FIELD = '#14FFFFFF'    # campo de busca/endereço
HOVER = '#14FFFFFF'
FONT = 'SF Pro Text'

styles = [
    # uma barra só: sem abas, sem bordas, cor do Finder
    # abas ficam (o Finder também tem), na cor da barra; é nessa fileira que estão os botões da janela
    ('Grid#TabContainerGrid > Border#LeftBottomBorderLine', ['Visibility=Collapsed']),
    ('Grid#TabContainerGrid > Border#RightBottomBorderLine', ['Visibility=Collapsed']),
    ('Grid#NavigationBarControlGrid', [f'Background={BAR}', 'Height=44', 'Padding=8,0,8,0']),
    ('FileExplorerExtensions.CommandBarControl_Wave1 > Grid, Grid#CommandBarControlRootGrid',
        [f'Background={BAR}', 'BorderThickness=0,0,0,1', 'BorderBrush=#99000000']),
    ('Microsoft.UI.Xaml.Controls.Border#BottomBorderLine', ['Visibility=Collapsed']),
    # navegação: só voltar e avançar (como no Finder)
    ('Microsoft.UI.Xaml.Controls.AppBarButton#upButton', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton#refreshButton', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton#backButton', ['Width=32', 'CornerRadius=6']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton#forwardButton', ['Width=32', 'CornerRadius=6']),
    # endereço discreto (sem caixa) e busca arredondada, como o campo do Finder
    ('Grid#FileExplorerAddressBarGrid > Grid#LayoutRoot > TextBox > Grid@CommonStates > Border#BorderElement',
        ['Background=Transparent', 'BorderThickness=0']),
    ('AutoSuggestBox#FileExplorerSearchBox > Grid#LayoutRoot > TextBox > Grid@CommonStates > Border#BorderElement',
        [f'Background={FIELD}', 'BorderThickness=0', 'CornerRadius=7']),
    ('AutoSuggestBox#FileExplorerSearchBox', ['MaxWidth=220', 'Height=28']),
    # barra de comandos enxuta: o Finder não tem recortar/copiar/colar/renomear na barra
    ('CommandBar#FileExplorerCommandBar', ['Background=Transparent']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Cut]', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Copy]', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Paste]', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Rename]', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Recortar]', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Copiar]', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Colar]', ['Visibility=Collapsed']),
    ('Microsoft.UI.Xaml.Controls.AppBarButton[ToolTipService.ToolTip=Renomear]', ['Visibility=Collapsed']),
    # fonte do Mac em toda a parte XAML (barra de cima)
    # (Segoe Fluent Icons como reserva: os ícones das barras são letras dessa fonte)
    ('Microsoft.UI.Xaml.Controls.TextBlock', [f'FontFamily={FONT}, Segoe Fluent Icons']),
    ('Microsoft.UI.Xaml.Controls.TextBox', [f'FontFamily={FONT}, Segoe Fluent Icons']),
]

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

out = ['Windows Registry Editor Version 5.00', '']
# estilizador oficial ligado; o fork local desligado (só pode haver um consumidor de XAML)
out += [f'[{MOD}\\windows-11-file-explorer-styler]', '"Disabled"=dword:00000000', '']
out += [f'[{MOD}\\local@windows-11-file-explorer-styler-fork]', '"Disabled"=dword:00000001', '']
out += [f'[-{MOD}\\windows-11-file-explorer-styler\\Settings]', '']
out += [f'[{MOD}\\windows-11-file-explorer-styler\\Settings]',
        '"theme"=""', '"backgroundTranslucentEffect"=""', '"backgroundTranslucentEffectRegion"=""',
        '"styleConstants[0]"=""', '"themeResourceVariables[0]"=""',
        '"explorerFrameContainerHeight"=dword:00000000', '"xamlDiagnosticsHandling"="alert"']
for i, (t, ss) in enumerate(styles):
    out.append(f'"controlStyles[{i}].target"={q(t)}')
    for j, st in enumerate(ss):
        out.append(f'"controlStyles[{i}].styles[{j}]"={q(st)}')
out.append('')
# fonte da lista e da barra lateral (DirectUI)
out += [f'[{MOD}\\explorer-font-changer]', '"Disabled"=dword:00000000', '']
out += [f'[{MOD}\\explorer-font-changer\\Settings]', f'"font.name"={q(FONT)}', '']

here = os.path.dirname(os.path.abspath(__file__))
io.open(os.path.join(here, 'tema-finder.reg'), 'w', encoding='utf-16', newline='\r\n').write('\n'.join(out) + '\n')
print('ok', len(styles), 'alvos')
