@echo off
rem 4AIAD: install an already built MFEM variant (library + headers only; examples are not installed).
rem Usage: install_mfem.bat double|single   (the MFEM clone must have the 4AIAD patch applied)
setlocal
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "PREC=%~1"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin;%PATH%"
set "BLD=%DEPS%\_build\mfem-4.10-cuda-%PREC%"
set "SRC=%DEPS%\_src\mfem"
git -C "%SRC%" apply --check "%DEPS%\patches\mfem-4.10-msvc-cuda.patch" 2>nul && git -C "%SRC%" apply "%DEPS%\patches\mfem-4.10-msvc-cuda.patch"
call :install
set "RC=%ERRORLEVEL%"
git -C "%SRC%" checkout -- .
exit /b %RC%

:install
cmake -S "%SRC%" -B "%BLD%" -DMFEM_ENABLE_EXAMPLES=OFF || exit /b 2
cmake --build "%BLD%" --target mfem || exit /b 3
cmake --install "%BLD%" || exit /b 4
exit /b 0
