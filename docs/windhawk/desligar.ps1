# Desfaz o tema Finder (volta as configurações salvas antes) e encerra o Windhawk.
# Rodar como administrador.
$base = 'HKLM\SOFTWARE\Windhawk\Engine\Mods'
# Settings do estilizador e da fonte: apaga o que gravamos antes de reimportar o backup
reg.exe delete "$base\windows-11-file-explorer-styler\Settings" /f | Out-Null
reg.exe delete "$base\explorer-font-changer\Settings" /f | Out-Null
foreach ($f in Get-ChildItem "$PSScriptRoot\backup-*.reg") { reg.exe import $f.FullName }
& "$env:ProgramFiles\Windhawk\windhawk.exe" -exit -wait
Stop-Service Windhawk -ErrorAction SilentlyContinue
