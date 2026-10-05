@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Remove-Keypad.ps1"
exit /b %ERRORLEVEL%
