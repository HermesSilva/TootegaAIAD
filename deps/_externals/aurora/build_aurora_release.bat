@echo off
rem 4AIAD spike S3: configure and build Aurora (DirectX backend) with VS 18 / MSVC v145.
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..\..") do set "DEPS=%%~fI"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
set "SRC=%DEPS%\forks\aurora-4AIAD"
set "BLD=%DEPS%\_build\aurora"
cmake -S "%SRC%" -B "%BLD%" -G "Visual Studio 18 2026" -A x64 ^
  -DEXTERNALS_ROOT=%DEPS:\=/%/_externals/aurora ^
  -DENABLE_IMAGE_PROCESSING_RESOLVER=OFF -DENABLE_TESTS=OFF -DENABLE_USDVIEW=OFF || exit /b 2
cmake --build "%BLD%" --config Release -- /m /v:minimal || exit /b 3
exit /b 0
