@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Keypad.ps1"
exit /b %ERRORLEVEL%
