@echo off
setlocal
cd /d "%~dp0"
set "BLACK=%~1"
set "WHITE=%~2"
set "OUTPUT=%~3"
if not defined BLACK set "BLACK=black.png"
if not defined WHITE set "WHITE=white.png"
if not defined OUTPUT set "OUTPUT=transparent.png"
if not exist "%BLACK%" (
  echo Missing black capture: %BLACK%
  exit /b 2
)
if not exist "%WHITE%" (
  echo Missing white capture: %WHITE%
  exit /b 2
)
node reconstruct-alpha.mjs "%BLACK%" "%WHITE%" "%OUTPUT%"
endlocal
