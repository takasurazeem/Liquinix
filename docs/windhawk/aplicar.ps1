# Aplica tema-finder.reg (ou outro .reg) e faz o Windhawk recarregar os mods do Explorador:
# desliga-os por alguns segundos e liga de novo. Rodar como administrador.
param([string]$reg = "$PSScriptRoot\tema-finder.reg")
$base = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods'
foreach ($m in 'windows-11-file-explorer-styler', 'explorer-font-changer') { Set-ItemProperty "$base\$m" -Name Disabled -Value 1 }
Start-Sleep 3
reg.exe import $reg
