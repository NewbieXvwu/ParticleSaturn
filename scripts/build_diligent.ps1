# Pure PowerShell build script - no cmd.exe dependency
# This completely bypasses Clink AutoRun interference

param(
    [Parameter(Mandatory=$true)][string]$Action,
    [Parameter(Mandatory=$true)][string]$SrcDir,
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$Config,
    [switch]$FastBuild
)

$ErrorActionPreference = "Stop"

# Fix encoding for MSVC/CMake output (MSVC uses system codepage, typically GBK on Chinese Windows)
[Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936)

# Try to locate Ninja (faster incremental builds than NMake for large projects).
function Find-NinjaPath {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }

    $candidates = @()
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $root) { continue }
        $candidates += Join-Path $root "Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
        $candidates += Join-Path $root "Microsoft Visual Studio\2019\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    }
    foreach ($pattern in $candidates) {
        $hit = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit -and $hit.FullName) { return $hit.FullName }
    }
    return $null
}

$ninjaPath = Find-NinjaPath
$generator = if ($ninjaPath) { "Ninja" } else { "NMake Makefiles" }

# Fast iteration mode: disable slow LTCG (/GL) in Release builds.
# - Default OFF (only enabled by -FastBuild or env var PARTICLESATURN_FAST_BUILD=1).
$fastBuildEnv = $env:PARTICLESATURN_FAST_BUILD
$fastBuild =
    if ($FastBuild.IsPresent) {
        $true
    } elseif ($fastBuildEnv -ne $null -and $fastBuildEnv -ne "") {
        ($fastBuildEnv -ne "0")
    } else {
        $false
    }

# Create build directory if needed
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

# If the existing build directory was configured with a different generator, reset CMake cache.
$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cache = Get-Content $cacheFile -ErrorAction SilentlyContinue
    $genLine = $cache | Where-Object { $_ -like "CMAKE_GENERATOR:*" } | Select-Object -First 1
    if ($genLine) {
        $oldGen = ($genLine -split "=", 2)[1].Trim()
        if ($oldGen -and $oldGen -ne $generator) {
            Write-Host "Generator changed ($oldGen -> $generator). Resetting CMake cache in $BuildDir ..."
            Remove-Item -Force $cacheFile -ErrorAction SilentlyContinue
            $cmakeFilesDir = Join-Path $BuildDir "CMakeFiles"
            if (Test-Path $cmakeFilesDir) { Remove-Item -Recurse -Force $cmakeFilesDir -ErrorAction SilentlyContinue }
        }
    }
}

$cmakeArgs = @(
    "-S", $SrcDir,
    "-B", $BuildDir,
    "-G", $generator,
    "-DCMAKE_BUILD_TYPE=$Config"
)
if ($ninjaPath) {
    $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$ninjaPath"
}
if ($fastBuild) {
    $cmakeArgs += "-DPARTICLESATURN_FAST_BUILD=ON"
} else {
    $cmakeArgs += "-DPARTICLESATURN_FAST_BUILD=OFF"
}

switch ($Action.ToLower()) {
    "clean" {
        $cacheFile = Join-Path $BuildDir "CMakeCache.txt"
        if (Test-Path $cacheFile) {
            Write-Host "Cleaning build..."
            & cmake --build $BuildDir --target clean
            exit $LASTEXITCODE
        }
        exit 0
    }
    "build" {
        Write-Host "Configuring with CMake..."
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "Building ParticleSaturn.Diligent..."
        if ($generator -eq "Ninja") {
            & cmake --build $BuildDir --target ParticleSaturn.Diligent --parallel
        } else {
            & cmake --build $BuildDir --target ParticleSaturn.Diligent
        }
        exit $LASTEXITCODE
    }
    "rebuild" {
        Write-Host "Configuring with CMake..."
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "Cleaning..."
        & cmake --build $BuildDir --target clean
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Write-Host "Building ParticleSaturn.Diligent..."
        if ($generator -eq "Ninja") {
            & cmake --build $BuildDir --target ParticleSaturn.Diligent --parallel
        } else {
            & cmake --build $BuildDir --target ParticleSaturn.Diligent
        }
        exit $LASTEXITCODE
    }
    default {
        Write-Error "Unknown action: $Action"
        exit 2
    }
}
