@echo off
rem Recompila sequoia-lockscreen.dll com o compilador do próprio Windhawk (leva alguns minutos). Sem admin.
cd /d "%~dp0"
set C=C:\Program Files\Windhawk\Compiler
"%C%\bin\clang++.exe" -std=c++23 -O2 -shared -target x86_64-w64-mingw32 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -DNTDDI_VERSION=0x0A000008 -D__USE_MINGW_ANSI_STDIO=0 -DWH_MOD "-DWH_MOD_ID=L\"local@sequoia-lockscreen\"" "-DWH_MOD_VERSION=L\"1.0\"" -I"%C%\include" -include windhawk_api.h -x c++ sequoia-lockscreen.wh.cpp -x none "C:\Program Files\Windhawk\Engine\1.7.3\64\windhawk.lib" -lcomctl32 -lole32 -loleaut32 -lruntimeobject -lshlwapi -static -w -o sequoia-lockscreen.dll
