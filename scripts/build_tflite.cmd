@echo off
REM TensorFlow Lite Build Script - Windows x64 CPU + XNNPACK
REM Target: Hand Tracking minimal size

cd /d "%~dp0.."

REM ---------------------------------------------------------------------------
REM Ensure MSVC environment is initialized (so standard headers like <limits.h> exist).
REM Running CMake/Ninja from a plain shell without VsDevCmd often fails.
REM ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set NEED_VSENV=0
if "%VCToolsInstallDir%"=="" set NEED_VSENV=1
if "%INCLUDE%"=="" set NEED_VSENV=1
echo %INCLUDE% | findstr /I "\\VC\\Tools\\MSVC" >nul || set NEED_VSENV=1

if "%NEED_VSENV%"=="1" (
    echo Initializing Visual Studio build environment...
    set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist !VSWHERE! (
        echo ERROR: vswhere.exe not found at !VSWHERE!
        echo Please run this script from "Developer Command Prompt for VS" or install Visual Studio Build Tools.
        exit /b 1
    )

    for /f "usebackq delims=" %%i in (`!VSWHERE! -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set VSINSTALL=%%i
    )
    if "!VSINSTALL!"=="" (
        echo ERROR: Could not locate a Visual Studio installation with VC tools.
        exit /b 1
    )

    call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    if errorlevel 1 (
        echo ERROR: VsDevCmd.bat failed.
        exit /b 1
    )
)

set TFLITE_SRC=HandTracker\libs\tensorflow\tensorflow\lite
set TENSORFLOW_ROOT=HandTracker\libs\tensorflow
set BUILD_DIR=HandTracker\libs\tensorflow\tflite_build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo Configuring CMake...
cmake -S "%TFLITE_SRC%" -B "%BUILD_DIR%" ^
    -G Ninja ^
    -DCMAKE_OBJECT_PATH_MAX=180 ^
    -DTENSORFLOW_SOURCE_DIR="%CD%\%TENSORFLOW_ROOT%" ^
    -DCMAKE_BUILD_TYPE=MinSizeRel ^
    -DCMAKE_CXX_FLAGS_MINSIZEREL="/O1 /Ob1 /DNDEBUG /GL /Gy /Gw" ^
    -DCMAKE_C_FLAGS_MINSIZEREL="/O1 /Ob1 /DNDEBUG /GL /Gy /Gw" ^
    -DCMAKE_EXE_LINKER_FLAGS_MINSIZEREL="/LTCG /OPT:REF /OPT:ICF" ^
    -DCMAKE_SHARED_LINKER_FLAGS_MINSIZEREL="/LTCG /OPT:REF /OPT:ICF" ^
    -DCMAKE_MODULE_LINKER_FLAGS_MINSIZEREL="/LTCG /OPT:REF /OPT:ICF" ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DTFLITE_ENABLE_INSTALL=ON ^
    -DTFLITE_ENABLE_XNNPACK=ON ^
    -DTFLITE_ENABLE_RUY=ON ^
    -DTFLITE_ENABLE_GPU=OFF ^
    -DTFLITE_ENABLE_MMAP=OFF ^
    -DTFLITE_ENABLE_NNAPI=OFF ^
    -DTFLITE_ENABLE_RESOURCE=ON ^
    -DTFLITE_ENABLE_BENCHMARK_MODEL=OFF ^
    -DTFLITE_ENABLE_LABEL_IMAGE=OFF ^
    -DTFLITE_ENABLE_EXTERNAL_DELEGATE=OFF ^
    -DTFLITE_KERNEL_TEST=OFF ^
    -DFETCHCONTENT_QUIET=OFF
if errorlevel 1 exit /b %errorlevel%

echo.
echo CMake done, building (MinSizeRel)...
cmake --build "%BUILD_DIR%" --config MinSizeRel --parallel
if errorlevel 1 exit /b %errorlevel%

echo.
echo Build done, installing...
cmake --install "%BUILD_DIR%" --config MinSizeRel --prefix "%BUILD_DIR%\install"
if errorlevel 1 exit /b %errorlevel%

echo.
echo Consolidating .lib files to install/lib for CI caching...
if not exist "%BUILD_DIR%\install\lib" mkdir "%BUILD_DIR%\install\lib"
for /r "%BUILD_DIR%" %%f in (*.lib) do (
    copy /y "%%f" "%BUILD_DIR%\install\lib\" >nul
)

echo.
echo Cleaning unnecessary intermediate files to reduce cache size...
del /Q "%BUILD_DIR%\*.pdb" 2>nul
rmdir /S /Q "%BUILD_DIR%\CMakeFiles" 2>nul
for /d %%d in ("%BUILD_DIR%\_deps\*") do (
    if exist "%%d\CMakeFiles" rmdir /S /Q "%%d\CMakeFiles" 2>nul
)

if "%CI%"=="" pause
