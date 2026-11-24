# Desfaz o debloat-boot.ps1 (rode como administrador)
Set-Service -Name 'ROG Live Service' -StartupType Automatic
Set-Service -Name 'GameSDK Service' -StartupType Automatic
Set-Service -Name 'Okta Auto Update Service' -StartupType Automatic
Set-Service -Name 'OktaAuthenticatorService' -StartupType Automatic
Set-Service -Name 'OktaFeatureFlagService' -StartupType Automatic
Set-Service -Name 'tifSer' -StartupType Automatic
Set-Service -Name 'XTU3SERVICE' -StartupType Automatic
Set-Service -Name 'ExitLagPmService' -StartupType Automatic
Set-Service -Name 'ProtonVPN Service' -StartupType Automatic
Set-Service -Name 'NetTimeSvc' -StartupType Automatic
Set-Service -Name 'Bonjour Service' -StartupType Automatic
Set-Service -Name 'XboxService' -StartupType Automatic
Set-Service -Name 'WSLService' -StartupType Automatic
Enable-ScheduledTask -TaskPath '\' -TaskName 'MSIAfterburner'
Enable-ScheduledTask -TaskPath '\' -TaskName 'Okta Verify Activation Task'
Enable-ScheduledTask -TaskPath '\' -TaskName 'Tif Task'
Set-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run32' -Name 'NetTime' -Value ([byte[]](0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00))
