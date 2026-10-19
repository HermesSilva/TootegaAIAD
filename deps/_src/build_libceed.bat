@echo off
rem 4AIAD: libCEED (fork libceed-4AIAD, CMake build) as a static library, MSVC v145 (cl).
rem Usage: build_libceed.bat cpu|cuda [install prefix]
setlocal
set NoDefaultCurrentDirectoryInExePath=
set "VARIANT=%~1"
if "%VARIANT%"=="" set "VARIANT=cpu"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "SRC=%DEPS%\forks\libceed-4AIAD"
set "BLD=%DEPS%\_build\libceed-%VARIANT%"
set "INS=%~2"
if "%INS%"=="" set "INS=%DEPS%\libceed\1.0.1-%VARIANT%"
set "GPU=-DCEED_WITH_CUDA=OFF"
if /i "%VARIANT%"=="cuda" set "GPU=-DCEED_WITH_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75"

cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%INS%" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl %GPU% || exit /b 2
cmake --build "%BLD%" -- -k 0 || exit /b 3
cmake --install "%BLD%" || exit /b 4
exit /b 0
