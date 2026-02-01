@echo off
if "%1"=="" (
    echo Usage: build.bat [debug^|release]
    echo   debug   - Build Debug version
    echo   release - Build Release version
    exit /b 1
)

if /i "%1"=="debug" (
    set BUILD_TYPE=Debug
    echo Building Debug version...
) else if /i "%1"=="release" (
    set BUILD_TYPE=Release
    echo Building Release version...
) else (
    echo Invalid build type: %1
    echo Use: build.bat [debug^|release]
    exit /b 1
)

if not exist build mkdir build
if not exist build\conan mkdir build\conan

echo Installing dependencies...
conan install . -of=build/conan -s build_type=%BUILD_TYPE% -s compiler.cppstd=17 --build=missing
if errorlevel 1 (
    echo Error: conan install failed
    exit /b 1
)

echo Configuring CMake...
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake
if errorlevel 1 (
    echo Error: cmake configure failed
    exit /b 1
)

echo Building %BUILD_TYPE% version...
cmake --build build --config %BUILD_TYPE% --target nvr
if errorlevel 1 (
    echo Error: build failed
    exit /b 1
)

echo.
echo Build completed successfully!
echo Output: build\src\%BUILD_TYPE%\nvr.exe
