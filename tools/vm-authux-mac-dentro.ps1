# Roda DENTRO da VM (C:\SequoiaMac\authux-mac.ps1), como administrador.
# Troca o System32\AuthUX.dll (logon do Windows 7) pela versão estilo Mac (C:\SequoiaMac\AuthUX-mac.dll), guardando o original.
#   aplicar | desfazer
param([string]$Acao = 'aplicar')
$dll = "$env:WINDIR\System32\AuthUX.dll"
$orig = 'C:\SequoiaMac\originais\AuthUX-win7.dll'
$out = New-Object System.Collections.Generic.List[string]
New-Item -ItemType Directory -Force (Split-Path $orig) | Out-Null
function Gravar($src) {
    try { Copy-Item $src $dll -Force -ErrorAction Stop }
    catch {
        $velho = "$dll.$([DateTime]::Now.Ticks).old"
        Rename-Item $dll $velho -ErrorAction Stop
        Copy-Item $src $dll -Force -ErrorAction Stop
        $out.Add("o antigo estava em uso: renomeado para $velho")
    }
}
try {
    if ($Acao -eq 'aplicar') {
        if (-not (Test-Path $orig)) { Copy-Item $dll $orig; $out.Add("original guardado em $orig") }
        Gravar 'C:\SequoiaMac\AuthUX-mac.dll'
        $out.Add('AuthUX estilo Mac instalado')
    } else {
        if (Test-Path $orig) { Gravar $orig; $out.Add('AuthUX do Windows 7 devolvido') } else { $out.Add('sem original guardado') }
    }
    # arquivos do driver NVIDIA trazidos do PC (GPU-P): C:\SequoiaMac\nv -> System32/SysWOW64
    foreach ($dir in 'System32', 'SysWOW64') {
        $src = "C:\SequoiaMac\nv\$dir"
        if (Test-Path $src) {
            $k = 0
            foreach ($f in Get-ChildItem $src -File) {
                $dst = "$env:WINDIR\$dir\$($f.Name)"
                if (-not (Test-Path $dst)) { Copy-Item $f.FullName $dst -Force -ErrorAction SilentlyContinue; $k++ }
            }
            $out.Add("driver NVIDIA: $k arquivos novos em $dir")
        }
    }
    # papel dinâmico do Tahoe (vídeo + 1º quadro), claro e escuro
    if (Test-Path C:\SequoiaMac\tahoe) {
        $dst = "$env:WINDIR\Web\Screen\macOS"
        New-Item -ItemType Directory -Force $dst | Out-Null
        Copy-Item C:\SequoiaMac\tahoe\* $dst -Force
        $out.Add("papel dinâmico em $dst: " + ((Get-ChildItem $dst -Filter 'tahoe-*' | ForEach-Object Name) -join ', '))
    }
    $f = Get-Item $dll
    $out.Add("System32\AuthUX.dll: $($f.Length) bytes, $($f.LastWriteTime)")
} catch { $out.Add("erro: $($_.Exception.Message)") }
$out | Set-Content C:\SequoiaMac\resumo.txt -Encoding utf8
Start-Process notepad.exe C:\SequoiaMac\resumo.txt
