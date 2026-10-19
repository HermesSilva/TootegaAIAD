@echo off
rem 4AIAD spike S6: Palace (fork palace-4AIAD) superbuild on Windows - MSVC v145 (C/C++),
rem LLVM flang (Fortran, ARPACK-NG), MS-MPI (portable, deps/_toolchain), static OpenBLAS.
rem Usage: build_palace.bat cpu|cuda
setlocal
set NoDefaultCurrentDirectoryInExePath=
set "VARIANT=%~1"
if "%VARIANT%"=="" set "VARIANT=cpu"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
rem Sub-projects run cmake without -G; the default (Visual Studio) generator cannot drive flang.
set CMAKE_GENERATOR=Ninja

for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "MSMPI_INC=%DEPS%\_toolchain\msmpi-10.1.3\Include\"
set "MSMPI_LIB64=%DEPS%\_toolchain\msmpi-10.1.3\Lib\x64\"
rem x64 runtime (Bin holds the x86 one)
set "MSMPI_BIN=%DEPS%\_toolchain\msmpi-10.1.3\Bin64\"
set "PATH=%MSMPI_BIN%;%PATH%"
set "OPENBLAS_DIR=%DEPS%\openblas\0.3.34-static"
set "SRC=%DEPS%\forks\palace-4AIAD"
set "BLD=%DEPS%\_build\palace-%VARIANT%"
set "INS=%DEPS%\palace\%VARIANT%"

set "GPU=-DPALACE_WITH_CUDA=OFF"
if /i "%VARIANT%"=="cuda" set "GPU=-DPALACE_WITH_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75 -DCMAKE_CUDA_FLAGS=-Xcompiler=/Zc:preprocessor"

cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%INS%" ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_Fortran_COMPILER="C:/Program Files/LLVM/bin/flang.exe" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL ^
  -DBUILD_SHARED_LIBS=OFF ^
  %GPU% ^
  -DPALACE_WITH_OPENMP=OFF ^
  -DPALACE_WITH_SUPERLU=ON -DPALACE_WITH_STRUMPACK=OFF -DPALACE_WITH_MUMPS=OFF ^
  -DPALACE_WITH_SLEPC=OFF -DPALACE_WITH_ARPACK=ON ^
  -DPALACE_WITH_LIBXSMM=OFF -DPALACE_WITH_MAGMA=OFF -DPALACE_WITH_GSLIB=OFF ^
  -DPALACE_WITH_SUNDIALS=ON ^
  -DEXTERN_LIBCEED_URL="%DEPS:\=/%/forks/libceed-4AIAD" -DEXTERN_LIBCEED_GIT_TAG=main-4AIAD || exit /b 2
cmake --build "%BLD%" || exit /b 3
exit /b 0
