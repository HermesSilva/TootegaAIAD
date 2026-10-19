@echo off
rem 4AIAD M1.1: Project Chrono 10.0.0 core (Chrono_core) as a static library, MSVC v145, /MD.
rem No optional modules, no demos, no OpenMP (MSVC OpenMP would add vcomp140.dll - rule D18), no Thrust
rem (only the multicore collision uses it, and Chrono 10 does not build with the CUDA 13 CCCL 3 Thrust).
rem Eigen 3.4.1 (header-only) from deps/_src/eigen.
setlocal
set NoDefaultCurrentDirectoryInExePath=
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "SRC=%DEPS%\_src\chrono"
set "BLD=%DEPS%\_build\chrono-static"
set "INS=%DEPS%\chrono\10.0.0-static"

cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%INS%" ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL ^
  -DBUILD_SHARED_LIBS=OFF -DCH_USE_MSVC_STATIC_RUNTIME=OFF ^
  -DBUILD_DEMOS=OFF -DBUILD_BENCHMARKING=OFF -DBUILD_TESTING=OFF ^
  -DCH_ENABLE_OPENMP=OFF -DCH_USE_SIMD=ON ^
  -DCMAKE_DISABLE_FIND_PACKAGE_Thrust=ON ^
  -DEIGEN3_INCLUDE_DIR="%DEPS:\=/%/_src/eigen" || exit /b 2
cmake --build "%BLD%" || exit /b 3
cmake --install "%BLD%" || exit /b 4
exit /b 0
