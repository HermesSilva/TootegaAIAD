#Requires -Version 7
<#
Tootega AIAD - deployment: builds and tests (build-dist.ps1), stages the runnable package with the example models,
checks that the package runs on its own (its aiad.exe opens and checks every example), and zips it.

  pwsh -File .\deploy.ps1                 # build + every test + package: _deploy\TootegaAIAD-<version>-win-x64.zip
  pwsh -File .\deploy.ps1 -SkipBuild      # package the Dist\ already built
  pwsh -File .\deploy.ps1 -Publish        # ... and publish it as the GitHub release v<version> (outward: asks first,
                                          #     -Yes skips the question, as the CD workflow does)

The zip:
  TootegaAIAD-<version>\
    aiad.exe, aiad-mcp.exe, aiad-model.exe, the DLLs, usd\, MaterialX\, studio_env.hdr
    Examples\   the example models (<name>.usda + <name>.geom.usdc), CLAUDE.md and the agent's skill (.claude\)
    README.txt  requirements and how to start
Left out: debug symbols (.pdb), the build machine's state (.aiad), test outputs, crash dumps, caches.
#>
param([switch]$SkipBuild, [switch]$Publish, [switch]$Yes, [string]$Tag = '')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = $PSScriptRoot
$Dist = Join-Path $Repo 'Dist'
$Out = Join-Path $Repo '_deploy'
$Examples = 'Engine_1Cyl_OHC_Belt', 'Engine_1Cyl_OHV_GearTrain', 'Engine_V2-60_OHV_Chain', 'Watch_ETA6497'

function Step([string]$Name, [scriptblock]$Body)
{
    Write-Host "`n=== $Name" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $Body
    Write-Host ('    ok ({0:N1} s)' -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
}

# The version: the app's (kAppVersion) and the commit it is built from.
$appVersion = [regex]::Match((Get-Content (Join-Path $Repo 'src\aiad_app\AiPanels.cpp') -Raw), 'kAppVersion = "([^"]+)"').Groups[1].Value
if (-not $appVersion) { throw 'kAppVersion not found in src\aiad_app\AiPanels.cpp' }
$commit = (git -C $Repo rev-parse --short HEAD).Trim()
$dirty = [bool](git -C $Repo status --porcelain)
$version = "$appVersion+$commit$(if ($dirty) { '.dirty' })"
$name = "TootegaAIAD-$appVersion"
$zip = Join-Path $Out "TootegaAIAD-$appVersion-$commit-win-x64.zip"
Write-Host "Tootega AIAD $version"

if (-not $SkipBuild)
{
    Step 'Build and test (build-dist.ps1)' {
        & pwsh -NoProfile -File (Join-Path $Repo 'build-dist.ps1')
        if ($LASTEXITCODE -ne 0) { throw "build-dist.ps1 failed with exit code $LASTEXITCODE" }
    }
}
if (-not (Test-Path (Join-Path $Dist 'aiad.exe'))) { throw 'Dist\aiad.exe is missing: run without -SkipBuild' }

$stage = Join-Path $Out 'stage'
$root = Join-Path $stage $name
Step "Stage $name" {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $root | Out-Null
    $skip = '^(\.aiad|models|magneto_table\.txt|snapshot.*|ui_test.*|.*\.pdb|.*\.dmp|.*\.log(\.err)?)$'
    Get-ChildItem $Dist -Force | Where-Object { $_.Name -notmatch $skip } | ForEach-Object { Copy-Item $_.FullName $root -Recurse }
    # The examples: each document is its .usda and its .geom.usdc; the agent works in their folder (CLAUDE.md, skill).
    $ex = Join-Path $root 'Examples'
    New-Item -ItemType Directory $ex | Out-Null
    foreach ($model in $Examples)
    {
        foreach ($file in "$model.usda", "$model.geom.usdc")
        {
            $from = Join-Path $Repo "models\$file"
            if (-not (Test-Path $from)) { throw "example missing: models\$file" }
            Copy-Item $from $ex
        }
    }
    Copy-Item (Join-Path $Repo 'models\CLAUDE.md') $ex
    New-Item -ItemType Directory (Join-Path $ex '.claude\skills') -Force | Out-Null
    Copy-Item (Join-Path $Repo 'models\.claude\skills\aiad-mcp') (Join-Path $ex '.claude\skills') -Recurse
    $exampleList = ($Examples | ForEach-Object { "  Examples\$_.usda" }) -join "`r`n"
    @"
Tootega AIAD $version - Artificial Intelligence-Aided Design
https://github.com/HermesSilva/TootegaAIAD

Requirements
  Windows 10/11 x64, an NVIDIA GPU with DirectX 12 ray tracing (DXR) and a current NVIDIA driver.
  The AI chat uses the Claude Code CLI (claude.exe) when it is installed and signed in.

Start
  aiad.exe                          opens with no document (File > Open)
  aiad.exe Examples\<model>.usda    opens a model
  aiad.exe --check <model>.usda     the simulator's critique of a drawing, no window

Examples (a document is its <name>.usda and its <name>.geom.usdc; keep them together)
$exampleList

Keep the folder as it is: aiad.exe loads its DLLs, usd\ and MaterialX\ from beside it.
"@ | Set-Content -Encoding utf8 (Join-Path $root 'README.txt')
}

Step 'Check the package on its own (its aiad.exe opens and checks every example)' {
    foreach ($model in $Examples)
    {
        $document = Join-Path $root "Examples\$model.usda"
        $log = Join-Path $Out "check_$model.txt"
        $p = Start-Process -FilePath (Join-Path $root 'aiad.exe') -ArgumentList '--check', "`"$document`"" -WorkingDirectory $root -PassThru -NoNewWindow `
            -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        if (-not $p.WaitForExit(600000)) { $p.Kill(); throw "aiad.exe --check $model did not finish in 10 min" }
        if ($p.ExitCode -ne 0) { Get-Content $log, "$log.err" | Select-Object -Last 20 | Write-Host; throw "the package cannot run $model (aiad.exe --check exit $($p.ExitCode))" }
        Write-Host "    $model runs"
    }
    # The checks run from the staged folder: anything they wrote there is not part of the package.
    Get-ChildItem $root -Force | Where-Object { $_.Name -match '^(\.aiad|magneto_table\.txt|.*\.dmp)$' } | Remove-Item -Recurse -Force
}

Step "Zip $(Split-Path $zip -Leaf)" {
    Remove-Item $zip, "$zip.sha256" -Force -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $(Split-Path $zip -Leaf)" | Set-Content -Encoding ascii "$zip.sha256"
    Write-Host ('    {0:N0} MB, sha256 {1}' -f ((Get-Item $zip).Length / 1MB), $hash)
}

if ($Publish)
{
    $tag = if ($Tag) { $Tag } else { "v$appVersion" }
    if ($dirty) { throw "the working tree has uncommitted changes: commit them before publishing $tag" }
    if (-not $Yes)
    {
        $answer = Read-Host "Publish $(Split-Path $zip -Leaf) as the GitHub release $tag of HermesSilva/TootegaAIAD? (yes/no)"
        if ($answer -ne 'yes') { Write-Host 'Not published.'; return }
    }
    Step "Publish the GitHub release $tag" {
        $notes = "Tootega AIAD $version. Unzip and run aiad.exe; examples in Examples\. Requires Windows 10/11 x64 and an NVIDIA GPU with DXR."
        $exists = $true
        gh release view $tag --repo HermesSilva/TootegaAIAD *> $null
        if ($LASTEXITCODE -ne 0) { $exists = $false }
        if ($exists) { gh release upload $tag $zip "$zip.sha256" --clobber --repo HermesSilva/TootegaAIAD }
        else { gh release create $tag $zip "$zip.sha256" --repo HermesSilva/TootegaAIAD --title "Tootega AIAD $tag" --notes $notes --target (git -C $Repo rev-parse HEAD) }
        if ($LASTEXITCODE -ne 0) { throw "gh failed with exit code $LASTEXITCODE" }
    }
}
Write-Host "`nPackage: $zip" -ForegroundColor Green
