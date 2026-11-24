# Roda DENTRO da VM (C:\SequoiaMac\conferir.ps1): mostra se a Segoe UI está saindo como SF Pro e se o cursor do Mac está ativo.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$s = 'Hamburgefonstiv 0123'
foreach ($f in 'Segoe UI', 'SF Pro Text', 'Arial', 'Segoe UI Variable', 'MS Shell Dlg 2') {
    "{0,-20} largura GDI = {1}" -f $f, [Windows.Forms.TextRenderer]::MeasureText($s, (New-Object Drawing.Font $f, 20)).Width
}
''
Get-ItemProperty 'HKCU:\Control Panel\Cursors' | Select-Object '(default)', Arrow, IBeam | Format-List
Get-ItemProperty 'HKCU:\AppEvents\Schemes' | Select-Object '(default)' | Format-List
