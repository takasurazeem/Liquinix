# Debloat aprovado em 2026-11-07. Desfazer: docs\debloat-desfazer.ps1
# Sem parâmetro: parte do usuário (HKCU, apps UWP). Com -Admin: ponto de restauração, serviços e tarefas.
param([switch]$Admin)
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'debloat-log.txt'
function L($m) { $m | Tee-Object -FilePath $log -Append }

if ($Admin) {
    L "== ADMIN $(Get-Date -f s)"
    try { Checkpoint-Computer -Description 'Antes do debloat (shell-proprio)' -RestorePointType MODIFY_SETTINGS -EA Stop; L 'ponto de restauração: ok' }
    catch { L "ponto de restauração: FALHOU - $($_.Exception.Message)" }

    foreach ($s in 'DiagTrack','InventorySvc','PcaSvc','TrkWks','MapsBroker','WSAIFabricSvc',
                   'asus','AsusUpdateCheck','edgeupdate','GoogleUpdaterService157.0.8089.0','GoogleUpdaterInternalService157.0.8089.0') {
        $r = & sc.exe config "$s" start= demand 2>&1; L "servico $s -> Manual: $r"
    }
    foreach ($t in '\MiniToolPartitionWizard','\MobaUpdater','\Launch Adobe CCXProcess','\Ubisoft\Ubisoft Connect Background Update') {
        $p = $t.Substring(0, $t.LastIndexOf('\') + 1); $n = $t.Substring($t.LastIndexOf('\') + 1)
        try { Disable-ScheduledTask -TaskPath $p -TaskName $n -EA Stop | Out-Null; L "tarefa $t -> desativada" } catch { L "tarefa ${t}: FALHOU - $($_.Exception.Message)" }
    }
    return
}

L "== USUARIO $(Get-Date -f s)"
function SetDw($k, $n, $v) {
    try { if (!(Test-Path $k)) { New-Item $k -Force | Out-Null }; New-ItemProperty $k -Name $n -Value $v -PropertyType DWord -Force -EA Stop | Out-Null; L "$k\$n = $v" }
    catch { L "$k\${n}: FALHOU - $($_.Exception.Message)" }
}
# apps em 2º plano: Widgets/Feed, Phone Link, Game Bar
$ba = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\BackgroundAccessApplications'
foreach ($pfn in 'MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy','Microsoft.StartExperiencesApp_8wekyb3d8bbwe','Microsoft.WidgetsPlatformRuntime_8wekyb3d8bbwe',
                 'Microsoft.YourPhone_8wekyb3d8bbwe','MicrosoftWindows.CrossDevice_cw5n1h2txyewy',
                 'Microsoft.XboxGamingOverlay_8wekyb3d8bbwe','Microsoft.GamingApp_8wekyb3d8bbwe') {
    SetDw "$ba\$pfn" Disabled 1; SetDw "$ba\$pfn" DisabledByUser 1
}
SetDw 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' TaskbarDa 0
SetDw 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Mobility' CrossDeviceEnabled 0
SetDw 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CrossDeviceResume\Configuration' IsResumeAllowed 0
SetDw 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GameDVR' AppCaptureEnabled 0
SetDw 'HKCU:\System\GameConfigStore' GameDVR_Enabled 0
SetDw 'HKCU:\Software\Microsoft\GameBar' UseNexusForGameBarEnabled 0

# apps UWP inúteis (só para este usuário)
foreach ($a in 'Microsoft.MicrosoftSolitaireCollection','Microsoft.BingNews','Microsoft.GetHelp','Microsoft.ZuneVideo','Microsoft.People',
               'Microsoft.PowerAutomateDesktop','Microsoft.Windows.DevHome','Microsoft.BingSearch') {
    Get-AppxPackage $a | % { try { Remove-AppxPackage $_.PackageFullName -EA Stop; L "removido $($_.PackageFullName)" } catch { L "remover ${a}: FALHOU - $($_.Exception.Message)" } }
}

# encerra os processos de 2º plano desses itens (não são apps abertos pelo usuário)
Get-Process PhoneExperienceHost,CrossDeviceService,CrossDeviceResume,GameBar,GameBarFTServer,XboxGameBarWidgets,SpotifyXboxGamebarWebView,WidgetService,Widgets,MicrosoftStartFeedProvider -EA 0 |
    % { try { Stop-Process -Id $_.Id -Force -EA Stop; L "encerrado $($_.Name)" } catch { L "encerrar $($_.Name) - $($_.Exception.Message)" } }
