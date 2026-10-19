# Spike S5: builds and runs the Netgen meshing check on the USD BRep test solids.
# cuSPARSE/cuBLAS are DLL-only on Windows (D18 exception): the CUDA bin dirs must be on PATH.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$deps = 'D:\Tootega\Source\TootegaAIAD\deps'
$cuda = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin'

$env:NoDefaultCurrentDirectoryInExePath = $null
$p = Start-Process -FilePath $env:ComSpec -ArgumentList "/c `"`"$here\build.bat`" > `"$here\_build.log`" 2>&1`"" `
    -WorkingDirectory $here -NoNewWindow -Wait -PassThru
if ($p.ExitCode -ne 0) { Get-Content "$here\_build.log" -Tail 30; throw "build failed ($($p.ExitCode))" }

$env:PATH = "$cuda\x64;$cuda;$env:PATH"
$env:PXR_PLUGINPATH_NAME = "$deps\static\release\lib\usd"
$omniSolid = "$deps\usd-brep\1.0.0-v145-static\release\usd\omniSolid\resources"
$tests = "$deps\_src\usd-brep\TestFiles\brep_validator"

$failed = 0
# S5_MAXH (env) sets the mesh size relative to cbrt(volume), default 0.1.
# asset (relative to the test dir), expected B-rep faces (0 = take OCCT's count), volume tolerance
$cases = @(
    @('Cube', 6, '1e-6'),
    @('cubeWithVoid', 7, '1e-2'),
    @('Cone_SmConnect_Brep', 0, '1e-2'),
    @('mouse', 0, '1e-2'))
# coverage_limits/* are open surfaces (no enclosed volume): not volume-meshable by design.
foreach ($t in $cases)
{
    Write-Output "=== $($t[0]) ==="
    $work = "$here\_work\" + ($t[0] -replace '\\', '_')
    New-Item -ItemType Directory -Force $work | Out-Null
    & "$here\_build\s5_netgen.exe" $omniSolid "$tests\$($t[0]).usda" $work $t[1] $t[2] 2>&1 |
        Select-String -Pattern '^(SMLib|OCCT|Netgen:|MFEM|volume|ALL PASS|FAILURES|\[FAIL\])' | ForEach-Object Line
    Write-Output "exit $LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
exit $failed
