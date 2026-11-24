# Copia um arquivo do PC para dentro da VM (Hyper-V Guest Service Interface; não precisa de senha da VM).
# Uso: tools\vm-enviar.ps1 -Arquivo <caminho no PC> [-Destino <pasta na VM>]   (pede administrador)
param([switch]$Admin, [string]$Arquivo, [string]$Destino = 'C:\Users\Public\Desktop', [string]$Nome = 'AuthUX-Teste')
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'docs\ultima-execucao-vm.txt'
if (-not $Admin) {
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin -Arquivo `"$Arquivo`" -Destino `"$Destino`" -Nome `"$Nome`""
    exit $p.ExitCode
}
try {
    Enable-VMIntegrationService -VMName $Nome -Name 'Guest Service Interface'
    $alvo = Join-Path $Destino (Split-Path $Arquivo -Leaf)
    $erro = $null
    for ($i = 0; $i -lt 40; $i++) {   # a VM pode estar reiniciando: tenta por até ~4 min
        try { Copy-VMFile -Name $Nome -SourcePath $Arquivo -DestinationPath $alvo -FileSource Host -CreateFullPath -Force -ErrorAction Stop; $erro = $null; break }
        catch { $erro = $_.Exception.Message; Start-Sleep 6 }
    }
    if ($erro) { throw $erro }
    "ok`r`nenviado para a VM: $alvo ($([math]::Round((Get-Item $Arquivo).Length / 1MB)) MB)" | Set-Content $out -Encoding utf8
} catch { "erro: $($_.Exception.Message)" | Set-Content $out -Encoding utf8; exit 1 }
