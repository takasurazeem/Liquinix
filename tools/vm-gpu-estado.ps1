# Roda DENTRO da VM (C:\SequoiaMac\gpu.ps1): estado das placas de vídeo e o fim do log do AuthUX Mac (sem as teclas).
$l = Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) - $($_.Status) - código $($_.ConfigManagerErrorCode)" }
$l += ''
$l += Get-ChildItem "$env:WINDIR\Web\Screen\macOS" -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name)  $($_.Length)" }
$l += ''
$l += Get-Content C:\Users\Public\AuthUX-mac.log -Tail 400 -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'deo de fundo|tema |compos|Direct3D|erro' } | Select-Object -Last 25
$l | Set-Content C:\SequoiaMac\gpu.txt -Encoding utf8
Start-Process notepad.exe C:\SequoiaMac\gpu.txt
