# Roda DENTRO da VM, como administrador: troca C:\Windows\System32\AuthUX.dll pela C:\SequoiaMac\AuthUX-novo.dll
# sem reiniciar (a antiga, se estiver em uso pelo LogonUI, é renomeada para AuthUX-velha-HHmmss.dll).
# Resultado em C:\Users\Public\AuthUX-trocar.txt
$r = @("$(Get-Date -Format s) trocar AuthUX")
try {
    $novo = 'C:\SequoiaMac\AuthUX-novo.dll'
    $alvo = 'C:\Windows\System32\AuthUX.dll'
    if (-not (Test-Path $novo)) { throw "não achei $novo" }
    Get-ChildItem 'C:\Windows\System32\AuthUX-velha-*.dll' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    if (Test-Path $alvo) {
        takeown.exe /f $alvo /a 2>&1 | Out-Null
        icacls.exe $alvo /grant '*S-1-5-32-544:F' /q 2>&1 | Out-Null
        Rename-Item $alvo ("AuthUX-velha-{0}.dll" -f (Get-Date -Format 'HHmmss')) -ErrorAction Stop
    }
    Copy-Item $novo $alvo -Force -ErrorAction Stop
    $r += "trocada: $((Get-Item $alvo).Length) bytes"
} catch { $r += "erro: $($_.Exception.Message)" }
$r | Set-Content 'C:\Users\Public\AuthUX-trocar.txt' -Encoding utf8
