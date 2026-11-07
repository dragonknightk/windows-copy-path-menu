@echo off
setlocal
set "INSTALLED_EXE=%ProgramFiles%\CopyPathMenu\copy-path.exe"
if defined ProgramW6432 (
    if exist "%ProgramW6432%\CopyPathMenu\copy-path.exe" (
        set "INSTALLED_EXE=%ProgramW6432%\CopyPathMenu\copy-path.exe"
    )
)
set "RELEASE_EXE=%~dp0release\copy-path.exe"

if exist "%INSTALLED_EXE%" (
    start "" "%INSTALLED_EXE%" --config
    exit /b 0
)

if exist "%RELEASE_EXE%" (
    start "" "%RELEASE_EXE%" --config
    exit /b 0
)

echo CopyPathMenu executable was not found. Please run install.bat first.
pause
exit /b 1
