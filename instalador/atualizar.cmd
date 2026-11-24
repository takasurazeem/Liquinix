@echo off
rem Atualiza o WacOS ja instalado: troca so os programas, icones e sons (ajustes e o resto ficam).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar.ps1" -Atualizar
pause
