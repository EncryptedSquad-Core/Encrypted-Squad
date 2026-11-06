@echo off
setlocal

set "ROOT_DIR=%~dp0"
set "EXE_PATH="

if exist "%ROOT_DIR%build\Release\torture.exe" (
    set "EXE_PATH=%ROOT_DIR%build\Release\torture.exe"
) else if exist "%ROOT_DIR%build\torture.exe" (
    set "EXE_PATH=%ROOT_DIR%build\torture.exe"
) else if exist "%ROOT_DIR%build\bin\Release\torture.exe" (
    set "EXE_PATH=%ROOT_DIR%build\bin\Release\torture.exe"
) else if exist "%ROOT_DIR%build\bin\torture.exe" (
    set "EXE_PATH=%ROOT_DIR%build\bin\torture.exe"
) else if exist "%ROOT_DIR%torture.exe" (
    set "EXE_PATH=%ROOT_DIR%torture.exe"
)

if "%EXE_PATH%"=="" (
    echo [ERROR] torture.exe not found. Please run build.bat first. 1>&2
    exit /b 1
)

"%EXE_PATH%" %*
exit /b %ERRORLEVEL%