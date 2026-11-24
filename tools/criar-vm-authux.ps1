# Cria a VM de testes do AuthUX no Hyper-V (Geração 2, TPM virtual, Secure Boot) e confere o build da ISO.
# Uso: powershell -ExecutionPolicy Bypass -File tools\criar-vm-authux.ps1   (pede administrador)
# Desfazer: Remove-VM -Name AuthUX-Teste -Force; apagar a pasta $env:USERPROFILE\Hyper-V\AuthUX-Teste
param([switch]$Admin)
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'docs\ultima-execucao-vm.txt'
$log  = Join-Path $root 'docs\registro-mudancas.txt'
$iso  = '$env:USERPROFILE\Downloads\26200.9711_amd64_en-us_professional_85096553_convert\26100.1.240331-1435.GE_RELEASE_CLIENTPRO_OEMRET_X64FRE_EN-US.ISO'
$nome = 'AuthUX-Teste'
$pasta = "$env:USERPROFILE\Hyper-V\$nome"

if (-not $Admin) {
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin"
    exit $p.ExitCode
}

$r = @()
try {
    # build da ISO (lê o install.wim/esd sem instalar nada)
    $m = Mount-DiskImage -ImagePath $iso -PassThru
    $l = ($m | Get-Volume).DriveLetter
    $img = Get-ChildItem "${l}:\sources" -Include install.wim, install.esd -Recurse | Select-Object -First 1
    $i = Get-WindowsImage -ImagePath $img.FullName -Index 1
    $r += "ISO: $($i.ImageName)  versão $($i.Version)  idioma $($i.Languages -join ',')"
    Dismount-DiskImage -ImagePath $iso | Out-Null

    if (Get-VM -Name $nome -ErrorAction SilentlyContinue) { throw "já existe uma VM chamada $nome" }
    New-Item -ItemType Directory -Force $pasta | Out-Null
    $vm = New-VM -Name $nome -Generation 2 -MemoryStartupBytes 8GB -Path '$env:USERPROFILE\Hyper-V' `
                 -NewVHDPath "$pasta\disco.vhdx" -NewVHDSizeBytes 80GB -SwitchName 'Default Switch'
    Set-VM -VM $vm -ProcessorCount 4 -StaticMemory -AutomaticCheckpointsEnabled $false -CheckpointType Standard
    Set-VMFirmware -VM $vm -EnableSecureBoot On -SecureBootTemplate MicrosoftWindows
    Set-VMKeyProtector -VM $vm -NewLocalKeyProtector
    Enable-VMTPM -VM $vm
    $dvd = Add-VMDvdDrive -VM $vm -Path $iso -Passthru
    Set-VMFirmware -VM $vm -FirstBootDevice $dvd
    Set-VMVideo -VM $vm -HorizontalResolution 1920 -VerticalResolution 1080 -ResolutionType Single -ErrorAction SilentlyContinue
    $r += "VM $nome criada: Geração 2, 4 vCPU, 8 GB, disco 80 GB (dinâmico) em $pasta, TPM virtual, Secure Boot, rede Default Switch"

    $agora = Get-Date -Format 'yyyy-MM-dd HH:mm'
    Add-Content $log -Encoding utf8 ("`r`n[$agora] VM de testes do AuthUX criada no Hyper-V (tools\criar-vm-authux.ps1, como administrador): $nome em $pasta, ISO 26200.9711 do UUP dump. Desfazer: Remove-VM -Name $nome -Force e apagar a pasta $pasta.")
    ("ok`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
} catch {
    ("erro: $($_.Exception.Message)`r`n" + ($r -join "`r`n")) | Set-Content $out -Encoding utf8
    exit 1
}
