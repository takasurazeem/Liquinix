# Roda DENTRO da VM, na sessão aberta (como administrador). Executa vm-windows-mac-dentro.ps1 e mostra um resumo no Bloco de Notas.
# Uso na VM: powershell -EP Bypass -File C:\SequoiaMac\rodar.ps1 aplicar|desfazer|estado
param([string]$Acao = 'aplicar')
$r = & C:\SequoiaMac\vm-windows-mac-dentro.ps1 -Acao $Acao *>&1 | ForEach-Object { "$_" }
$r | Set-Content "C:\SequoiaMac\resultado-$Acao.txt" -Encoding utf8
$res = @($r | Where-Object { $_ -match 'erro|aviso|falh|patch|pronto|prancha|Exception|desfeito|recursos|arquivo |devolvid|cache|VM build|sess|backup|Shell =' })
$res | Set-Content C:\SequoiaMac\resumo.txt -Encoding utf8
Start-Process notepad.exe C:\SequoiaMac\resumo.txt
