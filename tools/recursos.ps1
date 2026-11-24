# Gera os recursos de cada programa: ícone (.ico feito do PNG do pacote em icones\sequoia) e VERSIONINFO
# com o nome em inglês que o Gerenciador de Tarefas mostra. Chamado pelo build.cmd; saída em bin\obj\res.
# Sem o pacote de ícones (pasta icones\sequoia), gera só a versão (o .exe fica com o ícone padrão).
param([string]$Out = (Join-Path $PSScriptRoot '..\bin\obj\res'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$pack = Join-Path $PSScriptRoot '..\icones\sequoia'
New-Item -ItemType Directory -Force $Out | Out-Null

# nome do arquivo | descrição (Gerenciador de Tarefas) | PNG do pacote
$itens = @(
    'WacOSShell|WacOS (Menu Bar, Dock and Spotlight)|Launchpad.png',
    'WacOSFinder|Finder|Finder.png',
    'WacOSSettings|System Settings|Settings.png',
    'WacOSActivityMonitor|Activity Monitor|Activity Monitor.png',
    'WacOSTerminal|Terminal|Terminal.png',
    'WacOSCalculator|Calculator|Calculator.png',
    'WacOSNotes|Notes|Notes.png',
    'WacOSScreenshot|Screenshot|Screenshot.png',
    'WacOSPreview|Preview|Preview.png',
    'WacOSReminders|Reminders|Reminders.png',
    'WacOSCalendar|Calendar|Calendar.png',
    'WacOSTextEdit|TextEdit|Textedit.png',
    'WacOSClock|Clock|Clock.png',
    'WacOSWeather|Weather|Weather.png',
    'WacOSMusic|Music|Music.png',
    'WacOSQuickTime|QuickTime Player|QuickTime Player.png',
    'WacOSForceQuit|Force Quit Applications|Activity Monitor.png',
    'WacOSContacts|Contacts|Contacts.png',
    'WacOSPhotos|Photos|Photos.png',
    'WacOSTimeMachine|Time Machine|Time Machine.png',
    'WacOSTitleBars|WacOS Title Bars|',
    'WacOSSetup|WacOS Setup|repo:docs\logo-256.png',
    'WacOSWindow|WacOS Window Frames|',
    'WacOSWindow32|WacOS Window Frames (32-bit)|'
)

# .ico com várias resoluções, cada uma guardada como PNG (aceito desde o Vista)
function Make-Ico([string]$png, [string]$ico) {
    $src = [Drawing.Image]::FromFile($png)
    $frames = @()
    try {
        foreach ($s in 16, 20, 24, 32, 40, 48, 64, 128, 256) {
            $bmp = New-Object Drawing.Bitmap $s, $s, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $g = [Drawing.Graphics]::FromImage($bmp)
            $g.InterpolationMode = 'HighQualityBicubic'; $g.SmoothingMode = 'HighQuality'; $g.PixelOffsetMode = 'HighQuality'; $g.CompositingQuality = 'HighQuality'
            if ($png -like '*logo*') { $g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half' }  # o logo é pixel art: sem borrar
            $g.DrawImage($src, 0, 0, $s, $s); $g.Dispose()
            $ms = New-Object IO.MemoryStream; $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
            $frames += , @($s, $ms.ToArray())
        }
    } finally { $src.Dispose() }
    $fs = [IO.File]::Create($ico); $w = New-Object IO.BinaryWriter $fs
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$frames.Count)
    $off = 6 + 16 * $frames.Count
    foreach ($f in $frames) {
        $s = $f[0]; $d = $f[1]
        $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$d.Length); $w.Write([UInt32]$off)
        $off += $d.Length
    }
    foreach ($f in $frames) { $w.Write([byte[]]$f[1]) }
    $w.Close()
}

foreach ($linha in $itens) {
    $nome, $desc, $png = $linha.Split('|')
    $ext = if ($nome -like 'WacOSWindow' -or $nome -like 'WacOSWindow32') { 'dll' } else { 'exe' }
    $iconLine = ''
    # "repo:<caminho>": um PNG do próprio repositório (o logo do WacOS, docs\logo-256.png) em vez do pacote de ícones
    $pngPath = if ($png -like 'repo:*') { Join-Path (Split-Path $PSScriptRoot -Parent) $png.Substring(5) } elseif ($png) { Join-Path $pack $png } else { '' }
    if ($pngPath -and (Test-Path $pngPath)) {
        $ico = Join-Path $Out "$nome.ico"
        if (!(Test-Path $ico) -or (Get-Item $ico).LastWriteTime -lt (Get-Item $pngPath).LastWriteTime) { Make-Ico $pngPath $ico }
        $iconLine = "1 ICON `"$nome.ico`""
        # o instalador desenha o logo na barra lateral: o PNG vai embutido (recurso 2, RCDATA)
        if ($nome -eq 'WacOSSetup') { $iconLine += "`r`n2 RCDATA `"" + ($pngPath -replace '\\', '\\') + "`"" }
    }
    $rc = @"
#include <winver.h>
$iconLine
VS_VERSION_INFO VERSIONINFO
FILEVERSION 1,0,0,0
PRODUCTVERSION 1,0,0,0
FILEOS VOS_NT_WINDOWS32
FILETYPE $(if ($ext -eq 'dll') { 'VFT_DLL' } else { 'VFT_APP' })
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "WacOS (open source)"
      VALUE "Comments", "https://github.com/renzyxx/WacOS"
      VALUE "LegalCopyright", "WacOS contributors"
      VALUE "FileDescription", "$desc"
      VALUE "FileVersion", "1.0.0.0"
      VALUE "InternalName", "$nome"
      VALUE "OriginalFilename", "$nome.$ext"
      VALUE "ProductName", "WacOS"
      VALUE "ProductVersion", "1.0.0.0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
    [IO.File]::WriteAllText((Join-Path $Out "$nome.rc"), $rc, [Text.Encoding]::ASCII)
}
Write-Host "recursos gerados em $Out"
