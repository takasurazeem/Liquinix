# Instala/atualiza o mod local "sequoia-lockscreen" do Windhawk: a tela de bloqueio do Windows (LockApp.exe)
# com cara de macOS Sequoia. O campo de senha/PIN (LogonUI.exe) continua o do Windows.
# Rodar como administrador (o Windhawk guarda os mods em HKLM e em C:\ProgramData\Windhawk).
# Desfazer: desligar.ps1 (ou desligar o mod na janela do Windhawk).
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$mods = 'C:\ProgramData\Windhawk\Engine\Mods\64'
# o nome da dll e os estilos vêm do .reg gerado por estilos.py (rodar "python estilos.py" antes, sem admin)
$name = (Select-String -Path "$here\sequoia-lockscreen.reg" -Pattern '"LibraryFileName"="([^"]+)"').Matches[0].Groups[1].Value
Copy-Item "$here\sequoia-lockscreen.dll" "$mods\$name"
Copy-Item "$here\sequoia-lockscreen.wh.cpp" 'C:\ProgramData\Windhawk\ModsSource\local@sequoia-lockscreen.wh.cpp' -Force
reg.exe import "$here\sequoia-lockscreen.reg"
# versões antigas desta dll que ficaram para trás
Get-ChildItem $mods -Filter 'local@sequoia-lockscreen_*.dll' | Where-Object Name -ne $name | Remove-Item -ErrorAction SilentlyContinue
Write-Host "instalado: $name"
