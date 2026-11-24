# Liga a janela de Abrir/Salvar no estilo Mac (DialogoMac.dll) só para o usuário atual (HKCU),
# para programas de 64 bits (DialogoMac.dll) e de 32 bits (DialogoMac32.dll).
# Só os programas listados em %APPDATA%\Shell\dialogo.txt usam o Finder ("*" = todos); os outros
# recebem o diálogo original do Windows. Desfazer: tools\dialogo-desligar.ps1
$bin = (Resolve-Path "$PSScriptRoot\..\bin").Path
$alvos = @(@{ raiz = 'HKCU:\Software\Classes\CLSID'; dll = "$bin\DialogoMac.dll" })
if (Test-Path "$bin\DialogoMac32.dll") { $alvos += @{ raiz = 'HKCU:\Software\Classes\WOW6432Node\CLSID'; dll = "$bin\DialogoMac32.dll" } }
foreach ($a in $alvos) {
    foreach ($clsid in '{DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7}', '{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}') {
        $k = "$($a.raiz)\$clsid\InprocServer32"
        New-Item -Force $k | Out-Null
        Set-ItemProperty $k -Name '(default)' -Value $a.dll
        Set-ItemProperty $k -Name 'ThreadingModel' -Value 'Apartment'
    }
}
$lista = Join-Path $env:APPDATA 'Shell\dialogo.txt'
if (-not (Test-Path $lista)) { New-Item -ItemType Directory -Force (Split-Path $lista) | Out-Null; Set-Content -Encoding utf8 $lista "dialogoteste.exe" }
"Ligado ($($alvos.Count) arquitetura(s)). Programas na lista ($lista):"; Get-Content $lista
