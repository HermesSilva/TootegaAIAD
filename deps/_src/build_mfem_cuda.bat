@echo off
rem 4AIAD spike S4: MFEM 4.10 + CUDA on Windows with MSVC v145 (VS 18), fp64 and fp32 variants.
rem Usage: build_mfem_cuda.bat double|single
setlocal
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "PREC=%~1"
if "%PREC%"=="" set "PREC=double"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3"
set "PATH=%CUDA_PATH%\bin;%PATH%"

set "SRC=%DEPS%\_src\mfem"
set "BLD=%DEPS%\_build\mfem-4.10-cuda-%PREC%"
set "INS=%DEPS%\mfem\4.10-cuda13.3-v145-%PREC%"

rem Local fixes are kept as patch files; the clone is always restored at the end.
git -C "%SRC%" apply "%DEPS%\patches\mfem-4.10-msvc-cuda.patch" || exit /b 5
call :build
set "RC=%ERRORLEVEL%"
git -C "%SRC%" checkout -- .
exit /b %RC%

:build
cmake -S "%SRC%" -B "%BLD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%INS%" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL ^
  -DMFEM_USE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75 ^
  -DCMAKE_CUDA_COMPILER="%CUDA_PATH%\bin\nvcc.exe" ^
  -DCMAKE_CUDA_FLAGS="-Xcompiler=/Zc:preprocessor" ^
  -DMFEM_PRECISION=%PREC% ^
  -DMFEM_ENABLE_EXAMPLES=ON || exit /b 2
cmake --build "%BLD%" --target mfem ex1 ex2 || exit /b 3
cmake --install "%BLD%" || exit /b 4
exit /b 0
