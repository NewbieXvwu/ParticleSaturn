@echo off
REM TensorFlow Lite Build Script - Windows x64 CPU + XNNPACK
REM Target: Hand Tracking minimal size

cd /d "%~dp0.."

set TFLITE_SRC=HandTracker\libs\tensorflow\tensorflow\lite
set TENSORFLOW_ROOT=HandTracker\libs\tensorflow
set BUILD_DIR=HandTracker\libs\tensorflow\tflite_build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo Configuring CMake...
cmake -S "%TFLITE_SRC%" -B "%BUILD_DIR%" ^
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

echo.
echo CMake done, building (MinSizeRel)...
cmake --build "%BUILD_DIR%" --config MinSizeRel --parallel

echo.
echo Build done, installing...
cmake --install "%BUILD_DIR%" --config MinSizeRel --prefix "%BUILD_DIR%\install"

if "%CI%"=="" pause
