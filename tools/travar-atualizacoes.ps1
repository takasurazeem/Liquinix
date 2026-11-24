# Trava o Windows Update no 25H2 (build 26200): continua recebendo as atualizações mensais de segurança,
# mas não troca de versão; não instala drivers pelo Windows Update; avisa antes de baixar/instalar e não reinicia sozinho.
# Uso:    powershell -ExecutionPolicy Bypass -File tools\travar-atualizacoes.ps1            (pede administrador)
# Desfazer: powershell -ExecutionPolicy Bypass -File tools\travar-atualizacoes.ps1 -Desfazer
# Backup do estado anterior: docs\backup-atualizacoes.json. Resultado: docs\ultima-execucao-atualizacoes.txt
param([switch]$Desfazer, [switch]$Admin)
$root = Split-Path $PSScriptRoot -Parent
$bk   = Join-Path $root 'docs\backup-atualizacoes.json'
$out  = Join-Path $root 'docs\ultima-execucao-atualizacoes.txt'
$log  = Join-Path $root 'docs\registro-mudancas.txt'

if (-not $Admin) {
    $a = if ($Desfazer) { '-Desfazer' } else { '' }
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin $a"
    "saída: $($p.ExitCode)" | Add-Content $out -Encoding utf8
    exit $p.ExitCode
}

$WU = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'
$AU = "$WU\AU"
$valores = @(
    @{ k = $WU; n = 'TargetReleaseVersion';            t = 'DWord';  v = 1 },
    @{ k = $WU; n = 'ProductVersion';                  t = 'String'; v = 'Windows 11' },
    @{ k = $WU; n = 'TargetReleaseVersionInfo';        t = 'String'; v = '25H2' },
    @{ k = $WU; n = 'ExcludeWUDriversInQualityUpdate'; t = 'DWord';  v = 1 },
    @{ k = $AU; n = 'NoAutoUpdate';                    t = 'DWord';  v = 0 },
    @{ k = $AU; n = 'AUOptions';                       t = 'DWord';  v = 2 },
    @{ k = $AU; n = 'NoAutoRebootWithLoggedOnUsers';   t = 'DWord';  v = 1 }
)
$agora = Get-Date -Format 'yyyy-MM-dd HH:mm'
$linhas = @()
try {
    if ($Desfazer) {
        if (-not (Test-Path $bk)) { throw "sem backup em $bk" }
        $b = Get-Content $bk -Raw -Encoding utf8 | ConvertFrom-Json
        foreach ($e in $b.valores) {
            if ($null -eq $e.antes) { Remove-ItemProperty -Path $e.k -Name $e.n -ErrorAction SilentlyContinue; $linhas += "$($e.k) [$($e.n)] removido" }
            else { Set-ItemProperty -Path $e.k -Name $e.n -Value $e.antes; $linhas += "$($e.k) [$($e.n)] = $($e.antes) (restaurado)" }
        }
        foreach ($k in @($AU, $WU)) {
            if (($b.chavesCriadas -contains $k) -and (Test-Path $k) -and -not (Get-ChildItem $k) -and -not (Get-Item $k).Property) { Remove-Item $k; $linhas += "$k removida (estava vazia)" }
        }
        Add-Content $log -Encoding utf8 ("`r`n[$agora] Windows Update destravado (tools\travar-atualizacoes.ps1 -Desfazer).`r`n" + (($linhas | % { "    $_" }) -join "`r`n"))
    } else {
        $criadas = @()
        foreach ($k in @($WU, $AU)) { if (-not (Test-Path $k)) { New-Item -Path $k -Force | Out-Null; $criadas += $k } }
        if (-not (Test-Path $bk)) {
            $antes = foreach ($e in $valores) {
                $cur = (Get-ItemProperty -Path $e.k -Name $e.n -ErrorAction SilentlyContinue).($e.n)
                [pscustomobject]@{ k = $e.k; n = $e.n; antes = $cur }
            }
            [pscustomobject]@{ data = $agora; chavesCriadas = $criadas; valores = $antes } | ConvertTo-Json -Depth 4 | Set-Content $bk -Encoding utf8
        }
        foreach ($e in $valores) {
            New-ItemProperty -Path $e.k -Name $e.n -PropertyType $e.t -Value $e.v -Force | Out-Null
            $linhas += "$($e.k -replace 'HKLM:\\','HKLM\') [$($e.n)] = $($e.v)"
        }
        Add-Content $log -Encoding utf8 ("`r`n[$agora] Windows Update travado no 25H2 (build 26200), sem drivers, avisar antes de baixar/instalar, sem reinício automático (tools\travar-atualizacoes.ps1, como administrador). Backup docs\backup-atualizacoes.json; desfazer: tools\travar-atualizacoes.ps1 -Desfazer`r`n" + (($linhas | % { "    $_   (antes: não existia)" }) -join "`r`n"))
    }
    "ok`r`n" + ($linhas -join "`r`n") | Set-Content $out -Encoding utf8
} catch {
    "erro: $($_.Exception.Message)" | Set-Content $out -Encoding utf8
    exit 1
}
