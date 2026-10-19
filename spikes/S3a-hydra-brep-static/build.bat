@echo off
set NoDefaultCurrentDirectoryInExePath=
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set "D=D:\Tootega\Source\TootegaAIAD\deps"
cmake -S "%~dp0." -B "%~dp0_build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%D%\static\release;%D%\_externals\aurora\MaterialX || exit /b 2
cmake --build "%~dp0_build" || exit /b 3
exit /b 0
