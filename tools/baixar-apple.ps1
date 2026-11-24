# Baixa, no próprio PC de quem instala, o que é da Apple e não pode ir no repositório do WacOS (o instalar.ps1 chama):
#  -Fontes <pasta>: SF Pro e SF Mono do site da Apple (developer.apple.com/fonts). Vêm em .dmg do Mac, com um .pkg
#                   dentro e as fontes no fundo; o 7-Zip abre as três camadas (instalado pelo winget se faltar).
#  -Sons <pasta>:   os 14 sons de alerta do macOS (Basso, Funk, Glass, Pop, Tink...) do repositório público
#                   github.com/extratone/macOSsystemsounds, convertidos de AIFF para WAV (o Shell toca .wav).
#  -Cursor <pasta>: o cursor "macOS Tahoe" (DeviantArt, jimmyxd2): o site não deixa baixar direto, então abre a página,
#                   espera o download e pega o .zip mais novo da pasta Downloads que tenha o arrowS.cur.
# Sai em cada pasta o que deu certo; nada é instalado aqui (quem instala e anota para o desinstalar é o instalar.ps1).
# -Procurar <pasta> e -SemAbrir: para testar a parte do cursor sem abrir o site (procura nessa pasta em vez de Downloads)
param([string]$Fontes = '', [string]$Sons = '', [string]$Cursor = '', [string]$Procurar = '', [switch]$SemAbrir)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # a barra de progresso deixa o download muito lento
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$tmp = Join-Path $env:TEMP ('wacos-apple-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null

function SeteZip {
    $c = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if ($c) { return $c }
    Write-Host '  O 7-Zip abre o .dmg da Apple e não está instalado. Instalando pelo winget (instalador oficial do Windows)...'
    try { & winget install --id 7zip.7zip -e --silent --accept-package-agreements --accept-source-agreements | Out-Null } catch {}
    $c = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $c) { Write-Host '  Não deu para instalar o 7-Zip. Instale em https://www.7-zip.org e rode o instalador de novo.' }
    return $c
}

# ---- fontes SF (Apple)
if ($Fontes) {
    $7z = SeteZip
    if ($7z) {
        New-Item -ItemType Directory -Force $Fontes | Out-Null
        foreach ($nome in 'SF-Pro', 'SF-Mono') {
            try {
                $dmg = Join-Path $tmp "$nome.dmg"
                Write-Host "  Baixando $nome.dmg do site da Apple..."
                Invoke-WebRequest -UseBasicParsing "https://devimages-cdn.apple.com/design/resources/download/$nome.dmg" -OutFile $dmg
                $d = Join-Path $tmp "$nome-dmg"; & $7z x $dmg "-o$d" -y | Out-Null
                $pkg = Get-ChildItem $d -Recurse -Filter *.pkg | Select-Object -First 1
                if (-not $pkg) { Write-Host "  $nome`: o .dmg não tem o .pkg esperado"; continue }
                $p = Join-Path $tmp "$nome-pkg"; & $7z x $pkg.FullName "-o$p" -y | Out-Null
                # Payload (cpio comprimido): às vezes sai um "Payload~" que precisa de mais uma volta
                $q = Join-Path $tmp "$nome-payload"
                foreach ($pl in Get-ChildItem $p -Recurse -File | Where-Object { $_.Name -like 'Payload*' }) { & $7z x $pl.FullName "-o$q" -y | Out-Null }
                foreach ($pl in Get-ChildItem $q -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'Payload*' }) { & $7z x $pl.FullName "-o$q\2" -y | Out-Null }
                # as mesmas famílias e pesos que o pacote completo leva (Text, Display, Rounded, Mono; sem Ultralight, Thin, Black)
                $n = 0
                Get-ChildItem $q -Recurse -File -Include *.otf, *.ttf -ErrorAction SilentlyContinue | Where-Object {
                    $f = $_.Name
                    $f -match '^SF' -and $f -notmatch '^SF-Pro(-Italic)?\.ttf$' -and $f -notmatch '(?i)ultralight|thin|black' -and
                    ($f -notmatch '(?i)italic' -or $f -match '^SF-Pro-Text')
                } | ForEach-Object { if (-not (Test-Path (Join-Path $Fontes $_.Name))) { Copy-Item $_.FullName $Fontes; $n++ } }
                Write-Host "  $nome`: $n fonte(s)"
            } catch { Write-Host "  $nome`: não deu ($($_.Exception.Message))" }
        }
    }
}

# ---- sons de alerta do macOS (AIFF -> WAV)
function AiffParaWav([string]$aiff, [string]$wav) {
    $b = [IO.File]::ReadAllBytes($aiff)
    function BE32($i) { ([int]$b[$i] -shl 24) -bor ([int]$b[$i + 1] -shl 16) -bor ([int]$b[$i + 2] -shl 8) -bor [int]$b[$i + 3] }
    function BE16($i) { ([int]$b[$i] -shl 8) -bor [int]$b[$i + 1] }
    if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'FORM') { throw 'não é AIFF' }
    $tipo = [Text.Encoding]::ASCII.GetString($b, 8, 4)  # AIFF ou AIFC
    $ch = 0; $bits = 0; $rate = 0; $dados = $null; $le = $false
    $i = 12
    while ($i + 8 -le $b.Length) {
        $id = [Text.Encoding]::ASCII.GetString($b, $i, 4); $len = BE32 ($i + 4); $c = $i + 8
        if ($id -eq 'COMM') {
            $ch = BE16 $c; $bits = BE16 ($c + 6)
            # taxa em ponto flutuante de 80 bits: expoente de 15 bits e mantissa de 64 (somada em double: sem estouro)
            $e = (BE16 ($c + 8)) -band 0x7FFF; $m = [double]0
            for ($k = 0; $k -lt 8; $k++) { $m = $m * 256 + $b[$c + 10 + $k] }
            $rate = [int][math]::Round($m * [math]::Pow(2, $e - 16383 - 63))
            if ($tipo -eq 'AIFC') { $comp = [Text.Encoding]::ASCII.GetString($b, $c + 18, 4); if ($comp -eq 'sowt') { $le = $true } elseif ($comp -ne 'NONE') { throw "compressão $comp" } }
        } elseif ($id -eq 'SSND') {
            $off = BE32 $c; $dados = New-Object byte[] ($len - 8 - $off); [Array]::Copy($b, $c + 8 + $off, $dados, 0, $dados.Length)
        }
        $i = $c + $len + ($len % 2)
    }
    if (-not $dados -or -not $ch -or -not $bits -or -not $rate) { throw 'AIFF incompleto' }
    $bps = [int]($bits / 8)
    if (-not $le -and $bps -gt 1) {  # big-endian -> little-endian, amostra por amostra
        for ($k = 0; $k + $bps -le $dados.Length; $k += $bps) { [Array]::Reverse($dados, $k, $bps) }
    } elseif ($bps -eq 1) { for ($k = 0; $k -lt $dados.Length; $k++) { $dados[$k] = [byte](($dados[$k] + 128) % 256) } }  # 8 bits: com sinal -> sem sinal
    if ($bps -eq 3) {  # 24 bits -> 16 (os dois bytes de cima de cada amostra): o PlaySound toca em qualquer PC
        $n16 = [int]($dados.Length / 3); $d16 = New-Object byte[] ($n16 * 2)
        for ($k = 0; $k -lt $n16; $k++) { $d16[2 * $k] = $dados[3 * $k + 1]; $d16[2 * $k + 1] = $dados[3 * $k + 2] }
        $dados = $d16; $bps = 2; $bits = 16
    }
    $fs = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $fs
    $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $dados.Length)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
    $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]$ch); $w.Write([int]$rate); $w.Write([int]($rate * $ch * $bps)); $w.Write([int16]($ch * $bps)); $w.Write([int16]$bits)
    $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$dados.Length); $w.Write($dados); $w.Flush()
    [IO.File]::WriteAllBytes($wav, $fs.ToArray())
}
if ($Sons) {
    New-Item -ItemType Directory -Force $Sons | Out-Null
    $base = 'https://raw.githubusercontent.com/extratone/macOSsystemsounds/main/aiff'
    $n = 0
    foreach ($s in 'Basso', 'Blow', 'Bottle', 'Frog', 'Funk', 'Glass', 'Hero', 'Morse', 'Ping', 'Pop', 'Purr', 'Sosumi', 'Submarine', 'Tink') {
        $wav = Join-Path $Sons "$s.wav"
        if (Test-Path $wav) { continue }
        try {
            $a = Join-Path $tmp "$s.aiff"
            Invoke-WebRequest -UseBasicParsing "$base/$s.aiff" -OutFile $a
            AiffParaWav $a $wav; $n++
        } catch { Write-Host "  $s`: não deu ($($_.Exception.Message))" }
    }
    Write-Host "  Sons do macOS: $n"
}

# ---- cursor macOS Tahoe (DeviantArt: precisa baixar pela página)
if ($Cursor) {
    $pagina = 'https://www.deviantart.com/jimmyxd2/art/1208233550'
    $downloads = (New-Object -ComObject Shell.Application).Namespace('shell:Downloads').Self.Path
    if (-not $downloads) { $downloads = Join-Path $env:USERPROFILE 'Downloads' }
    if ($Procurar) { $downloads = $Procurar }
    $desde = Get-Date
    $resp = ''
    if (-not $SemAbrir) {
        Write-Host "  Abrindo a página do cursor ($pagina)."
        Write-Host '  Clique em Download (o site pode pedir para entrar na conta do DeviantArt) e salve o arquivo na pasta Downloads.'
        Start-Process $pagina
        $resp = Read-Host '  Quando o download terminar, aperte Enter (ou digite p para pular)'
    }
    if ($resp.Trim().ToLower() -ne 'p') {
        # o arquivo mais novo da pasta Downloads (.zip, .rar ou .7z) que tenha o arrowS.cur (de preferência "With Shadow");
        # .rar e .7z precisam do 7-Zip. Uma pasta já extraída com o arrowS.cur também serve.
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $7zc = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
        $zips = Get-ChildItem $downloads -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.zip', '.rar', '.7z' } |
                Sort-Object LastWriteTime -Descending | Select-Object -First 15
        $achou = $false
        $soltas = Get-ChildItem $downloads -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 10 |
                  Where-Object { Get-ChildItem $_.FullName -Recurse -Filter arrowS.cur -ErrorAction SilentlyContinue | Select-Object -First 1 }
        foreach ($z in @($zips) + @($soltas)) {
            try {
                $x = Join-Path $tmp ('cursor-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
                if ($z.PSIsContainer) { $x = $z.FullName }
                elseif ($z.Extension -eq '.zip') {
                    $zip = [IO.Compression.ZipFile]::OpenRead($z.FullName)
                    $tem = $zip.Entries | Where-Object { $_.Name -ieq 'arrowS.cur' }
                    $zip.Dispose()
                    if (-not $tem) { continue }
                    Expand-Archive $z.FullName $x -Force
                } elseif ($7zc) {
                    if (-not ((& $7zc l $z.FullName) -match '(?i)arrowS\.cur')) { continue }
                    & $7zc x $z.FullName "-o$x" -y | Out-Null
                } else { continue }
                $setas = Get-ChildItem $x -Recurse -Filter arrowS.cur
                $pasta = ($setas | Where-Object { $_.DirectoryName -match '(?i)shadow' } | Select-Object -First 1)
                if (-not $pasta) { $pasta = $setas | Select-Object -First 1 }
                $nome = Split-Path $pasta.DirectoryName -Leaf
                $dst = Join-Path $Cursor $nome
                New-Item -ItemType Directory -Force $dst | Out-Null
                Copy-Item (Join-Path $pasta.DirectoryName '*') $dst -Force
                # papel do Windows = arquivo do pacote (o mapa usado neste cursor)
                $mapa = @{ AppStarting = 'waitS.ani'; Arrow = 'arrowS.cur'; Hand = 'linkS.cur'; Help = 'helpS.cur'; No = 'noS.cur'; NWPen = 'penS.cur';
                           SizeAll = 'moveS.cur'; SizeNESW = 'D1sizeS.cur'; SizeNS = 'HsizeS.cur'; SizeNWSE = 'D2sizeS.cur'; SizeWE = 'VsizeS.cur';
                           UpArrow = 'upS.cur'; Wait = 'busyS.ani'; Person = 'personS.cur'; Pin = 'locationS.cur'; Crosshair = 'crossS.cur'; IBeam = 'beamS.cur' }
                $linhas = foreach ($k in $mapa.Keys) { if (Test-Path (Join-Path $dst $mapa[$k])) { "$k=$($mapa[$k])" } }
                Set-Content -Encoding utf8 (Join-Path $Cursor 'cursores.txt') $linhas
                Write-Host "  Cursor: $nome ($($linhas.Count) ponteiros, de $($z.Name))"
                $achou = $true; break
            } catch { Write-Host "  $($z.Name): não deu ($($_.Exception.Message))" }
        }
        if (-not $achou) { Write-Host "  Não achei o .zip do cursor em $downloads (com o arrowS.cur dentro)." }
    }
}
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
