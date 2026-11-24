# Baixa icones do macOS Tahoe do macOSicons.com (API de busca) para icones\tahoe-macosicons.
# SO PARA USO PESSOAL: os icones do macOSicons.com nao tem licenca para redistribuir. A pasta fica fora do git
# (.gitignore) e o tools\empacotar.ps1 nao a leva. Nunca grave em icones\tahoe (MacTahoe, GPL-3.0, vai nas releases).
# A chave da API NAO fica no projeto: passe em -Chave ou na variavel de ambiente MACOSICONS_API_KEY.
# Plano gratis: 50 buscas por mes -> uma busca por icone; as respostas ficam em icones\tahoe-macosicons\api-cache
# e nao sao pedidas de novo; icones que ja existem na pasta sao pulados (use -Substituir para trocar).
# Cada item e "arquivo=busca": o arquivo tem o nome que o Shell procura (o mesmo do pacote Sequoia).
#   powershell -File tools\baixar-icones.ps1 -Itens "Finder=Finder Tahoe","trashempty@2x=Trash Tahoe"
#   powershell -File tools\baixar-icones.ps1 -Itens "Finder=Finder Tahoe" -SoMostrar
param(
    [Parameter(Mandatory = $true)][string[]]$Itens,
    [string]$Pasta = (Join-Path (Split-Path $PSScriptRoot -Parent) "icones\tahoe-macosicons"),
    [string]$Chave = $env:MACOSICONS_API_KEY,
    [switch]$SoMostrar,
    [switch]$Substituir,
    [switch]$Escuro      # prefere a variante escura (isDarkMode)
)
$ErrorActionPreference = "Stop"
if ((Split-Path $Pasta -Leaf) -eq "tahoe") { throw "icones\tahoe e so do MacTahoe (GPL-3.0); use outra pasta, ex.: icones\tahoe-macosicons" }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$cache = Join-Path $Pasta "api-cache"
New-Item -ItemType Directory -Force $Pasta, $cache | Out-Null
$creditos = Join-Path $Pasta "CREDITOS.txt"

# Do .icns, o maior PNG de dentro (ic10 = 1024 px); $null se nao houver PNG
function PngDoIcns([byte[]]$b) {
    $best = $null; $i = 8
    while ($i + 8 -le $b.Length) {
        $len = ([int]$b[$i + 4] -shl 24) -bor ([int]$b[$i + 5] -shl 16) -bor ([int]$b[$i + 6] -shl 8) -bor [int]$b[$i + 7]
        if ($len -lt 8 -or $i + $len -gt $b.Length) { break }
        if ($len -gt 16 -and $b[$i + 8] -eq 0x89 -and $b[$i + 9] -eq 0x50 -and $b[$i + 10] -eq 0x4E -and $b[$i + 11] -eq 0x47) {
            if (-not $best -or $len - 8 -gt $best.Length) { $best = [byte[]]$b[($i + 8)..($i + $len - 1)] }
        }
        $i += $len
    }
    return $best
}

# Nota de cada resultado: estilo Tahoe no nome, todas as palavras da busca no nome, a variante clara
# padrao (sem Dark/Clear/Tinted/Graphite), depois os mais baixados
function Nota($h, [string]$q) {
    $nome = "$($h.appName)".ToLower()
    $n = 0
    if ($nome -match "tahoe|\b26\b|liquid") { $n += 4000000 }
    $palavras = $q.ToLower() -split "\s+" | Where-Object { $_ -and $_ -ne "tahoe" }
    $todas = $true; foreach ($p in $palavras) { if (-not $nome.Contains($p)) { $todas = $false } }
    if ($todas) { $n += 2000000 }
    $escuro = [bool]$h.isDarkMode -or $nome -match "dark"
    if ($escuro -eq [bool]$Escuro) { $n += 1000000 }
    if ($nome -notmatch "clear|tinted|graphite|glass|mono|beta") { $n += 500000 }
    if ($h.isOfficial) { $n += 300000 }
    $n += [int]$h.downloads
    return $n
}

foreach ($item in $Itens) {
    $arquivo, $q = $item -split "=", 2
    if (-not $q) { $q = "$arquivo Tahoe" }
    $exato = $null  # "busca#Nome exato do resultado": escolhe esse resultado em vez da nota
    if ($q.Contains("#")) { $q, $exato = $q -split "#", 2 }
    $alvo = Join-Path $Pasta ($arquivo + ".png")
    if ((Test-Path $alvo) -and -not $Substituir -and -not $SoMostrar) { Write-Output "ja existe: $arquivo"; continue }
    $cf = Join-Path $cache (($q -replace '[^\w\- ]', '_') + ".json")
    if (Test-Path $cf) { $json = [IO.File]::ReadAllText($cf) }
    else {
        if (-not $Chave) { throw "Sem chave: defina MACOSICONS_API_KEY ou passe -Chave." }
        $body = @{ query = $q } | ConvertTo-Json -Compress
        try {
            $json = (Invoke-WebRequest -UseBasicParsing -Method Post -Uri "https://api.macosicons.com/api/v1/search" -ContentType "application/json" -Headers @{ "x-api-key" = $Chave } -Body $body).Content
        } catch { Write-Output "falhou a busca '$q': $($_.Exception.Message)"; continue }
        [IO.File]::WriteAllText($cf, $json)
    }
    $hits = @(($json | ConvertFrom-Json).hits)
    $ord = @($hits | Sort-Object { Nota $_ $q } -Descending)
    if ($exato) { $ord = @($ord | Where-Object { "$($_.appName)".Trim() -eq $exato.Trim() }) }
    if ($SoMostrar) {
        Write-Output "== $arquivo  ('$q', $($hits.Count) resultados)"
        $ord | Select-Object -First 8 | ForEach-Object { Write-Output ("  {0} | escuro={1} | {2} downloads" -f $_.appName, $_.isDarkMode, $_.downloads) }
        continue
    }
    if (-not $ord.Count) { Write-Output "nenhum resultado: $arquivo"; continue }
    $h = $ord[0]
    $png = $null
    if ($h.icnsUrl) { try { $png = PngDoIcns ((Invoke-WebRequest -UseBasicParsing -Uri $h.icnsUrl).Content) } catch { } }
    if (-not $png -and $h.lowResPngUrl) { $png = (Invoke-WebRequest -UseBasicParsing -Uri $h.lowResPngUrl).Content }
    if (-not $png) { Write-Output "sem PNG: $arquivo"; continue }
    [IO.File]::WriteAllBytes($alvo, [byte[]]$png)
    $autor = if ($h.designedBy) { $h.designedBy } elseif ($h.usersName) { $h.usersName } else { $h.credit }
    Add-Content -Encoding UTF8 $creditos "$arquivo`t$($h.appName)`t$autor`t$($h.uploadedBy)"
    Write-Output ("baixado: {0} <- '{1}' ({2} KB)" -f $arquivo, $h.appName, [int]($png.Length / 1024))
}
