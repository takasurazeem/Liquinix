# Desfaz o debloat de 2026-11-07 (docs\debloat.md). Rode sem parâmetros: a parte de admin pede UAC sozinha.
param([switch]$Admin)
$ErrorActionPreference = 'Continue'

if ($Admin) {
    # valores originais: todos Automático (Start=2); os com DelayedAutostart=1 eram "Automático (atraso)"
    $orig = @{ 'DiagTrack'='auto'; 'InventorySvc'='delayed-auto'; 'PcaSvc'='delayed-auto'; 'TrkWks'='auto'; 'MapsBroker'='delayed-auto';
               'WSAIFabricSvc'='auto'; 'asus'='delayed-auto'; 'AsusUpdateCheck'='auto'; 'edgeupdate'='delayed-auto';
               'GoogleUpdaterService157.0.8089.0'='auto'; 'GoogleUpdaterInternalService157.0.8089.0'='auto' }
    foreach ($s in $orig.Keys) { & sc.exe config "$s" start= $orig[$s] }
    # todas estavam ativas (Ready)
    foreach ($t in '\MiniToolPartitionWizard','\MobaUpdater','\Launch Adobe CCXProcess','\Ubisoft\Ubisoft Connect Background Update') {
        $p = $t.Substring(0, $t.LastIndexOf('\') + 1); $n = $t.Substring($t.LastIndexOf('\') + 1)
        Enable-ScheduledTask -TaskPath $p -TaskName $n | Out-Null
    }
    Write-Host 'Parte de admin desfeita.'; return
}

# --- HKCU: estes valores não existiam antes -> remover
$ba = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\BackgroundAccessApplications'
foreach ($pfn in 'MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy','Microsoft.StartExperiencesApp_8wekyb3d8bbwe','Microsoft.WidgetsPlatformRuntime_8wekyb3d8bbwe',
                 'Microsoft.YourPhone_8wekyb3d8bbwe','MicrosoftWindows.CrossDevice_cw5n1h2txyewy',
                 'Microsoft.XboxGamingOverlay_8wekyb3d8bbwe','Microsoft.GamingApp_8wekyb3d8bbwe') {
    Remove-ItemProperty "$ba\$pfn" -Name Disabled,DisabledByUser -EA 0
}
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name TaskbarDa -EA 0
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR' -Name AppCaptureEnabled -EA 0
Remove-ItemProperty 'HKCU:\System\GameConfigStore' -Name GameDVR_Enabled -EA 0
Remove-ItemProperty 'HKCU:\Software\Microsoft\GameBar' -Name UseNexusForGameBarEnabled -EA 0
# --- HKCU: estes eram 1
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Mobility' -Name CrossDeviceEnabled -Value 1 -Type DWord
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CrossDeviceResume\Configuration' -Name IsResumeAllowed -Value 1 -Type DWord

# --- apps UWP removidos: reinstalar pela Store (abre a página de cada um)
foreach ($pfn in 'Microsoft.MicrosoftSolitaireCollection_8wekyb3d8bbwe','Microsoft.BingNews_8wekyb3d8bbwe','Microsoft.GetHelp_8wekyb3d8bbwe',
                 'Microsoft.ZuneVideo_8wekyb3d8bbwe','Microsoft.People_8wekyb3d8bbwe','Microsoft.PowerAutomateDesktop_8wekyb3d8bbwe',
                 'Microsoft.Windows.DevHome_8wekyb3d8bbwe','Microsoft.BingSearch_8wekyb3d8bbwe') {
    Write-Host "Reinstalar pela Store: ms-windows-store://pdp/?PFN=$pfn"
}
Write-Host 'Para reabrir cada um: Start-Process "ms-windows-store://pdp/?PFN=<PFN>"'

# --- parte de admin (UAC)
Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"",'-Admin'
Write-Host 'Desfeito. Reinicie (ou saia e entre) para os serviços voltarem ao boot.'

# 07/11/2026: inicialização de Proton VPN, Proton Drive e Edge desativada (StartupApproved). Para reativar:
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' -Name 'Proton VPN' -Value ([byte[]](0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00))
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' -Name 'Proton Drive' -Value ([byte[]](0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00))
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' -Name 'MicrosoftEdgeAutoLaunch_2BBAAFDEA3B726A4173DC71097ECEADD' -Value ([byte[]](0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00))

# 08/11/2026: Discord, Chrome em 2º plano e SoundID fora do login. Para reativar:
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' -Name 'Discord' -Value ([byte[]](0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00))
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' -Name 'GoogleChromeAutoLaunch_108A468FEB252381077E9E0B3BAEFA6F' -Value ([byte[]](0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00))
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' -Name 'SoundID Reference.exe' -Value ([byte[]](0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00))
