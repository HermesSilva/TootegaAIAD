@echo off
set NoDefaultCurrentDirectoryInExePath=
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake -S "%~dp0." -B "%~dp0_build" -G "Visual Studio 18 2026" -A x64 || exit /b 2
cmake --build "%~dp0_build" --config Release --target s3b_aurora -- /m /v:minimal || exit /b 3
exit /b 0
