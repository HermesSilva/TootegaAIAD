@echo off
rem 4AIAD spike S5: Netgen 6.2.2604 (nglib + ngcore) as static libraries, with a static OCCT 7.8.1
rem built by Netgen's superbuild. No GUI, no Python. VS 18 / MSVC v145.
rem Consumers must define NETGEN_STATIC (see deps/patches/netgen-6.2.2604-static.patch).
setlocal
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
set "SRC=%DEPS%\_src\netgen"
set "BLD=%DEPS%\_build\netgen-static"
set "INS=%DEPS%\netgen\6.2.2604-static"

git -C "%SRC%" apply "%DEPS%\patches\netgen-6.2.2604-static.patch" || exit /b 5
call :build
set "RC=%ERRORLEVEL%"
git -C "%SRC%" checkout -- .
exit /b %RC%

:build
cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="%INS%" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL ^
  -DUSE_SUPERBUILD=ON -DBUILD_OCC=ON -DUSE_OCC=ON ^
  -DNGLIB_LIBRARY_TYPE=STATIC -DNGCORE_LIBRARY_TYPE=STATIC ^
  -DUSE_GUI=OFF -DUSE_PYTHON=OFF -DUSE_NATIVE_ARCH=OFF -DBUILD_STUB_FILES=OFF ^
  -DENABLE_UNIT_TESTS=OFF -DUSE_JPEG=OFF -DUSE_MPEG=OFF -DUSE_CGNS=OFF || exit /b 2
cmake --build "%BLD%" || exit /b 3
exit /b 0
