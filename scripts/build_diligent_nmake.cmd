@echo off
setlocal enableextensions

rem Usage:
rem   build_diligent_nmake.cmd build   "<srcDir>" "<buildDir>" "<cfg>"
rem   build_diligent_nmake.cmd rebuild "<srcDir>" "<buildDir>" "<cfg>"
rem   build_diligent_nmake.cmd clean   "<srcDir>" "<buildDir>" "<cfg>"

set "ACTION=%~1"
set "SRC=%~2"
set "BDIR=%~3"
set "CFG=%~4"

if "%ACTION%"=="" goto :usage
if "%SRC%"=="" goto :usage
if "%BDIR%"=="" goto :usage
if "%CFG%"=="" goto :usage

if not exist "%BDIR%" mkdir "%BDIR%"
> "%BDIR%\\_build_invoked.txt" echo %DATE% %TIME% %ACTION% %CFG%

if /i "%ACTION%"=="clean" goto :do_clean
if /i "%ACTION%"=="build" goto :do_build
if /i "%ACTION%"=="rebuild" goto :do_rebuild

:usage
echo Usage:
echo   %~nx0 build   ^"<srcDir^>" ^"<buildDir^>" ^"<cfg^>"
echo   %~nx0 rebuild ^"<srcDir^>" ^"<buildDir^>" ^"<cfg^>"
echo   %~nx0 clean   ^"<srcDir^>" ^"<buildDir^>" ^"<cfg^>"
exit /b 2

:do_build
cmake -S "%SRC%" -B "%BDIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%CFG%
if errorlevel 1 exit /b %errorlevel%
cmake --build "%BDIR%" --target ParticleSaturn.Diligent
exit /b %errorlevel%

:do_rebuild
cmake -S "%SRC%" -B "%BDIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%CFG%
if errorlevel 1 exit /b %errorlevel%
cmake --build "%BDIR%" --target clean
if errorlevel 1 exit /b %errorlevel%
cmake --build "%BDIR%" --target ParticleSaturn.Diligent
exit /b %errorlevel%

:do_clean
if not exist "%BDIR%\\CMakeCache.txt" exit /b 0
cmake --build "%BDIR%" --target clean
exit /b %errorlevel%
