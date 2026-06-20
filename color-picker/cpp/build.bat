@echo off
setlocal
cd /d "%~dp0"

rem ── set up MSVC if not already in a developer environment ──
if not "%INCLUDE%"=="" goto compile
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto noenv
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto noenv
if exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not "%INCLUDE%"=="" goto compile

:noenv
echo [build] MSVC environment not found.
echo         Open "x64 Native Tools Command Prompt for VS" and run build.bat again.
exit /b 1

:compile
if not exist obj mkdir obj
cl /nologo /EHsc /std:c++17 /O2 /DNDEBUG /Foobj\ ^
   app.cpp ui.cpp colordlg.cpp sniffer.cpp search.cpp swirl.cpp texture.cpp ^
   /Fe:picker.exe
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK -^> picker.exe
