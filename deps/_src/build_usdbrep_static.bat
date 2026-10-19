@echo off
rem 4AIAD: build NVIDIA USD BRep library targets as STATIC libraries against the static OpenUSD 25.11.
rem Prerequisites: patches usd-brep-msvc-v145.patch + usd-brep-static.patch applied, "repo build -g -r" done.
setlocal
set NoDefaultCurrentDirectoryInExePath=
for %%I in ("%~dp0..") do set "DEPS=%%~fI"
rem Python 3.10+ (the original builds used Packman's python 3.12.14).
if not defined AIAD_PYTHON set "AIAD_PYTHON=python"
set "R=%DEPS%\_src\usd-brep"
set "USD=%DEPS%\static\release"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem Point premake's USD location (a Packman junction) at the static OpenUSD build.
if exist "%R%\_build\target-deps\usd\release" rmdir "%R%\_build\target-deps\usd\release" || exit /b 2
mklink /J "%R%\_build\target-deps\usd\release" "%USD%" || exit /b 3

rem Static archives must not embed their dependencies (see the script).
"%AIAD_PYTHON%" "%DEPS%\patches\usd-brep-static-postgen.py" "%R%\_compiler\vs2022" || exit /b 5

msbuild "%R%\_compiler\vs2022\usd-brep.sln" /m /v:minimal /nologo /p:Configuration=release /p:Platform=x64 ^
  /t:SMLib;SM_API;SM_API_USD;BREP_SM_USD;BREP_USD_DATA;USD_BREP_UTILS;BREP_STAGING;OCCT_BREP_IMPORT;OCCT_BREP_EXPORT;HYDRA_USD_BREP || exit /b 4
exit /b 0
