@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "POWERSHELL_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

if exist "%POWERSHELL_EXE%" (
    "%POWERSHELL_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%bin_to_ihex_gui.ps1"
    goto :eof
)

echo PowerShell not found.
pause
