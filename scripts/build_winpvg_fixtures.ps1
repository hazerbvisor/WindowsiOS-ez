# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Fxc = "fxc.exe"
)
$ErrorActionPreference = "Stop"
$source = Join-Path $PSScriptRoot "..\tests\shaders\wpvg_d3d11.hlsl"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$tests = @(
    @{Entry="vs_main"; Profile="vs_5_0"; Name="vertex.cso"},
    @{Entry="ps_red"; Profile="ps_5_0"; Name="red.cso"},
    @{Entry="ps_green"; Profile="ps_5_0"; Name="green.cso"}
)
foreach ($test in $tests) {
    $out = Join-Path $OutputDirectory $test.Name
    & $Fxc /nologo /T $test.Profile /E $test.Entry /Fo $out $source
    if ($LASTEXITCODE -ne 0) { throw "FXC failed for $($test.Entry)" }
    $bytes = [IO.File]::ReadAllBytes($out)
    if ($bytes.Length -lt 32 -or [Text.Encoding]::ASCII.GetString($bytes,0,4) -ne "DXBC") {
        throw "Not a DXBC file: $out"
    }
    Get-FileHash -Algorithm SHA256 $out
}
