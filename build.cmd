@echo off
rem Compila o shell proprio (C++ Win32 + Direct2D, Build Tools 2022).
rem Os programas tem nomes em ingles com prefixo Sequoia (src\comum\nomes.h) e icone + descricao
rem gerados por tools\recursos.ps1 (bin\obj\res).
rem   build.cmd           compila tudo
rem   build.cmd finder    so o Finder (WacOSFinder.exe)
rem   build.cmd menu      so a bancada do menu (MenuTeste.exe)
rem   build.cmd shell     so o shell (WacOSShell.exe: barra de menus + Dock)
rem   build.cmd ajustes   so o Ajustes do Sistema (WacOSSettings.exe)
rem   build.cmd monitor   so o Monitor de Atividade (WacOSActivityMonitor.exe)
rem   build.cmd terminal  so o Terminal (WacOSTerminal.exe: PowerShell e Prompt de Comando)
rem   build.cmd calculadora  so a Calculadora (WacOSCalculator.exe)
rem   build.cmd notas     so o Notas (WacOSNotes.exe)
rem   build.cmd captura   so a Captura de Tela (WacOSScreenshot.exe: Win+Shift+3/4/5)
rem   build.cmd preview   so a Pre-Visualizacao (WacOSPreview.exe: imagens e PDF)
rem   build.cmd apps      so Lembretes, Calendario e TextEdit
rem   build.cmd janelas   barra de titulo do Mac nos outros programas (WacOSWindow.dll, 32 e 64 bits, e WacOSTitleBars.exe)
rem   build.cmd setup     so o WacOSSetup.exe (instalar, atualizar e desinstalar)
rem   build.cmd titulos   so o WacOSTitleBars.exe (as barras de titulo sem o Shell)
rem   build.cmd bandeja   so o prototipo da bandeja (BandejaTeste.exe)
setlocal
set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
rem sem o Build Tools (ex.: a maquina do GitHub Actions, com o Visual Studio completo): o vswhere acha o que tiver C++
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (echo Visual Studio 2022 com C++ nao encontrado. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin\obj mkdir bin\obj
if not exist bin\ferramentas mkdir bin\ferramentas

rem icones e descricoes (VERSIONINFO) de cada programa
set "RES=bin\obj\res"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\recursos.ps1 -Out %RES% >nul || exit /b 1
for %%n in (WacOSShell WacOSFinder WacOSSettings WacOSActivityMonitor WacOSTerminal WacOSCalculator WacOSNotes WacOSScreenshot WacOSPreview WacOSReminders WacOSCalendar WacOSTextEdit WacOSClock WacOSWeather WacOSContacts WacOSPhotos WacOSTimeMachine WacOSMusic WacOSQuickTime WacOSForceQuit WacOSTitleBars WacOSSetup WacOSWindow WacOSWindow32) do (
  rc /nologo /fo %RES%\%%n.res %RES%\%%n.rc >nul || exit /b 1
)

set CFLAGS=/nologo /EHsc /O2 /W3 /std:c++20 /utf-8 /DUNICODE /D_UNICODE /Fo:bin\obj\
set LIBS=user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib d2d1.lib dwrite.lib dwmapi.lib shcore.lib uuid.lib advapi32.lib
set LFLAGS=/link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED %LIBS%

if /i "%1"=="dialogo" (
  cl %CFLAGS% /LD src\dialogo\dialogo.cpp /Fe:bin\DialogoMac.dll /link /IMPLIB:bin\obj\DialogoMac.lib %LIBS% || exit /b 1
  cl %CFLAGS% src\tools\dialogo_teste.cpp /Fe:bin\ferramentas\DialogoTeste.exe %LFLAGS% || exit /b 1
  exit /b 0
)

if /i "%1"=="janelas" (
  rem barra de titulo do Mac nas janelas dos outros programas: DLL de 64 bits e de 32 (esta roda no rundll32 de 32 bits do Windows)
  cl %CFLAGS% /LD src\janelas\macjanela.cpp src\janelas\controles.cpp %RES%\WacOSWindow.res /Fe:bin\WacOSWindow.dll /link /IMPLIB:bin\obj\WacOSWindow.lib %LIBS% || exit /b 1
  call :titulos || exit /b 1
  call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
  if not exist bin\obj32 mkdir bin\obj32
  cl /nologo /EHsc /O2 /W3 /std:c++20 /utf-8 /DUNICODE /D_UNICODE /Fo:bin\obj32\ /LD src\janelas\macjanela.cpp src\janelas\controles.cpp %RES%\WacOSWindow32.res /Fe:bin\WacOSWindow32.dll /link /IMPLIB:bin\obj32\WacOSWindow32.lib %LIBS% || exit /b 1
  exit /b 0
)

if /i "%1"=="setup" (
  call :setup || exit /b 1
  exit /b 0
)

if /i "%1"=="titulos" (
  call :titulos || exit /b 1
  exit /b 0
)

if /i "%1"=="bandeja" (
  cl %CFLAGS% src\tools\bandeja_teste.cpp /Fe:bin\ferramentas\BandejaTeste.exe %LFLAGS% || exit /b 1
  exit /b 0
)

cl %CFLAGS% /c src\menu\context_menu.cpp || exit /b 1

if /i "%1"=="shell" goto shell
if /i "%1"=="ajustes" goto ajustes
if /i "%1"=="monitor" goto monitor
if /i "%1"=="terminal" goto terminal
if /i "%1"=="calculadora" goto calculadora
if /i "%1"=="notas" goto notas
if /i "%1"=="captura" goto captura
if /i "%1"=="preview" goto preview
if /i "%1"=="apps" goto apps
if /i not "%1"=="finder" (
  cl %CFLAGS% src\tools\menu_teste.cpp bin\obj\context_menu.obj /Fe:bin\ferramentas\MenuTeste.exe %LFLAGS% || exit /b 1
)
if /i not "%1"=="menu" (
  if exist src\finder\main.cpp (
    cl %CFLAGS% src\finder\*.cpp bin\obj\context_menu.obj %RES%\WacOSFinder.res /Fe:bin\WacOSFinder.exe %LFLAGS% d3d11.lib dxgi.lib dcomp.lib windowscodecs.lib || exit /b 1
  )
)
if /i not "%1"=="" exit /b 0

:ajustes
cl %CFLAGS% src\ajustes\*.cpp src\finder\gfx.cpp src\finder\shadow.cpp src\finder\textfield.cpp src\finder\tahoe.cpp bin\obj\context_menu.obj %RES%\WacOSSettings.res /Fe:bin\WacOSSettings.exe %LFLAGS% d3d11.lib dxgi.lib dcomp.lib windowscodecs.lib || exit /b 1
if /i "%1"=="ajustes" exit /b 0

:monitor
cl %CFLAGS% src\monitor\main.cpp src\finder\gfx.cpp src\finder\shadow.cpp src\finder\tahoe.cpp bin\obj\context_menu.obj %RES%\WacOSActivityMonitor.res /Fe:bin\WacOSActivityMonitor.exe %LFLAGS% windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib || exit /b 1
if /i "%1"=="monitor" exit /b 0

:terminal
cl %CFLAGS% src\terminal\main.cpp bin\obj\context_menu.obj %RES%\WacOSTerminal.res /Fe:bin\WacOSTerminal.exe %LFLAGS% windowscodecs.lib || exit /b 1
if /i "%1"=="terminal" exit /b 0

:calculadora
cl %CFLAGS% src\calculadora\main.cpp bin\obj\context_menu.obj %RES%\WacOSCalculator.res /Fe:bin\WacOSCalculator.exe %LFLAGS% windowscodecs.lib || exit /b 1
if /i "%1"=="calculadora" exit /b 0

:notas
rem objetos numa pasta propria (gfx/shadow/textfield do Finder e main.obj nao colidem com outros alvos)
if not exist bin\obj\notas mkdir bin\obj\notas
cl %CFLAGS% /Fo:bin\obj\notas\ src\notas\*.cpp src\finder\gfx.cpp src\finder\shadow.cpp src\finder\textfield.cpp bin\obj\context_menu.obj %RES%\WacOSNotes.res /Fe:bin\WacOSNotes.exe %LFLAGS% d3d11.lib dxgi.lib dcomp.lib windowscodecs.lib imm32.lib || exit /b 1
if /i "%1"=="notas" exit /b 0

:captura
rem objetos numa pasta propria (main.obj nao colide com outros alvos)
if not exist bin\obj\captura mkdir bin\obj\captura
cl %CFLAGS% /Fo:bin\obj\captura\ src\captura\main.cpp bin\obj\context_menu.obj %RES%\WacOSScreenshot.res /Fe:bin\WacOSScreenshot.exe %LFLAGS% d3d11.lib dxgi.lib windowscodecs.lib winmm.lib || exit /b 1
if /i "%1"=="captura" exit /b 0
:preview
if not exist bin\obj\preview mkdir bin\obj\preview
cl %CFLAGS% /Fo:bin\obj\preview\ src\preview\main.cpp %RES%\WacOSPreview.res /Fe:bin\WacOSPreview.exe %LFLAGS% windowscodecs.lib || exit /b 1
if /i "%1"=="preview" exit /b 0
:apps
if not exist bin\obj\lembretes mkdir bin\obj\lembretes
cl %CFLAGS% /Fo:bin\obj\lembretes\ src\lembretes\main.cpp %RES%\WacOSReminders.res /Fe:bin\WacOSReminders.exe %LFLAGS% || exit /b 1
if not exist bin\obj\calendario mkdir bin\obj\calendario
cl %CFLAGS% /Fo:bin\obj\calendario\ src\calendario\main.cpp %RES%\WacOSCalendar.res /Fe:bin\WacOSCalendar.exe %LFLAGS% || exit /b 1
if not exist bin\obj\textedit mkdir bin\obj\textedit
cl %CFLAGS% /Fo:bin\obj\textedit\ src\textedit\main.cpp %RES%\WacOSTextEdit.res /Fe:bin\WacOSTextEdit.exe %LFLAGS% || exit /b 1
if not exist bin\obj\relogio mkdir bin\obj\relogio
cl %CFLAGS% /Fo:bin\obj\relogio\ src\relogio\main.cpp %RES%\WacOSClock.res /Fe:bin\WacOSClock.exe %LFLAGS% || exit /b 1
if not exist bin\obj\clima mkdir bin\obj\clima
cl %CFLAGS% /Fo:bin\obj\clima\ src\clima\main.cpp %RES%\WacOSWeather.res /Fe:bin\WacOSWeather.exe %LFLAGS% || exit /b 1
if not exist bin\obj\contatos mkdir bin\obj\contatos
cl %CFLAGS% /Fo:bin\obj\contatos\ src\contatos\main.cpp %RES%\WacOSContacts.res /Fe:bin\WacOSContacts.exe %LFLAGS% || exit /b 1
if not exist bin\obj\fotos mkdir bin\obj\fotos
cl %CFLAGS% /Fo:bin\obj\fotos\ src\fotos\main.cpp %RES%\WacOSPhotos.res /Fe:bin\WacOSPhotos.exe %LFLAGS% || exit /b 1
if not exist bin\obj\timemachine mkdir bin\obj\timemachine
cl %CFLAGS% /Fo:bin\obj\timemachine\ src\timemachine\main.cpp %RES%\WacOSTimeMachine.res /Fe:bin\WacOSTimeMachine.exe %LFLAGS% || exit /b 1
if not exist bin\obj\musica mkdir bin\obj\musica
cl %CFLAGS% /Fo:bin\obj\musica\ src\musica\main.cpp %RES%\WacOSMusic.res /Fe:bin\WacOSMusic.exe %LFLAGS% || exit /b 1
if not exist bin\obj\quicktime mkdir bin\obj\quicktime
cl %CFLAGS% /Fo:bin\obj\quicktime\ src\quicktime\main.cpp %RES%\WacOSQuickTime.res /Fe:bin\WacOSQuickTime.exe %LFLAGS% || exit /b 1
if not exist bin\obj\forcar mkdir bin\obj\forcar
cl %CFLAGS% /Fo:bin\obj\forcar\ src\forcar\main.cpp %RES%\WacOSForceQuit.res /Fe:bin\WacOSForceQuit.exe %LFLAGS% version.lib || exit /b 1
if /i "%1"=="apps" exit /b 0

:shell
rem /Zi + /DEBUG: simbolos em bin\obj\WacOSShell.pdb (perfil de CPU e falhas); /OPT:REF,ICF mantem o exe enxuto
cl %CFLAGS% /Zi /Fd:bin\obj\shell.pdb src\shell\*.cpp bin\obj\context_menu.obj %RES%\WacOSShell.res /Fe:bin\WacOSShell.exe %LFLAGS% version.lib wlanapi.lib powrprof.lib propsys.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib dxguid.lib /DEBUG /OPT:REF /OPT:ICF /PDB:bin\obj\WacOSShell.pdb || exit /b 1
call :setup || exit /b 1
exit /b 0

rem so as barras de titulo, sem o Shell: src\titulos + o janelas.cpp do Shell (objetos em pasta propria)
:titulos
if not exist bin\obj\titulos mkdir bin\obj\titulos
cl %CFLAGS% /Fo:bin\obj\titulos\ src\titulos\main.cpp src\shell\janelas.cpp %RES%\WacOSTitleBars.res /Fe:bin\WacOSTitleBars.exe %LFLAGS% || exit /b 1
exit /b 0

rem o instalador unico (instalar, atualizar, desinstalar): src\setup
:setup
if not exist bin\obj\setup mkdir bin\obj\setup
cl %CFLAGS% /Fo:bin\obj\setup\ src\setup\main.cpp %RES%\WacOSSetup.res /Fe:bin\WacOSSetup.exe %LFLAGS% windowscodecs.lib taskschd.lib winhttp.lib bcrypt.lib || exit /b 1
exit /b 0
