# Monta o pacote do Liquinix: dist\Liquinix-AAAA-MM-DD.zip, o que vai nas releases do GitHub.
# Dentro: os programas (sem logs e sem as ferramentas de teste), os ícones que estão no repositório (MacTahoe,
# GPL-3.0), os scripts e o instalador. No outro PC: extrair e abrir LiquinixSetup.exe.
# NUNCA entra no pacote o que é da Apple ou de terceiros sem licença para redistribuir: fontes SF, sons do
# macOS, cursores, ícones fora do git (pacote Sequoia, SF Symbols, macOSicons...) e os ícones do sistema de
# bin\icones-sistema (gerados a partir do pacote Sequoia). O instalador baixa fontes, sons e cursor no próprio
# PC da pessoa, de onde os donos publicam (tools\baixar-apple.ps1). Uma checagem no fim recusa o pacote se
# algum desses arquivos aparecer.
# -SemSistema: sem a janela de Abrir/Salvar do Mac injetada nos programas
# -SemAjustes: sem os ajustes deste PC (as releases sempre usam)
# -Atualizacao: o mesmo que -SemAjustes (mantido por compatibilidade); -Zip escolhe o nome
param([switch]$SemAjustes, [switch]$SemSistema, [switch]$Atualizacao, [string]$Zip = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin  = Join-Path $root 'bin'
$data = Get-Date -Format 'yyyy-MM-dd'
$pkg  = Join-Path $root "dist\Liquinix"
$zip  = if ($Zip) { $Zip } else { Join-Path $root "dist\Liquinix-$data.zip" }
if ($Atualizacao) { $SemAjustes = $true }

if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force "$pkg\bin" | Out-Null

# programas: só o que o Shell usa (bin\ferramentas, bin\obj, logs e cópias .run/.old ficam de fora)
$progs = 'LiquinixShell.exe', 'LiquinixFinder.exe', 'LiquinixSettings.exe', 'LiquinixActivityMonitor.exe', 'LiquinixTerminal.exe',
         'LiquinixCalculator.exe', 'LiquinixNotes.exe', 'LiquinixScreenshot.exe', 'LiquinixPreview.exe', 'LiquinixReminders.exe', 'LiquinixCalendar.exe', 'LiquinixTextEdit.exe', 'LiquinixClock.exe', 'LiquinixWeather.exe', 'LiquinixContacts.exe', 'LiquinixPhotos.exe', 'LiquinixTimeMachine.exe', 'LiquinixMusic.exe', 'LiquinixQuickTime.exe', 'LiquinixForceQuit.exe', 'LiquinixTitleBars.exe', 'LiquinixWindow.dll', 'LiquinixWindow32.dll',
         'DialogoMac.dll', 'DialogoMac32.dll'
if ($SemSistema) { $progs = $progs | Where-Object { $_ -notlike 'DialogoMac*' } }
$progs += 'versao.txt'  # a versão (o GitHub Actions grava; o atualizador compara)
foreach ($p in $progs) {
    if (Test-Path "$bin\$p") { Copy-Item "$bin\$p" "$pkg\bin\$p" } else { Write-Host "faltando: bin\$p" }
}

# extras opcionais do instalador: janela de Abrir/Salvar do Mac
# (os ícones do sistema de bin\icones-sistema não vão: são gerados a partir do pacote Sequoia, de terceiros;
# os scripts deles vão só para o desinstalar poder desfazer o que um pacote antigo trocou)
# Os scripts de DESFAZER vão sempre (mesmo com -SemSistema): um PC que recebeu esses extras de um pacote antigo
# precisa deles para o desinstalar devolver os ícones e a janela do Windows
New-Item -ItemType Directory -Force "$pkg\tools" | Out-Null
foreach ($s in 'icones-sistema.ps1', 'icones-sistema-extra.ps1', 'dialogo-desligar.ps1', 'musica-registrar.ps1', 'quicktime-registrar.ps1', 'atualizar-github.ps1', 'baixar-apple.ps1') { Copy-Item "$PSScriptRoot\$s" "$pkg\tools\$s" }
if (-not $SemSistema) {
    Copy-Item "$PSScriptRoot\dialogo-ligar.ps1" "$pkg\tools\dialogo-ligar.ps1"
}

# ícones: só os arquivos que estão no git (icones\tahoe, MacTahoe GPL-3.0, e o LEIA-ME). Pastas baixadas ou
# geradas neste PC (sequoia, simbolos, extra, tahoe-macosicons, leopard...) ficam de fora.
# Sons, fontes e cursores não vão: o instalador baixa no PC da pessoa.
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git não encontrado: precisa dele para saber quais ícones estão no repositório' }
$icones = @(git -C $root -c core.quotepath=off ls-files -- icones)
if ($LASTEXITCODE -ne 0 -or -not $icones) { throw 'git ls-files icones falhou' }
foreach ($rel in $icones) {
    $dst = Join-Path $pkg $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item (Join-Path $root $rel) $dst
}

# ajustes deste PC (rodando por você, não pelo Claude: o AppData dele é outra cópia)
if (-not $SemAjustes) {
    $cfg = Join-Path $env:APPDATA 'Shell'
    New-Item -ItemType Directory -Force "$pkg\ajustes" | Out-Null
    $cfgs = 'ajustes.txt', 'janelas-excluidas.txt', 'dialogo.txt', 'mesa.txt'
    if ($SemSistema) { $cfgs = $cfgs | Where-Object { $_ -ne 'dialogo.txt' } }
    foreach ($f in $cfgs) {
        if (Test-Path "$cfg\$f") { Copy-Item "$cfg\$f" "$pkg\ajustes\$f" }
    }
    # o que só vale neste PC: o valor antigo da barra do Windows nos outros monitores (o Shell devolveria o
    # deste PC no outro) e o monitor do Dock (o nome do monitor é deste PC)
    $aj = "$pkg\ajustes\ajustes.txt"
    if (Test-Path $aj) {
        $linhas = Get-Content $aj -Encoding utf8 | Where-Object { $_ -notmatch '^(barra_windows_monitores_antes|dock_monitor)=' }
        Set-Content $aj $linhas -Encoding utf8
    }
}

Copy-Item "$root\instalador\*" $pkg
# o instalador único (instalar, atualizar, desinstalar), na raiz do pacote: é o que a pessoa abre
if (Test-Path "$bin\LiquinixSetup.exe") { Copy-Item "$bin\LiquinixSetup.exe" "$pkg\LiquinixSetup.exe" } else { Write-Host 'faltando: bin\LiquinixSetup.exe' }
# checagem final: nada da Apple ou de terceiros sem licença no pacote
$proibidos = Get-ChildItem $pkg -Recurse -File | Where-Object {
    $_.Extension -in '.otf', '.ttf', '.ttc', '.dfont', '.cur', '.ani', '.wav', '.aif', '.aiff', '.caf', '.mp3', '.m4a', '.heic', '.mp4', '.mov' -or
    $_.FullName -match '\\(fontes|sons|cursor|cursor-leopard|icones-sistema)\\'
}
$rels = @($icones | ForEach-Object { $_ -replace '/', '\' })
$foraDoGit = Get-ChildItem (Join-Path $pkg 'icones') -Recurse -File | Where-Object { $rels -notcontains $_.FullName.Substring($pkg.Length + 1) }
$ruins = @($proibidos) + @($foraDoGit) | Where-Object { $_ }
if ($ruins) {
    $ruins | ForEach-Object { Write-Host "não pode ir no pacote: $($_.FullName.Substring($pkg.Length + 1))" }
    Remove-Item $pkg -Recurse -Force
    throw 'pacote recusado: tem arquivos da Apple ou de terceiros'
}

if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $pkg -Recurse -Force
"Pronto: $zip ({0:N0} MB)" -f ((Get-Item $zip).Length / 1MB)
