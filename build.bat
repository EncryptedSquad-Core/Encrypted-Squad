@echo off
setlocal

set "ROOT_DIR=%~dp0"
set "BUILD_DIR=%ROOT_DIR%build"

echo ===================================================
echo Encrypted Squad - KVDB Offline Release Build
echo ===================================================

where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [ERROR] cmake not found in PATH. Please ensure CMake is installed. 1>&2
    exit /b 1
)

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

echo [1/2] Configuring CMake (Release)...
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed. 1>&2
    exit /b 1
)

echo [2/2] Building targets (Release)...
cmake --build "%BUILD_DIR%" --config Release --parallel
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed. 1>&2
    exit /b 1
)

echo ===================================================
echo [SUCCESS] Build complete!
echo   Engine CLI : %BUILD_DIR%\kvdb.exe (via kvdb.cmd)
echo   Torture CLI: %BUILD_DIR%\torture.exe (via torture.cmd)
echo ===================================================
exit /b 0