@echo off
REM OpenCV Minimal Build Script - For ParticleSaturn Project
REM Only keep VideoCapture + HighGUI + Basic Image Processing (DNN removed)

cd /d "%~dp0.."

set OPENCV_SRC=HandTracker\libs\opencv
set BUILD_DIR=%OPENCV_SRC%\build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S "%OPENCV_SRC%" -B "%BUILD_DIR%" ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DBUILD_WITH_STATIC_CRT=ON ^
    -DENABLE_LTO=ON ^
    -DBUILD_opencv_world=ON ^
    -DBUILD_opencv_core=ON ^
    -DBUILD_opencv_imgproc=ON ^
    -DBUILD_opencv_imgcodecs=ON ^
    -DBUILD_opencv_videoio=ON ^
    -DBUILD_opencv_highgui=ON ^
    -DBUILD_opencv_dnn=OFF ^
    -DBUILD_opencv_gapi=OFF ^
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
    -DBUILD_opencv_python_tests=OFF ^
    -DBUILD_opencv_java=OFF ^
    -DBUILD_opencv_java_bindings_generator=OFF ^
    -DBUILD_opencv_objc_bindings_generator=OFF ^
    -DBUILD_opencv_js=OFF ^
    -DBUILD_JAVA=OFF ^
    -DBUILD_TESTS=OFF ^
    -DBUILD_PERF_TESTS=OFF ^
    -DBUILD_EXAMPLES=OFF ^
    -DBUILD_DOCS=OFF ^
    -DBUILD_JPEG=ON ^
    -DBUILD_PNG=ON ^
    -DBUILD_ZLIB=ON ^
    -DBUILD_PROTOBUF=OFF ^
    -DBUILD_JASPER=OFF ^
    -DBUILD_OPENJPEG=OFF ^
    -DBUILD_TIFF=OFF ^
    -DBUILD_WEBP=OFF ^
    -DBUILD_OPENEXR=OFF ^
    -DBUILD_IPP_IW=OFF ^
    -DBUILD_ITT=OFF ^
    -DWITH_PROTOBUF=OFF ^
    -DWITH_DSHOW=ON ^
    -DWITH_MSMF=ON ^
    -DWITH_MSMF_DXVA=ON ^
    -DWITH_WIN32UI=ON ^
    -DWITH_JPEG=ON ^
    -DWITH_PNG=ON ^
    -DWITH_ADE=OFF ^
    -DWITH_CUDA=OFF ^
    -DWITH_OPENCL=OFF ^
    -DWITH_OPENCLAMDBLAS=OFF ^
    -DWITH_OPENCLAMDFFT=OFF ^
    -DWITH_OPENCL_D3D11_NV=OFF ^
    -DWITH_OPENGL=OFF ^
    -DWITH_DIRECTX=OFF ^
    -DWITH_FFMPEG=OFF ^
    -DWITH_GSTREAMER=OFF ^
    -DWITH_TIFF=OFF ^
    -DWITH_WEBP=OFF ^
    -DWITH_OPENEXR=OFF ^
    -DWITH_OPENJPEG=OFF ^
    -DWITH_JASPER=OFF ^
    -DWITH_IMGCODEC_HDR=OFF ^
    -DWITH_IMGCODEC_SUNRASTER=OFF ^
    -DWITH_IMGCODEC_PXM=OFF ^
    -DWITH_IMGCODEC_PFM=OFF ^
    -DWITH_IPP=OFF ^
    -DWITH_TBB=OFF ^
    -DWITH_EIGEN=OFF ^
    -DWITH_LAPACK=OFF ^
    -DWITH_QT=OFF ^
    -DWITH_VTK=OFF ^
    -DOPENCV_DNN_OPENCL=OFF ^
    -DOPENCV_DNN_TFLITE=OFF

echo.
echo CMake configuration done, building...
cmake --build "%BUILD_DIR%" --config Release

echo.
echo Build done, installing...
cmake --install "%BUILD_DIR%" --config Release --prefix "%BUILD_DIR%\install"

if "%CI%"=="" pause
