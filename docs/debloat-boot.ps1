# Debloat do boot (08/11/2026), escolhido pelo usuário. Precisa rodar como administrador.
# Serviços vão para Manual (não Desativado: abrem quando o app pedir), tarefas de login ficam desativadas.
# Antes de mudar, grava o estado original em docs\debloat-boot-desfazer.ps1 (rode esse arquivo como
# administrador para voltar tudo como estava).
$ErrorActionPreference = 'Continue'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$undoFile = Join-Path $dir 'debloat-boot-desfazer.ps1'
$log = Join-Path $dir 'debloat-boot.log'
$undo = @('# Desfaz o debloat-boot.ps1 (rode como administrador)')
"=== $(Get-Date) ===" | Out-File $log -Encoding utf8

$services = 'ROG Live Service', 'GameSDK Service',            # ASUS ROG Live / GameSDK
            'MBAMService',                                     # Malwarebytes
            'Okta Auto Update Service', 'OktaAuthenticatorService', 'OktaFeatureFlagService',
            'tifSer', 'XTU3SERVICE',                           # Rise Mode Temp, Intel XTU
            'ExitLagPmService', 'ProtonVPN Service',
            'NetTimeSvc', 'Bonjour Service',                   # atualizador de hora NetTime, Bonjour
            'XboxService', 'WSLService'
foreach ($n in $services) {
    $s = Get-CimInstance Win32_Service -Filter "Name='$n'"
    if (-not $s) { "${n}: não existe" | Out-File $log -Append; continue }
    $orig = switch ($s.StartMode) { 'Auto' { 'Automatic' } 'Manual' { 'Manual' } 'Disabled' { 'Disabled' } default { $s.StartMode } }
    $delayed = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$n" -Name DelayedAutostart -ErrorAction SilentlyContinue).DelayedAutostart
    try {
        Set-Service -Name $n -StartupType Manual -ErrorAction Stop
        $undo += "Set-Service -Name '$n' -StartupType $orig"
        if ($delayed -eq 1) { $undo += "Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\$n' -Name DelayedAutostart -Value 1" }
        "${n}: $orig -> Manual" | Out-File $log -Append
    } catch { "${n}: NÃO mudou ($($_.Exception.Message))" | Out-File $log -Append }
}

$tasks = '\MSIAfterburner', '\Okta Verify Activation Task', '\Tif Task'
foreach ($t in $tasks) {
    $path = $t.Substring(0, $t.LastIndexOf('\') + 1); $name = $t.Substring($t.LastIndexOf('\') + 1)
    $task = Get-ScheduledTask -TaskPath $path -TaskName $name -ErrorAction SilentlyContinue
    if (-not $task) { "${t}: não existe" | Out-File $log -Append; continue }
    if ($task.State -ne 'Disabled') {
        try { Disable-ScheduledTask -TaskPath $path -TaskName $name -ErrorAction Stop | Out-Null
              $undo += "Enable-ScheduledTask -TaskPath '$path' -TaskName '$name'"
              "${t}: desativada" | Out-File $log -Append }
        catch { "${t}: NÃO mudou ($($_.Exception.Message))" | Out-File $log -Append }
    }
}

# NetTime na inicialização (Run de 32 bits da máquina)
$k = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run32'
$old = (Get-ItemProperty $k -Name 'NetTime' -ErrorAction SilentlyContinue).NetTime
if ($old -and $old[0] -eq 2) {
    $undo += "Set-ItemProperty '$k' -Name 'NetTime' -Value ([byte[]](" + (($old | ForEach-Object { '0x' + $_.ToString('X2') }) -join ',') + "))"
    Set-ItemProperty $k -Name 'NetTime' -Value ([byte[]](@(3, 0, 0, 0) + [BitConverter]::GetBytes((Get-Date).ToFileTime())))
    'NetTime (Run32): desativado' | Out-File $log -Append
}

$undo | Out-File $undoFile -Encoding utf8
'pronto' | Out-File $log -Append
