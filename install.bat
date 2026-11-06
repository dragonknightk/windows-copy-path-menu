@echo off
setlocal
set "SELF_BAT=%~f0"
set "PS64=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" (
    set "PS64=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
)

net session >nul 2>&1
if %errorlevel% neq 0 (
    "%PS64%" -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%SystemRoot%\System32\cmd.exe' -ArgumentList '/c', ('\"\"' + $env:SELF_BAT + '\"\"') -Verb RunAs"
    exit /b
)

"%PS64%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1"
set "EXIT_CODE=%errorlevel%"
if /i not "%~1"=="--no-pause" pause
exit /b %EXIT_CODE%
