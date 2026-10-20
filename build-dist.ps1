#Requires -Version 7
<#
Tootega AIAD - builds aiad.exe and assembles the self-contained Dist\ folder.
Runs top-down; every step prints its duration; the script stops at the first error.

  pwsh -File .\build-dist.ps1               # build + unit tests + Dist + snapshot test + UI test
  pwsh -File .\build-dist.ps1 -SkipSnapshot # build + unit tests + Dist only
#>
param([switch]$SkipSnapshot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo     = $PSScriptRoot
$Src      = Join-Path $Repo 'src'
$Solution = Join-Path $Repo 'TootegaAIAD.slnx'
$Dist     = Join-Path $Repo 'Dist'
$Deps     = Join-Path $Repo 'deps'
$Vs       = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$MSBuild  = Join-Path $Vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$CudaBin  = Join-Path $(if ($env:CUDA_PATH_V13_3) { $env:CUDA_PATH_V13_3 } else { $env:CUDA_PATH }) 'bin\x64'
$Runtime  = Join-Path $Repo '_build\Release\bin'   # aiad.exe, aiad-mcp.exe, aiad-model.exe and the tests
$Externals = Join-Path $Deps '_externals\aurora'
$DllDirs = @($Runtime, (Join-Path $Externals 'DXC\bin'), (Join-Path $Externals 'NRD\bin'), (Join-Path $Externals 'NRI\bin'),
             (Join-Path $Externals 'Slang\bin'), $CudaBin)

function Step([string]$Name, [scriptblock]$Body)
{
    Write-Host "`n=== $Name" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $Body
    Write-Host ('    ok ({0:N1} s)' -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
}

function Invoke-Native([string]$Exe, [string[]]$Arguments)
{
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path $Exe -Leaf) failed with exit code $LASTEXITCODE" }
}

Step '1/9 Stop leftover build processes of this repository' {
    $names = 'cmake.exe', 'MSBuild.exe', 'cl.exe', 'link.exe', 'ninja.exe', 'aiad.exe'
    $stale = Get-CimInstance Win32_Process |
        Where-Object { $names -contains $_.Name -and $_.CommandLine -and $_.CommandLine -like '*TootegaAIAD*' }
    foreach ($p in $stale)
    {
        Write-Host "    stopping $($p.Name) (pid $($p.ProcessId))"
        Stop-Process -Id $p.ProcessId -Force
    }
    if (-not $stale) { Write-Host '    none' }
}

Step '2/9 Visual Studio 18 developer environment (x64, MSVC v145)' {
    Import-Module (Join-Path $Vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $Vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $env:NoDefaultCurrentDirectoryInExePath = $null
    Write-Host "    cl: $((Get-Command cl.exe).Source)"
}

Step '3/9 TootegaAIAD.slnx loads in Visual Studio (devenv, on a copy so the IDE state in .vs stays out)' {
    $devenv = Join-Path $Vs 'Common7\IDE\devenv.com'
    $probe = Join-Path $Repo 'TootegaAIAD.loadcheck.slnx'
    Copy-Item $Solution $probe -Force
    try { $out = & $devenv $probe /Build 'Release|x64' /Project aiad_core 2>&1 }
    finally
    {
        Remove-Item $probe -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $Repo '.vs\TootegaAIAD.loadcheck.slnx') -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($LASTEXITCODE -ne 0 -or ($out -match 'not loaded correctly'))
    {
        $out | Select-Object -Last 8 | ForEach-Object { Write-Host "    $_" }
        throw 'TootegaAIAD.slnx does not load in Visual Studio'
    }
    Write-Host '    loads; aiad_core builds from the IDE'
}

$Tests = @('kernel_test', 'document_test', 'multibody_test', 'command_bus_test', 'agent_session_test', 'mcp_bridge_test', 'claude_cli_test')
Step '4/9 Build the solution (MSBuild, Release x64): aiad.exe, aiad-mcp.exe, aiad-model.exe, tests' {
    Invoke-Native $MSBuild @($Solution, '/p:Configuration=Release', '/p:Platform=x64', '/m', '/v:minimal', '/nologo')
    foreach ($exe in 'aiad.exe', 'aiad-mcp.exe', 'aiad-model.exe')
    {
        if (-not (Test-Path (Join-Path $Runtime $exe))) { throw "$exe not found in $Runtime" }
    }
}
Step '5/9 Unit tests (kernel, document, multibody, Command Bus, agent session, MCP <-> bridge, live Claude CLI handshake)' {
    foreach ($test in $Tests)
    {
        $exe = Join-Path $Runtime "$test.exe"
        if (-not (Test-Path $exe)) { throw "$test.exe not built" }
        & $exe | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "$test failed (exit $LASTEXITCODE)" }
    }
}

Step '6/9 Assemble Dist (exe, DLLs, runtime)' {
    # Keep a previously computed magneto table (it takes about 70 s to compute).
    $cache = Join-Path $Dist 'magneto_table.txt'
    $savedCache = Join-Path $env:TEMP 'aiad_magneto_table.txt'
    if (Test-Path $cache) { Copy-Item $cache $savedCache -Force }
    # Keep the user's state: the app's settings and layout (.aiad) and the agent's sessions in the models folder
    # (models\.aiad: TimeLine). Put back over the fresh copy below.
    $kept = Join-Path $env:TEMP 'aiad_dist_state'
    Remove-Item $kept -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($state in '.aiad', 'models\.aiad')
    {
        $from = Join-Path $Dist $state
        if (Test-Path $from) { Copy-Item $from (Join-Path $kept $state) -Recurse -Force -ErrorAction Stop }
    }
    if (Test-Path $Dist)
    {
        try { Remove-Item $Dist -Recurse -Force -ErrorAction Stop }
        catch { throw "Dist is in use - close Dist\aiad.exe (or any file opened from Dist) and run again. ($($_.Exception.Message))" }
    }
    New-Item -ItemType Directory $Dist | Out-Null
    Copy-Item (Join-Path $Runtime 'aiad.exe') $Dist
    Copy-Item (Join-Path $Runtime 'aiad-mcp.exe') $Dist   # the agent's MCP server (stdio, static)
    foreach ($pdb in 'aiad.pdb', 'aiad-mcp.pdb') { if (Test-Path (Join-Path $Runtime $pdb)) { Copy-Item (Join-Path $Runtime $pdb) $Dist } }   # crash stacks

    # Imported DLLs, resolved recursively with dumpbin; OS DLLs are skipped. Rule D18: only these
    # third-party DLLs may be imported (NRD/NRI/amd_ags/Slang: Aurora, pending removal).
    $allowed = '^(cusparse64_|cublas64_|cublasLt64_|nvJitLink_|dxcompiler|dxil|slang|NRD|NRI|amd_ags_x64)'
    $system = Join-Path $env:WINDIR 'System32'
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $queue.Enqueue((Join-Path $Runtime 'aiad.exe'))
    $queue.Enqueue((Join-Path $Runtime 'aiad-mcp.exe'))
    while ($queue.Count -gt 0)
    {
        $module = $queue.Dequeue()
        $imports = & dumpbin /nologo /dependents $module | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
        foreach ($dll in $imports)
        {
            if (-not $seen.Add($dll)) { continue }
            if ($dll -match '^(api|ext)-ms-' -or (Test-Path (Join-Path $system $dll))) { continue }
            if ($dll -notmatch $allowed) { throw "D18 violation: $(Split-Path $module -Leaf) imports $dll (must be linked statically)" }
            $found = $DllDirs | ForEach-Object { Join-Path $_ $dll } | Where-Object { Test-Path $_ } | Select-Object -First 1
            if (-not $found) { throw "imported DLL not found: $dll (needed by $(Split-Path $module -Leaf))" }
            Write-Host "    $dll"
            Copy-Item $found $Dist
            $queue.Enqueue($found)
        }
    }

    # Loaded at run time (LoadLibrary) by Aurora: DXC and Slang.
    foreach ($pattern in 'dxcompiler.dll', 'dxil.dll')
    {
        Copy-Item (Join-Path $Externals "DXC\bin\$pattern") $Dist -Force
    }
    Get-ChildItem (Join-Path $Externals 'Slang\bin') -Filter 'slang*.dll' | ForEach-Object { Copy-Item $_.FullName $Dist -Force }

    # MSVC runtime, app-local (VC++ redistributable files).
    $crt = Get-ChildItem (Join-Path $Vs 'VC\Redist\MSVC') -Directory |
        ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC145.CRT' } | Where-Object { Test-Path $_ } |
        Sort-Object | Select-Object -Last 1
    if (-not $crt) { throw 'MSVC runtime redistributable folder (Microsoft.VC145.CRT) not found' }
    Copy-Item (Join-Path $crt '*.dll') $Dist

    # The drawings (documents) the app opens.
    Copy-Item (Join-Path $Repo 'models') (Join-Path $Dist 'models') -Recurse
    $tool = Get-Item (Join-Path $Runtime 'aiad-model.exe') -ErrorAction SilentlyContinue
    if ($tool) { Copy-Item $tool.FullName $Dist }
    if (Test-Path $savedCache) { Copy-Item $savedCache $cache }
    foreach ($state in '.aiad', 'models\.aiad')
    {
        $from = Join-Path $kept $state
        if (Test-Path $from)
        {
            $to = Join-Path $Dist $state
            New-Item -ItemType Directory $to -Force | Out-Null
            Copy-Item (Join-Path $from '*') $to -Recurse -Force
            Write-Host "    kept $state"
        }
    }
}

Step '7/9 Assemble Dist (USD plugins, MaterialX)' {
    Copy-Item (Join-Path $Deps 'static\release\lib\usd') (Join-Path $Dist 'usd') -Recurse
    Copy-Item (Join-Path $Deps 'usd-brep\1.0.0-v145-static\release\usd\hdUsdBrep') (Join-Path $Dist 'usd') -Recurse
    Copy-Item (Join-Path $Deps 'usd-brep\1.0.0-v145-static\release\usd\omniSolid') (Join-Path $Dist 'usd') -Recurse
    $hdAurora = Join-Path $Dist 'usd\hdAurora\resources'
    New-Item -ItemType Directory $hdAurora -Force | Out-Null
    Copy-Item (Join-Path $Deps 'forks\aurora-4AIAD\Libraries\HdAurora\resources-4AIAD\plugInfo.json') $hdAurora
    # Aurora looks for <exe dir>\MaterialX\libraries.
    $materialX = Join-Path $Externals 'MaterialX\libraries'
    if (-not (Test-Path $materialX)) { throw "MaterialX libraries not found in $materialX" }
    New-Item -ItemType Directory (Join-Path $Dist 'MaterialX') | Out-Null
    Copy-Item $materialX (Join-Path $Dist 'MaterialX') -Recurse
    $size = (Get-ChildItem $Dist -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
    Write-Host ('    Dist: {0:N0} MB' -f $size)
}

if ($SkipSnapshot) { Write-Host "`nDone (snapshot skipped): $Dist" -ForegroundColor Green; return }

Step '8/9 Snapshot test from Dist (engine runs 2 s, Aurora renders a PNG; limit 5 min)' {
    $exe = Join-Path $Dist 'aiad.exe'
    $png = Join-Path $Dist 'snapshot.png'
    $log = Join-Path $Dist 'snapshot.log'
    $p = Start-Process $exe -ArgumentList '--snapshot', "`"$png`"" -WorkingDirectory $Dist -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit(300000))
    {
        Stop-Process -Id $p.Id -Force
        throw "snapshot did not finish in 5 min (see $log)"
    }
    Get-Content $log | ForEach-Object { Write-Host "    $_" }
    if ($p.ExitCode -ne 0) { Get-Content "$log.err" | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }; throw "snapshot failed with exit code $($p.ExitCode)" }
    if (-not (Test-Path $png)) { throw 'snapshot.png was not written' }
}

Step '9/9 UI test from Dist (real window: aiad-mcp.exe -> bridge -> Command Bus, simulation job, chat; limit 5 min)' {
    $exe = Join-Path $Dist 'aiad.exe'
    $png = Join-Path $Dist 'ui_test.png'
    $log = Join-Path $Dist 'ui_test.log'
    $p = Start-Process $exe -ArgumentList '--ui-test', "`"$png`"" -WorkingDirectory $Dist -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit(300000))
    {
        Stop-Process -Id $p.Id -Force
        throw "UI test did not finish in 5 min (see $log)"
    }
    Get-Content $log | ForEach-Object { Write-Host "    $_" }
    if ($p.ExitCode -ne 0) { Get-Content "$log.err" | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }; throw "UI test failed with exit code $($p.ExitCode)" }
}

Write-Host "`nDone: $Dist\aiad.exe" -ForegroundColor Green
