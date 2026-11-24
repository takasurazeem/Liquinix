# Faz o SequoiaShell iniciar sozinho toda vez que você entra no Windows.
# Usa uma tarefa agendada "ao fazer logon" (roda na hora, sem esperar a fila de inicialização do Explorer,
# que travava ou pulava o Shell). Rode com o botão direito > "Executar com o PowerShell".
# Para desfazer: Unregister-ScheduledTask -TaskName SequoiaShell -Confirm:$false
$exe = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'bin\SequoiaShell.exe'
$user = "$env:USERDOMAIN\$env:USERNAME"
$action = New-ScheduledTaskAction -Execute $exe -WorkingDirectory (Split-Path -Parent $exe)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -Priority 4 -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'SequoiaShell' -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
    -Description 'Inicia o SequoiaShell (estilo macOS) ao entrar no Windows. Para remover: Unregister-ScheduledTask -TaskName SequoiaShell' -Force | Out-Null
# a entrada antiga da fila do Explorer não é mais necessária (a tarefa cuida disso)
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'ShellProprio' -ErrorAction SilentlyContinue
Write-Host "Pronto: o SequoiaShell vai iniciar a cada login ($exe)."
Read-Host 'Enter para fechar'
