# Updates Liquinix from GitHub (System Settings > General > Software Update).
# Every commit on GitHub builds a new package (GitHub Actions, .github\workflows\build.yml) published as the
# "latest" release, with bin\versao.txt inside (line 1: the version, e.g. build-12; line 2: the commit; line 3: the date).
#  -Verificar -Saida <file>: only checks (System Settings calls it when the page opens) and writes one item per line:
#     atual=<installed version>  novo=<latest version>  data=<its date>  erro=<reason>  commit=<message> (what is missing)
#  without -Verificar: downloads the .zip, extracts it to a temp folder and runs its instalar.ps1 -Atualizar -Silencioso
#     (replaces only programs, icons, sounds and tools; settings stay)
# Private repository: the GitHub token goes in %APPDATA%\Shell\github-token.txt (read-only is enough).
param([switch]$Verificar, [string]$Saida = '', [string]$Bin = '')
$ErrorActionPreference = 'Stop'
$repo = 'takasurazeem/Liquinix'
if (-not $Bin) { $Bin = Join-Path $env:LOCALAPPDATA 'Programs\Liquinix\bin' }
$out = New-Object System.Collections.Generic.List[string]

function Grava { if ($Saida) { Set-Content -Encoding utf8 $Saida $out } }
function Fim($msg, $espera = 4) { if ($Verificar) { Grava; exit }; Write-Host $msg; Start-Sleep $espera; exit }
function Erro($msg) { $out.Add("erro=$msg"); Fim $msg 8 }

if (-not $Verificar) { $host.UI.RawUI.WindowTitle = 'Liquinix - Update' }
$atual = ''; $atualSha = ''
$vf = Join-Path $Bin 'versao.txt'
if (Test-Path $vf) { $v = @(Get-Content $vf); if ($v.Count -ge 1) { $atual = $v[0].Trim() }; if ($v.Count -ge 2) { $atualSha = $v[1].Trim() } }
$out.Add("atual=$atual")
if (-not $Verificar) { Write-Host "Installed Liquinix: $(if ($atual) { $atual } else { 'unknown version' })" }

$h = @{ 'User-Agent' = 'Liquinix-atualizador'; 'Accept' = 'application/vnd.github+json' }
$tf = Join-Path $env:APPDATA 'Shell\github-token.txt'
if (Test-Path $tf) { $h['Authorization'] = 'Bearer ' + (Get-Content $tf -TotalCount 1).Trim() }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $Verificar) { Write-Host 'Looking for the latest version on GitHub...' }
try { $rel = Invoke-RestMethod -Headers $h -Uri "https://api.github.com/repos/$repo/releases/latest" }
catch {
    $code = 0; try { $code = $_.Exception.Response.StatusCode.value__ } catch {}
    if ($code -eq 404) { Erro "no release found at github.com/$repo (private repository? put a token in $tf)" }
    Erro "could not reach GitHub ($($_.Exception.Message))"
}
$novo = $rel.tag_name
$out.Add("novo=$novo")
$out.Add("data=$(([datetime]$rel.published_at).ToLocalTime().ToString('dd/MM/yyyy HH:mm'))")
if ($atual -and $atual -eq $novo) { Fim 'Liquinix is up to date.' }

# what is missing: the commits between the installed version and the new one (else the release notes)
$msgs = @()
if ($atualSha) {
    try {
        $cmp = Invoke-RestMethod -Headers $h -Uri "https://api.github.com/repos/$repo/compare/$atualSha...$novo"
        $msgs = @($cmp.commits | ForEach-Object { ($_.commit.message -split "`n")[0] })
        [array]::Reverse($msgs)  # newest first
    } catch {}
}
if (-not $msgs -and $rel.body) { $msgs = @($rel.body -split "`n" | ForEach-Object { $_.Trim().TrimStart('-', '*', ' ') } | Where-Object { $_ }) }
foreach ($m in ($msgs | Select-Object -First 40)) { $out.Add("commit=$m") }
if ($Verificar) { Fim '' }

Write-Host "Latest version: $novo"
if ($msgs) { Write-Host 'What changes:'; $msgs | Select-Object -First 20 | ForEach-Object { Write-Host "  - $_" } }
$asset = $rel.assets | Where-Object { $_.name -like '*.zip' } | Select-Object -First 1
if (-not $asset) { Fim 'The latest release has no .zip package.' 8 }
$tmp = Join-Path $env:TEMP ('liquinix-atualizar-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
$zip = Join-Path $tmp $asset.name
Write-Host "Downloading $($asset.name) ($([math]::Round($asset.size / 1MB)) MB)..."
$dh = $h.Clone(); $dh['Accept'] = 'application/octet-stream'
$ProgressPreference = 'SilentlyContinue'  # the PowerShell progress bar makes downloads very slow
Invoke-WebRequest -UseBasicParsing -Headers $dh -Uri $asset.url -OutFile $zip
# the package must be exactly the one GitHub published (the release lists its SHA-256 as "digest")
if ($asset.digest -match '^sha256:([0-9a-fA-F]{64})$') {
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $Matches[1].ToUpper()) { Remove-Item $tmp -Recurse -Force; Fim 'The download does not match the published package (SHA-256). Nothing was installed.' 8 }
    Write-Host 'SHA-256 checked.'
}
Write-Host 'Extracting...'
Expand-Archive $zip (Join-Path $tmp 'pacote') -Force
$script = Get-ChildItem (Join-Path $tmp 'pacote') -Recurse -Filter instalar.ps1 | Select-Object -First 1
if (-not $script) { Fim 'The package has no instalar.ps1.' 8 }
Write-Host 'Installing (only the files that changed; Liquinix closes and reopens by itself)...'
# in this same PowerShell, no second PowerShell started on the downloaded file
& $script.FullName -Atualizar -Silencioso
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Fim "Done: Liquinix $novo installed."
