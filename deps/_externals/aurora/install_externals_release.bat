@echo off
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..\..") do set "DEPS=%%~fI"
rem Python 3.10+ (the original builds used Packman's python 3.12.14).
if not defined AIAD_PYTHON set "AIAD_PYTHON=python"
rem 4AIAD: build Aurora externals with VS 18 (MSVC v145), host OpenUSD 25.11, static MaterialX.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
set "PATH=%DEPS%\_toolchain\nasm-2.16.03;%PATH%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
"%AIAD_PYTHON%" "%DEPS%\forks\aurora-4AIAD\Scripts\installExternals.py" "%DEPS%\_externals\aurora" ^
  --usd-root "%DEPS%\openusd\0.25.11-py312-v142\release" ^
  --materialx-static --no-oiio --generator "Visual Studio 18 2026" --build-variant Release -v
exit /b %ERRORLEVEL%
