# Debloat — 2026-11-07

Desfazer tudo: `powershell -ExecutionPolicy Bypass -File docs\debloat-desfazer.ps1` (pede UAC para a parte de admin).
Aplicação: `docs\debloat-aplicar.ps1` (usuário) + `-Admin` (UAC). Log: `docs\debloat-log.txt`.

## Mantido de propósito
Shell.exe/MacJanela, Explorer, StartAllBack, Windhawk, HackBGRT, WpnService/WpnUserService (Central de Notificações),
CDPSvc (o Control Center lê `CDP\NearShareChannelUserAuthzPolicy`), **WSearch** (o Finder `src/finder/search.cpp` e o fallback
do Spotlight usam `Search.CollatorDSO`), Relógio/Calendário (os widgets abrem `ms-clock:` e `outlookcal:`), áudio e licenças de
música (Waves, CodeMeter, PACE, Antares, Sonarworks), Vanguard, NVIDIA, Xbox/Gaming Services, Defender, Malwarebytes, Okta, WSL,
SysMain, Spooler, Everything, reWASD, Armoury Crate, Discord/Proton/Chrome/Edge no boot (o usuário preferiu manter).

## Mudanças (valor original → novo)

### Serviços (HKLM\SYSTEM\CurrentControlSet\Services\<nome>\Start), admin
| Serviço | Original | Novo |
|---|---|---|
| DiagTrack | Automático (2) | Manual (3) |
| InventorySvc | Automático atrasado (2, Delayed=1) | Manual |
| PcaSvc | Automático atrasado | Manual |
| TrkWks | Automático | Manual |
| MapsBroker | Automático atrasado | Manual |
| WSAIFabricSvc | Automático | Manual |
| asus (ASUS Update) | Automático atrasado | Manual |
| AsusUpdateCheck | Automático | Manual |
| edgeupdate | Automático atrasado | Manual |
| GoogleUpdaterService157.0.8089.0 | Automático | Manual |
| GoogleUpdaterInternalService157.0.8089.0 | Automático | Manual |

### Tarefas agendadas (todas estavam Ready → Disabled), admin
`\MiniToolPartitionWizard`, `\MobaUpdater`, `\Launch Adobe CCXProcess`, `\Ubisoft\Ubisoft Connect Background Update`

### Registro HKCU
| Chave \ valor | Original | Novo |
|---|---|---|
| `...\BackgroundAccessApplications\<PFN>` Disabled, DisabledByUser — para WebExperience, StartExperiencesApp, WidgetsPlatformRuntime, YourPhone, CrossDevice, XboxGamingOverlay, GamingApp | não existiam | 1, 1 |
| `...\Explorer\Advanced\TaskbarDa` | não existia | **não aplicado** (bloqueado pelo UCPD do Windows; nada mudou) |
| `...\CurrentVersion\Mobility\CrossDeviceEnabled` | 1 | 0 |
| `...\CrossDeviceResume\Configuration\IsResumeAllowed` | 1 | 0 |
| `...\CurrentVersion\GameDVR\AppCaptureEnabled` | não existia | 0 |
| `HKCU\System\GameConfigStore\GameDVR_Enabled` | não existia | 0 |
| `HKCU\Software\Microsoft\GameBar\UseNexusForGameBarEnabled` | não existia | 0 |

### Apps UWP removidos (só deste usuário; voltam pela Store)
MicrosoftSolitaireCollection, BingNews, GetHelp, ZuneVideo, People, PowerAutomateDesktop, Windows.DevHome, BingSearch.
People e Dev Home foram descontinuados pela Microsoft e podem não estar mais na Store.

### Resultado
Ponto de restauração "Antes do debloat (shell-proprio)" criado. RAM em uso: 22.247 MB e 496 processos antes → 21.649 MB e 491 processos
logo depois (sem reiniciar; os serviços em Manual só deixam de subir no próximo boot).

### Não alterado
SoundID Reference no boot: a entrada em `StartupApproved\Run` é órfã (não existe valor Run nem processo), nada a fazer.
