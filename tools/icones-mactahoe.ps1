# Monta icones\tahoe a partir do tema MacTahoe (github.com/vinceliuice/MacTahoe-icon-theme, GPL-3.0).
# O tema e de Linux (SVG, nomes do freedesktop); aqui cada icone vira um PNG de 512 px com o nome que o
# Shell procura (o mesmo do pacote Sequoia: "Finder", "Settings", "folder downloads", "trashempty@2x"...).
# Uso pessoal: os icones sao do autor do tema (ver CREDITOS.txt gerado na pasta).
#   powershell -File tools\icones-mactahoe.ps1 -Tema <pasta MacTahoe-icon-theme-main descompactada>
param(
    [Parameter(Mandatory = $true)][string]$Tema,
    [string]$Pasta = (Join-Path (Split-Path $PSScriptRoot -Parent) "icones\tahoe"),
    [int]$Tamanho = 512,
    # o desenho dos ícones de app do MacTahoe ocupa 87% da imagem; o Shell foi calibrado com os 80% do pacote
    # Sequoia (Dock, Launchpad, Alt+Tab): 0,92 deixa o desenho nos mesmos 80%. Pastas e Lixeira ficam inteiras.
    [double]$EscalaApps = 0.92
)
$ErrorActionPreference = "Stop"
$svg2png = Join-Path (Split-Path $PSScriptRoot -Parent) "bin\ferramentas\svg2png.exe"
if (-not (Test-Path $svg2png)) { throw "Falta bin\ferramentas\svg2png.exe (cl src\tools\svg2png.cpp)" }
New-Item -ItemType Directory -Force $Pasta | Out-Null

# nome no Shell -> nomes no tema (o primeiro que existir)
$mapa = [ordered]@{
    "Finder"            = "nautilus", "org.gnome.Nautilus", "system-file-manager"
    "Settings"          = "gnome-control-center", "org.gnome.Settings", "preferences-system", "systemsettings"
    "Calculator"        = "gnome-calculator", "org.gnome.Calculator", "accessories-calculator"
    "Terminal"          = "utilities-terminal", "org.gnome.Terminal", "terminal"
    "Activity Monitor"  = "gnome-system-monitor", "utilities-system-monitor", "system-monitor"
    "Notes"             = "org.gnome.Notes", "gnome-notes", "notes", "accessories-notes"
    "Launchpad"         = "appgrid", "launchpad", "view-app-grid"
    "Screenshot"        = "org.gnome.Screenshot", "gnome-screenshot", "accessories-screenshot"
    "Preview"           = "org.gnome.Loupe", "eog", "org.gnome.eog", "image-viewer"
    "Photos"            = "org.gnome.Photos", "gnome-photos", "shotwell"
    "Music"             = "org.gnome.Music", "gnome-music", "rhythmbox"
    "Safari"            = "safari"
    "App Store"         = "org.gnome.Software", "gnome-software", "system-software-install"
    "Mail"              = "org.gnome.Evolution", "evolution", "internet-mail", "thunderbird"
    "Calendar"          = "org.gnome.Calendar", "gnome-calendar", "calendar"
    "Weather"           = "org.gnome.Weather", "gnome-weather"
    "Clock"             = "org.gnome.clocks", "gnome-clocks"
    "Maps"              = "org.gnome.Maps", "gnome-maps"
    "Contacts"          = "org.gnome.Contacts", "gnome-contacts"
    "TextEdit"          = "org.gnome.TextEditor", "gedit", "accessories-text-editor"
    "Google Chrome"     = "google-chrome"
    "Firefox"           = "firefox"
    "Microsoft Edge"    = "microsoft-edge", "msedge"
    "Discord"           = "discord"
    "Spotify"           = "spotify-client", "spotify"
    "Steam"             = "steam"
    "Telegram"          = "telegram", "org.telegram.desktop"
    "Visual Studio Code" = "visual-studio-code", "code", "vscode", "com.visualstudio.code"
    "OBS Studio"        = "com.obsproject.Studio", "obs"
    "VLC"               = "vlc"
    "Claude"            = "claude"
    "folder default"    = "folder", "blue-folder", "folder-blue"
    "folder downloads"  = "folder-download", "blue-folder-download"
    "folder docs"       = "folder-documents", "blue-folder-documents"
    "folder desktop"    = "user-desktop", "blue-user-desktop"
    "folder home"       = "user-home", "blue-user-home"
    "folder music"      = "folder-music", "blue-folder-music"
    "folder pictures"   = "folder-pictures", "folder-images", "blue-folder-images"
    "folder videos"     = "folder-videos", "blue-folder-videos"
    "folder cloud"      = "folder-cloud", "blue-folder-cloud"
    "folder games"      = "folder-games", "blue-folder-games"
    "folder library"    = "folder-library"
    "trashempty@2x"     = "user-trash"
    "trashfull@2x"      = "user-trash-full"
}

$dirs = foreach ($d in "src\apps\scalable", "links\apps\scalable", "src\places\scalable", "links\places\scalable", "src\devices\scalable", "links\devices\scalable") {
    $p = Join-Path $Tema $d; if (Test-Path $p) { $p }
}

# no zip do GitHub os links simbolicos viram arquivos de texto com o caminho do alvo
function Resolver([string]$f, [int]$n = 0) {
    if ($n -gt 6 -or -not (Test-Path $f)) { return $null }
    $len = (Get-Item $f).Length
    if ($len -lt 300) {
        $t = ([IO.File]::ReadAllText($f)).Trim()
        if (-not $t.StartsWith("<")) {
            $prox = Join-Path (Split-Path $f) $t
            if (-not (Test-Path $prox)) {  # o alvo costuma estar em src\..., e o link em links\...
                foreach ($d in $dirs) { $c = Join-Path $d (Split-Path $t -Leaf); if (Test-Path $c) { $prox = $c; break } }
            }
            return Resolver $prox ($n + 1)
        }
    }
    return (Resolve-Path $f).Path
}

$ok = 0; $faltam = @()
$cred = @("Icones do tema MacTahoe de vinceliuice (https://github.com/vinceliuice/MacTahoe-icon-theme), GPL-3.0.", "Convertidos de SVG para PNG ($Tamanho px) por tools\icones-mactahoe.ps1. Uso pessoal.", "")
foreach ($alvo in $mapa.Keys) {
    $src = $null
    foreach ($nome in $mapa[$alvo]) {
        foreach ($d in $dirs) { $c = Join-Path $d ($nome + ".svg"); if (Test-Path $c) { $src = Resolver $c; if ($src) { break } } }
        if ($src) { break }
    }
    if (-not $src) { $faltam += $alvo; continue }
    $out = Join-Path $Pasta ($alvo + ".png")
    $k = if ($alvo -like "folder *" -or $alvo -like "trash*") { 1 } else { $EscalaApps }
    & $svg2png $src $out $Tamanho $k
    if ($LASTEXITCODE -eq 0) { $ok++; $cred += "$alvo`t$(Split-Path $src -Leaf)" } else { $faltam += "$alvo (SVG nao desenhou)" }
}
Set-Content -Encoding UTF8 (Join-Path $Pasta "CREDITOS.txt") $cred
Write-Output "convertidos: $ok"
if ($faltam) { Write-Output ("sem icone no tema: " + ($faltam -join ", ")) }
