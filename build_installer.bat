@echo off
setlocal enabledelayedexpansion

REM ========================================
REM  NVR Service Installer Builder v3.0
REM  Supports WiX 4.x and Inno Setup
REM ========================================

REM Enable ANSI color support (Windows 10+)
for /F "tokens=4 delims=." %%a in ('ver') do set "WIN_MINOR=%%a"
set "MIN_WIN10=15063"
if %WIN_MINOR% GEQ %MIN_WIN10% (
    reg add HKCU\Console /v VirtualTerminalLevel /t REG_DWORD /d 1 /f >nul 2>&1
)

REM Setup color codes (ESC = ASCII 27)
for /f %%E in ('echo prompt $E ^| cmd') do set "ESC=%%E"
set "GREEN=%ESC%[92m"
set "RED=%ESC%[91m"
set "YELLOW=%ESC%[93m"
set "BLUE=%ESC%[94m"
set "CYAN=%ESC%[96m"
set "RESET=%ESC%[0m"

REM Default values
set "VERSION=1.0.0"
set "CONFIG_DIR=installer_output"
set "EXE_NAME=nvr.exe"
set "INSTALLER_NAME=nvr-service-setup"
set "BUILD_SYSTEM=wix"

REM Parse command line arguments
set "CLEAN=0"
set "SKIP_BUILD=0"
set "SIGN=0"
set "VERSION="
set "BUILD_SYSTEM="

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--clean" set "CLEAN=1"
if /i "%~1"=="--skip-build" set "SKIP_BUILD=1"
if /i "%~1"=="--sign" set "SIGN=1"
if /i "%~1"=="--version" (
    shift
    if not "%~1"=="" set "VERSION=%~1"
)
if /i "%~1"=="--wix" set "BUILD_SYSTEM=wix"
if /i "%~1"=="--innosetup" set "BUILD_SYSTEM=innosetup"
shift
goto parse_args

:args_done

echo.
echo %GREEN%========================================
echo   NVR Service Installer Builder v3.0
echo========================================%RESET%
echo.

REM Default to WiX
if "%BUILD_SYSTEM%"=="" set "BUILD_SYSTEM=wix"

echo Build Options:
echo   - Version: %VERSION%
echo   - Build System: %BUILD_SYSTEM%
echo   - Clean Files: %CLEAN%
echo   - Skip Build: %SKIP_BUILD%
echo   - Code Signing: %SIGN%
echo.

REM ========================================
REM Step 1: Clean
REM ========================================
if "%CLEAN%"=="1" (
    echo [%CYAN%1/4%RESET%] Cleaning old files...
    if exist %CONFIG_DIR% (
        rd /s /q %CONFIG_DIR% 2>nul
        echo     Deleted: %CONFIG_DIR%
    )
    if exist build (
        rd /s /q build 2>nul
        echo     Deleted: build
    )
    echo.
) else (
    echo [%CYAN%1/4%RESET%] Checking files...
    if exist %CONFIG_DIR% (
        echo     Found: %CONFIG_DIR%
    )
    echo.
)

REM ========================================
REM Step 2: Build
REM ========================================
if "%SKIP_BUILD%"=="0" (
    echo [%CYAN%2/4%RESET%] Building Release version...
    echo     Building project...
    call build.bat release >nul 2>&1

    if errorlevel 1 (
        echo     %RED%X Build failed%RESET%
        pause
        exit /b 1
    )

    echo     √ Build successful
    echo.
) else (
    echo [%CYAN%2/4%RESET%] Skipping build, using existing files...
    echo.
)

REM ========================================
REM Step 3: Build Installer
REM ========================================
echo [%CYAN%3/4%RESET%] Building installer...
echo     Using: %BUILD_SYSTEM%

if /i "%BUILD_SYSTEM%"=="wix" (
    echo     Checking for WiX Toolset...

    REM Check if wix.exe is in PATH
    where wix >nul 2>&1
    if errorlevel 1 (
        echo     %YELLOW%X WiX Toolset not found in PATH%RESET%
        echo.
        echo     Please install WiX Toolset v4.x:
        echo     https://wixtoolset.org/releases/
        echo.
        echo     Or use: winget install WiX.Toolset
        echo.
        echo     To use Inno Setup instead, run:
        echo     build_installer.bat --innosetup
        echo.
        pause
        exit /b 1
    )

    echo     √ WiX Toolset found

    REM Configure and build with CMake
    echo     Configuring CMake...
    if not exist build mkdir build
    if not exist build\conan mkdir build\conan

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
        -DCMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake >nul 2>&1

    if errorlevel 1 (
        echo     %RED%X CMake configuration failed%RESET%
        echo.
        echo     Try running manually:
        echo     cmake -S . -B build -G "Visual Studio 17 2022" -A x64
        echo.
        pause
        exit /b 1
    )

    echo     √ CMake configured
    echo     Building MSI installer...

    REM Build the package target
    cmake --build build --config Release --target package >nul 2>&1

    if errorlevel 1 (
        echo     %RED%X Package build failed%RESET%
        echo.
        echo     Try running manually:
        echo     cd build
        echo     cpack -G WIX
        echo.
        pause
        exit /b 1
    )

    echo     √ MSI installer created

) else if /i "%BUILD_SYSTEM%"=="innosetup" (
    echo     Checking for Inno Setup...
    where iscc >nul 2>&1
    if errorlevel 1 (
        echo     %YELLOW%X Inno Setup not found%RESET%
        echo.
        echo     Please install Inno Setup:
        echo     https://jrsoftware.org/isdl.php
        echo.
        echo     Or use Scoop:
        echo     scoop install innosetup
        echo.
        echo     To use WiX instead, run:
        echo     build_installer.bat --wix
        echo.
        pause
        exit /b 1
    )

    echo     √ Inno Setup found
    echo     Building installer...

    iscc installer.iss >nul 2>&1

    if errorlevel 1 (
        echo     %RED%X Installer build failed%RESET%
        pause
        exit /b 1
    )

    echo     √ Installer created
)

echo.

REM ========================================
REM Summary
REM ========================================
echo ========================================
echo   %GREEN%√ All Complete%RESET%
echo ========================================
echo.

echo %CYAN%Output Files:%RESET%
if /i "%BUILD_SYSTEM%"=="wix" (
    echo   - Executable: build\src\Release\%EXE_NAME%
    echo   - Installer: build\*.msi
) else (
    echo   - Executable: build\src\Release\%EXE_NAME%
    echo   - Installer: %CONFIG_DIR%\%INSTALLER_NAME%-%VERSION%.exe
)

echo.
echo %CYAN%Next Steps:%RESET%
echo   1. Test the installer
echo   2. Verify service installation
echo   3. Optional: Sign the installer
echo   4. Upload to GitHub Release
echo.

pause
