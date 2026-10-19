@echo off
rem 4AIAD spike S6: OpenBLAS 0.3.34 as a static library (BLAS + LAPACK in C, no Fortran), sequential
rem (parallelism comes from MPI), DYNAMIC_ARCH. The x86 kernels are GNU-syntax assembly that cl.exe
rem cannot compile, so this one package uses clang-cl (same MSVC ABI, CRT /MD and linker).
rem clang 23 makes -Wincompatible-pointer-types an error (AVX-512 micro-kernels): downgraded.
setlocal
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set "SRC=%DEPS%\_src\openblas"
set "BLD=%DEPS%\_build\openblas-static"
set "INS=%DEPS%\openblas\0.3.34-static"

if exist "%BLD%" rmdir /s /q "%BLD%"
cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%INS%" ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_ASM_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL ^
  -DCMAKE_C_FLAGS="/W0 -Wno-incompatible-pointer-types" ^
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON ^
  -DNOFORTRAN=ON -DC_LAPACK=ON -DBUILD_LAPACK_DEPRECATED=ON ^
  -DDYNAMIC_ARCH=ON -DUSE_THREAD=OFF -DUSE_OPENMP=OFF -DINTERFACE64=OFF ^
  -DBUILD_TESTING=OFF -DBUILD_BENCHMARKS=OFF || exit /b 2
cmake --build "%BLD%" || exit /b 3
cmake --install "%BLD%" || exit /b 4
exit /b 0
