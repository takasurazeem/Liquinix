#pragma once
// Nomes dos executáveis do projeto (em inglês, prefixo Sequoia). O Gerenciador de Tarefas mostra a
// descrição e o ícone de cada um (recursos gerados por tools\recursos.ps1 no build.cmd).
// Os nomes antigos (Shell.exe, Arquivos.exe...) só aparecem onde é preciso reconhecer processos velhos.
#define EXE_SHELL        L"WacOSShell.exe"            // antes Shell.exe
#define EXE_FINDER       L"WacOSFinder.exe"           // antes Arquivos.exe
#define EXE_SETTINGS     L"WacOSSettings.exe"         // antes Ajustes.exe
#define EXE_MONITOR      L"WacOSActivityMonitor.exe"  // antes MonitorAtividade.exe
#define EXE_TERMINAL     L"WacOSTerminal.exe"         // antes Terminal.exe
#define EXE_CALCULATOR   L"WacOSCalculator.exe"       // antes Calculadora.exe
#define EXE_NOTES        L"WacOSNotes.exe"            // antes Notas.exe
#define EXE_SCREENSHOT   L"WacOSScreenshot.exe"       // Captura de Tela (Win+Shift+3/4/5)
#define EXE_PREVIEW      L"WacOSPreview.exe"          // Pré-Visualização (imagens e PDF)
#define EXE_REMINDERS    L"WacOSReminders.exe"        // Lembretes
#define EXE_CALENDAR     L"WacOSCalendar.exe"         // Calendário
#define EXE_TEXTEDIT     L"WacOSTextEdit.exe"         // TextEdit
#define EXE_CLOCK        L"WacOSClock.exe"            // Relógio
#define EXE_WEATHER      L"WacOSWeather.exe"          // Clima
#define EXE_CONTACTS     L"WacOSContacts.exe"         // Contatos
#define EXE_PHOTOS       L"WacOSPhotos.exe"           // Fotos
#define EXE_TIMEMACHINE  L"WacOSTimeMachine.exe"      // Time Machine
#define EXE_MUSIC        L"WacOSMusic.exe"            // Música
#define EXE_QUICKTIME    L"WacOSQuickTime.exe"        // QuickTime Player
#define EXE_FORCEQUIT    L"WacOSForceQuit.exe"        // Forçar Encerrar Aplicativos (Ctrl+Alt+Esc)
#define EXE_TITLEBARS    L"WacOSTitleBars.exe"         // só as barras de título do Mac, sem o Shell (src	itulos)
#define DLL_WINDOW       L"WacOSWindow.dll"          // antes MacJanela.dll
#define DLL_WINDOW32     L"WacOSWindow32.dll"         // antes MacJanela32.dll
// (WacOSWindow32Host.exe saiu no build-12: o hospedeiro de 32 bits agora é o rundll32 do Windows)
