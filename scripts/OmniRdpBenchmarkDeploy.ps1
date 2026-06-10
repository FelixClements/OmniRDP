$script:OmniRdpBenchmarkExeNames = @(
    "OmniRDP.exe",
    "OmniRDP-svc.exe",
    "OmniRDP-tray.exe"
)

$script:OmniRdpBenchmarkDllNames = @(
    "freerdp-client3.dll",
    "freerdp-server3.dll",
    "freerdp3.dll",
    "winpr3.dll",
    "libcrypto-3-x64.dll",
    "libssl-3-x64.dll",
    "libusb-1.0.dll",
    "zlib1.dll"
)

function Test-OmniRdpBenchmarkElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-OmniRdpBenchmarkSha256 {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ""
    }

    $hash = Get-FileHash -LiteralPath $Path -Algorithm SHA256
    return $hash.Hash
}

function Get-OmniRdpBenchmarkDependencySearchRoots {
    param(
        [string]$FreeRdpBuildDir,
        [string]$VcpkgBinDir
    )

    $roots = New-Object System.Collections.Generic.List[string]
    if ($FreeRdpBuildDir -and (Test-Path -LiteralPath $FreeRdpBuildDir)) {
        foreach ($relative in @(
                "libfreerdp\Release",
                "winpr\libwinpr\Release",
                "client\common\Release",
                "server\common\Release",
                "client\Windows\Release",
                "server\Windows\Release",
                "client\Windows\cli\Release")) {
            $path = Join-Path $FreeRdpBuildDir $relative
            if (Test-Path -LiteralPath $path) {
                $roots.Add((Resolve-Path -LiteralPath $path).Path)
            }
        }
    }
    if ($VcpkgBinDir -and (Test-Path -LiteralPath $VcpkgBinDir)) {
        $roots.Add((Resolve-Path -LiteralPath $VcpkgBinDir).Path)
    }
    return $roots.ToArray()
}

function Find-OmniRdpBenchmarkDependency {
    param(
        [string]$Name,
        [string[]]$SearchRoots
    )

    foreach ($root in $SearchRoots) {
        $candidate = Join-Path $root $Name
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return ""
}

function Get-OmniRdpBenchmarkInstallPayload {
    param(
        [string]$ReleaseDir = "OmniRDP\build\Release",
        [string]$InstallDir = "C:\Program Files\OmniRDP",
        [string]$FreeRdpBuildDir = "freerdp-3.26.0\build",
        [string]$VcpkgBinDir = "C:\tools\vcpkg\installed\x64-windows\bin"
    )

    $resolvedRelease = Resolve-Path -LiteralPath $ReleaseDir -ErrorAction Stop
    $searchRoots = Get-OmniRdpBenchmarkDependencySearchRoots `
        -FreeRdpBuildDir $FreeRdpBuildDir `
        -VcpkgBinDir $VcpkgBinDir
    $payload = New-Object System.Collections.Generic.List[object]

    foreach ($name in $script:OmniRdpBenchmarkExeNames) {
        $source = Join-Path $resolvedRelease.Path $name
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Required OmniRDP build artifact missing: $source"
        }
        $payload.Add([pscustomobject]@{
                Name = $name
                Kind = "exe"
                Required = $true
                Source = (Resolve-Path -LiteralPath $source).Path
                Destination = (Join-Path $InstallDir $name)
            })
    }

    foreach ($name in $script:OmniRdpBenchmarkDllNames) {
        $source = Find-OmniRdpBenchmarkDependency -Name $name -SearchRoots $searchRoots
        $payload.Add([pscustomobject]@{
                Name = $name
                Kind = "dll"
                Required = $false
                Source = $source
                Destination = (Join-Path $InstallDir $name)
            })
    }

    return $payload.ToArray()
}

function Write-OmniRdpBenchmarkPayloadManifest {
    param(
        [object[]]$Payload,
        [string]$Path
    )

    $rows = foreach ($item in $Payload) {
        [pscustomobject]@{
            name = $item.Name
            kind = $item.Kind
            required = $item.Required
            source = $item.Source
            source_sha256 = $(if ($item.Source) { Get-OmniRdpBenchmarkSha256 -Path $item.Source } else { "" })
            destination = $item.Destination
            destination_sha256 = $(if (Test-Path -LiteralPath $item.Destination) { Get-OmniRdpBenchmarkSha256 -Path $item.Destination } else { "" })
        }
    }
    $rows | ConvertTo-Csv -NoTypeInformation | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Install-OmniRdpBenchmarkBuild {
    param(
        [string]$ReleaseDir = "OmniRDP\build\Release",
        [string]$InstallDir = "C:\Program Files\OmniRDP",
        [string]$ServiceName = "OmniRDP",
        [string]$FreeRdpBuildDir = "freerdp-3.26.0\build",
        [string]$VcpkgBinDir = "C:\tools\vcpkg\installed\x64-windows\bin",
        [string]$OutputRoot = ".omo\evidence\rfx-threaded-dirty-batching\installed-deploy",
        [switch]$PlanOnly
    )

    $runStamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $runRoot = Join-Path $OutputRoot $runStamp
    New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
    $runRoot = (Resolve-Path -LiteralPath $runRoot).Path

    $payload = Get-OmniRdpBenchmarkInstallPayload `
        -ReleaseDir $ReleaseDir `
        -InstallDir $InstallDir `
        -FreeRdpBuildDir $FreeRdpBuildDir `
        -VcpkgBinDir $VcpkgBinDir
    Write-OmniRdpBenchmarkPayloadManifest `
        -Payload $payload `
        -Path (Join-Path $runRoot "payload-before.csv")

    if ($PlanOnly) {
        "plan_only=true`nrun_root=$runRoot" |
            Set-Content -LiteralPath (Join-Path $runRoot "result.txt") -Encoding UTF8
        return $runRoot
    }

    if (-not (Test-OmniRdpBenchmarkElevated)) {
        "elevated=false`nrun_root=$runRoot" |
            Set-Content -LiteralPath (Join-Path $runRoot "result.txt") -Encoding UTF8
        throw "Installing benchmark binaries requires an elevated PowerShell session."
    }

    $service = Get-Service -Name $ServiceName -ErrorAction Stop
    $service | Format-List * |
        Out-File -LiteralPath (Join-Path $runRoot "service-before.txt") -Encoding UTF8

    $backupDir = Join-Path $runRoot "backup-installed"
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    foreach ($item in $payload) {
        if (Test-Path -LiteralPath $item.Destination) {
            Copy-Item -LiteralPath $item.Destination `
                -Destination (Join-Path $backupDir $item.Name) `
                -Force
        }
    }

    if ($service.Status -ne "Stopped") {
        Stop-Service -Name $ServiceName -Force
        $service.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(30))
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    foreach ($item in $payload) {
        if ($item.Source) {
            Copy-Item -LiteralPath $item.Source -Destination $item.Destination -Force
        }
    }

    Start-Service -Name $ServiceName
    $service = Get-Service -Name $ServiceName -ErrorAction Stop
    $service.WaitForStatus("Running", [TimeSpan]::FromSeconds(30))
    $service | Format-List * |
        Out-File -LiteralPath (Join-Path $runRoot "service-after.txt") -Encoding UTF8

    Write-OmniRdpBenchmarkPayloadManifest `
        -Payload $payload `
        -Path (Join-Path $runRoot "payload-after.csv")
    "installed=true`nrun_root=$runRoot" |
        Set-Content -LiteralPath (Join-Path $runRoot "result.txt") -Encoding UTF8
    return $runRoot
}
