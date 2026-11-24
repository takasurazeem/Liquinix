# Instala o WacOS neste PC (só para o seu usuário; não precisa de administrador).
#  - Programas em %LOCALAPPDATA%\Programs\WacOS (bin, icones, sons)
#  - Fontes SF Pro só para o seu usuário (se vierem na pasta "fontes")
#  - Atalho "WacOS" no Menu Iniciar e, se você quiser, início automático ao entrar no Windows
# Tudo o que muda fica anotado em %LOCALAPPDATA%\Programs\WacOS\instalacao.log
# Para desfazer: desinstalar.cmd
# -Atualizar (atualizar.cmd): só troca os programas, ícones e sons de uma instalação que já existe; não mexe em
# ajustes, fontes, cursor, atalho nem início automático, e não faz perguntas além de fechar o que está aberto.
# -Silencioso (o botão Atualizar Agora do Ajustes, tools\atualizar-github.ps1): sem perguntas; fecha o que estiver aberto
# O que instalar (perguntado no começo; fica em modo.txt e a atualização mantém): o WacOS completo, só os apps
# escolhidos (atalhos no Menu Iniciar, pasta "WacOS"), só as barras de título do Mac nos outros programas
# (WacOSTitleBars.exe) ou apps + barras. Fora do completo: a barra de tarefas do Windows, o Explorador e as pastas
# ficam como estão e não há tarefa de login do Shell.
param([switch]$Atualizar, [switch]$Silencioso)
$ErrorActionPreference = 'Stop'
$src  = $PSScriptRoot
$dest = Join-Path $env:LOCALAPPDATA 'Programs\WacOS'
$log  = Join-Path $dest 'instalacao.log'
# o nome antigo (Sequoia Shell, até 16/11/2026): a instalação velha é trocada por esta
$velho = Join-Path $env:LOCALAPPDATA 'Programs\SequoiaShell'
$temVelho = Test-Path (Join-Path $velho 'bin\SequoiaShell.exe')

function Pergunta($texto) { if ($Silencioso) { return $true }; (Read-Host "$texto (s/n)").Trim().ToLower().StartsWith('s') }
function Sair { if (-not $Silencioso) { Read-Host 'Enter para sair' }; exit }
function Anota($linha) { Add-Content -Encoding utf8 $log ("{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $linha) }

Write-Host ("WacOS - " + $(if ($Atualizar) { "atualizar`n" } else { "instalador`n" }))
# Rodado do código-fonte (o "Code > Download ZIP" do GitHub): não tem os programas compilados. O pacote certo é o
# WacOS.zip da página de versões (17/11/2026: no outro PC deu "Start-Process ... cannot find the file specified")
if (-not (Test-Path (Join-Path $src '..\bin\WacOSShell.exe')) -and -not (Test-Path (Join-Path $src 'bin\WacOSShell.exe'))) {
    Write-Host "Esta pasta é o código-fonte do WacOS, sem os programas compilados." -ForegroundColor Yellow
    Write-Host "This folder is the WacOS source code, without the compiled programs." -ForegroundColor Yellow
    Write-Host "`nBaixe o WacOS.zip pronto em / Download the ready-made WacOS.zip from:"
    Write-Host "  https://github.com/renzyxx/WacOS/releases/latest"
    Write-Host "`nExtraia e rode o instalar.cmd de dentro dele. / Extract it and run instalar.cmd from inside it."
    Start-Process 'https://github.com/renzyxx/WacOS/releases/latest' -ErrorAction SilentlyContinue
    Sair
}
if ($Atualizar -and -not (Test-Path (Join-Path $dest 'bin\WacOSShell.exe')) -and -not $temVelho) {
    Write-Host "O WacOS ainda não está instalado neste PC ($dest). Use instalar.cmd."
    Sair
}
$shellAberto = [bool](Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue)
$build = [Environment]::OSVersion.Version.Build
if ($build -lt 22621) {
    Write-Host "Aviso: este Windows (build $build) é mais antigo que o Windows 11 22H2. Partes do Shell (vidro, captura) podem não funcionar."
    if (-not (Pergunta 'Continuar mesmo assim?')) { exit }
}

# Shell aberto (atualização): fecha do jeito certo, devolvendo a barra do Windows. Quem fecha é o WacOSShell.exe do
# pacote (--sair, src\shell\ajudante.cpp), sem C# embutido no script (o antivírus estranhava)
# um WacOSShell.exe que já tem o modo ajudante (os antigos abririam o Shell em vez de ajudar)
function Ajudante([string[]]$lista) {
    foreach ($e in $lista) {
        if (-not (Test-Path $e)) { continue }
        $b = [IO.File]::ReadAllBytes($e)
        foreach ($o in 0, 1) { if ([Text.Encoding]::Unicode.GetString($b, $o, $b.Length - $o).Contains('--desfazer-janelas')) { return (Resolve-Path $e).Path } }
    }
    return $null
}
$ajudante = Ajudante @((Join-Path $src '..\bin\WacOSShell.exe'), (Join-Path $src 'bin\WacOSShell.exe'), (Join-Path $dest 'bin\WacOSShell.exe'))
if (Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue) {
    if (-not $Atualizar -and -not (Pergunta 'O WacOS está aberto. Fechar para instalar a versão nova?')) { exit }
    if ($Atualizar) { Write-Host 'Fechando o WacOS para atualizar (ele abre de novo no fim)...' }
    if ($ajudante) { Start-Process $ajudante -ArgumentList '--sair' -Wait }  # o mesmo do atalho "sair do Shell"
    else { Write-Host 'Feche o WacOS com Ctrl+Alt+Shift+F12. / Quit WacOS with Ctrl+Alt+Shift+F12.' }
    for ($i = 0; $i -lt 40 -and (Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    if (Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue) { Write-Host 'O Shell não fechou. Feche-o (Ctrl+Alt+Shift+F12) e rode o instalador de novo.'; Sair }
}
# os nossos apps abertos também seguram os arquivos
# só as barras (WacOSTitleBars): ao sair devolve as janelas ao Windows; o --sair pede isso
$barrasAbertas = [bool](Get-Process WacOSTitleBars -ErrorAction SilentlyContinue)
if ($barrasAbertas) {
    $tb = Join-Path $dest 'bin\WacOSTitleBars.exe'
    if (Test-Path $tb) { Start-Process $tb -ArgumentList '--sair' -Wait }
    for ($i = 0; $i -lt 20 -and (Get-Process WacOSTitleBars -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 250 }
}
$apps = 'WacOSFinder','WacOSSettings','WacOSActivityMonitor','WacOSTerminal','WacOSCalculator','WacOSNotes','WacOSScreenshot','WacOSWindow32Host',
        'WacOSPreview','WacOSReminders','WacOSCalendar','WacOSTextEdit','WacOSClock','WacOSWeather','WacOSContacts','WacOSPhotos',
        'WacOSTimeMachine','WacOSMusic','WacOSQuickTime','WacOSForceQuit','WacOSTitleBars',
        'SequoiaFinder','SequoiaSettings','SequoiaActivityMonitor','SequoiaTerminal','SequoiaCalculator','SequoiaNotes','SequoiaScreenshot','SequoiaWindow32Host'
$abertos = Get-Process $apps -ErrorAction SilentlyContinue
if ($abertos) {
    Write-Host ("Abertos: " + (($abertos | Select-Object -ExpandProperty ProcessName -Unique) -join ', '))
    if (-not (Pergunta 'Fechar esses apps?')) { exit }
    $abertos | Stop-Process -Force
    Start-Sleep 1
}

# 1. Programas, ícones e sons
New-Item -ItemType Directory -Force $dest | Out-Null
Anota "instalando de $src"
# cursor-leopard: o cursor do tema Mac OS X Leopard, em <instalação>\cursor\Leopard (o Shell aplica ao entrar no tema)
if (Test-Path "$src\cursor-leopard\Leopard") {
    robocopy "$src\cursor-leopard\Leopard" "$dest\cursor\Leopard" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Anota 'copiado: cursor\Leopard'
}
foreach ($pasta in 'bin', 'icones', 'sons', 'tools') {
    if (Test-Path "$src\$pasta") {
        robocopy "$src\$pasta" "$dest\$pasta" /E /NFL /NDL /NJH /NJS /NP /XF *.log | Out-Null
        Anota "copiado: $pasta"
    }
}
Get-ChildItem $dest -Recurse -File | Unblock-File  # veio da internet/zip: sem o aviso de "arquivo baixado"
# o hospedeiro de 32 bits virou o rundll32 do Windows (build-12): o exe antigo sai
Remove-Item "$dest\bin\WacOSWindow32Host.exe" -Force -ErrorAction SilentlyContinue
# o instalador novo (build-15) vai junto: o "Atualizar Agora" do Ajustes abre ele (sem ele, cai neste script)
if (Test-Path "$src\WacOSSetup.exe") { Copy-Item "$src\WacOSSetup.exe" "$dest\WacOSSetup.exe" -Force -ErrorAction SilentlyContinue; Unblock-File "$dest\WacOSSetup.exe" -ErrorAction SilentlyContinue }
Write-Host "Programas copiados para $dest"

# 1b. Instalação com o nome antigo (Sequoia Shell): a tarefa do login e o atalho passam para o WacOS e a pasta velha
#     sai. Os caminhos que o Windows guardava (Win+E, Música, Shell do Windows) o WacOSShell corrige ao abrir.
if ($temVelho) {
    $exeNovo = Join-Path $dest 'bin\WacOSShell.exe'
    $t = Get-ScheduledTask -TaskName 'SequoiaShell' -ErrorAction SilentlyContinue
    if ($t) {
        $a = New-ScheduledTaskAction -Execute $exeNovo -WorkingDirectory (Split-Path $exeNovo)
        Register-ScheduledTask -TaskName 'WacOS' -Action $a -Trigger $t.Triggers -Settings $t.Settings -Principal $t.Principal `
            -Description 'Inicia o WacOS ao entrar no Windows (desinstalar.cmd remove)' -Force | Out-Null
        Unregister-ScheduledTask -TaskName 'SequoiaShell' -Confirm:$false
        Anota 'tarefa agendada: SequoiaShell trocada por WacOS'
    }
    $lnkVelho = Join-Path ([Environment]::GetFolderPath('Programs')) 'Sequoia Shell.lnk'
    if (Test-Path $lnkVelho) {
        Remove-Item $lnkVelho
        $ws0 = New-Object -ComObject WScript.Shell
        $l0 = $ws0.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS.lnk'))
        $l0.TargetPath = $exeNovo; $l0.WorkingDirectory = Split-Path $exeNovo; $l0.IconLocation = "$exeNovo,0"
        $l0.Description = 'WacOS (barra de menus e Dock do Mac)'; $l0.Save()
        Anota 'atalho: Sequoia Shell.lnk trocado por WacOS.lnk'
    }
    # só os programas com o nome velho saem: o cursor do Mac e os ícones do sistema no registro apontam para
    # arquivos da pasta antiga (cursor\, bin\icones-sistema\), que ficam onde estão
    Get-ChildItem (Join-Path $velho 'bin') -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'Sequoia*' } | Remove-Item -Force -ErrorAction SilentlyContinue
    Anota "programas antigos (Sequoia*) removidos de $velho\bin; cursor e ícones do sistema continuam lá"
    Write-Host 'A instalação antiga (Sequoia Shell) foi trocada pelo WacOS.'
}

# 1d. O que instalar. Atualização e -Silencioso: o que já estava (modo.txt; sem ele, o completo)
$modoArq = Join-Path $dest 'modo.txt'
$modo = if (Test-Path $modoArq) { (Get-Content $modoArq -Encoding utf8 | Select-Object -First 1).Trim() } else { 'completo' }
# os apps que dá para instalar avulsos: exe|nome no Menu Iniciar
$catalogo = @(
    'WacOSFinder|Finder', 'WacOSSettings|System Settings', 'WacOSActivityMonitor|Activity Monitor', 'WacOSTerminal|Terminal',
    'WacOSCalculator|Calculator', 'WacOSNotes|Notes', 'WacOSScreenshot|Screenshot', 'WacOSPreview|Preview', 'WacOSReminders|Reminders',
    'WacOSCalendar|Calendar', 'WacOSTextEdit|TextEdit', 'WacOSClock|Clock', 'WacOSWeather|Weather', 'WacOSContacts|Contacts',
    'WacOSPhotos|Photos', 'WacOSTimeMachine|Time Machine', 'WacOSMusic|Music', 'WacOSQuickTime|QuickTime Player',
    'WacOSForceQuit|Force Quit Applications'
) | ForEach-Object { $p = $_ -split '\|'; [pscustomobject]@{ Exe = $p[0]; Nome = $p[1] } } | Where-Object { Test-Path (Join-Path $dest "bin\$($_.Exe).exe") }
if (-not $Atualizar -and -not $Silencioso) {
    Write-Host ""
    Write-Host "O que instalar? / What to install?"
    Write-Host "  1) WacOS completo (barra de menus, Dock, Finder...) / Full WacOS (menu bar, Dock, Finder...)"
    Write-Host "  2) Só os apps que eu escolher / Only the apps I choose"
    Write-Host "  3) Só as barras de título do Mac nos outros programas / Only the Mac title bars on other apps"
    Write-Host "  4) Apps + barras de título / Apps + title bars"
    $r = (Read-Host "1-4 [1]").Trim()
    $modo = switch ($r) { '2' { 'apps' } '3' { 'barras' } '4' { 'apps+barras' } default { 'completo' } }
}
$completo = $modo -eq 'completo'
$escolhidos = @()
if ($modo -like 'apps*') {
    $ja = @(Get-Content $modoArq -Encoding utf8 -ErrorAction SilentlyContinue | Select-Object -Skip 1)  # os escolhidos da vez anterior
    if ($Atualizar -or $Silencioso) { $escolhidos = @($catalogo | Where-Object { $ja -contains $_.Exe }) }
    else {
        Write-Host ""
        for ($i = 0; $i -lt $catalogo.Count; $i++) { Write-Host ("  {0,2}) {1}" -f ($i + 1), $catalogo[$i].Nome) }
        $r = Read-Host "Números separados por vírgula, ou Enter para todos / Numbers separated by commas, or Enter for all"
        if ($r.Trim()) { $escolhidos = @($r -split '[,; ]+' | Where-Object { $_ -match '^\d+$' -and [int]$_ -ge 1 -and [int]$_ -le $catalogo.Count } | ForEach-Object { $catalogo[[int]$_ - 1] }) }
        else { $escolhidos = @($catalogo) }
    }
}
Set-Content -Encoding utf8 $modoArq (@($modo) + @($escolhidos | ForEach-Object { $_.Exe }))
Anota ("modo: $modo " + (($escolhidos | ForEach-Object { $_.Exe }) -join ','))

if ($Atualizar) {  # só os arquivos; o resto (ajustes, fontes, cursor, atalho, início automático) fica como estava
    # Música/QuickTime já registrados por esta instalação: registra de novo (nomes em inglês desde a issue #2 do GitHub)
    foreach ($r in @(@('SequoiaMusic.Audio', 'musica-registrar.ps1', 'WacOSMusic.exe'), @('SequoiaQuickTime.Video', 'quicktime-registrar.ps1', 'WacOSQuickTime.exe'))) {
        $cmdAtual = (Get-ItemProperty "HKCU:\Software\Classes\$($r[0])\shell\open\command" -ErrorAction SilentlyContinue).'(default)'
        $scr = Join-Path $dest "tools\$($r[1])"
        if ($cmdAtual -and $cmdAtual -like "*$dest\bin\*" -and (Test-Path $scr)) { & $scr -Exe (Join-Path $dest "bin\$($r[2])") | Out-Null }
    }
    Anota 'atualização concluída'
    if ($barrasAbertas) { Start-Process (Join-Path $dest 'bin\WacOSTitleBars.exe') -WorkingDirectory (Join-Path $dest 'bin') }
    $exe = Join-Path $dest 'bin\WacOSShell.exe'
    if ($shellAberto) { Start-Process $exe -WorkingDirectory (Split-Path $exe); Write-Host "`nAtualizado. O WacOS abriu de novo." }
    elseif ($Silencioso) { Write-Host "`nAtualizado." }
    else { Write-Host "`nAtualizado."; if (Pergunta 'Abrir o WacOS agora?') { Start-Process $exe -WorkingDirectory (Split-Path $exe) } }
    exit
}

# 1c. O que é da Apple e não vem no pacote do GitHub (fontes SF, sons de alerta, cursor): baixado aqui, no próprio
#     PC, de onde a Apple e os autores publicam (tools\baixar-apple.ps1). O pacote completo feito no PC do autor já traz.
$baixar = Join-Path $src 'tools\baixar-apple.ps1'
if (-not (Test-Path $baixar)) { $baixar = Join-Path $dest 'tools\baixar-apple.ps1' }
$apple = Join-Path $env:TEMP 'wacos-apple'
$temFontes = [bool](Get-ChildItem "$src\fontes" -File -Include *.otf, *.ttf -Recurse -ErrorAction SilentlyContinue)
$temSons = [bool](Get-ChildItem "$dest\sons\mac" -Filter *.wav -ErrorAction SilentlyContinue)
$temCursor = [bool](Get-ChildItem "$src\cursor" -Directory -ErrorAction SilentlyContinue)
if ((Test-Path $baixar) -and -not $temFontes -and
    (Pergunta 'Baixar as fontes SF Pro e SF Mono do site da Apple (developer.apple.com/fonts; licença da Apple)?')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $baixar -Fontes "$apple\fontes"
}
if ($completo -and (Test-Path $baixar) -and -not $temSons -and
    (Pergunta 'Baixar os sons de alerta do macOS (github.com/extratone/macOSsystemsounds)?')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $baixar -Sons "$dest\sons\mac"
    Anota "sons do macOS baixados em $dest\sons\mac"
}
if ($completo -and (Test-Path $baixar) -and -not $temCursor -and
    (Pergunta 'Usar o cursor do macOS Tahoe (baixado do DeviantArt, autor jimmyxd2)?')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $baixar -Cursor "$apple\cursor"
}

# 2. Fontes (só para este usuário)
$fontes = @(Get-ChildItem "$src\fontes" -File -Include *.otf, *.ttf -Recurse -ErrorAction SilentlyContinue) +
          @(Get-ChildItem "$apple\fontes" -File -Include *.otf, *.ttf -Recurse -ErrorAction SilentlyContinue)
if ($fontes) {
    $fdir = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Fonts'
    $freg = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts'
    New-Item -ItemType Directory -Force $fdir | Out-Null
    if (-not (Test-Path $freg)) { New-Item -Force $freg | Out-Null }
    $n = 0
    foreach ($f in $fontes) {
        $alvo = Join-Path $fdir $f.Name
        if (-not (Test-Path $alvo)) { Copy-Item $f.FullName $alvo; Anota "fonte: $alvo" }
        $nome = "$($f.BaseName) ($(if ($f.Extension -eq '.otf') { 'OpenType' } else { 'TrueType' }))"
        if ((Get-ItemProperty $freg -Name $nome -ErrorAction SilentlyContinue).$nome -ne $alvo) {
            Set-ItemProperty $freg -Name $nome -Value $alvo
            Anota "registro: $freg | $nome = $alvo"
        }
        $n++
    }
    Write-Host "Fontes instaladas: $n"
}

# 2b. Cursor do Mac (só para este usuário; o anterior fica anotado e o desinstalar devolve)
$curRaiz = "$src\cursor"
if (-not (Get-ChildItem $curRaiz -Directory -ErrorAction SilentlyContinue)) { $curRaiz = "$apple\cursor" }  # o baixado agora
$cur = Get-ChildItem $curRaiz -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($completo -and $cur -and ($curRaiz -like "$apple*" -or (Pergunta "Usar o cursor do Mac ($($cur.Name))?"))) {
    $cdir = Join-Path $dest "cursor\$($cur.Name)"
    robocopy $cur.FullName $cdir /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Get-ChildItem $cdir -File | Unblock-File
    $creg = 'HKCU:\Control Panel\Cursors'
    $mapa = Get-Content "$curRaiz\cursores.txt" -Encoding utf8  # papel=arquivo (ex.: Arrow=arrowS.cur)
    $antes = Get-ItemProperty $creg
    foreach ($linha in $mapa) {
        if ($linha -notmatch '^([^=]+)=(.+)$') { continue }
        $papel = $Matches[1]; $arq = Join-Path $cdir $Matches[2]
        if (-not (Test-Path $arq)) { continue }
        Anota ("cursor-antes: {0}={1}" -f $papel, $antes.$papel)
        Set-ItemProperty $creg -Name $papel -Value $arq
    }
    Anota ("cursor-antes: (default)={0}" -f $antes.'(default)')
    Set-ItemProperty $creg -Name '(default)' -Value $cur.Name
    Set-ItemProperty $creg -Name 'Scheme Source' -Value 1 -Type DWord
    Anota "registro: $creg (cursor do Mac)"
    if ($ajudante = Ajudante @((Join-Path $dest 'bin\WacOSShell.exe'))) { Start-Process $ajudante -ArgumentList '--aplicar-cursor' -Wait }  # SPI_SETCURSORS: aplica agora
    Write-Host 'Cursor do Mac aplicado.'
}

Remove-Item $apple -Recurse -Force -ErrorAction SilentlyContinue  # o baixado já foi copiado

# 2c. Só os apps e/ou só as barras de título: atalhos no Menu Iniciar (pasta "WacOS") e, para as barras, o início
#     automático próprio (tarefa "WacOS Title Bars"). Nada do Shell: sem trocar a barra de tarefas, o Explorador
#     ou as pastas, e sem a tarefa "WacOS". Os apps leem o tema em %APPDATA%\Shell\ajustes.txt (Ajustes do Sistema).
if (-not $completo) {
    $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS'
    New-Item -ItemType Directory -Force $menu | Out-Null
    Get-ChildItem $menu -Filter *.lnk -ErrorAction SilentlyContinue | Remove-Item -Force  # refeitos pela escolha de agora
    Anota "menu iniciar: $menu"
    $ws = New-Object -ComObject WScript.Shell
    function Atalho($nome, $exe) {
        $l = $ws.CreateShortcut((Join-Path $menu "$nome.lnk"))
        $l.TargetPath = $exe; $l.WorkingDirectory = Split-Path $exe; $l.IconLocation = "$exe,0"; $l.Save()
    }
    # quem vinha do WacOS completo: o Shell não abre mais sozinho
    if (Get-ScheduledTask -TaskName 'WacOS' -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName 'WacOS' -Confirm:$false
        Anota 'tarefa agendada WacOS removida (modo sem o Shell)'
    }
    Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS.lnk') -ErrorAction SilentlyContinue
    foreach ($a in $escolhidos) { Atalho $a.Nome (Join-Path $dest "bin\$($a.Exe).exe") }
    if ($escolhidos) { Write-Host ("Apps no Menu Iniciar (pasta WacOS) / Apps in the Start menu (WacOS folder): " + (($escolhidos | ForEach-Object { $_.Nome }) -join ', ')) }
    if ($modo -like '*barras') {
        $tb = Join-Path $dest 'bin\WacOSTitleBars.exe'
        Atalho 'WacOS Title Bars' $tb
        if (Pergunta 'Ligar as barras de título sozinhas toda vez que você entrar no Windows? / Start the title bars at every sign-in?') {
            $user = "$env:USERDOMAIN\$env:USERNAME"
            $action = New-ScheduledTaskAction -Execute $tb -WorkingDirectory (Split-Path $tb)
            $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
            $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
            $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
            Register-ScheduledTask -TaskName 'WacOS Title Bars' -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
                -Description 'Mac title bars on other apps, without the WacOS Shell (desinstalar.cmd removes it)' -Force | Out-Null
            Anota 'tarefa agendada: WacOS Title Bars (ao entrar)'
        }
        Start-Process $tb -WorkingDirectory (Split-Path $tb)
        Write-Host 'Barras de título ligadas. Desligar: WacOSTitleBars.exe --sair (ou desinstalar.cmd).'
        Write-Host 'Title bars are on. Turn off: WacOSTitleBars.exe --sair (or desinstalar.cmd).'
    }
    if (($escolhidos | Where-Object { $_.Exe -eq 'WacOSMusic' }) -and (Test-Path "$dest\tools\musica-registrar.ps1") -and
        (Pergunta 'Registrar o Música para abrir arquivos de áudio? / Register Music for audio files?')) {
        & (Join-Path $dest 'tools\musica-registrar.ps1') -Exe (Join-Path $dest 'bin\WacOSMusic.exe') -AbrirAjustes
        Anota 'Música registrado como app de áudio (desinstalar.cmd remove)'
    }
    if (($escolhidos | Where-Object { $_.Exe -eq 'WacOSQuickTime' }) -and (Test-Path "$dest\tools\quicktime-registrar.ps1") -and
        (Pergunta 'Registrar o QuickTime Player para abrir vídeos? / Register QuickTime Player for videos?')) {
        & (Join-Path $dest 'tools\quicktime-registrar.ps1') -Exe (Join-Path $dest 'bin\WacOSQuickTime.exe') -AbrirAjustes
        Anota 'QuickTime registrado como app de vídeo (desinstalar.cmd remove)'
    }
    Anota 'instalação concluída (sem o Shell)'
    Write-Host "`nPronto / Done. Desinstalar / Uninstall: desinstalar.cmd"
    Sair
}

# 3. Ajustes (se vierem no pacote e este PC ainda não tiver os seus)
$cfg = Join-Path $env:APPDATA 'Shell'
if (Test-Path "$src\ajustes") {
    New-Item -ItemType Directory -Force $cfg | Out-Null
    foreach ($f in Get-ChildItem "$src\ajustes" -File) {
        $alvo = Join-Path $cfg $f.Name
        if (-not (Test-Path $alvo)) { Copy-Item $f.FullName $alvo; Anota "ajuste: $alvo" }
    }
}

# 3b. PC novo sem ajustes: o modo do Mac completo. explorer=fundo = a área de trabalho e o botão direito são
#     os do Shell (o Explorador fica escondido, rodando só para os apps da Loja); pastas abrem no Finder.
$ajustes = Join-Path $cfg 'ajustes.txt'
if (-not (Test-Path $ajustes)) {
    New-Item -ItemType Directory -Force $cfg | Out-Null
    Set-Content -Encoding utf8 $ajustes "explorer=fundo`nfinder_padrao=1"
    Anota "ajuste: $ajustes (padrões: explorer=fundo, finder_padrao=1)"
}
# pastas, discos e Win+E abrem no Finder (só HKCU; o desinstalar devolve)
$finder = Join-Path $dest 'bin\WacOSFinder.exe'
if ((Get-Content $ajustes -ErrorAction SilentlyContinue) -notcontains 'finder_padrao=0') {
    Start-Process $finder -ArgumentList '--quieto', '--padrao', 'on' -Wait
    Anota 'pastas no Finder: WacOSFinder.exe --padrao on'
}

# 3c. Extras opcionais (cada um tem desfazer no desinstalar.cmd)
New-Item -ItemType Directory -Force (Join-Path $dest 'docs') | Out-Null   # backups dos extras ficam aqui
$tools = Join-Path $dest 'tools'
if ((Test-Path "$tools\dialogo-ligar.ps1") -and (Test-Path "$src\bin\DialogoMac.dll") -and (Pergunta 'Usar a janela de Abrir/Salvar do Mac nos programas?')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$tools\dialogo-ligar.ps1" | Out-Null
    Anota 'extra: dialogo do Mac ligado (tools\dialogo-ligar.ps1)'
    Write-Host "Janela de Abrir/Salvar do Mac ligada (os programas que a usam ficam em $cfg\dialogo.txt; '*' = todos)."
}
if ((Test-Path "$tools\icones-sistema.ps1") -and (Test-Path "$src\bin\icones-sistema") -and (Pergunta 'Trocar os ícones do próprio Windows (pastas, discos, Lixeira, tipos de arquivo) pelos do Mac?')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$tools\icones-sistema.ps1" | Out-Null
    Anota 'extra: icones do sistema (HKCU) aplicados (tools\icones-sistema.ps1)'
    if (Pergunta 'Também nas pastas do usuário (Documentos, Downloads...) e no ícone padrão de pasta do sistema? (pede administrador)') {
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$tools\icones-sistema-extra.ps1" | Out-Null
        Anota 'extra: icones das pastas e HKLM aplicados (tools\icones-sistema-extra.ps1)'
    }
    Write-Host 'Ícones do Mac aplicados (podem levar alguns segundos para aparecer).'
}

# 4. Atalho no Menu Iniciar
$exe = Join-Path $dest 'bin\WacOSShell.exe'
$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS.lnk'
$ws = New-Object -ComObject WScript.Shell
$l = $ws.CreateShortcut($lnk)
$l.TargetPath = $exe; $l.WorkingDirectory = Split-Path $exe; $l.IconLocation = "$exe,0"
$l.Description = 'WacOS (barra de menus e Dock do Mac)'
$l.Save()
Anota "atalho: $lnk"
Write-Host 'Atalho "WacOS" criado no Menu Iniciar (para fixar na barra: botão direito > Fixar na barra de tarefas)'

# 5. Iniciar com o Windows
if (Pergunta 'Abrir o WacOS sozinho toda vez que você entrar no Windows?') {
    $user = "$env:USERDOMAIN\$env:USERNAME"
    $action = New-ScheduledTaskAction -Execute $exe -WorkingDirectory (Split-Path $exe)
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -Priority 4 -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName 'WacOS' -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
        -Description 'Inicia o WacOS ao entrar no Windows (desinstalar.cmd remove)' -Force | Out-Null
    Anota 'tarefa agendada: WacOS (ao entrar)'
    Write-Host 'Início automático ligado.'
}

# 6. Música como app de áudio (aparece em "Abrir com" e nos apps padrão do Windows; o padrão quem escolhe é você)
$mr = Join-Path $dest 'tools\musica-registrar.ps1'
if ((Test-Path $mr) -and (Pergunta 'Registrar o Música para abrir arquivos de áudio (você escolhe como padrão nos Ajustes do Windows)?')) {
    & $mr -Exe (Join-Path $dest 'bin\WacOSMusic.exe') -AbrirAjustes
    Anota 'Música registrado como app de áudio (HKCU\Software\Classes\SequoiaMusic.Audio; desinstalar.cmd remove)'
}

$qr = Join-Path $dest 'tools\quicktime-registrar.ps1'
if ((Test-Path $qr) -and (Pergunta 'Registrar o QuickTime Player para abrir vídeos (você escolhe como padrão nos Ajustes do Windows)?')) {
    & $qr -Exe (Join-Path $dest 'bin\WacOSQuickTime.exe') -AbrirAjustes
    Anota 'QuickTime registrado como app de vídeo (HKCU\Software\Classes\SequoiaQuickTime.Video; desinstalar.cmd remove)'
}

Anota 'instalação concluída'
Write-Host "`nPronto. Saída do Shell: Ctrl+Alt+Shift+F12. Emergência (devolve o Explorador): Ctrl+Alt+Shift+F11."
if (Pergunta 'Abrir o WacOS agora?') { Start-Process $exe -WorkingDirectory (Split-Path $exe) }
