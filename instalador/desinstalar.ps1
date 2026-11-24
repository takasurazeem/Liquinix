# Remove o WacOS instalado pelo instalar.ps1: fecha o Shell (devolvendo a barra do Windows), tira o
# início automático, o atalho, as fontes que o instalador colocou e a pasta dos programas.
# Seus ajustes (%APPDATA%\Shell) só saem se você disser que sim.
$ErrorActionPreference = 'Continue'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\WacOS'
$log  = Join-Path $dest 'instalacao.log'
function Pergunta($texto) { (Read-Host "$texto (s/n)").Trim().ToLower().StartsWith('s') }

Write-Host "WacOS - desinstalar`n"
if (-not (Pergunta "Remover o WacOS de $dest?")) { exit }

# as chamadas do Windows (fechar o Shell, devolver as janelas, cursor) são feitas pelo próprio WacOSShell.exe (modo
# ajudante, src\shell\ajudante.cpp), sem C# embutido no script (o antivírus estranhava)
# um WacOSShell.exe que já tem o modo ajudante (os antigos abririam o Shell em vez de ajudar)
function Ajudante([string[]]$lista) {
    foreach ($e in $lista) {
        if (-not (Test-Path $e)) { continue }
        $b = [IO.File]::ReadAllBytes($e)
        foreach ($o in 0, 1) { if ([Text.Encoding]::Unicode.GetString($b, $o, $b.Length - $o).Contains('--desfazer-janelas')) { return (Resolve-Path $e).Path } }
    }
    return $null
}
$ajudante = Ajudante @((Join-Path $dest 'bin\WacOSShell.exe'), (Join-Path $PSScriptRoot 'bin\WacOSShell.exe'), (Join-Path $PSScriptRoot '..\bin\WacOSShell.exe'))
if ($ajudante -and -not $ajudante.StartsWith($dest)) {  # o do pacote: copiado para fora, a pasta da instalação sai no fim
    $c = Join-Path $env:TEMP 'WacOSShell-ajudante.exe'; Copy-Item $ajudante $c -Force; $ajudante = $c
}
if (Get-Process WacOSShell -ErrorAction SilentlyContinue) {
    if ($ajudante) { Start-Process $ajudante -ArgumentList '--sair' -Wait }
    for ($i = 0; $i -lt 40 -and (Get-Process WacOSShell -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
}
if (Get-Process WacOSShell -ErrorAction SilentlyContinue) { Write-Host 'O Shell não fechou: feche com Ctrl+Alt+Shift+F12 e rode de novo.'; Read-Host 'Enter para sair'; exit }

# Só as barras de título (WacOSTitleBars, instalação sem o Shell): sai devolvendo as janelas ao Windows
if (Get-Process WacOSTitleBars -ErrorAction SilentlyContinue) {
    $tb = Join-Path $dest 'bin\WacOSTitleBars.exe'
    if (Test-Path $tb) { Start-Process $tb -ArgumentList '--sair' -Wait }
    for ($i = 0; $i -lt 20 -and (Get-Process WacOSTitleBars -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 250 }
    Get-Process WacOSTitleBars -ErrorAction SilentlyContinue | Stop-Process -Force
}

# Janelas dos apps com a barra do Mac: o Shell manda cada uma voltar ao normal ao sair, mas com pouco tempo
# (300 ms) e uma vez só; app ocupado ou janela escondida ficava com a barra do Mac. Aqui manda de novo, com
# mais tempo, a todas que ainda estão marcadas, e confere.
if ($ajudante) {
    $sobrou = (Start-Process $ajudante -ArgumentList '--desfazer-janelas' -Wait -PassThru).ExitCode
    if ($sobrou -gt 0) { Write-Host "$sobrou janela(s) não responderam (app travado): voltam ao normal quando o app for fechado e aberto de novo." }
}
Get-Process WacOSFinder, WacOSSettings, WacOSActivityMonitor, WacOSTerminal, WacOSCalculator, WacOSNotes, WacOSScreenshot, WacOSWindow32Host,
            WacOSPreview, WacOSReminders, WacOSCalendar, WacOSTextEdit, WacOSClock, WacOSWeather, WacOSContacts, WacOSPhotos,
            WacOSTimeMachine, WacOSMusic, WacOSQuickTime, WacOSForceQuit -ErrorAction SilentlyContinue | Stop-Process -Force
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }

# barra do Windows nos outros monitores (o Shell desliga enquanto roda e devolve ao sair; se não devolveu, devolve aqui)
$cfgAj = Join-Path $env:APPDATA 'Shell\ajustes.txt'
$antes = if (Test-Path $cfgAj) { (Get-Content $cfgAj -Encoding utf8 | Where-Object { $_ -match '^barra_windows_monitores_antes=(.+)$' } | Select-Object -First 1) } else { $null }
if ($antes -match '=(.+)$') {
    $v = $Matches[1].Trim(); $adv = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
    if ($v -eq 'x') { Remove-ItemProperty $adv -Name MMTaskbarEnabled -ErrorAction SilentlyContinue } else { Set-ItemProperty $adv -Name MMTaskbarEnabled -Value ([int]$v) -Type DWord }
    (Get-Content $cfgAj -Encoding utf8) | Where-Object { $_ -notmatch '^barra_windows_monitores_antes=' } | Set-Content $cfgAj -Encoding utf8
    Write-Host 'Barra do Windows nos outros monitores devolvida.'
}

Unregister-ScheduledTask -TaskName 'WacOS' -Confirm:$false -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'SequoiaShell' -Confirm:$false -ErrorAction SilentlyContinue  # nome antigo
Unregister-ScheduledTask -TaskName 'WacOS Title Bars' -Confirm:$false -ErrorAction SilentlyContinue  # só as barras de título
# pastas voltam a abrir no Explorador (desfaz o --padrao on)
$finder = Join-Path $dest 'bin\WacOSFinder.exe'
if (Test-Path $finder) { Start-Process $finder -ArgumentList '--quieto', '--padrao', 'off' -Wait }
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS') -Recurse -Force -ErrorAction SilentlyContinue  # atalhos dos apps avulsos

# extras: janela de Abrir/Salvar e ícones do sistema voltam aos do Windows. Decide pelo que está aplicado de
# verdade (chaves e backups), não só pelo log: instalação antiga, atualizada ou com o log perdido também volta.
$dlg = $false
foreach ($raiz in 'HKCU:\Software\Classes\CLSID', 'HKCU:\Software\Classes\WOW6432Node\CLSID') {
    foreach ($clsid in '{DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7}', '{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}') {
        $k = "$raiz\$clsid"
        if ((Test-Path $k) -and ((Get-ItemProperty "$k\InprocServer32" -ErrorAction SilentlyContinue).'(default)' -match 'DialogoMac')) { Remove-Item -Recurse -Force $k; $dlg = $true }
    }
}
if ($dlg) { Write-Host 'Janela de Abrir/Salvar do Windows devolvida.' }
$docs = Join-Path $dest 'docs'
if ((Test-Path "$docs\backup-icones-sistema.json") -or (Test-Path "$docs\backup-desktopini") -or (Test-Path "$docs\backup-icones-maquina.json")) {
    if (Test-Path "$dest\tools\icones-sistema.ps1") {
        Write-Host 'Devolvendo os ícones do Windows (se pedir administrador, aceite: é para desfazer a parte das pastas)...'
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$dest\tools\icones-sistema.ps1" -Desfazer
        Get-Content "$dest\bin\icones-sistema\ultima-execucao*.txt" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }
        if ((Test-Path "$docs\backup-icones-sistema.json") -or (Test-Path "$docs\backup-desktopini") -or (Test-Path "$docs\backup-icones-maquina.json")) {
            Write-Host 'Parte dos ícones não voltou. A pasta da instalação fica (os backups estão nela); rode o desinstalar de novo.'
            Read-Host 'Enter para sair'; exit
        }
        Write-Host 'Ícones do Windows devolvidos.'
    } else { Write-Host "Os ícones do Mac estão aplicados, mas falta $dest\tools\icones-sistema.ps1 para desfazer. Nada foi apagado."; Read-Host 'Enter para sair'; exit }
}

# tema Leopard ligado: o cursor dele (em $dest\cursor\Leopard, que vai ser apagado) dá lugar ao que o Shell guardou
# ao entrar no tema (%APPDATA%\Shell\cursor-antes.txt, papel=caminho)
$curAntes = Join-Path $env:APPDATA 'Shell\cursor-antes.txt'
$curReg = 'HKCU:\Control Panel\Cursors'
if ((Test-Path $curAntes) -and ((Get-ItemProperty $curReg -ErrorAction SilentlyContinue).Arrow -like "$dest\cursor\Leopard\*")) {
    foreach ($l in Get-Content $curAntes -Encoding utf8) { if ($l -match '^([^=]+)=(.*)$') { Set-ItemProperty $curReg -Name $Matches[1] -Value $Matches[2] } }
    Remove-Item $curAntes -ErrorAction SilentlyContinue
    if ($ajudante) { Start-Process $ajudante -ArgumentList '--aplicar-cursor' -Wait }
    Write-Host 'Cursor de antes do tema Leopard devolvido.'
}

# cursor: volta o que estava antes
if (Test-Path $log) {
    $creg = 'HKCU:\Control Panel\Cursors'
    $voltou = $false
    foreach ($l in Get-Content $log) {
        if ($l -match 'cursor-antes: ([^=]+)=(.*)$') { Set-ItemProperty $creg -Name $Matches[1] -Value $Matches[2]; $voltou = $true }
    }
    if ($voltou) {
        if ($ajudante) { Start-Process $ajudante -ArgumentList '--aplicar-cursor' -Wait }
    }
}

# fontes e entradas de registro que o instalador anotou
if (Test-Path $log) {
    foreach ($l in Get-Content $log) {
        if ($l -match 'registro: (.+?) \| (.+?) = (.+)$') { Remove-ItemProperty -Path $Matches[1] -Name $Matches[2] -ErrorAction SilentlyContinue }
        elseif ($l -match 'fonte: (.+)$') { Remove-Item $Matches[1] -ErrorAction SilentlyContinue }
    }
}
Remove-Item 'HKCU:\Software\ShellProprio' -Recurse -Force -ErrorAction SilentlyContinue  # cores das barras e opções da DLL das janelas
Remove-Item 'HKCU:\Software\Classes\AppUserModelId\WacOS.Atualizacao' -Recurse -Force -ErrorAction SilentlyContinue  # remetente do aviso de atualização
# QuickTime registrado como app de vídeo (tools\quicktime-registrar.ps1)
$qr = Join-Path $dest 'tools\quicktime-registrar.ps1'; if (Test-Path $qr) { & $qr -Desfazer }
# Música registrado como app de áudio (tools\musica-registrar.ps1): sai de "Abrir com" e dos apps padrão
$mr = Join-Path $dest 'tools\musica-registrar.ps1'
if (Test-Path $mr) { & $mr -Desfazer } else {
    foreach ($e in '.mp3', '.m4a', '.flac', '.wav', '.aac', '.ogg', '.wma', '.aif', '.aiff', '.opus') { Remove-ItemProperty "HKCU:\Software\Classes\$e\OpenWithProgids" -Name 'SequoiaMusic.Audio' -ErrorAction SilentlyContinue }
    Remove-Item 'HKCU:\Software\Classes\SequoiaMusic.Audio', 'HKCU:\Software\Classes\Applications\WacOSMusic.exe', 'HKCU:\Software\SequoiaShell\Musica' -Recurse -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name 'Música (Sequoia)', 'WacOS Music' -ErrorAction SilentlyContinue
}
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path $dest) { Write-Host "Alguns arquivos estavam em uso; apague $dest depois de reiniciar o PC." }

$cfg = Join-Path $env:APPDATA 'Shell'
if ((Test-Path $cfg) -and (Pergunta "Apagar também os seus ajustes ($cfg)?")) { Remove-Item $cfg -Recurse -Force -ErrorAction SilentlyContinue }

# Limpeza completa (opcional, numa caixa): o Windows guarda os ícones do Mac no cache de ícones e de miniaturas
# e eles continuavam aparecendo depois de desinstalar; também sobram pastas e chaves de versões antigas.
Add-Type -AssemblyName System.Windows.Forms
$resp = [System.Windows.Forms.MessageBox]::Show(
    "Limpar também o cache de ícones e miniaturas do Windows e o que sobrou do WacOS?`n`n" +
    "- cache de ícones e de miniaturas (o Windows refaz sozinho; ícones do Mac que ficaram presos somem)`n" +
    "- pastas de versões antigas (Sequoia Shell), do Finder e de binários antigos`n" +
    "- chaves do registro do WacOS que sobraram`n`n" +
    "A área de trabalho e a barra de tarefas do Windows piscam por alguns segundos.",
    'WacOS - limpeza', [System.Windows.Forms.MessageBoxButtons]::YesNo, [System.Windows.Forms.MessageBoxIcon]::Question)
if ($resp -eq [System.Windows.Forms.DialogResult]::Yes) {
    # o Explorador segura os arquivos de cache: fecha, apaga e abre de novo
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    $exp = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Explorer'
    Remove-Item (Join-Path $env:LOCALAPPDATA 'IconCache.db') -Force -ErrorAction SilentlyContinue
    Get-ChildItem $exp -Filter 'iconcache_*.db' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem $exp -Filter 'thumbcache_*.db' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Start-Process explorer.exe
    Start-Process ie4uinit.exe -ArgumentList '-show' -ErrorAction SilentlyContinue  # avisa o Windows para reler os ícones
    # sobras: instalação antiga (só se o cursor do Mac não estiver mais em uso), Finder, binários antigos, registro
    $velho = Join-Path $env:LOCALAPPDATA 'Programs\SequoiaShell'
    $cursorEmUso = (Get-ItemProperty 'HKCU:\Control Panel\Cursors' -ErrorAction SilentlyContinue).Arrow -like "$velho*"
    if ((Test-Path $velho) -and -not $cursorEmUso) { Remove-Item $velho -Recurse -Force -ErrorAction SilentlyContinue }
    foreach ($p in (Join-Path $env:APPDATA 'Arquivos'), (Join-Path $env:LOCALAPPDATA 'SequoiaBinariosAntigos')) {
        if (Test-Path $p) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
    }
    foreach ($k in 'HKCU:\Software\SequoiaShell', 'HKCU:\Software\Classes\SequoiaMusic.Audio', 'HKCU:\Software\Classes\SequoiaQuickTime.Video',
                   'HKCU:\Software\Classes\Applications\WacOSMusic.exe', 'HKCU:\Software\Classes\Applications\WacOSQuickTime.exe',
                   'HKCU:\Software\Classes\Applications\SequoiaMusic.exe', 'HKCU:\Software\Classes\Applications\SequoiaQuickTime.exe') {
        Remove-Item $k -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host 'Cache de ícones e sobras do WacOS limpos.'
}
Write-Host 'WacOS removido.'
Read-Host 'Enter para fechar'
