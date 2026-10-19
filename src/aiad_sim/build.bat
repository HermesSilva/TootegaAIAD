@echo off
rem Builds aiad_sim standalone (MSVC v145, Ninja) and runs its tests.
set NoDefaultCurrentDirectoryInExePath=
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cmake -S "%~dp0." -B "%~dp0_build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 2
cmake --build "%~dp0_build" || exit /b 3
ctest --test-dir "%~dp0_build" --output-on-failure || exit /b 4
exit /b 0
