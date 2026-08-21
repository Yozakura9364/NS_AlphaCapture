@echo off
setlocal
cd /d "%~dp0\..\.."
node --test tmp/ff14-transparent-capture/capture-contract.test.mjs tmp/ff14-transparent-capture/reconstruct-alpha.test.mjs
if errorlevel 1 exit /b 1
cd /d "%~dp0"
node generate-example.mjs
call make-transparent.cmd examples\black.png examples\white.png examples\transparent.png
node verify-output.mjs examples\transparent.png
endlocal
