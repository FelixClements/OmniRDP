param(
    [string]$ReleaseDir = "OmniRDP\build\Release",
    [string]$InstallDir = "C:\Program Files\OmniRDP",
    [string]$ServiceName = "OmniRDP",
    [string]$FreeRdpBuildDir = "freerdp-3.26.0\build",
    [string]$VcpkgBinDir = "C:\tools\vcpkg\installed\x64-windows\bin",
    [string]$OutputRoot = ".omo\evidence\rfx-threaded-dirty-batching\installed-deploy",
    [switch]$PlanOnly
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "OmniRdpBenchmarkDeploy.ps1")

$runRoot = Install-OmniRdpBenchmarkBuild `
    -ReleaseDir $ReleaseDir `
    -InstallDir $InstallDir `
    -ServiceName $ServiceName `
    -FreeRdpBuildDir $FreeRdpBuildDir `
    -VcpkgBinDir $VcpkgBinDir `
    -OutputRoot $OutputRoot `
    -PlanOnly:$PlanOnly

Write-Host "Deploy artifacts: $runRoot"
