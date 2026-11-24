#pragma once
// Nomes dos executáveis do projeto (em inglês, prefixo Sequoia). O Gerenciador de Tarefas mostra a
// descrição e o ícone de cada um (recursos gerados por tools\recursos.ps1 no build.cmd).
// Os nomes antigos (Shell.exe, Arquivos.exe...) só aparecem onde é preciso reconhecer processos velhos.
#define EXE_SHELL        L"LiquinixShell.exe"            // antes Shell.exe
#define EXE_FINDER       L"LiquinixFinder.exe"           // antes Arquivos.exe
#define EXE_SETTINGS     L"LiquinixSettings.exe"         // antes Ajustes.exe
#define EXE_MONITOR      L"LiquinixActivityMonitor.exe"  // antes MonitorAtividade.exe
#define EXE_TERMINAL     L"LiquinixTerminal.exe"         // antes Terminal.exe
#define EXE_CALCULATOR   L"LiquinixCalculator.exe"       // antes Calculadora.exe
#define EXE_NOTES        L"LiquinixNotes.exe"            // antes Notas.exe
#define EXE_SCREENSHOT   L"LiquinixScreenshot.exe"       // Captura de Tela (Win+Shift+3/4/5)
#define EXE_PREVIEW      L"LiquinixPreview.exe"          // Pré-Visualização (imagens e PDF)
#define EXE_REMINDERS    L"LiquinixReminders.exe"        // Lembretes
#define EXE_CALENDAR     L"LiquinixCalendar.exe"         // Calendário
#define EXE_TEXTEDIT     L"LiquinixTextEdit.exe"         // TextEdit
#define EXE_CLOCK        L"LiquinixClock.exe"            // Relógio
#define EXE_WEATHER      L"LiquinixWeather.exe"          // Clima
#define EXE_CONTACTS     L"LiquinixContacts.exe"         // Contatos
#define EXE_PHOTOS       L"LiquinixPhotos.exe"           // Fotos
#define EXE_TIMEMACHINE  L"LiquinixTimeMachine.exe"      // Time Machine
#define EXE_MUSIC        L"LiquinixMusic.exe"            // Música
#define EXE_QUICKTIME    L"LiquinixQuickTime.exe"        // QuickTime Player
#define EXE_FORCEQUIT    L"LiquinixForceQuit.exe"        // Forçar Encerrar Aplicativos (Ctrl+Alt+Esc)
#define EXE_TITLEBARS    L"LiquinixTitleBars.exe"         // só as barras de título do Mac, sem o Shell (src	itulos)
#define DLL_WINDOW       L"LiquinixWindow.dll"          // antes MacJanela.dll
#define DLL_WINDOW32     L"LiquinixWindow32.dll"         // antes MacJanela32.dll
// (LiquinixWindow32Host.exe saiu no build-12: o hospedeiro de 32 bits agora é o rundll32 do Windows)
