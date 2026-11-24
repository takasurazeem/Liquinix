# Teste das opções "Executar com o PowerShell" e "Executar como Administrador" do Finder.
# Não muda nada no PC: só mostra uma mensagem e se está como administrador.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host ''
Write-Host '  Funcionou! O script rodou pelo Finder.' -ForegroundColor Green
Write-Host ('  Pasta: ' + (Get-Location).Path)
if ($admin) { Write-Host '  Rodando como ADMINISTRADOR.' -ForegroundColor Yellow }
else { Write-Host '  Rodando como usuário normal.' }
Write-Host ''
Read-Host '  Enter para fechar'
