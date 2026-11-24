# Desliga a janela de Abrir/Salvar no estilo Mac: apaga as chaves criadas em HKCU (64 e 32 bits).
# Os programas voltam a usar o diálogo do Windows (os que estiverem abertos, ao abrir o próximo).
foreach ($raiz in 'HKCU:\Software\Classes\CLSID', 'HKCU:\Software\Classes\WOW6432Node\CLSID') {
    foreach ($clsid in '{DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7}', '{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}') {
        Remove-Item -Recurse -Force "$raiz\$clsid" -ErrorAction SilentlyContinue
    }
}
"Desligado: os programas voltam a usar o diálogo do Windows."
