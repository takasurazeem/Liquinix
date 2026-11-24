# Troca os ícones do próprio Windows pelos do Mac — só no registro do usuário (HKCU), nada do sistema.
#   aplicar:  powershell -ExecutionPolicy Bypass -File tools\icones-sistema.ps1
#   desfazer: powershell -ExecutionPolicy Bypass -File tools\icones-sistema.ps1 -Desfazer
# Antes de mudar, guarda o valor antigo de cada item em docs\backup-icones-sistema.json; o -Desfazer
# devolve exatamente esses valores (e apaga as chaves que não existiam). Os .ico: tools\icones-sistema.py.
# Precisa rodar fora do contêiner do Claude (ele mesmo lança pelo WMI), senão o HKCU gravado é uma cópia.
param([switch]$Desfazer)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ico = Join-Path $root 'bin\icones-sistema'
$bkp = Join-Path $root 'docs\backup-icones-sistema.json'
$log = Join-Path $root 'docs\registro-mudancas.txt'
$out = Join-Path $root 'bin\icones-sistema\ultima-execucao.txt'
$HK = [Microsoft.Win32.Registry]::CurrentUser
$lines = New-Object System.Collections.Generic.List[string]
function Say($s) { $lines.Add($s) }

# avisar o Explorador: pelo WacOSShell.exe (modo ajudante), sem C# embutido no script
$ajudante = Join-Path $PSScriptRoot '..\bin\WacOSShell.exe'
function AvisarIcones { if (Test-Path $ajudante) { Start-Process $ajudante -ArgumentList '--avisar-icones' -Wait } }

function Refresh {
    AvisarIcones  # SHCNE_ASSOCCHANGED, SHCNF_FLUSH
    try { Start-Process "$env:WINDIR\System32\ie4uinit.exe" -ArgumentList '-show' -Wait } catch {}
}

if ($Desfazer) {
    & (Join-Path $PSScriptRoot 'icones-sistema-extra.ps1') -Desfazer  # desktop.ini e HKLM também
    if (-not (Test-Path $bkp)) { Say 'sem backup: nada a desfazer'; $lines | Set-Content $out -Encoding utf8; exit }
    $items = Get-Content $bkp -Raw -Encoding utf8 | ConvertFrom-Json
    [array]::Reverse($items)
    foreach ($it in $items) {
        $k = $HK.OpenSubKey($it.key, $true)
        if ($it.existed) {
            if ($k) { $k.SetValue($it.name, $it.old, [Microsoft.Win32.RegistryValueKind]$it.kind); Say "restaurado HKCU\$($it.key) [$($it.name)] = $($it.old)" }
        } elseif ($k) { try { $k.DeleteValue($it.name) } catch {}; Say "removido HKCU\$($it.key) [$($it.name)]" }
        if ($k) { $k.Close() }
    }
    # chaves criadas por nós e que ficaram vazias
    foreach ($it in $items) {
        foreach ($c in @($it.created)) {
            if (-not $c) { continue }
            $k = $HK.OpenSubKey($c)
            if ($k -and $k.ValueCount -eq 0 -and $k.SubKeyCount -eq 0) { $k.Close(); $HK.DeleteSubKey($c, $false); Say "chave apagada HKCU\$c" } elseif ($k) { $k.Close() }
        }
    }
    Remove-Item $bkp
    Refresh
    Add-Content $log -Encoding utf8 ("`r`n[$(Get-Date -Format 'yyyy-MM-dd HH:mm')] Ícones do Windows: DESFEITO (tools\icones-sistema.ps1 -Desfazer). Valores antigos devolvidos.")
    $lines | Set-Content $out -Encoding utf8
    exit
}

if (Test-Path $bkp) { Say 'já aplicado (há backup): rode -Desfazer antes de aplicar de novo'; $lines | Set-Content $out -Encoding utf8; exit }

$changes = New-Object System.Collections.Generic.List[object]
function SetVal($key, $name, $data) {
    # cria a chave guardando quais partes eram novas (para o desfazer apagar só o que criamos)
    $created = @()
    $parts = $key -split '\\'
    for ($i = 1; $i -le $parts.Length; $i++) {
        $p = ($parts[0..($i - 1)] -join '\')
        $t = $HK.OpenSubKey($p)
        if ($t) { $t.Close() } else { $created += $p }
    }
    [array]::Reverse($created)
    $k = $HK.CreateSubKey($key)
    $old = $k.GetValue($name, $null, 'DoNotExpandEnvironmentNames')
    $kind = if ($old -ne $null) { $k.GetValueKind($name).ToString() } else { 'String' }
    $changes.Add([pscustomobject]@{ key = $key; name = $name; existed = ($old -ne $null); old = $old; kind = $kind; created = $created })
    $k.SetValue($name, $data, [Microsoft.Win32.RegistryValueKind]::String)
    $k.Close()
    Say "HKCU\$key [$(if ($name) { $name } else { '(Padrão)' })] = $data   (antes: $(if ($old -ne $null) { $old } else { 'não existia' }))"
}
function I($n) { "$ico\$n.ico,0" }

# pastas comuns
SetVal 'Software\Classes\Directory\DefaultIcon' '' (I 'pasta')
SetVal 'Software\Classes\Folder\DefaultIcon' '' (I 'pasta')
SetVal 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Icons' '3' (I 'pasta')
SetVal 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Icons' '4' (I 'pasta')

# Este Computador, pasta do usuário e as pastas dele que aparecem em "Este Computador"
$cl = @{
    '{20D04FE0-3AEA-1069-A2D8-08002B30309D}' = 'computador'
    '{59031a47-3f72-44a7-89c5-5595fe6b30ee}' = 'pasta-home'
    '{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}' = 'pasta-desktop'
    '{d3162b92-9365-467a-956b-92703aca08af}' = 'pasta-docs'
    '{088e3905-0323-4b02-9826-5d99428e115f}' = 'pasta-downloads'
    '{3dfdf296-dbec-4fb4-81d1-6a3438bcf4de}' = 'pasta-music'
    '{24ad3ad4-a569-4530-98e1-ab02f9417aa8}' = 'pasta-pictures'
    '{f86fa3ab-70d2-4fc7-9c99-fcbf05467f3a}' = 'pasta-videos'
}
foreach ($g in $cl.Keys) { SetVal "Software\Classes\CLSID\$g\DefaultIcon" '' (I $cl[$g]) }

# Lixeira (este caminho já é por usuário no Windows)
$rb = 'Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{645FF040-5081-101B-9F08-00AA002F954E}\DefaultIcon'
SetVal $rb '' (I 'lixeira-cheia')
SetVal $rb 'full' (I 'lixeira-cheia')
SetVal $rb 'empty' (I 'lixeira-vazia')

# unidades: HD para os discos, CD para os leitores
foreach ($d in [IO.DriveInfo]::GetDrives()) {
    $L = $d.Name.Substring(0, 1)
    $n = switch ($d.DriveType) { 'Fixed' { 'disco' } 'Removable' { 'disco' } 'CDRom' { 'cd' } default { $null } }
    if ($n) { SetVal "Software\Classes\Applications\Explorer.exe\Drives\$L\DefaultIcon" '' (I $n) }
}

# tipos de arquivo: o ProgID que o Windows usa de fato (UserChoice, senão o padrão da extensão)
$docs = 'txt','log','ini','md','rtf','pdf','png','jpg','jpeg','gif','bmp','webp','heic','svg','mp3','wav','flac','m4a','ogg','aif','aiff',
        'mp4','mkv','mov','avi','webm','html','htm','json','js','py','cpp','h','css','xml','ps1','bat','cmd','docx','xlsx','pptx','csv','zip','7z','rar'
$alias = @{ jpeg = 'jpg'; aiff = 'aif'; htm = 'html'; '7z' = 'zip'; rar = 'zip' }
$cat = @{}
'txt','log','ini','md','rtf' | % { $cat[$_] = 'texto' }
'pdf','png','jpg','jpeg','gif','bmp','webp','heic','svg' | % { $cat[$_] = 'imagem' }
'mp3','wav','flac','m4a','ogg','aif','aiff' | % { $cat[$_] = 'audio' }
'mp4','mkv','mov','avi','webm' | % { $cat[$_] = 'video' }
'html','htm','xml' | % { $cat[$_] = 'web' }
'json','js','py','cpp','h','css','ps1','bat','cmd' | % { $cat[$_] = 'codigo' }
'zip','7z','rar' | % { $cat[$_] = 'zip' }
# o ícone é do ProgID: tipos que dividem o mesmo ganham o ícone do grupo (sem a extensão escrita)
$groups = [ordered]@{}
foreach ($e in $docs) {
    $pid_ = $null
    $uc = $HK.OpenSubKey("Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$e\UserChoice")
    if ($uc) { $pid_ = $uc.GetValue('ProgId'); $uc.Close() }
    if (-not $pid_) { $c = [Microsoft.Win32.Registry]::ClassesRoot.OpenSubKey(".$e"); if ($c) { $pid_ = $c.GetValue(''); $c.Close() } }
    if (-not $pid_) { Say "  .$e sem ProgID: fica o documento genérico"; continue }
    if (-not $groups.Contains($pid_)) { $groups[$pid_] = New-Object System.Collections.Generic.List[string] }
    $groups[$pid_].Add($e)
}
foreach ($pid_ in $groups.Keys) {
    $es = $groups[$pid_]
    $names = @($es | % { if ($alias.ContainsKey($_)) { $alias[$_] } else { $_ } } | Select -Unique)
    $cats = @($es | % { $cat[$_] } | Select -Unique)
    $f = if ($names.Count -eq 1) { $names[0] }
         elseif ($cats.Count -eq 1) { $cats[0] }
         elseif (-not ($cats | ? { $_ -ne 'audio' -and $_ -ne 'video' })) { 'midia' }
         else { 'generico' }
    Say "  $pid_ <- .$($es -join ' .')"
    SetVal "Software\Classes\$pid_\DefaultIcon" '' (I "doc-$f")
}
SetVal 'Software\Classes\Unknown\DefaultIcon' '' (I 'doc-generico')  # tipos sem programa

$changes | ConvertTo-Json -Depth 4 | Set-Content $bkp -Encoding utf8
Refresh
$n = $changes.Count
Add-Content $log -Encoding utf8 ("`r`n[$(Get-Date -Format 'yyyy-MM-dd HH:mm')] Ícones do Windows trocados pelos do Mac (tools\icones-sistema.ps1). $n valores no HKCU; " +
    "valores antigos em docs\backup-icones-sistema.json. Desfazer: powershell -ExecutionPolicy Bypass -File tools\icones-sistema.ps1 -Desfazer`r`n" + (($lines | % { "    $_" }) -join "`r`n"))
Say "total: $n"
$lines | Set-Content $out -Encoding utf8
