@echo off
rem 4AIAD: static third-party stack (oneTBB, OpenSubdiv, OpenUSD 25.11) with VS 18 / MSVC v145.
rem MaterialX 1.39.5 (+ Slang generator, static) is reused from the Aurora externals.
rem CRT: dynamic (/MD, VC++ redistributable) on purpose - see Docs/02-arquitetura.md, D18.
rem Usage: build_static_stack.bat [tbb] [osd] [usd]   (no argument = all, in order)
setlocal enableextensions
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5

set "SRC=%DEPS%\_src"
set "BLD=%DEPS%\_build\static"
set "PFX=%DEPS%\static\release"
set "MTLX=%DEPS%\_externals\aurora\MaterialX"
set "COMMON=-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%PFX% -DCMAKE_PREFIX_PATH=%PFX%;%MTLX% -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"

set "STAGES=%*"
if "%STAGES%"=="" set "STAGES=tbb osd usd"

for %%S in (%STAGES%) do (
  call :stage_%%S || exit /b 10
)
echo ALL STAGES OK
exit /b 0

:stage_tbb
echo === oneTBB (static) ===
cmake -S "%SRC%\oneTBB" -B "%BLD%\oneTBB" %COMMON% -DBUILD_SHARED_LIBS=OFF -DTBB_TEST=OFF -DTBB_EXAMPLES=OFF -DTBB_STRICT=OFF || exit /b 1
cmake --build "%BLD%\oneTBB" || exit /b 1
cmake --install "%BLD%\oneTBB" || exit /b 1
exit /b 0

:stage_osd
echo === OpenSubdiv (static, CPU only) ===
cmake -S "%SRC%\OpenSubdiv" -B "%BLD%\OpenSubdiv" %COMMON% -DBUILD_SHARED_LIBS=OFF ^
  -DNO_EXAMPLES=ON -DNO_TUTORIALS=ON -DNO_REGRESSION=ON -DNO_DOC=ON -DNO_TESTS=ON ^
  -DNO_OMP=ON -DNO_TBB=ON -DNO_CUDA=ON -DNO_OPENCL=ON -DNO_DX=ON -DNO_OPENGL=ON ^
  -DNO_GLEW=ON -DNO_GLFW=ON -DNO_PTEX=ON -DNO_METAL=ON || exit /b 1
cmake --build "%BLD%\OpenSubdiv" || exit /b 1
cmake --install "%BLD%\OpenSubdiv" || exit /b 1
exit /b 0

:stage_usd
echo === OpenUSD 25.11 (static, no Python, no GL/Vulkan, MaterialX on) ===
cmake -S "%SRC%\OpenUSD" -B "%BLD%\OpenUSD" %COMMON% ^
  -DBUILD_SHARED_LIBS=OFF -DPXR_BUILD_MONOLITHIC=OFF ^
  -DPXR_ENABLE_PYTHON_SUPPORT=OFF -DPXR_ENABLE_GL_SUPPORT=OFF -DPXR_ENABLE_VULKAN_SUPPORT=OFF ^
  -DPXR_BUILD_IMAGING=ON -DPXR_BUILD_USD_IMAGING=ON -DPXR_ENABLE_MATERIALX_SUPPORT=ON ^
  -DPXR_BUILD_USD_TOOLS=OFF -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF ^
  -DPXR_BUILD_DOCUMENTATION=OFF -DPXR_ENABLE_PTEX_SUPPORT=OFF -DPXR_ENABLE_OPENVDB_SUPPORT=OFF ^
  -DPXR_BUILD_OPENIMAGEIO_PLUGIN=OFF -DPXR_BUILD_OPENCOLORIO_PLUGIN=OFF -DPXR_BUILD_EMBREE_PLUGIN=OFF ^
  -DPXR_BUILD_PRMAN_PLUGIN=OFF -DPXR_BUILD_ALEMBIC_PLUGIN=OFF -DPXR_BUILD_DRACO_PLUGIN=OFF ^
  -DTBB_ROOT=%PFX% -DOpenSubdiv_DIR=%PFX%/lib/cmake/OpenSubdiv -DMaterialX_DIR=%MTLX%/lib/cmake/MaterialX || exit /b 1
cmake --build "%BLD%\OpenUSD" || exit /b 1
cmake --install "%BLD%\OpenUSD" || exit /b 1
exit /b 0
