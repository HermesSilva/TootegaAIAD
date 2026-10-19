#Requires -Version 7
<#
Tootega AIAD - packs the prebuilt dependencies that the application build consumes into 7z archives
(one per package, each below GitHub's 2 GB asset limit), writes deps\prebuilt.json (names, SHA-256,
the deps path they were built in) and optionally publishes them as a GitHub release.
Maintainer tool; users run fetch-prebuilt.ps1.

  pwsh -File deps\pack-prebuilt.ps1                       # archives in deps\_dist + manifest
  pwsh -File deps\pack-prebuilt.ps1 -Publish              # ... and create the release with gh
#>
param([string]$Tag = "deps-$(Get-Date -Format 'yyyy.MM.dd')", [switch]$Publish)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Deps     = $PSScriptRoot
$Out      = Join-Path $Deps '_dist'
$SevenZip = 'C:\Program Files\7-Zip\7z.exe'
$Repo     = 'HermesSilva/TootegaAIAD'
$MaxBytes = 2GB - 16MB

# Package = folder under deps\ + excludes (7z wildcards, relative to that folder's contents).
$Packages = @(
    @{ Name = 'openusd-static';   Path = 'static/release';                  Exclude = @() }
    @{ Name = 'usd-brep';         Path = 'usd-brep/1.0.0-v145-static';       Exclude = @() }
    @{ Name = 'mfem-cuda-double'; Path = 'mfem/4.10-cuda13.3-v145-double';   Exclude = @() }
    @{ Name = 'chrono-static';    Path = 'chrono/10.0.0-static';             Exclude = @('data') }
    @{ Name = 'aurora-externals'; Path = '_externals/aurora';                Exclude = @('build', 'src', '*.log') }
)

if (-not (Test-Path $SevenZip)) { throw "7-Zip not found at $SevenZip (winget install 7zip.7zip)" }
New-Item -ItemType Directory -Force $Out | Out-Null

$manifest = [ordered]@{
    release  = $Tag
    repo     = $Repo
    # Absolute paths inside the packages (CMake configs) point here; fetch-prebuilt.ps1 rewrites them.
    origin   = ($Deps -replace '\\', '/')
    packages = @()
}
foreach ($p in $Packages)
{
    $src = Join-Path $Deps $p.Path
    if (-not (Test-Path $src)) { throw "missing $src - build it first (deps\README.md)" }
    $file = "aiad-deps-$($p.Name).7z"
    $archive = Join-Path $Out $file
    Remove-Item $archive -ErrorAction SilentlyContinue
    Write-Host "=== $($p.Name) <- deps/$($p.Path)" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    # Paths in the archive are relative to deps\, so extraction into deps\ restores the layout.
    $args7 = @('a', '-t7z', '-mx=7', '-mmt=on', '-bso0', '-bsp0', $archive, $p.Path)
    # Names are top-level entries of the package; wildcards (*.log) apply at any depth.
    foreach ($x in $p.Exclude) { $args7 += if ($x.Contains('*')) { "-xr!$x" } else { "-x!$($p.Path)/$x" } }
    Push-Location $Deps
    try { & $SevenZip @args7; if ($LASTEXITCODE -ne 0) { throw "7z failed ($LASTEXITCODE) for $($p.Name)" } }
    finally { Pop-Location }
    $size = (Get-Item $archive).Length
    if ($size -gt $MaxBytes) { throw "$file is $([int]($size/1MB)) MB, above the 2 GB release asset limit: split the package" }
    $hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host ('    {0:N0} MB, {1:N0} s, sha256 {2}' -f ($size/1MB), $sw.Elapsed.TotalSeconds, $hash)
    $manifest.packages += [ordered]@{ name = $p.Name; path = $p.Path; file = $file; size = $size; sha256 = $hash }
}

$manifestPath = Join-Path $Deps 'prebuilt.json'
$manifest | ConvertTo-Json -Depth 5 | Set-Content $manifestPath -Encoding utf8NoBOM
Write-Host "manifest: $manifestPath"

if ($Publish)
{
    $notes = @"
Prebuilt third-party dependencies for building Tootega AIAD on Windows x64 with Visual Studio 2026
(MSVC v145, /MD) and CUDA 13.3. Install them with ``pwsh -File deps\fetch-prebuilt.ps1`` from a clone
of the repository; that script verifies the SHA-256 listed in ``deps/prebuilt.json``.

Each package keeps the license files of its components (OpenUSD: Tomorrow Open Source Technology
License; USD BRep/SMLib, oneTBB, MaterialX: Apache-2.0; MFEM, Project Chrono: BSD-3-Clause; Aurora
externals: see each folder).
"@
    $files = $manifest.packages | ForEach-Object { Join-Path $Out $_.file }
    gh release create $Tag @files --repo $Repo --title "Prebuilt dependencies $Tag" --notes $notes
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
}
