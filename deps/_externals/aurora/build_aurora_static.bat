@echo off
rem 4AIAD spike S3b: Aurora + HdAurora as static libraries, no OpenGL, against the static OpenUSD 25.11.
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..\..") do set "DEPS=%%~fI"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
set "SRC=%DEPS%\forks\aurora-4AIAD"
set "BLD=%DEPS%\_build\aurora-static"
cmake -S "%SRC%" -B "%BLD%" -G "Visual Studio 18 2026" -A x64 ^
  -DEXTERNALS_ROOT=%DEPS:\=/%/_externals/aurora ^
  -Dpxr_ROOT=%DEPS:\=/%/static/release ^
  -DAIAD_INTEGRATION=ON -DENABLE_IMAGE_PROCESSING_RESOLVER=OFF ^
  -DENABLE_TESTS=OFF -DENABLE_USDVIEW=OFF -DENABLE_APPLICATIONS=OFF || exit /b 2
cmake --build "%BLD%" --config Release -- /m /v:minimal || exit /b 3
exit /b 0
