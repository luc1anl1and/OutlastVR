@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL exit /b 1
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
if not exist builds mkdir builds
cl /nologo /EHsc /MT /Ithird_party\minhook\include /Fobuilds\index_input_tests.obj /Febuilds\index_input_tests.exe tests\index_input_tests.cpp
if errorlevel 1 exit /b 1
builds\index_input_tests.exe
exit /b %errorlevel%
