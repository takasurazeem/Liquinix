# Publishes a new Liquinix version on GitHub, built on this PC. The package holds only what Liquinix may
# redistribute: the programs, the icons tracked in git (MacTahoe, GPL-3.0), scripts and the installer.
# Apple's SF fonts, macOS sounds, cursors and any untracked icon packs are never included: tools\empacotar.ps1
# leaves them out and refuses to build a package that contains them. The installer downloads fonts, sounds and
# the cursor on the user's own PC instead (tools\baixar-apple.ps1).
#   1. next version = build-<number of build-N releases + 1>
#   2. writes bin\versao.txt (version, commit, date): the updater compares it with the latest release
#   3. tools\empacotar.ps1 -SemSistema -SemAjustes (no personal settings) -> dist\Liquinix-<date>.zip
#   4. git push, then a release with that zip as the latest
param([string]$Notas = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$repo = 'takasurazeem/Liquinix'
$log = Join-Path $root 'dist\publicar.log'
function L($s) { $s | Tee-Object -FilePath $log -Append | Write-Host }
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
"" | Set-Content $log

$n = @(gh release list -R $repo --limit 1000 --json tagName --jq '.[].tagName' | Where-Object { $_ -match '^build-\d+$' }).Count + 1
$tag = "build-$n"
git push -q origin main 2>&1 | Out-Null
$sha = (git rev-parse HEAD).Trim()
L "versão: $tag ($sha)"
Set-Content -Encoding utf8 (Join-Path $root 'bin\versao.txt') @($tag, $sha, (Get-Date -Format 'yyyy-MM-dd HH:mm'))

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'empacotar.ps1') -SemSistema -SemAjustes *>&1 | ForEach-Object { L "$_" }
$zip = Get-ChildItem (Join-Path $root 'dist') -Filter 'Liquinix-*.zip' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $zip) { L 'ERRO: o pacote não foi gerado'; exit 1 }

if (-not $Notas) {
    $antes = gh release list -R $repo --limit 1 --json tagName --jq '.[0].tagName'
    $Notas = if ($antes) { (git log --pretty=format:'- %s' "$antes..HEAD") -join "`n" } else { '- New version' }
}
$nf = Join-Path $root 'dist\notas.md'
Set-Content -Encoding utf8 $nf $Notas
gh release create $tag $zip.FullName -R $repo --target $sha --title "Liquinix $tag" --notes-file $nf --latest 2>&1 | ForEach-Object { L "$_" }
L "publicado: $tag ($([math]::Round($zip.Length / 1MB)) MB)"
