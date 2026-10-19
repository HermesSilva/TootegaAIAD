#Requires -Version 7
<#
Tootega AIAD - downloads the prebuilt third-party dependencies listed in deps\prebuilt.json from the
GitHub release, verifies their SHA-256, extracts them into deps\ and rewrites the absolute paths
recorded in their CMake/pkg-config files to this clone's deps folder.

  pwsh -File deps\fetch-prebuilt.ps1            # skips packages already installed from the same archive
  pwsh -File deps\fetch-prebuilt.ps1 -Force     # reinstall everything
  pwsh -File deps\fetch-prebuilt.ps1 -From D:\x # use archives already downloaded into D:\x
#>
param([switch]$Force, [string]$From)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is far slower with the progress bar

$Deps     = $PSScriptRoot
$Manifest = Get-Content (Join-Path $Deps 'prebuilt.json') -Raw | ConvertFrom-Json
$Download = if ($From) { $From } else { Join-Path $Deps '_download' }
$SevenZip = 'C:\Program Files\7-Zip\7z.exe'
$Here     = $Deps -replace '\\', '/'
# Text files that may carry the build machine's absolute paths.
$TextExt  = '.cmake', '.pc', '.json', '.txt', '.h', '.hpp', '.in'

New-Item -ItemType Directory -Force $Download | Out-Null
foreach ($p in $Manifest.packages)
{
    $target = Join-Path $Deps $p.path
    $marker = Join-Path $target '.aiad-prebuilt'
    if (-not $Force -and (Test-Path $marker) -and (Get-Content $marker -Raw).Trim() -eq $p.sha256)
    {
        Write-Host "=== $($p.name): up to date" -ForegroundColor Green
        continue
    }
    Write-Host "=== $($p.name) -> deps/$($p.path)" -ForegroundColor Cyan

    $archive = Join-Path $Download $p.file
    if (-not (Test-Path $archive) -or (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $p.sha256)
    {
        if ($From) { throw "$archive missing or with a different SHA-256" }
        $url = "https://github.com/$($Manifest.repo)/releases/download/$($Manifest.release)/$($p.file)"
        Write-Host ('    downloading {0} ({1:N0} MB)' -f $url, ($p.size / 1MB))
        Invoke-WebRequest $url -OutFile $archive
        $hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $p.sha256) { throw "$($p.file): SHA-256 $hash, expected $($p.sha256)" }
    }

    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    if (Test-Path $SevenZip) { & $SevenZip x $archive "-o$Deps" -y -bso0 -bsp0 }
    else { tar -xf $archive -C $Deps }   # Windows' bsdtar reads 7z
    if ($LASTEXITCODE -ne 0) { throw "extracting $($p.file) failed ($LASTEXITCODE)" }

    # Relocate: the packages were built under $Manifest.origin.
    if ($Manifest.origin -ne $Here)
    {
        $origins = @($Manifest.origin, ($Manifest.origin -replace '/', '\'))
        $locals  = @($Here, ($Here -replace '/', '\'))
        $changed = 0
        Get-ChildItem $target -Recurse -File | Where-Object { $TextExt -contains $_.Extension.ToLowerInvariant() } | ForEach-Object {
            $text = [IO.File]::ReadAllText($_.FullName)
            $new = $text
            for ($i = 0; $i -lt 2; $i++) { $new = $new.Replace($origins[$i], $locals[$i], [StringComparison]::OrdinalIgnoreCase) }
            if ($new -ne $text) { [IO.File]::WriteAllText($_.FullName, $new); $changed++ }
        }
        Write-Host "    relocated paths in $changed files"
    }
    Set-Content $marker $p.sha256 -Encoding ascii
}
Write-Host "Prebuilt dependencies ready (release $($Manifest.release))." -ForegroundColor Green
