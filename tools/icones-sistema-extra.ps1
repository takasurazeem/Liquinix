# Segunda parte da troca de ícones (a primeira é tools\icones-sistema.ps1, só HKCU):
#  1) desktop.ini das pastas do usuário (Mesa, Documentos, Downloads, Imagens, Música, Vídeos): IconResource -> ícone do Mac
#  2) HKLM "Shell Icons" 0/3/4 (arquivo genérico e pasta) — o Windows só lê o ícone de pasta daqui. Pede administrador.
#   aplicar:  powershell -ExecutionPolicy Bypass -File tools\icones-sistema-extra.ps1
#   desfazer: powershell -ExecutionPolicy Bypass -File tools\icones-sistema-extra.ps1 -Desfazer
# Backups: docs\backup-desktopini\ (cópias dos desktop.ini + lista.json) e docs\backup-icones-maquina.json.
param([switch]$Desfazer, [switch]$Maquina)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ico = Join-Path $root 'bin\icones-sistema'
$bdir = Join-Path $root 'docs\backup-desktopini'
$bini = Join-Path $bdir 'lista.json'
$bhk = Join-Path $root 'docs\backup-icones-maquina.json'
$log = Join-Path $root 'docs\registro-mudancas.txt'
$out = Join-Path $ico ($(if ($Maquina) { 'ultima-execucao-maquina.txt' } else { 'ultima-execucao-extra.txt' }))
$lines = New-Object System.Collections.Generic.List[string]
function Say($s) { $lines.Add($s) }
function Fim { $lines | Set-Content $out -Encoding utf8 }
function Stamp { Get-Date -Format 'yyyy-MM-dd HH:mm' }
# avisar o Explorador: pelo LiquinixShell.exe (modo ajudante), sem C# embutido no script
$ajudante = Join-Path $PSScriptRoot '..\bin\LiquinixShell.exe'
function AvisarIcones { if (Test-Path $ajudante) { Start-Process $ajudante -ArgumentList '--avisar-icones' -Wait } }
function AvisarPasta($p) { if (Test-Path $ajudante) { Start-Process $ajudante -ArgumentList '--avisar-pasta', ('"' + $p + '"') -Wait } }
function Refresh {
    AvisarIcones  # SHCNE_ASSOCCHANGED
    try { Start-Process "$env:WINDIR\System32\ie4uinit.exe" -ArgumentList '-show' -Wait } catch {}
}
function Elevated($a) {
    try { $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Maquina $a"; Say "parte da máquina (administrador): saída $($p.ExitCode)" }
    catch { Say "parte da máquina: permissão de administrador negada ($($_.Exception.Message))" }
}

# ---------------- HKLM (processo elevado) ----------------
$SI = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Shell Icons'
if ($Maquina) {
    $LM = [Microsoft.Win32.Registry]::LocalMachine
    if ($Desfazer) {
        if (Test-Path $bhk) {
            $b = Get-Content $bhk -Raw -Encoding utf8 | ConvertFrom-Json
            $k = $LM.OpenSubKey($SI, $true)
            foreach ($it in $b.values) {
                if ($it.existed) { $k.SetValue($it.name, $it.old); Say "restaurado HKLM\$SI [$($it.name)] = $($it.old)" }
                else { try { $k.DeleteValue($it.name) } catch {}; Say "removido HKLM\$SI [$($it.name)]" }
            }
            $empty = $k.ValueCount -eq 0 -and $k.SubKeyCount -eq 0; $k.Close()
            if ($b.created -and $empty) { $LM.DeleteSubKey($SI, $false); Say "chave apagada HKLM\$SI" }
            Remove-Item $bhk
            Refresh
            Add-Content $log -Encoding utf8 "`r`n[$(Stamp)] HKLM Shell Icons: DESFEITO (tools\icones-sistema-extra.ps1 -Desfazer)."
        }
        Fim; exit
    }
    if (Test-Path $bhk) { Say 'HKLM já aplicado'; Fim; exit }
    $t = $LM.OpenSubKey($SI); $created = -not $t; if ($t) { $t.Close() }
    $k = $LM.CreateSubKey($SI)
    $vals = @()
    foreach ($pair in @(@('0', 'doc-generico'), @('3', 'pasta'), @('4', 'pasta'))) {
        $old = $k.GetValue($pair[0], $null, 'DoNotExpandEnvironmentNames')
        $vals += [pscustomobject]@{ name = $pair[0]; existed = ($old -ne $null); old = $old }
        $k.SetValue($pair[0], "$ico\$($pair[1]).ico,0", [Microsoft.Win32.RegistryValueKind]::String)
        Say "HKLM\$SI [$($pair[0])] = $ico\$($pair[1]).ico,0   (antes: $(if ($old -ne $null) { $old } else { 'não existia' }))"
    }
    $k.Close()
    [pscustomobject]@{ created = $created; values = $vals } | ConvertTo-Json -Depth 4 | Set-Content $bhk -Encoding utf8
    Refresh
    Add-Content $log -Encoding utf8 ("`r`n[$(Stamp)] HKLM Shell Icons (pasta e arquivo genérico) -> ícones do Mac (tools\icones-sistema-extra.ps1, como administrador). " +
        "Chave $(if ($created) { 'criada' } else { 'já existia' }). Backup docs\backup-icones-maquina.json; desfazer: tools\icones-sistema-extra.ps1 -Desfazer`r`n" + (($lines | % { "    $_" }) -join "`r`n"))
    Fim; exit
}

# ---------------- desktop.ini (usuário) ----------------
function ReadIni($p) {
    $bytes = [IO.File]::ReadAllBytes($p)
    $uni = $bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE
    $enc = if ($uni) { [Text.Encoding]::Unicode } else { [Text.Encoding]::Default }
    return @{ text = $enc.GetString($bytes, $(if ($uni) { 2 } else { 0 }), $bytes.Length - $(if ($uni) { 2 } else { 0 })); uni = $uni }
}
function WriteIni($p, $text, $uni) {
    $enc = if ($uni) { New-Object Text.UnicodeEncoding($false, $true) } else { [Text.Encoding]::Default }
    if (Test-Path $p) { (Get-Item $p -Force).Attributes = 'Normal' }
    [IO.File]::WriteAllText($p, $text, $enc)
    (Get-Item $p -Force).Attributes = 'Hidden, System, Archive'
}

if ($Desfazer) {
    if (Test-Path $bini) {
        foreach ($it in (Get-Content $bini -Raw -Encoding utf8 | ConvertFrom-Json)) {
            $p = Join-Path $it.folder 'desktop.ini'
            if ($it.existed) {
                if (Test-Path $p) { (Get-Item $p -Force).Attributes = 'Normal' }
                Copy-Item (Join-Path $bdir $it.copy) $p -Force
                (Get-Item $p -Force).Attributes = $it.attrs
                Say "restaurado $p"
            } else {
                if (Test-Path $p) { (Get-Item $p -Force).Attributes = 'Normal'; Remove-Item $p -Force }
                if ($it.folderAttrs) { (Get-Item $it.folder -Force).Attributes = $it.folderAttrs }
                Say "removido $p"
            }
            AvisarPasta $it.folder  # SHCNE_UPDATEDIR
        }
        Remove-Item $bdir -Recurse -Force
        Add-Content $log -Encoding utf8 "`r`n[$(Stamp)] desktop.ini das pastas do usuário: DESFEITO (cópias originais devolvidas)."
    } else { Say 'desktop.ini: sem backup' }
    if (Test-Path $bhk) { Elevated '-Desfazer' }
    Refresh; Fim; exit
}

if (Test-Path $bini) { Say 'desktop.ini já aplicado' }
else {
    $usf = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders'
    $dl = [Environment]::ExpandEnvironmentVariables($usf.'{374DE290-123F-4565-9164-39C4925E467B}')
    $folders = [ordered]@{
        ([Environment]::GetFolderPath('Desktop')) = 'pasta-desktop'
        ([Environment]::GetFolderPath('MyDocuments')) = 'pasta-docs'
        $dl = 'pasta-downloads'
        ([Environment]::GetFolderPath('MyPictures')) = 'pasta-pictures'
        ([Environment]::GetFolderPath('MyMusic')) = 'pasta-music'
        ([Environment]::GetFolderPath('MyVideos')) = 'pasta-videos'
    }
    New-Item -ItemType Directory -Force $bdir | Out-Null
    $list = @(); $n = 0
    foreach ($f in $folders.Keys) {
        if (-not $f -or -not (Test-Path $f)) { Say "  pasta não encontrada: $f"; continue }
        $p = Join-Path $f 'desktop.ini'
        $icon = "$ico\$($folders[$f]).ico,0"
        $n++
        if (Test-Path $p) {
            $copy = "$n-$(Split-Path $f -Leaf).ini"
            Copy-Item $p (Join-Path $bdir $copy) -Force
            (Get-Item (Join-Path $bdir $copy) -Force).Attributes = 'Normal'
            $attrs = (Get-Item $p -Force).Attributes.ToString()
            $r = ReadIni $p
            $ls = [Collections.Generic.List[string]]($r.text -split "`r?`n")
            $ls = [Collections.Generic.List[string]]@($ls | ? { $_ -notmatch '^\s*(IconResource|IconFile|IconIndex)\s*=' })
            $sec = -1; for ($i = 0; $i -lt $ls.Count; $i++) { if ($ls[$i] -match '^\s*\[\.ShellClassInfo\]') { $sec = $i; break } }
            if ($sec -lt 0) { $ls.Insert(0, '[.ShellClassInfo]'); $sec = 0 }
            $ls.Insert($sec + 1, "IconResource=$icon")
            WriteIni $p ($ls -join "`r`n") $r.uni
            $list += [pscustomobject]@{ folder = $f; existed = $true; copy = $copy; attrs = $attrs }
        } else {
            $fa = (Get-Item $f -Force).Attributes.ToString()
            WriteIni $p "[.ShellClassInfo]`r`nIconResource=$icon`r`n" $true
            (Get-Item $f -Force).Attributes = (Get-Item $f -Force).Attributes -bor [IO.FileAttributes]::ReadOnly  # o Windows só lê o desktop.ini de pasta marcada
            $list += [pscustomobject]@{ folder = $f; existed = $false; folderAttrs = $fa }
        }
        AvisarPasta $f
        Say "$p -> IconResource=$icon"
    }
    ConvertTo-Json @($list) -Depth 3 | Set-Content $bini -Encoding utf8
    Add-Content $log -Encoding utf8 ("`r`n[$(Stamp)] desktop.ini das pastas do usuário -> ícones do Mac (tools\icones-sistema-extra.ps1). Cópias originais em docs\backup-desktopini\; desfazer: tools\icones-sistema-extra.ps1 -Desfazer`r`n" + (($lines | % { "    $_" }) -join "`r`n"))
}
Elevated ''
Refresh
Fim
