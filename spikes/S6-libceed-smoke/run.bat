@echo off
rem 4AIAD spike S6: smoke test of the libceed-4AIAD CMake build (static, clang-cl) with upstream
rem tests on the CPU backends. A test passes when it exits 0 (upstream tests abort on mismatch).
setlocal
set NoDefaultCurrentDirectoryInExePath=
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CEED=D:\Tootega\Source\TootegaAIAD\deps\libceed\1.0.1-cpu"
set "TESTS=D:\Tootega\Source\TootegaAIAD\deps\forks\libceed-4AIAD\tests"
set "OUT=%~dp0_build"
if not exist "%OUT%" mkdir "%OUT%"
set FAILED=0
for %%t in (t001-ceed t100-vector t300-basis t301-basis t500-operator t501-operator t510-operator t520-operator) do (
  "C:\Program Files\LLVM\bin\clang-cl.exe" /nologo /O2 /MD /w -DCEED_SKIP_VISIBILITY -I"%CEED%\include" -I"%TESTS%" ^
    "%TESTS%\%%t.c" /Fe"%OUT%\%%t.exe" /Fo"%OUT%\%%t.obj" /link "%CEED%\lib\ceed.lib" >nul || (echo BUILD FAIL %%t & set FAILED=1)
  for %%b in (/cpu/self/ref/serial /cpu/self/opt/blocked /cpu/self/avx/blocked) do (
    "%OUT%\%%t.exe" %%b >nul 2>&1 && (echo PASS %%t %%b) || (echo FAIL %%t %%b & set FAILED=1)
  )
)
exit /b %FAILED%
