$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "OmniRdpBenchmarkDeploy.ps1")

$repoRoot = Split-Path -Parent $PSScriptRoot
$tempRoot = Join-Path $repoRoot ".omo\evidence\rfx-threaded-dirty-batching\deploy-helper-fixture"
if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}

$releaseDir = Join-Path $tempRoot "release"
$installDir = Join-Path $tempRoot "install"
$freeRdpDir = Join-Path $tempRoot "freerdp"
$freeRdpLibDir = Join-Path $freeRdpDir "libfreerdp\Release"
$vcpkgDir = Join-Path $tempRoot "vcpkg"
New-Item -ItemType Directory -Force -Path $releaseDir, $installDir, $freeRdpLibDir, $vcpkgDir | Out-Null

foreach ($name in @("OmniRDP.exe", "OmniRDP-svc.exe", "OmniRDP-tray.exe")) {
    Set-Content -LiteralPath (Join-Path $releaseDir $name) -Value "new-$name" -Encoding ASCII
}
Set-Content -LiteralPath (Join-Path $freeRdpLibDir "freerdp3.dll") -Value "freerdp" -Encoding ASCII
Set-Content -LiteralPath (Join-Path $vcpkgDir "zlib1.dll") -Value "zlib" -Encoding ASCII
Set-Content -LiteralPath (Join-Path $installDir "OmniRDP-svc.exe") -Value "old-svc" -Encoding ASCII

$payload = Get-OmniRdpBenchmarkInstallPayload `
    -ReleaseDir $releaseDir `
    -InstallDir $installDir `
    -FreeRdpBuildDir $freeRdpDir `
    -VcpkgBinDir $vcpkgDir

foreach ($name in @("OmniRDP.exe", "OmniRDP-svc.exe", "OmniRDP-tray.exe")) {
    $item = $payload | Where-Object { $_.Name -eq $name } | Select-Object -First 1
    if (-not $item -or -not $item.Required -or $item.Kind -ne "exe" -or -not $item.Source) {
        throw "Required exe payload missing or malformed: $name"
    }
}

$freerdp = $payload | Where-Object { $_.Name -eq "freerdp3.dll" } | Select-Object -First 1
if (-not $freerdp -or $freerdp.Kind -ne "dll" -or $freerdp.Required -or -not $freerdp.Source) {
    throw "freerdp3.dll optional dependency was not discovered."
}

$zlib = $payload | Where-Object { $_.Name -eq "zlib1.dll" } | Select-Object -First 1
if (-not $zlib -or $zlib.Kind -ne "dll" -or $zlib.Required -or -not $zlib.Source) {
    throw "zlib1.dll optional dependency was not discovered."
}

$manifest = Join-Path $tempRoot "payload.csv"
Write-OmniRdpBenchmarkPayloadManifest -Payload $payload -Path $manifest
$manifestContent = Get-Content -LiteralPath $manifest -Raw
if ($manifestContent -notmatch "source_sha256" -or $manifestContent -notmatch "destination_sha256") {
    throw "Payload manifest did not include source/destination hashes."
}

$planRoot = Install-OmniRdpBenchmarkBuild `
    -ReleaseDir $releaseDir `
    -InstallDir $installDir `
    -ServiceName "NoSuchOmniRdpFixtureService" `
    -FreeRdpBuildDir $freeRdpDir `
    -VcpkgBinDir $vcpkgDir `
    -OutputRoot (Join-Path $tempRoot "plan") `
    -PlanOnly
if (-not (Test-Path -LiteralPath (Join-Path $planRoot "payload-before.csv"))) {
    throw "Plan-only deploy did not write payload manifest."
}
if ((Get-Content -LiteralPath (Join-Path $installDir "OmniRDP-svc.exe") -Raw) -notmatch "old-svc") {
    throw "Plan-only deploy modified the install directory."
}

$brokenRelease = Join-Path $tempRoot "broken-release"
New-Item -ItemType Directory -Force -Path $brokenRelease | Out-Null
$missingThrew = $false
try {
    [void](Get-OmniRdpBenchmarkInstallPayload `
            -ReleaseDir $brokenRelease `
            -InstallDir $installDir `
            -FreeRdpBuildDir $freeRdpDir `
            -VcpkgBinDir $vcpkgDir)
} catch {
    $missingThrew = ($_.Exception.Message -like "*Required OmniRDP build artifact missing*")
}
if (-not $missingThrew) {
    throw "Missing required executable did not throw the expected error."
}

Remove-Item -LiteralPath $tempRoot -Recurse -Force
Write-Host "OmniRDP benchmark deploy helper tests passed"
