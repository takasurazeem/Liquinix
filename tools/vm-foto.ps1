# Salva uma foto da tela da VM AuthUX-Teste em docs\vm-tela.png (só lê; pede administrador).
param([switch]$Admin, [string]$Nome = 'AuthUX-Teste')
$root = Split-Path $PSScriptRoot -Parent
$png  = Join-Path $root 'docs\vm-tela.png'
if (-not $Admin) {
    Remove-Item $png -ErrorAction SilentlyContinue
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Admin -Nome `"$Nome`""
    exit $p.ExitCode
}
Add-Type -AssemblyName System.Drawing
$ns = 'root\virtualization\v2'
$vm = Get-CimInstance -Namespace $ns -ClassName Msvm_ComputerSystem -Filter "ElementName='$Nome'"
$vs = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_VirtualSystemSettingData | Where-Object VirtualSystemType -eq 'Microsoft:Hyper-V:System:Realized'
$vid = Get-CimInstance -Namespace $ns -ClassName Msvm_VideoHead | Where-Object SystemName -eq $vm.Name | Select-Object -First 1
$w = [int]$vid.CurrentHorizontalResolution; $h = [int]$vid.CurrentVerticalResolution
$svc = Get-CimInstance -Namespace $ns -ClassName Msvm_VirtualSystemManagementService
$res = Invoke-CimMethod -InputObject $svc -MethodName GetVirtualSystemThumbnailImage -Arguments @{ TargetSystem = $vs; WidthPixels = [uint16]$w; HeightPixels = [uint16]$h }
$bytes = $res.ImageData
$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format16bppRgb565)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$bd = $bmp.LockBits($rect, 'WriteOnly', $bmp.PixelFormat)
[System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $bd.Scan0, [Math]::Min($bytes.Length, $bd.Stride * $h))
$bmp.UnlockBits($bd)
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
