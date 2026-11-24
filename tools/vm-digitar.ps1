# Digita um texto no teclado virtual da VM (Hyper-V Msvm_Keyboard) e, com -Enter, aperta Enter. Só afeta a VM.
# Uso: tools\vm-digitar.ps1 -Texto "comando" [-Enter] [-Nome AuthUX-Teste]   (pede administrador)
param([switch]$Admin, [string]$Texto = '', [switch]$Enter, [string]$Nome = 'AuthUX-Teste', [string]$Teclas = '', [string]$Combo = '', [string]$Clique = '')
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'docs\ultima-execucao-vm.txt'
if (-not $Admin) {
    Remove-Item $out -ErrorAction SilentlyContinue
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Texto))
    $a = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin -Nome `"$Nome`" -Texto `"b64:$b64`""
    if ($Enter) { $a += ' -Enter' }
    if ($Teclas) { $a += " -Teclas $Teclas" }
    if ($Combo) { $a += " -Combo $Combo" }
    if ($Clique) { $a += " -Clique $Clique" }
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList $a
    exit $p.ExitCode
}
$log = @()
function Nums($s) { if ($s) { @($s -split '[,;]+' | Where-Object { $_ } | ForEach-Object { [int]$_ }) } else { @() } }
try {
    $kTeclas = Nums $Teclas; $kCombo = Nums $Combo; $kClique = Nums $Clique
    if ($Texto.StartsWith('b64:')) { $Texto = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($Texto.Substring(4))) }
    $ns = 'root\virtualization\v2'
    $vm = Get-CimInstance -Namespace $ns -ClassName Msvm_ComputerSystem -Filter "ElementName='$Nome'"
    $kb = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_Keyboard
    if ($Texto -eq 'CTRLALTDEL') { Invoke-CimMethod -InputObject $kb -MethodName TypeCtrlAltDel | ForEach-Object { $log += "ctrl+alt+del -> $($_.ReturnValue)" }; $Texto = '' }
    if ($kClique.Count -eq 2) {
        $ms = Get-CimInstance -Namespace $ns -ClassName Msvm_SyntheticMouse | Where-Object SystemName -eq $vm.Name; $log += "mouse=$([bool]$ms)"
        Invoke-CimMethod -InputObject $ms -MethodName SetAbsolutePosition -Arguments @{ horizontalPosition = $kClique[0]; verticalPosition = $kClique[1] } | ForEach-Object { $log += "posicao -> $($_.ReturnValue)" }
        Start-Sleep -Milliseconds 300
        Invoke-CimMethod -InputObject $ms -MethodName ClickButton -Arguments @{ buttonIndex = 1 } | ForEach-Object { $log += "clique -> $($_.ReturnValue)" }
        Start-Sleep -Milliseconds 800
    }
    if ($kCombo.Count) {
        foreach ($t in $kCombo) { Invoke-CimMethod -InputObject $kb -MethodName PressKey -Arguments @{ keyCode = $t } | Out-Null; Start-Sleep -Milliseconds 80 }
        [array]::Reverse($kCombo)
        foreach ($t in $kCombo) { Invoke-CimMethod -InputObject $kb -MethodName ReleaseKey -Arguments @{ keyCode = $t } | Out-Null; Start-Sleep -Milliseconds 80 }
        Start-Sleep -Milliseconds 1500
    }
    if ($Texto) { $r = Invoke-CimMethod -InputObject $kb -MethodName TypeText -Arguments @{ asciiText = $Texto } }
    foreach ($t in $kTeclas) { Invoke-CimMethod -InputObject $kb -MethodName TypeKey -Arguments @{ keyCode = $t } | ForEach-Object { $log += "tecla $t -> $($_.ReturnValue)" }; Start-Sleep -Milliseconds 150 }
    if ($Enter) { Invoke-CimMethod -InputObject $kb -MethodName TypeKey -Arguments @{ keyCode = 13 } | Out-Null }
    ("ok $($r.ReturnValue) " + ($log -join "; ")) | Set-Content $out -Encoding utf8
} catch { "erro: $($_.Exception.Message)" | Set-Content $out -Encoding utf8; exit 1 }
