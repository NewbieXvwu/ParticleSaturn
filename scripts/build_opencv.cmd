@echo off
REM OpenCV 精简构建脚本 - 用于 ParticleSaturn 项目
REM 此脚本记录了项目所需的 OpenCV CMake 配置

cd /d "%~dp0.."

set OPENCV_SRC=HandTracker\libs\opencv
set BUILD_DIR=%OPENCV_SRC%\build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S "%OPENCV_SRC%" -B "%BUILD_DIR%" ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DBUILD_WITH_STATIC_CRT=ON ^
    -DBUILD_opencv_world=ON ^
    -DBUILD_opencv_core=ON ^
    -DBUILD_opencv_imgproc=ON ^
    -DBUILD_opencv_imgcodecs=ON ^
    -DBUILD_opencv_videoio=ON ^
    -DBUILD_opencv_highgui=ON ^
    -DBUILD_opencv_dnn=ON ^
    -DBUILD_opencv_apps=OFF ^
    -DBUILD_opencv_calib3d=OFF ^
    -DBUILD_opencv_features2d=OFF ^
    -DBUILD_opencv_flann=OFF ^
    -DBUILD_opencv_ml=OFF ^
    -DBUILD_opencv_objdetect=OFF ^
    -DBUILD_opencv_photo=OFF ^
    -DBUILD_opencv_stitching=OFF ^
    -DBUILD_opencv_video=OFF ^
    -DBUILD_opencv_python3=OFF ^
    -DBUILD_opencv_python_bindings_generator=OFF ^
    -DBUILD_opencv_java=OFF ^
    -DBUILD_opencv_java_bindings_generator=OFF ^
    -DBUILD_opencv_js=OFF ^
    -DBUILD_TESTS=OFF ^
    -DBUILD_PERF_TESTS=OFF ^
    -DBUILD_EXAMPLES=OFF ^
    -DBUILD_DOCS=OFF ^
    -DBUILD_JPEG=ON ^
    -DBUILD_PNG=ON ^
    -DBUILD_ZLIB=ON ^
    -DBUILD_PROTOBUF=ON ^
    -DBUILD_TIFF=OFF ^
    -DBUILD_WEBP=OFF ^
    -DBUILD_OPENEXR=OFF ^
    -DBUILD_OPENJPEG=OFF ^
    -DBUILD_IPP_IW=OFF ^
    -DBUILD_ITT=OFF ^
    -DWITH_CUDA=OFF ^
    -DWITH_OPENCL=OFF ^
    -DWITH_OPENGL=OFF ^
    -DWITH_FFMPEG=OFF ^
    -DWITH_GSTREAMER=OFF ^
    -DWITH_DSHOW=ON ^
    -DWITH_MSMF=ON ^
    -DWITH_MSMF_DXVA=ON ^
    -DWITH_WIN32UI=ON ^
    -DWITH_JPEG=ON ^
    -DWITH_PNG=ON ^
    -DWITH_PROTOBUF=ON ^
    -DWITH_TIFF=OFF ^
    -DWITH_WEBP=OFF ^
    -DWITH_OPENEXR=OFF ^
    -DWITH_OPENJPEG=OFF ^
    -DWITH_IPP=OFF ^
    -DWITH_TBB=OFF ^
    -DWITH_EIGEN=OFF ^
    -DWITH_LAPACK=OFF ^
    -DWITH_QT=OFF ^
    -DWITH_VTK=OFF ^
    -DOPENCV_DNN_OPENCL=OFF

echo.
echo CMake 配置完成
echo 编译命令: cmake --build "%BUILD_DIR%" --config Release
