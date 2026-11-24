# Monta o instalador do WacOS para levar a outro PC: dist\WacOS-AAAA-MM-DD.zip
# Dentro: os programas (sem logs e sem as ferramentas de teste), ícones, sons, as fontes SF instaladas neste
# PC, o cursor do Mac em uso e o instalar.cmd / desinstalar.cmd. No outro PC: extrair e abrir instalar.cmd.
# Os ícones, sons, fontes e cursor do Mac são da Apple: o pacote é só para uso pessoal, nos seus PCs.
# -SemSistema: sem o que mexe no próprio Windows (janela de Abrir/Salvar do Mac injetada nos programas e ícones
# do sistema trocados no registro); o resto (apps, Dock, Finder, fontes, cursor opcional) continua
# -Atualizacao (GitHub Actions): pacote só de atualização: sem fontes, cursor e ajustes deste PC; -Zip escolhe o nome
param([switch]$SemAjustes, [switch]$SemSistema, [switch]$Atualizacao, [string]$Zip = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin  = Join-Path $root 'bin'
$data = Get-Date -Format 'yyyy-MM-dd'
$pkg  = Join-Path $root "dist\WacOS"
$zip  = if ($Zip) { $Zip } else { Join-Path $root "dist\WacOS-$data.zip" }
if ($Atualizacao) { $SemAjustes = $true }

if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force "$pkg\bin" | Out-Null

# programas: só o que o Shell usa (bin\ferramentas, bin\obj, logs e cópias .run/.old ficam de fora)
$progs = 'WacOSShell.exe', 'WacOSFinder.exe', 'WacOSSettings.exe', 'WacOSActivityMonitor.exe', 'WacOSTerminal.exe',
         'WacOSCalculator.exe', 'WacOSNotes.exe', 'WacOSScreenshot.exe', 'WacOSPreview.exe', 'WacOSReminders.exe', 'WacOSCalendar.exe', 'WacOSTextEdit.exe', 'WacOSClock.exe', 'WacOSWeather.exe', 'WacOSContacts.exe', 'WacOSPhotos.exe', 'WacOSTimeMachine.exe', 'WacOSMusic.exe', 'WacOSQuickTime.exe', 'WacOSForceQuit.exe', 'WacOSTitleBars.exe', 'WacOSWindow.dll', 'WacOSWindow32.dll',
         'DialogoMac.dll', 'DialogoMac32.dll'
if ($SemSistema) { $progs = $progs | Where-Object { $_ -notlike 'DialogoMac*' } }
$progs += 'versao.txt'  # a versão (o GitHub Actions grava; o atualizador compara)
foreach ($p in $progs) {
    if (Test-Path "$bin\$p") { Copy-Item "$bin\$p" "$pkg\bin\$p" } else { Write-Host "faltando: bin\$p" }
}

# extras opcionais do instalador: ícones do Mac no próprio Windows e janela de Abrir/Salvar do Mac
# (os scripts acham as coisas pela pasta de cima: <instalação>\bin\icones-sistema e <instalação>\docs)
# Os scripts de DESFAZER vão sempre (mesmo com -SemSistema): um PC que recebeu esses extras de um pacote antigo
# precisa deles para o desinstalar devolver os ícones e a janela do Windows
New-Item -ItemType Directory -Force "$pkg\tools" | Out-Null
foreach ($s in 'icones-sistema.ps1', 'icones-sistema-extra.ps1', 'dialogo-desligar.ps1', 'musica-registrar.ps1', 'quicktime-registrar.ps1', 'atualizar-github.ps1', 'baixar-apple.ps1') { Copy-Item "$PSScriptRoot\$s" "$pkg\tools\$s" }
if (-not $SemSistema) {
    New-Item -ItemType Directory -Force "$pkg\bin\icones-sistema" | Out-Null
    Get-ChildItem "$bin\icones-sistema" -File | Where-Object { $_.Extension -in '.ico', '.ps1' } | Copy-Item -Destination "$pkg\bin\icones-sistema"
    Copy-Item "$PSScriptRoot\dialogo-ligar.ps1" "$pkg\tools\dialogo-ligar.ps1"
}

# ícones e sons (a pasta inteira de cada, com icones\leopard: o tema Mac OS X Leopard saiu em 20/11/2026)
foreach ($pasta in 'icones', 'sons') {
    if (Test-Path "$root\$pasta") { robocopy "$root\$pasta" "$pkg\$pasta" /E /NFL /NDL /NJH /NJS /NP | Out-Null }
}
# o cursor do tema Leopard (o Shell aplica ao entrar no tema e devolve o anterior ao sair: ponteiro::TemaLeopard)
if (Test-Path "$root\cursor\Leopard") { robocopy "$root\cursor\Leopard" "$pkg\cursor-leopard\Leopard" /E /NFL /NDL /NJH /NJS /NP | Out-Null }

# fontes SF instaladas neste PC (por usuário e do sistema)
if (-not $Atualizacao) {
New-Item -ItemType Directory -Force "$pkg\fontes" | Out-Null
foreach ($d in (Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Fonts'), (Join-Path $env:WINDIR 'Fonts')) {
    # só as famílias e pesos que os apps usam (Text, Display, Rounded, Mono; Light a Heavy; itálico só no
    # Text, das Notas). Sem isso o pacote passava de 230 MB (o "SF Pro" variável sozinho tem 40 MB)
    Get-ChildItem $d -File -ErrorAction SilentlyContinue | Where-Object {
        $n = $_.Name
        $n -match '^SF' -and $_.Extension -in '.otf', '.ttf' -and $n -notmatch '^SF-Pro(-Italic)?\.ttf$' -and
        $n -notmatch '(?i)ultralight|thin|black' -and ($n -notmatch '(?i)italic' -or $n -match '^SF-Pro-Text')
    } |
        ForEach-Object { if (-not (Test-Path "$pkg\fontes\$($_.Name)")) { Copy-Item $_.FullName "$pkg\fontes\" } }
}

}

# cursor em uso (se for um esquema de arquivos próprio): a pasta dele e o mapa papel=arquivo
$creg = Get-ItemProperty 'HKCU:\Control Panel\Cursors'
# com o tema Leopard ligado neste PC o cursor em uso é o do Leopard (esse vai à parte, em cursor-leopard): vai o que o Shell guardou
# antes de trocar (%APPDATA%\Shell\cursor-antes.txt, papel=caminho), o cursor do Tahoe
$antes = Join-Path $env:APPDATA 'Shell\cursor-antes.txt'
if ((Split-Path (Split-Path $creg.Arrow) -Leaf) -eq 'Leopard' -and (Test-Path $antes)) {
    $creg = [pscustomobject]@{}
    foreach ($l in Get-Content $antes -Encoding utf8) { if ($l -match '^([^=]+)=(.*)$') { $creg | Add-Member -NotePropertyName $Matches[1] -NotePropertyValue $Matches[2] -Force } }
}
$arrow = $creg.Arrow
if (-not $Atualizacao -and $arrow -and (Test-Path $arrow)) {
    $cdir = Split-Path $arrow
    $nome = Split-Path $cdir -Leaf
    robocopy $cdir "$pkg\cursor\$nome" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    $mapa = foreach ($papel in 'AppStarting', 'Arrow', 'Crosshair', 'Hand', 'Help', 'IBeam', 'No', 'NWPen', 'Person', 'Pin', 'SizeAll',
                               'SizeNESW', 'SizeNS', 'SizeNWSE', 'SizeWE', 'UpArrow', 'Wait') {
        $v = $creg.$papel
        if ($v -and (Split-Path $v) -eq $cdir) { "$papel=$(Split-Path $v -Leaf)" }
    }
    Set-Content -Encoding utf8 "$pkg\cursor\cursores.txt" $mapa
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
if (Test-Path "$bin\WacOSSetup.exe") { Copy-Item "$bin\WacOSSetup.exe" "$pkg\WacOSSetup.exe" } else { Write-Host 'faltando: bin\WacOSSetup.exe' }
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $pkg -Recurse -Force
"Pronto: $zip ({0:N0} MB)" -f ((Get-Item $zip).Length / 1MB)
