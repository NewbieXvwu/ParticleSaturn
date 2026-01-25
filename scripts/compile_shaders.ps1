# Shader Pre-compilation Script
# Compiles HLSL -> DXBC/DXIL, GLSL -> SPIR-V and generates C++ byte arrays
#
# Usage: .\compile_shaders.ps1 [-Clean] [-Verbose]

param(
    [switch]$Clean,
    [switch]$VerboseOutput
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ShaderSrcDir = Join-Path $ProjectRoot "src\shaders"
$GeneratedDir = Join-Path $ProjectRoot "src\generated"
$OutputHeader = Join-Path $GeneratedDir "ShaderBytecodes.h"

# Ensure output directory exists
if (-not (Test-Path $GeneratedDir)) {
    New-Item -ItemType Directory -Path $GeneratedDir -Force | Out-Null
}

if ($Clean) {
    Write-Host "Cleaning generated shader bytecodes..."
    if (Test-Path $OutputHeader) {
        Remove-Item $OutputHeader -Force
    }
    exit 0
}

# ============================================================================
# Find shader compilers
# ============================================================================

function Find-DXC {
    # Try Windows SDK
    $sdkPaths = @(
        "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe"
    )
    foreach ($pattern in $sdkPaths) {
        $hit = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue | Sort-Object -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }

    # Try Visual Studio
    $vsPaths = @(
        "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dxc.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dxc.exe"
    )
    foreach ($pattern in $vsPaths) {
        $hit = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue | Sort-Object -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }

    # Try PATH
    $cmd = Get-Command dxc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    return $null
}

function Find-FXC {
    $sdkPaths = @(
        "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe"
    )
    foreach ($pattern in $sdkPaths) {
        $hit = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue | Sort-Object -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }

    $cmd = Get-Command fxc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    return $null
}

function Find-GlslangValidator {
    # Try Vulkan SDK
    $vulkanSdk = $env:VULKAN_SDK
    if ($vulkanSdk) {
        $path = Join-Path $vulkanSdk "Bin\glslangValidator.exe"
        if (Test-Path $path) { return $path }
    }

    # Try PATH
    $cmd = Get-Command glslangValidator -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    return $null
}

$DXC = Find-DXC
$FXC = Find-FXC
$GLSLANG = Find-GlslangValidator

Write-Host "Shader compilers:"
Write-Host "  DXC: $(if ($DXC) { $DXC } else { 'NOT FOUND' })"
Write-Host "  FXC: $(if ($FXC) { $FXC } else { 'NOT FOUND' })"
Write-Host "  glslangValidator: $(if ($GLSLANG) { $GLSLANG } else { 'NOT FOUND' })"

if (-not $DXC -and -not $FXC) {
    Write-Error "Neither DXC nor FXC found. Cannot compile HLSL shaders."
    exit 1
}

if (-not $GLSLANG) {
    Write-Warning "glslangValidator not found. Vulkan shaders will not be pre-compiled."
}

# ============================================================================
# Shader definitions
# ============================================================================

$Shaders = @(
    # Graphics shaders (VS + PS)
    @{ Name = "FullscreenQuad"; VS = $true; PS = $true; CS = $false; MS = $false; HLSL = $true; GLSL = $true },
    @{ Name = "BloomDownsample"; VS = $true; PS = $true; CS = $false; MS = $false; HLSL = $true; GLSL = $true },
    @{ Name = "BloomBlur"; VS = $true; PS = $true; CS = $false; MS = $false; HLSL = $true; GLSL = $true },
    @{ Name = "AcrylicComposite"; VS = $true; PS = $true; CS = $false; MS = $false; HLSL = $true; GLSL = $true },
    @{ Name = "Star"; VS = $true; PS = $true; CS = $false; MS = $false; HLSL = $true; GLSL = $true },
    @{ Name = "SaturnParticle"; VS = $true; PS = $true; CS = $false; MS = $false; HLSL = $true; GLSL = $true },
    @{ Name = "SevenSeg"; VS = $true; PS = $true; CS = $false; MS = $false; HLSL = $true; GLSL = $true },

    # Compute shaders
    @{ Name = "SaturnCompute"; VS = $false; PS = $false; CS = $true; MS = $false; HLSL = $true; GLSL = $true },
    @{ Name = "SaturnInit"; VS = $false; PS = $false; CS = $true; MS = $false; HLSL = $true; GLSL = $true },

    # Mesh shader (D3D12 only)
    @{ Name = "SaturnParticleMesh"; VS = $false; PS = $true; CS = $false; MS = $true; HLSL = $true; GLSL = $false }
)

# ============================================================================
# Compilation functions
# ============================================================================

function Compile-HLSL {
    param(
        [string]$InputFile,
        [string]$OutputFile,
        [string]$EntryPoint,
        [string]$Profile
    )

    $compiler = $null
    $args = @()

    # Use DXC for SM 6.x (mesh shaders), FXC for SM 5.x (D3D11 compatibility)
    if ($Profile -match "^[a-z]+_6_") {
        if (-not $DXC) {
            Write-Warning "DXC required for $Profile but not found, skipping $InputFile"
            return $false
        }
        $compiler = $DXC
        $args = @("-T", $Profile, "-E", $EntryPoint, "-Fo", $OutputFile, $InputFile, "-O3")
    } else {
        # SM 5.x: prefer FXC for D3D11 compatibility (true DXBC format)
        # DXC's SM 5.0 output may not work correctly on D3D11
        $compiler = if ($FXC) { $FXC } else { $DXC }
        if ($compiler -eq $FXC) {
            $args = @("/T", $Profile, "/E", $EntryPoint, "/Fo", $OutputFile, "/O3", $InputFile)
        } else {
            $args = @("-T", $Profile, "-E", $EntryPoint, "-Fo", $OutputFile, $InputFile, "-O3")
        }
    }

    if ($VerboseOutput) {
        Write-Host "  $compiler $($args -join ' ')"
    }

    # Use [System.IO.Path]::GetTempFileName() for PowerShell 4.0 compatibility
    $stderrFile = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $compiler -ArgumentList $args -NoNewWindow -Wait -PassThru -RedirectStandardError $stderrFile
        return $process.ExitCode -eq 0
    } finally {
        Remove-Item $stderrFile -Force -ErrorAction SilentlyContinue
    }
}

function Compile-GLSL {
    param(
        [string]$InputFile,
        [string]$OutputFile,
        [string]$Stage
    )

    if (-not $GLSLANG) { return $false }

    $args = @("-V", "-S", $Stage, "-o", $OutputFile, $InputFile)

    if ($VerboseOutput) {
        Write-Host "  $GLSLANG $($args -join ' ')"
    }

    # Use [System.IO.Path]::GetTempFileName() for PowerShell 4.0 compatibility
    $stderrFile = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $GLSLANG -ArgumentList $args -NoNewWindow -Wait -PassThru -RedirectStandardError $stderrFile
        return $process.ExitCode -eq 0
    } finally {
        Remove-Item $stderrFile -Force -ErrorAction SilentlyContinue
    }
}

function Convert-ToByteArray {
    param(
        [string]$BinaryFile,
        [string]$ArrayName
    )

    if (-not (Test-Path $BinaryFile)) {
        return $null
    }

    $bytes = [System.IO.File]::ReadAllBytes($BinaryFile)
    $sb = [System.Text.StringBuilder]::new()

    [void]$sb.AppendLine("alignas(4) constexpr unsigned char ${ArrayName}[] = {")

    for ($i = 0; $i -lt $bytes.Length; $i += 16) {
        [void]$sb.Append("    ")
        $end = [Math]::Min($i + 16, $bytes.Length)
        for ($j = $i; $j -lt $end; $j++) {
            [void]$sb.Append("0x$($bytes[$j].ToString('X2'))")
            if ($j -lt $bytes.Length - 1) {
                [void]$sb.Append(", ")
            }
        }
        [void]$sb.AppendLine()
    }

    [void]$sb.AppendLine("};")
    return $sb.ToString()
}

# ============================================================================
# Main compilation loop
# ============================================================================

$TempDir = Join-Path $env:TEMP "ParticleSaturn_ShaderCompile"
if (-not (Test-Path $TempDir)) {
    New-Item -ItemType Directory -Path $TempDir -Force | Out-Null
}

$HeaderContent = [System.Text.StringBuilder]::new()
[void]$HeaderContent.AppendLine("// Auto-generated shader bytecodes - DO NOT EDIT")
[void]$HeaderContent.AppendLine("// Generated by scripts/compile_shaders.ps1")
[void]$HeaderContent.AppendLine("#pragma once")
[void]$HeaderContent.AppendLine()
[void]$HeaderContent.AppendLine("#include <cstddef>")
[void]$HeaderContent.AppendLine()
[void]$HeaderContent.AppendLine("namespace ParticleSaturn::ShaderBytecodes {")
[void]$HeaderContent.AppendLine()

$SuccessCount = 0
$FailCount = 0
$SkipCount = 0

foreach ($shader in $Shaders) {
    $name = $shader.Name
    Write-Host "Compiling $name..."

    $hlslDir = Join-Path $ShaderSrcDir "hlsl"
    $glslDir = Join-Path $ShaderSrcDir "glsl"

    # HLSL Vertex Shader
    if ($shader.VS -and $shader.HLSL) {
        $src = Join-Path $hlslDir "${name}_VS.hlsl"
        if (Test-Path $src) {
            $out = Join-Path $TempDir "${name}_VS.dxbc"
            if (Compile-HLSL -InputFile $src -OutputFile $out -EntryPoint "main" -Profile "vs_5_0") {
                $bytes = Convert-ToByteArray -BinaryFile $out -ArrayName "${name}_VS_DXBC"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            } else {
                Write-Warning "  Failed to compile ${name}_VS.hlsl"
                $FailCount++
            }
        } else {
            Write-Warning "  Source not found: $src"
            $SkipCount++
        }
    }

    # HLSL Pixel Shader
    if ($shader.PS -and $shader.HLSL) {
        $src = Join-Path $hlslDir "${name}_PS.hlsl"
        if (Test-Path $src) {
            $out = Join-Path $TempDir "${name}_PS.dxbc"
            if (Compile-HLSL -InputFile $src -OutputFile $out -EntryPoint "main" -Profile "ps_5_0") {
                $bytes = Convert-ToByteArray -BinaryFile $out -ArrayName "${name}_PS_DXBC"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            } else {
                Write-Warning "  Failed to compile ${name}_PS.hlsl"
                $FailCount++
            }
        } else {
            Write-Warning "  Source not found: $src"
            $SkipCount++
        }
    }

    # HLSL Compute Shader
    if ($shader.CS -and $shader.HLSL) {
        $src = Join-Path $hlslDir "${name}_CS.hlsl"
        if (Test-Path $src) {
            $out = Join-Path $TempDir "${name}_CS.dxbc"
            if (Compile-HLSL -InputFile $src -OutputFile $out -EntryPoint "main" -Profile "cs_5_0") {
                $bytes = Convert-ToByteArray -BinaryFile $out -ArrayName "${name}_CS_DXBC"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            } else {
                Write-Warning "  Failed to compile ${name}_CS.hlsl"
                $FailCount++
            }
        } else {
            Write-Warning "  Source not found: $src"
            $SkipCount++
        }
    }

    # HLSL Mesh Shader (SM 6.5)
    if ($shader.MS -and $shader.HLSL) {
        $src = Join-Path $hlslDir "${name}_MS.hlsl"
        if (Test-Path $src) {
            $out = Join-Path $TempDir "${name}_MS.dxil"
            if (Compile-HLSL -InputFile $src -OutputFile $out -EntryPoint "main" -Profile "ms_6_5") {
                $bytes = Convert-ToByteArray -BinaryFile $out -ArrayName "${name}_MS_DXIL"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            } else {
                Write-Warning "  Failed to compile ${name}_MS.hlsl (mesh shader)"
                $FailCount++
            }
        } else {
            Write-Warning "  Source not found: $src"
            $SkipCount++
        }

        # Also compile the PS for mesh shader PSO with SM 6.5
        $srcPS = Join-Path $hlslDir "${name}_PS.hlsl"
        if (Test-Path $srcPS) {
            $outPS = Join-Path $TempDir "${name}_MeshPS.dxil"
            if (Compile-HLSL -InputFile $srcPS -OutputFile $outPS -EntryPoint "main" -Profile "ps_6_5") {
                $bytes = Convert-ToByteArray -BinaryFile $outPS -ArrayName "${name}_MeshPS_DXIL"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            }
        }
    }

    # GLSL Vertex Shader -> SPIR-V
    if ($shader.VS -and $shader.GLSL -and $GLSLANG) {
        $src = Join-Path $glslDir "${name}_VS.glsl"
        if (Test-Path $src) {
            $out = Join-Path $TempDir "${name}_VS.spv"
            if (Compile-GLSL -InputFile $src -OutputFile $out -Stage "vert") {
                $bytes = Convert-ToByteArray -BinaryFile $out -ArrayName "${name}_VS_SPIRV"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            } else {
                Write-Warning "  Failed to compile ${name}_VS.glsl"
                $FailCount++
            }
        } else {
            Write-Warning "  Source not found: $src"
            $SkipCount++
        }
    }

    # GLSL Fragment Shader -> SPIR-V
    if ($shader.PS -and $shader.GLSL -and $GLSLANG) {
        $src = Join-Path $glslDir "${name}_PS.glsl"
        if (Test-Path $src) {
            $out = Join-Path $TempDir "${name}_PS.spv"
            if (Compile-GLSL -InputFile $src -OutputFile $out -Stage "frag") {
                $bytes = Convert-ToByteArray -BinaryFile $out -ArrayName "${name}_PS_SPIRV"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            } else {
                Write-Warning "  Failed to compile ${name}_PS.glsl"
                $FailCount++
            }
        } else {
            Write-Warning "  Source not found: $src"
            $SkipCount++
        }
    }

    # GLSL Compute Shader -> SPIR-V
    if ($shader.CS -and $shader.GLSL -and $GLSLANG) {
        $src = Join-Path $glslDir "${name}_CS.glsl"
        if (Test-Path $src) {
            $out = Join-Path $TempDir "${name}_CS.spv"
            if (Compile-GLSL -InputFile $src -OutputFile $out -Stage "comp") {
                $bytes = Convert-ToByteArray -BinaryFile $out -ArrayName "${name}_CS_SPIRV"
                if ($bytes) {
                    [void]$HeaderContent.AppendLine($bytes)
                    $SuccessCount++
                }
            } else {
                Write-Warning "  Failed to compile ${name}_CS.glsl"
                $FailCount++
            }
        } else {
            Write-Warning "  Source not found: $src"
            $SkipCount++
        }
    }
}

[void]$HeaderContent.AppendLine("} // namespace ParticleSaturn::ShaderBytecodes")

# Write output header
$HeaderContent.ToString() | Out-File -FilePath $OutputHeader -Encoding utf8

Write-Host ""
Write-Host "Shader compilation complete:"
Write-Host "  Success: $SuccessCount"
Write-Host "  Failed:  $FailCount"
Write-Host "  Skipped: $SkipCount"
Write-Host "  Output:  $OutputHeader"

# Cleanup temp files
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

if ($FailCount -gt 0) {
    exit 1
}
