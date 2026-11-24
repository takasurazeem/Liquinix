# Registra o QuickTime Player (WacOSQuickTime.exe) como app de vídeo do usuário (HKCU): aparece em "Abrir com" e em
# Ajustes > Aplicativos > Aplicativos padrão, onde o usuário escolhe o QuickTime como padrão (o Windows não deixa
# um programa trocar o padrão sozinho). -Desfazer apaga tudo o que foi criado aqui.
#   powershell -File tools\quicktime-registrar.ps1 [-Exe <caminho do WacOSQuickTime.exe>] [-Desfazer] [-AbrirAjustes]
param([string]$Exe = (Join-Path (Split-Path $PSScriptRoot -Parent) 'bin\WacOSQuickTime.exe'), [switch]$Desfazer, [switch]$AbrirAjustes)
$ErrorActionPreference = 'Stop'
$prog = 'SequoiaQuickTime.Video'
$exts = '.mp4', '.mov', '.m4v', '.mkv', '.avi', '.wmv', '.webm', '.mpg', '.mpeg', '.3gp'
$cls = 'HKCU:\Software\Classes'
$capKey = 'HKCU:\Software\SequoiaShell\QuickTime\Capabilities'
$regName = 'WacOS QuickTime Player'
$regAntigo = 'QuickTime Player (Sequoia)'  # nome das versões antigas

if ($Desfazer) {
    foreach ($e in $exts) { Remove-ItemProperty "$cls\$e\OpenWithProgids" -Name $prog -ErrorAction SilentlyContinue }
    Remove-Item "$cls\$prog" -Recurse -ErrorAction SilentlyContinue
    Remove-Item "$cls\Applications\WacOSQuickTime.exe" -Recurse -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name $regName, $regAntigo -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\SequoiaShell\QuickTime' -Recurse -ErrorAction SilentlyContinue
    "QuickTime: registro desfeito"
} else {
    if (-not (Test-Path $Exe)) { throw "não achei $Exe" }
    $cmd = "`"$Exe`" `"%1`""
    New-Item "$cls\$prog\shell\open\command" -Force | Out-Null
    Set-ItemProperty "$cls\$prog" -Name '(default)' -Value 'Movie'
    Set-ItemProperty "$cls\$prog" -Name 'FriendlyTypeName' -Value 'Movie'
    New-Item "$cls\$prog\DefaultIcon" -Force | Out-Null
    Set-ItemProperty "$cls\$prog\DefaultIcon" -Name '(default)' -Value "`"$Exe`",0"
    Set-ItemProperty "$cls\$prog\shell\open\command" -Name '(default)' -Value $cmd
    New-Item "$cls\Applications\WacOSQuickTime.exe\shell\open\command" -Force | Out-Null
    Set-ItemProperty "$cls\Applications\WacOSQuickTime.exe" -Name 'FriendlyAppName' -Value 'QuickTime Player'
    Set-ItemProperty "$cls\Applications\WacOSQuickTime.exe\shell\open\command" -Name '(default)' -Value $cmd
    New-Item "$cls\Applications\WacOSQuickTime.exe\SupportedTypes" -Force | Out-Null
    New-Item "$capKey\FileAssociations" -Force | Out-Null
    Set-ItemProperty $capKey -Name 'ApplicationName' -Value 'QuickTime Player'
    Set-ItemProperty $capKey -Name 'ApplicationDescription' -Value 'WacOS QuickTime Player'
    Set-ItemProperty $capKey -Name 'ApplicationIcon' -Value "`"$Exe`",0"
    foreach ($e in $exts) {
        # sem -Force numa chave que já existe: o New-Item -Force recria a chave e apagava os outros apps dela
        if (-not (Test-Path "$cls\$e\OpenWithProgids")) { New-Item "$cls\$e\OpenWithProgids" -Force | Out-Null }
        New-ItemProperty "$cls\$e\OpenWithProgids" -Name $prog -PropertyType None -Value ([byte[]]@()) -Force | Out-Null
        Set-ItemProperty "$cls\Applications\WacOSQuickTime.exe\SupportedTypes" -Name $e -Value ''
        Set-ItemProperty "$capKey\FileAssociations" -Name $e -Value $prog
    }
    # idem: com -Force a chave era recriada e todos os apps registrados no Windows sumiam dela
    if (-not (Test-Path 'HKCU:\Software\RegisteredApplications')) { New-Item 'HKCU:\Software\RegisteredApplications' -Force | Out-Null }
    Remove-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name $regAntigo -ErrorAction SilentlyContinue
    Set-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name $regName -Value 'Software\SequoiaShell\QuickTime\Capabilities'
    "QuickTime registrado ($Exe)"
}
# avisa o Explorador que as associações mudaram
# avisar o Explorador: pelo WacOSShell.exe (modo ajudante), sem C# embutido no script
$ajudante = Join-Path $PSScriptRoot '..\bin\WacOSShell.exe'
function AvisarIcones { if (Test-Path $ajudante) { Start-Process $ajudante -ArgumentList '--avisar-icones' -Wait } }
AvisarIcones
if ($AbrirAjustes -and -not $Desfazer) { Start-Process ("ms-settings:defaultapps?registeredAppUser=" + [uri]::EscapeDataString($regName)) }
