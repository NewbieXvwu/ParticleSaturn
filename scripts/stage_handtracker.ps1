# Stage HandTracker runtime assets into the Diligent output folder.
# - Copies the two .tflite models next to the executable (non-embedded model mode).
# - Copies HandTracker.dll for the dynamic-link mode.
#
# This script is designed to be called from MSBuild targets (ParticleSaturn.Diligent.targets).

param(
    [Parameter(Mandatory=$true)][string]$SrcDir,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [Parameter(Mandatory=$true)][string]$Platform,
    [Parameter(Mandatory=$true)][string]$HandTrackerConfig,
    [Parameter(Mandatory=$true)][ValidateSet("dll","static")][string]$LinkMode
)

$ErrorActionPreference = "Stop"

# Kill processes that may lock our output files
function Stop-LockingProcesses([string]$exeName) {
    $procs = Get-Process -Name $exeName -ErrorAction SilentlyContinue
    if ($procs) {
        Write-Host "Terminating $exeName to release file locks..."
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }
}

function Copy-FileIfExists([string]$from, [string]$toDir) {
    if (Test-Path $from) {
        $destPath = Join-Path $toDir (Split-Path -Leaf $from)
        $maxRetries = 3
        for ($i = 1; $i -le $maxRetries; $i++) {
            try {
                Copy-Item -Path $from -Destination $toDir -Force
                Write-Host "Staged: $(Split-Path -Leaf $from)"
                return $true
            } catch [System.IO.IOException] {
                if ($i -lt $maxRetries) {
                    Write-Host "File locked, retrying ($i/$maxRetries)..."
                    Start-Sleep -Milliseconds 500
                } else {
                    throw
                }
            }
        }
    }
    Write-Host "Missing (skip): $from"
    return $false
}

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# Models are always staged (unless EMBED_MODELS is used, which is handled separately in code/build flags).
$modelsDir = Join-Path $SrcDir "HandTracker/models"
Copy-FileIfExists (Join-Path $modelsDir "palm_detection_full.tflite") $OutDir | Out-Null
Copy-FileIfExists (Join-Path $modelsDir "hand_landmark_full.tflite") $OutDir | Out-Null

# DLL is only needed for dynamic linking.
if ($LinkMode -eq "dll") {
    # Kill exe that may lock the DLL before copying
    Stop-LockingProcesses "ParticleSaturn.Diligent"
    
    $handOutDir = Join-Path $SrcDir ("bin/{0}/{1}" -f $Platform, $HandTrackerConfig)
    Copy-FileIfExists (Join-Path $handOutDir "HandTracker.dll") $OutDir | Out-Null
    # PDB is useful for dev/debug, but optional. Copy if present.
    Copy-FileIfExists (Join-Path $handOutDir "HandTracker.pdb") $OutDir | Out-Null
}

