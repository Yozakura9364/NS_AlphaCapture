@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products Microsoft.VisualStudio.Product.Community -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /DNDEBUG /W4 rules-unit-test.cpp /Fe:rules-unit-test.exe
if errorlevel 1 exit /b 1
rules-unit-test.exe
if errorlevel 1 exit /b 1
rc /nologo /fo NS_AlphaCapture.res NS_AlphaCapture.rc
if errorlevel 1 exit /b 1
cl /nologo /LD /O2 /EHsc /std:c++17 /utf-8 /DNDEBUG /W4 /Iinclude NS_AlphaCapture.cpp NS_AlphaCapture.res /link user32.lib windowscodecs.lib ole32.lib /OUT:NS_AlphaCapture.addon64
if errorlevel 1 exit /b 1
del /q NS_AlphaCapture.obj NS_AlphaCapture.exp NS_AlphaCapture.lib NS_AlphaCapture.res rules-unit-test.obj >nul 2>&1
echo ADDON BUILD OK
endlocal
