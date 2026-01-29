@echo off
setlocal enabledelayedexpansion

REM ========================================
REM  NVR Service 安装包构建脚本
REM  版本: 2.0
REM ========================================

REM 启用 ANSI 颜色支持 (Windows 10+)
for /F "tokens=4 delims=." %%a in ('ver') do set "WIN_MINOR=%%a"
set "MIN_WIN10=15063"
if %WIN_MINOR% GEQ %MIN_WIN10% (
    reg add HKCU\Console /v VirtualTerminalLevel /t REG_DWORD /d 1 /f >nul 2>&1
)

REM 设置颜色代码 (ESC = ASCII 27)
for /f %%E in ('echo prompt $E ^| cmd') do set "ESC=%%E"
set "GREEN=%ESC%[92m"
set "RED=%ESC%[91m"
set "YELLOW=%ESC%[93m"
set "BLUE=%ESC%[94m"
set "CYAN=%ESC%[96m"
set "RESET=%ESC%[0m"

REM 设置默认值
set "VERSION=1.0.0"
set "CONFIG_DIR=installer_output"
set "EXE_NAME=nvr.exe"
set "INSTALLER_NAME=nvr-service-setup"
set "INNO_SETUP_PATH=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"

REM 解析命令行参数
set "CLEAN=0"
set "SKIP_BUILD=0"
set "SIGN=0"
set "VERSION="

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--clean" set "CLEAN=1"
if /i "%~1"=="--skip-build" set "SKIP_BUILD=1"
if /i "%~1"=="--sign" set "SIGN=1"
if /i "%~1"=="--version" (
    shift
    if not "%~1"=="" set "VERSION=%~1"
)
shift
goto parse_args

:args_done

echo.
echo %GREEN%========================================
echo   NVR Service 安装包构建工具 v2.0
echo========================================%RESET%
echo.

REM 检查构建目录
if not exist "build" (
    echo %RED%错误: build 目录不存在%RESET%
    echo.
    echo 请先运行以下命令创建构建目录:
    echo   build.bat debug
    echo.
    pause
    exit /b 1
)

REM 显示配置
echo %CYAN%构建配置:%RESET%
echo   - 版本号: %VERSION%
echo   - 输出目录: %CONFIG_DIR%
echo   - 清理旧文件: %CLEAN%
echo   - 跳过构建: %SKIP_BUILD%
echo   - 数字签名: %SIGN%
echo.

REM ========================================
REM 步骤 1: 清理
REM ========================================
if "%CLEAN%"=="1" (
    echo [%CYAN%1/4%RESET%] 清理旧文件...
    if exist %CONFIG_DIR% (
        rd /s /q %CONFIG_DIR% 2>nul
        echo     已删除: %CONFIG_DIR%
    ) else (
        echo     跳过清理（目录不存在）
    )
    echo.
) else (
    echo [%CYAN%1/4%RESET%] 保留旧文件...
    if exist %CONFIG_DIR% (
        echo     保留: %CONFIG_DIR%
    )
    echo.
)

REM ========================================
REM 步骤 2: 构建
REM ========================================
if "%SKIP_BUILD%"=="0" (
    echo [%CYAN%2/4%RESET%] 构建 Release 版本...
    echo     正在编译项目...
    call build.bat release >nul 2>&1

    if errorlevel 1 (
        echo %RED%     ? 构建失败！%RESET%
        pause
        exit /b 1
    )

    echo     ? 构建成功
    echo.
) else (
    echo [%CYAN%2/4%RESET%] 跳过构建（使用现有文件）...
    echo.
)

REM ========================================
REM 步骤 3: 准备文件
REM ========================================
echo [%CYAN%3/4%RESET%] 准备安装包文件...

REM 确保输出目录存在
if not exist %CONFIG_DIR% (
    mkdir %CONFIG_DIR%
)

REM 复制主程序
echo     - 复制主程序...
copy /Y build\src\Release\%EXE_NAME% %CONFIG_DIR%\ >nul 2>&1
if errorlevel 1 (
    echo     %RED%? 文件不存在: build\src\Release\%EXE_NAME%%RESET%
    pause
    exit /b 1
)

REM 复制配置文件
if exist "config.yaml.example" (
    echo     - 复制配置示例...
    copy /Y config.yaml.example %CONFIG_DIR%\ >nul 2>&1
)

REM 复制文档文件
if exist "INSTALL.md" (
    echo     - 复制安装文档...
    copy /Y INSTALL.md %CONFIG_DIR%\ >nul 2>&1
)

if exist "README.md" (
    echo     - 复制 README...
    copy /Y README.md %CONFIG_DIR%\ >nul 2>&1
)

if exist "LICENSE" (
    echo     - 复制许可证...
    copy /Y LICENSE %CONFIG_DIR%\ >nul 2>&1
)

echo     ? 文件准备完成
echo.

REM ========================================
REM 步骤 4: 生成安装包
REM ========================================
echo [%CYAN%4/4%RESET%] 生成安装包...
echo     检查 Inno Setup...

REM 检查 Inno Setup 是否安装
where iscc >nul 2>&1
if errorlevel 1 (
    echo     %YELLOW%  ? Inno Setup 未找到%RESET%
    echo.
    echo     请下载安装 Inno Setup:
    echo     https://jrsoftware.org/isdl.php
    echo.
    echo     或使用 Scoop 安装:
    echo     scoop install innosetup
    echo.
    echo     当前状态: 文件已准备好到 %CONFIG_DIR%
    echo.
    echo     安装 Inno Setup 后, 手动运行:
    echo       iscc installer.iss
    echo.
    goto :skip_compile
)

echo     ? Inno Setup 已找到
echo     正在编译安装脚本...

REM 生成安装包
iscc installer.iss >nul 2>&1

if errorlevel 1 (
    echo     %RED%     ? 安装包生成失败！%RESET%
    pause
    exit /b 1
)

echo     ? 安装包生成成功
echo.

REM ========================================
REM 完成
REM ========================================
:skip_compile
echo ========================================
echo   %GREEN%? 构建完成！%RESET%
echo ========================================
echo.
echo %CYAN%输出文件:%RESET%
echo   - 可执行文件: build\src\Release\%EXE_NAME%
echo   - 安装包: %CONFIG_DIR%\%INSTALLER_NAME%-%VERSION%.exe
echo.
echo %CYAN%文件信息:%RESET%
dir "%CONFIG_DIR%\%INSTALLER_NAME%-%VERSION%.exe" 2>nul | findstr /C:"nvr-service-setup"

echo.
echo %YELLOW%下一步:%RESET%
echo   1. 测试安装程序: %CONFIG_DIR%\%INSTALLER_NAME%-%VERSION%.exe
echo   2. 在虚拟机中测试完整安装流程
echo   3. 数字签名（可选）
echo   4. 上传到 GitHub Release
echo.

pause
