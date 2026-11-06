@echo off
setlocal

set "ROOT_DIR=%~dp0"
set "EXE_PATH="

if exist "%ROOT_DIR%build\Release\kvdb.exe" (
    set "EXE_PATH=%ROOT_DIR%build\Release\kvdb.exe"
) else if exist "%ROOT_DIR%build\kvdb.exe" (
    set "EXE_PATH=%ROOT_DIR%build\kvdb.exe"
) else if exist "%ROOT_DIR%build\bin\Release\kvdb.exe" (
    set "EXE_PATH=%ROOT_DIR%build\bin\Release\kvdb.exe"
) else if exist "%ROOT_DIR%build\bin\kvdb.exe" (
    set "EXE_PATH=%ROOT_DIR%build\bin\kvdb.exe"
) else if exist "%ROOT_DIR%kvdb.exe" (
    set "EXE_PATH=%ROOT_DIR%kvdb.exe"
)

if "%EXE_PATH%"=="" (
    echo [ERROR] kvdb.exe not found. Please run build.bat first. 1>&2
    exit /b 1
)

"%EXE_PATH%" %*
exit /b %ERRORLEVEL%