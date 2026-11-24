# Tira o mod "sequoia-lockscreen": a tela de bloqueio do Windows volta ao normal. Rodar como administrador.
# (A imagem de fundo da tela de bloqueio continua a floresta: troque em Configurações > Personalização >
#  Tela de bloqueio, ou volte para o Windows em Destaque.)
reg.exe delete 'HKLM\SOFTWARE\Windhawk\Engine\Mods\local@sequoia-lockscreen' /f
Remove-Item 'C:\ProgramData\Windhawk\ModsSource\local@sequoia-lockscreen.wh.cpp' -ErrorAction SilentlyContinue
Start-Sleep 2
Get-ChildItem 'C:\ProgramData\Windhawk\Engine\Mods\64' -Filter 'local@sequoia-lockscreen_*.dll' | Remove-Item -ErrorAction SilentlyContinue
Write-Host 'mod sequoia-lockscreen removido'
