# Cria um disco de dados (pacotes.vhdx) com as atualizações 26200.9711 e o AuthUX, liga na VM AuthUX-Teste e abre a janela da VM.
# Uso: powershell -ExecutionPolicy Bypass -File tools\vm-authux-pacotes.ps1   (pede administrador)
param([switch]$Admin)
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'docs\ultima-execucao-vm.txt'
$log  = Join-Path $root 'docs\registro-mudancas.txt'
$uup  = '$env:USERPROFILE\Downloads\26200.9711_amd64_en-us_professional_85096553_convert\UUPs'
$nome = 'AuthUX-Teste'
$vhd  = "$env:USERPROFILE\Hyper-V\$nome\pacotes.vhdx"

if (-not $Admin) {
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin"
    exit $p.ExitCode
}

$r = @()
try {
    if (-not (Test-Path $vhd)) {
        New-VHD -Path $vhd -SizeBytes 12GB -Dynamic | Out-Null
        $d = Mount-VHD -Path $vhd -Passthru | Initialize-Disk -PartitionStyle GPT -PassThru |
             New-Partition -UseMaximumSize -AssignDriveLetter | Format-Volume -FileSystem NTFS -NewFileSystemLabel PACOTES -Confirm:$false
        $l = $d.DriveLetter
        New-Item -ItemType Directory "${l}:\atualizacoes", "${l}:\authux" | Out-Null
        foreach ($f in 'Windows11.0-KB5043080-x64.msu', 'Windows11.0-KB5127648-x64.msu', 'Windows11.0-KB5054156-x64.cab') {
            Copy-Item (Join-Path $uup $f) "${l}:\atualizacoes\"
        }
        Copy-Item '$env:USERPROFILE\Documents\AuthUX\x64\Release.GE\AuthUX.dll' "${l}:\authux\"
        @'
@echo off
rem Rode como administrador DENTRO da VM. Ordem: checkpoint 24H2, cumulativa 26100.9711, habilitação 25H2 (26200).
cd /d %~dp0
dism /online /add-package /packagepath:Windows11.0-KB5043080-x64.msu /norestart
dism /online /add-package /packagepath:Windows11.0-KB5127648-x64.msu /norestart
dism /online /add-package /packagepath:Windows11.0-KB5054156-x64.cab /norestart
echo.
echo Pronto. Reinicie a VM.
pause
'@ | Set-Content "${l}:\atualizacoes\instalar.cmd" -Encoding ascii
        Dismount-VHD -Path $vhd
        $r += "pacotes.vhdx criado com atualizacoes\ (KB5043080, KB5127648, KB5054156, instalar.cmd) e authux\AuthUX.dll"
    }
    if (-not (Get-VMHardDiskDrive -VMName $nome | Where-Object Path -eq $vhd)) {
        Add-VMHardDiskDrive -VMName $nome -Path $vhd
        $r += "pacotes.vhdx ligado na VM"
    }
    Start-Process vmconnect.exe -ArgumentList "localhost `"$nome`""
    $r += "janela da VM aberta"
    $agora = Get-Date -Format 'yyyy-MM-dd HH:mm'
    Add-Content $log -Encoding utf8 ("`r`n[$agora] VM ${nome}: disco de dados $vhd (atualizações 26200.9711 + AuthUX.dll) ligado na VM (tools\vm-authux-pacotes.ps1). Só afeta a VM.")
    ("ok`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
} catch {
    ("erro: $($_.Exception.Message)`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
    exit 1
}
