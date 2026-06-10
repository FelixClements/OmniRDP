param(
    [string]$ConfigPath = "C:\ProgramData\OmniRDP\config.ini",
    [string]$InstanceName = "vm2",
    [string]$ServiceName = "OmniRDP",
    [ValidateSet("installed-service", "worktree-console")]
    [string]$RuntimeMode = "installed-service",
    [string]$OutputRoot = ".omo\evidence\rdp-codec-fps\benchmark-runs",
    [string[]]$Rows = @("A", "B", "C", "D", "E", "F"),
    [ValidateSet("D", "E", "F")]
    [string]$BestBackendRow = "E",
    [string]$BackendPasswordOverride = "",
    [int]$DragSeconds = 30,
    [int]$AutoStartDelaySeconds = -1,
    [switch]$Apply,
    [switch]$BenchmarkViewerAuthNone,
    [ValidateSet("", "debug", "info", "trace", "warn", "error")]
    [string]$BenchmarkLogLevel = "",
    [switch]$SkipMstsc,
    [switch]$SkipRestart,
    [switch]$KeepLastConfig,
    [string]$WorktreeReleaseDir = "OmniRDP\build\Release",
    [string]$FreeRdpBuildDir = "freerdp-3.26.0\build-release",
    [string]$VcpkgBinDir = "C:\tools\vcpkg\installed\x64-windows\bin",
    [switch]$AutoWFreeRdp,
    [string]$WFreeRdpPath = "",
    [switch]$WFreeRdpFullscreen,
    [switch]$PrepBrowserAboutBlank,
    [int]$PrepClickX = 700,
    [int]$PrepClickY = 250,
    [string]$PrepClickSequence = "",
    [switch]$AutoDrag,
    [int]$DragStartX = 1000,
    [int]$DragStartY = 210,
    [int]$DragEndX = 650,
    [int]$DragEndY = 300,
    [int]$DragStepCount = 24
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RdpCodecBenchmarkSummary.ps1")
. (Join-Path $PSScriptRoot "RdpBenchmarkAutomation.ps1")

function Get-IniValue {
    param(
        [string[]]$Lines,
        [string]$Section,
        [string]$Key,
        [string]$Default = ""
    )

    $inSection = $false
    foreach ($line in $Lines) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.+)\]$') {
            $inSection = ($Matches[1] -ieq $Section)
            continue
        }
        if ($inSection -and $trimmed -match ('^' + [regex]::Escape($Key) + '\s*=\s*(.*)$')) {
            return $Matches[1].Trim()
        }
    }

    return $Default
}

function Set-IniValue {
    param(
        [string[]]$Lines,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )

    $result = New-Object System.Collections.Generic.List[string]
    $inSection = $false
    $sectionSeen = $false
    $keySet = $false

    foreach ($line in $Lines) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.+)\]$') {
            if ($inSection -and -not $keySet) {
                $result.Add("$Key = $Value")
                $keySet = $true
            }
            $inSection = ($Matches[1] -ieq $Section)
            if ($inSection) {
                $sectionSeen = $true
            }
            $result.Add($line)
            continue
        }

        if ($inSection -and $trimmed -match ('^' + [regex]::Escape($Key) + '\s*=')) {
            if (-not $keySet) {
                $result.Add("$Key = $Value")
                $keySet = $true
            }
            continue
        }

        $result.Add($line)
    }

    if (-not $sectionSeen) {
        $result.Add("")
        $result.Add("[$Section]")
    }
    if (-not $keySet) {
        $result.Add("$Key = $Value")
    }

    return $result.ToArray()
}

function Set-InstanceValues {
    param(
        [string[]]$Lines,
        [string]$Instance,
        [hashtable]$Values
    )

    $section = "instance:$Instance"
    $updated = $Lines
    foreach ($key in $Values.Keys) {
        $updated = Set-IniValue -Lines $updated -Section $section -Key $key -Value ([string]$Values[$key])
    }
    return $updated
}

function Redact-ConfigLines {
    param([string[]]$Lines)

    foreach ($line in $Lines) {
        if ($line.Trim() -match '^(backend\.password|viewer\.password|password)\s*=') {
            $name = $line.Split("=", 2)[0].Trim()
            "$name = <redacted>"
        } else {
            $line
        }
    }
}

function Get-RowSettings {
    param(
        [string]$Row,
        [string]$BestRow
    )

    $rowName = $Row.ToUpperInvariant()
    $base = @{
        "backend.gfx.decode_only_enabled" = "false"
        "backend.gfx.rfx_enabled" = "false"
        "viewer.gfx.enabled" = "false"
        "viewer.gfx.codec" = "uncompressed"
        "viewer.gfx.rfx_threading_enabled" = "false"
        "viewer.gfx.dirty_max_in_flight_frames" = "1"
        "viewer.gfx.dirty_max_in_flight_bytes" = "4194304"
    }

    switch ($rowName) {
        "A" { return $base }
        "B" {
            $base["viewer.gfx.enabled"] = "true"
            $base["viewer.gfx.codec"] = "rfx"
            return $base
        }
        "C" {
            $base["viewer.gfx.enabled"] = "true"
            $base["viewer.gfx.codec"] = "rfx"
            return $base
        }
        "D" {
            $base["backend.gfx.decode_only_enabled"] = "true"
            $base["viewer.gfx.enabled"] = "true"
            $base["viewer.gfx.codec"] = "rfx"
            return $base
        }
        "E" {
            $base["backend.gfx.rfx_enabled"] = "true"
            $base["viewer.gfx.enabled"] = "true"
            $base["viewer.gfx.codec"] = "rfx"
            return $base
        }
        "F" {
            $base["backend.gfx.decode_only_enabled"] = "true"
            $base["backend.gfx.rfx_enabled"] = "true"
            $base["viewer.gfx.enabled"] = "true"
            $base["viewer.gfx.codec"] = "rfx"
            return $base
        }
        "G" {
            $best = Get-RowSettings -Row $BestRow -BestRow $BestRow
            $best["viewer.gfx.rfx_threading_enabled"] = "true"
            return $best
        }
        "H" {
            $best = Get-RowSettings -Row "G" -BestRow $BestRow
            $best["viewer.gfx.dirty_max_in_flight_frames"] = "2"
            $best["viewer.gfx.dirty_max_in_flight_bytes"] = "8388608"
            return $best
        }
        default { throw "Unknown row '$Row'. Use A, B, C, D, E, F, G, or H." }
    }
}

function Start-CpuSample {
    param(
        [string]$Service,
        [int]$Seconds,
        [string]$Path
    )

    Start-Job -ArgumentList $Service, $Seconds, $Path -ScriptBlock {
        param($ServiceName, $DurationSeconds, $OutputPath)
        function Get-DescendantProcessIds {
            param(
                [int[]]$ParentIds,
                [object[]]$Processes
            )

            $all = New-Object System.Collections.Generic.HashSet[int]
            $frontier = New-Object System.Collections.Generic.Queue[int]
            foreach ($id in $ParentIds) {
                [void]$all.Add($id)
                $frontier.Enqueue($id)
            }

            while ($frontier.Count -gt 0) {
                $parent = $frontier.Dequeue()
                foreach ($proc in $Processes) {
                    if ([int]$proc.ParentProcessId -eq $parent -and -not $all.Contains([int]$proc.ProcessId)) {
                        [void]$all.Add([int]$proc.ProcessId)
                        $frontier.Enqueue([int]$proc.ProcessId)
                    }
                }
            }

            return $all
        }

        $samples = New-Object System.Collections.Generic.List[string]
        $samples.Add("timestamp,process_id,process_name,parent_process_id,cpu_seconds,cpu_percent,working_set_bytes")
        $stopAt = (Get-Date).AddSeconds($DurationSeconds)
        while ((Get-Date) -lt $stopAt) {
            try {
                $svc = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
                if ($svc -and $svc.ProcessId -gt 0) {
                    $cimProcesses = Get-CimInstance Win32_Process
                    $perfProcesses = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -ErrorAction SilentlyContinue
                    $targetIds = Get-DescendantProcessIds -ParentIds @([int]$svc.ProcessId) -Processes $cimProcesses
                    foreach ($targetId in $targetIds) {
                        $proc = Get-Process -Id $targetId -ErrorAction SilentlyContinue
                        $cimProc = $cimProcesses | Where-Object { [int]$_.ProcessId -eq $targetId } | Select-Object -First 1
                        $perfProc = $perfProcesses | Where-Object { [int]$_.IDProcess -eq $targetId } | Select-Object -First 1
                        if ($proc -and $cimProc) {
                            $cpuPercent = ""
                            if ($perfProc) {
                                $cpuPercent = $perfProc.PercentProcessorTime
                            }
                            $samples.Add(("{0:o},{1},{2},{3},{4},{5},{6}" -f (Get-Date), $proc.Id, $proc.ProcessName, $cimProc.ParentProcessId, $proc.CPU, $cpuPercent, $proc.WorkingSet64))
                        }
                    }
                } else {
                    $samples.Add(("{0:o},0,,,,," -f (Get-Date)))
                }
            } catch {
                $samples.Add(("{0:o},0,,,,," -f (Get-Date)))
            }
            Start-Sleep -Seconds 1
        }
        Set-Content -LiteralPath $OutputPath -Value $samples -Encoding UTF8
    }
}

function Start-CpuSampleForRootProcessIds {
    param(
        [int[]]$RootProcessIds,
        [int]$Seconds,
        [string]$Path
    )

    $rootCsv = ($RootProcessIds | ForEach-Object { [string]([int]$_) }) -join ","
    Start-Job -ArgumentList $rootCsv, $Seconds, $Path -ScriptBlock {
        param($RootCsv, $DurationSeconds, $OutputPath)
        $Roots = @()
        if ($RootCsv) {
            $Roots = $RootCsv.Split(",") | Where-Object { $_.Length -gt 0 } | ForEach-Object { [int]$_ }
        }
        function Get-DescendantProcessIds {
            param(
                [int[]]$ParentIds,
                [object[]]$Processes
            )

            $all = New-Object System.Collections.Generic.HashSet[int]
            $frontier = New-Object System.Collections.Generic.Queue[int]
            foreach ($id in $ParentIds) {
                if ($id -gt 0) {
                    [void]$all.Add($id)
                    $frontier.Enqueue($id)
                }
            }

            while ($frontier.Count -gt 0) {
                $parent = $frontier.Dequeue()
                foreach ($proc in $Processes) {
                    if ([int]$proc.ParentProcessId -eq $parent -and -not $all.Contains([int]$proc.ProcessId)) {
                        [void]$all.Add([int]$proc.ProcessId)
                        $frontier.Enqueue([int]$proc.ProcessId)
                    }
                }
            }

            return $all
        }

        $samples = New-Object System.Collections.Generic.List[string]
        $samples.Add("timestamp,process_id,process_name,parent_process_id,cpu_seconds,cpu_percent,working_set_bytes")
        $stopAt = (Get-Date).AddSeconds($DurationSeconds)
        while ((Get-Date) -lt $stopAt) {
            try {
                $cimProcesses = Get-CimInstance Win32_Process
                $perfProcesses = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -ErrorAction SilentlyContinue
                $targetIds = Get-DescendantProcessIds -ParentIds $Roots -Processes $cimProcesses
                foreach ($targetId in $targetIds) {
                    $proc = Get-Process -Id $targetId -ErrorAction SilentlyContinue
                    $cimProc = $cimProcesses | Where-Object { [int]$_.ProcessId -eq $targetId } | Select-Object -First 1
                    $perfProc = $perfProcesses | Where-Object { [int]$_.IDProcess -eq $targetId } | Select-Object -First 1
                    if ($proc -and $cimProc) {
                        $cpuPercent = ""
                        if ($perfProc) {
                            $cpuPercent = $perfProc.PercentProcessorTime
                        }
                        $samples.Add(("{0:o},{1},{2},{3},{4},{5},{6}" -f (Get-Date), $proc.Id, $proc.ProcessName, $cimProc.ParentProcessId, $proc.CPU, $cpuPercent, $proc.WorkingSet64))
                    }
                }
            } catch {
                $samples.Add(("{0:o},0,,,,," -f (Get-Date)))
            }
            Start-Sleep -Seconds 1
        }
        Set-Content -LiteralPath $OutputPath -Value $samples -Encoding UTF8
    }
}

function Copy-InstanceLogs {
    param(
        [string]$Instance,
        [string]$Destination,
        [string]$LogRoot = "C:\ProgramData\OmniRDP\logs"
    )

    $source = Join-Path $LogRoot $Instance
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $Destination -Recurse -Force
    }
}

function Write-ProcessTreeSnapshotByRoot {
    param(
        [int[]]$RootProcessIds,
        [string]$Path
    )

    $all = Get-CimInstance Win32_Process
    $rows = New-Object System.Collections.Generic.List[object]
    $queue = New-Object System.Collections.Generic.Queue[int]
    $seen = New-Object System.Collections.Generic.HashSet[int]
    foreach ($rootId in $RootProcessIds) {
        if ($rootId -gt 0) {
            $queue.Enqueue([int]$rootId)
            [void]$seen.Add([int]$rootId)
        }
    }

    while ($queue.Count -gt 0) {
        $id = $queue.Dequeue()
        $proc = $all | Where-Object { [int]$_.ProcessId -eq $id } | Select-Object -First 1
        if ($proc) {
            $rows.Add($proc)
        }
        foreach ($child in ($all | Where-Object { [int]$_.ParentProcessId -eq $id })) {
            if (-not $seen.Contains([int]$child.ProcessId)) {
                [void]$seen.Add([int]$child.ProcessId)
                $queue.Enqueue([int]$child.ProcessId)
            }
        }
    }

    if ($rows.Count -eq 0) {
        "No process tree found for root process ids: $($RootProcessIds -join ',')" |
            Set-Content -LiteralPath $Path -Encoding UTF8
        return
    }

    $rows |
        Select-Object ProcessId,ParentProcessId,Name,ExecutablePath,CommandLine |
        Format-List * |
        Out-File -LiteralPath $Path -Encoding UTF8
}

function Write-InstanceProcessSnapshot {
    param(
        [string]$Service,
        [string]$Path
    )

    $svc = Get-CimInstance Win32_Service -Filter "Name='$Service'"
    if (-not $svc) {
        "Service '$Service' not found" | Set-Content -LiteralPath $Path -Encoding UTF8
        return
    }

    $all = Get-CimInstance Win32_Process
    $rows = New-Object System.Collections.Generic.List[object]
    $queue = New-Object System.Collections.Generic.Queue[int]
    $seen = New-Object System.Collections.Generic.HashSet[int]
    if ($svc.ProcessId -gt 0) {
        $queue.Enqueue([int]$svc.ProcessId)
        [void]$seen.Add([int]$svc.ProcessId)
    }

    while ($queue.Count -gt 0) {
        $id = $queue.Dequeue()
        $proc = $all | Where-Object { [int]$_.ProcessId -eq $id } | Select-Object -First 1
        if ($proc) {
            $rows.Add($proc)
        }
        foreach ($child in ($all | Where-Object { [int]$_.ParentProcessId -eq $id })) {
            if (-not $seen.Contains([int]$child.ProcessId)) {
                [void]$seen.Add([int]$child.ProcessId)
                $queue.Enqueue([int]$child.ProcessId)
            }
        }
    }

    $rows |
        Select-Object ProcessId,ParentProcessId,Name,ExecutablePath,CommandLine |
        Format-List * |
        Out-File -LiteralPath $Path -Encoding UTF8
}

function Restart-OrStartService {
    param([string]$Name)

    $service = Get-Service -Name $Name -ErrorAction Stop
    if ($service.Status -eq "Running") {
        Restart-Service -Name $Name -Force
    } else {
        Start-Service -Name $Name
    }
}

function Restore-ServiceState {
    param(
        [string]$Name,
        [string]$OriginalStatus
    )

    if ($OriginalStatus -eq "NotFound") {
        return
    }

    if ($OriginalStatus -eq "Running") {
        Restart-OrStartService -Name $Name
        return
    }

    $service = Get-Service -Name $Name -ErrorAction Stop
    if ($service.Status -ne "Stopped") {
        Stop-Service -Name $Name -Force
    }
}

function Stop-ServiceIfPresent {
    param([string]$Name)

    $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne "Stopped") {
        Stop-Service -Name $Name -Force
    }
}

function Get-ExecutablePathFromCommandLine {
    param([string]$CommandLine)

    if ($CommandLine -match '^\s*"([^"]+)"') {
        return $Matches[1]
    }
    if ($CommandLine -match '^\s*(\S+)') {
        return $Matches[1]
    }
    return ""
}

function Get-Sha256Hex {
    param([string]$Path)

    if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
        $hash = Get-FileHash -LiteralPath $Path -Algorithm SHA256
        return $hash.Hash
    }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $bytes = $sha.ComputeHash($stream)
        return [System.BitConverter]::ToString($bytes).Replace("-", "")
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Write-BinaryProvenance {
    param(
        [string]$Service,
        [string]$Path
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("service_name=$Service")
    $svc = Get-CimInstance Win32_Service -Filter "Name='$Service'" -ErrorAction SilentlyContinue
    if ($svc) {
        $serviceExe = Get-ExecutablePathFromCommandLine -CommandLine $svc.PathName
        $lines.Add("service_path_name=$($svc.PathName)")
        $lines.Add("service_exe=$serviceExe")
        if ($serviceExe -and (Test-Path -LiteralPath $serviceExe)) {
            $lines.Add("service_exe_sha256=$(Get-Sha256Hex -Path $serviceExe)")
        }
    } else {
        $lines.Add("service_not_found=true")
    }

    $worktreeRelease = Join-Path (Get-Location) "OmniRDP\build\Release"
    $lines.Add("worktree_release_dir=$worktreeRelease")
    foreach ($exeName in @("OmniRDP.exe", "OmniRDP-svc.exe", "OmniRDP-tray.exe")) {
        $exePath = Join-Path $worktreeRelease $exeName
        $lines.Add(("worktree_{0}={1}" -f $exeName, $exePath))
        if (Test-Path -LiteralPath $exePath) {
            $lines.Add(("worktree_{0}.sha256={1}" -f $exeName, (Get-Sha256Hex -Path $exePath)))
        } else {
            $lines.Add(("worktree_{0}.missing=true" -f $exeName))
        }
    }
    $lines.Add("note=Windows service runs the installed service_exe, not the worktree build outputs.")
    Set-Content -LiteralPath $Path -Value $lines -Encoding UTF8
}

function Get-FreeRdpPathEntries {
    param(
        [string]$BuildDir,
        [string]$VcpkgDir
    )

    $entries = New-Object System.Collections.Generic.List[string]
    foreach ($relative in @(
            "libfreerdp\Release",
            "winpr\libwinpr\Release",
            "client\common\Release",
            "server\common\Release",
            "client\Windows\Release",
            "server\Windows\Release",
            "client\Windows\cli\Release")) {
        $path = Join-Path $BuildDir $relative
        if (Test-Path -LiteralPath $path) {
            $entries.Add((Resolve-Path -LiteralPath $path).Path)
        }
    }
    if ($VcpkgDir -and (Test-Path -LiteralPath $VcpkgDir)) {
        $entries.Add((Resolve-Path -LiteralPath $VcpkgDir).Path)
    }
    return $entries.ToArray()
}

function Start-WorktreeRuntime {
    param(
        [string]$ReleaseDir,
        [string]$RuntimeConfigPath,
        [string]$RowRoot,
        [string]$FreeRdpDir,
        [string]$VcpkgDir
    )

    $resolvedRelease = Resolve-Path -LiteralPath $ReleaseDir
    $svcExe = Join-Path $resolvedRelease.Path "OmniRDP-svc.exe"
    if (-not (Test-Path -LiteralPath $svcExe)) {
        throw "Worktree service binary not found: $svcExe"
    }

    $oldPath = $env:PATH
    $pathEntries = Get-FreeRdpPathEntries -BuildDir $FreeRdpDir -VcpkgDir $VcpkgDir
    if ($pathEntries.Count -gt 0) {
        $env:PATH = (($pathEntries + @($oldPath)) -join ";")
    }
    try {
        return Start-Process -FilePath $svcExe `
            -ArgumentList @("--run", "--config", $RuntimeConfigPath) `
            -WorkingDirectory $resolvedRelease.Path `
            -PassThru `
            -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $RowRoot "worktree-svc.out.txt") `
            -RedirectStandardError (Join-Path $RowRoot "worktree-svc.err.txt")
    } finally {
        $env:PATH = $oldPath
    }
}

function Stop-ProcessTree {
    param([int]$RootProcessId)

    if ($RootProcessId -le 0) {
        return
    }

    $all = Get-CimInstance Win32_Process
    $ids = New-Object System.Collections.Generic.List[int]
    $queue = New-Object System.Collections.Generic.Queue[int]
    $seen = New-Object System.Collections.Generic.HashSet[int]
    $queue.Enqueue($RootProcessId)
    [void]$seen.Add($RootProcessId)
    while ($queue.Count -gt 0) {
        $id = $queue.Dequeue()
        $ids.Add($id)
        foreach ($child in ($all | Where-Object { [int]$_.ParentProcessId -eq $id })) {
            if (-not $seen.Contains([int]$child.ProcessId)) {
                [void]$seen.Add([int]$child.ProcessId)
                $queue.Enqueue([int]$child.ProcessId)
            }
        }
    }

    $idArray = $ids.ToArray()
    [array]::Reverse($idArray)
    foreach ($id in $idArray) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
}

function Wait-ViewerPort {
    param(
        [string]$HostName,
        [int]$Port,
        [int]$TimeoutSeconds,
        [string]$Path
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lines = New-Object System.Collections.Generic.List[string]
    while ((Get-Date) -lt $deadline) {
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $connect = $client.BeginConnect($HostName, $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(500)) {
                $client.EndConnect($connect)
                $client.Close()
                $lines.Add(("{0:o} reachable {1}:{2}" -f (Get-Date), $HostName, $Port))
                Set-Content -LiteralPath $Path -Value $lines -Encoding UTF8
                return $true
            }
            $client.Close()
        } catch {
            $lines.Add(("{0:o} not reachable {1}:{2} {3}" -f (Get-Date), $HostName, $Port, $_.Exception.Message))
        }
        Start-Sleep -Milliseconds 500
    }
    $lines.Add(("{0:o} timeout waiting for {1}:{2}" -f (Get-Date), $HostName, $Port))
    Set-Content -LiteralPath $Path -Value $lines -Encoding UTF8
    return $false
}

function Ensure-DesktopAutomationTypes {
    if (-not ("OmniRdpBenchmarkNative" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class OmniRdpBenchmarkNative {
  [DllImport("user32.dll")]
  public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")]
  public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")]
  public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")]
  public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
}
"@
    }
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
}

function Resolve-WFreeRdpPath {
    param(
        [string]$ExplicitPath,
        [string]$FreeRdpDir
    )

    if ($ExplicitPath -and (Test-Path -LiteralPath $ExplicitPath)) {
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    $candidate = Join-Path $FreeRdpDir "client\Windows\cli\Release\wfreerdp.exe"
    if (Test-Path -LiteralPath $candidate) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }
    throw "wfreerdp.exe not found. Pass -WFreeRdpPath or set -FreeRdpBuildDir."
}

function Start-WFreeRdpViewer {
    param(
        [string]$ViewerExe,
        [string]$HostName,
        [int]$Port,
        [string]$RowRoot,
        [string]$FreeRdpDir,
        [string]$VcpkgDir,
        [bool]$Fullscreen
    )

    $oldPath = $env:PATH
    $pathEntries = Get-FreeRdpPathEntries -BuildDir $FreeRdpDir -VcpkgDir $VcpkgDir
    if ($pathEntries.Count -gt 0) {
        $env:PATH = (($pathEntries + @($oldPath)) -join ";")
    }
    try {
        $viewerArgs = @("/v:$HostName`:$Port", "/u:test", "/p:test", "/cert:ignore", "/bpp:32", "+gfx", "/log-level:INFO")
        if ($Fullscreen) {
            $viewerArgs += "/f"
        }
        return Start-Process -FilePath $ViewerExe `
            -ArgumentList $viewerArgs `
            -PassThru `
            -WindowStyle Normal `
            -RedirectStandardOutput (Join-Path $RowRoot "wfreerdp.out.txt") `
            -RedirectStandardError (Join-Path $RowRoot "wfreerdp.err.txt")
    } finally {
        $env:PATH = $oldPath
    }
}

function Wait-ProcessWindow {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutSeconds,
        [string]$ActionLogPath
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $fresh = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
        if (-not $fresh) {
            Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} viewer process exited before window" -f (Get-Date)) -Encoding UTF8
            return $false
        }
        if ($fresh.MainWindowHandle -ne 0) {
            Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} viewer window handle={1}" -f (Get-Date), $fresh.MainWindowHandle) -Encoding UTF8
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} timed out waiting for viewer window" -f (Get-Date)) -Encoding UTF8
    return $false
}

function Set-ProcessWindowForeground {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$ActionLogPath
    )

    Ensure-DesktopAutomationTypes
    $fresh = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if (-not $fresh -or $fresh.MainWindowHandle -eq 0) {
        Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} foreground_skipped missing viewer window" -f (Get-Date)) -Encoding UTF8
        return
    }

    [OmniRdpBenchmarkNative]::ShowWindow($fresh.MainWindowHandle, 9) | Out-Null
    [OmniRdpBenchmarkNative]::SetForegroundWindow($fresh.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} foreground_viewer_window handle={1}" -f (Get-Date), $fresh.MainWindowHandle) -Encoding UTF8
}

function Invoke-RemoteBrowserAboutBlankPrep {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$ClickX,
        [int]$ClickY,
        [string]$ActionLogPath
    )

    Ensure-DesktopAutomationTypes
    $fresh = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if (-not $fresh -or $fresh.MainWindowHandle -eq 0) {
        Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} browser_about_blank_prep_skipped missing viewer window" -f (Get-Date)) -Encoding UTF8
        return
    }

    [OmniRdpBenchmarkNative]::ShowWindow($fresh.MainWindowHandle, 9) | Out-Null
    [OmniRdpBenchmarkNative]::SetForegroundWindow($fresh.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
    [OmniRdpBenchmarkNative]::SetCursorPos($ClickX, $ClickY) | Out-Null
    Start-Sleep -Milliseconds 100
    [OmniRdpBenchmarkNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 50
    [OmniRdpBenchmarkNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    [System.Windows.Forms.SendKeys]::SendWait("^a")
    Start-Sleep -Milliseconds 250
    [System.Windows.Forms.SendKeys]::SendWait("about:blank")
    Start-Sleep -Milliseconds 100
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
    Start-Sleep -Seconds 2
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} browser_about_blank_prep_sent click={1},{2}" -f (Get-Date), $ClickX, $ClickY) -Encoding UTF8
}

function Invoke-DesktopClickSequence {
    param(
        [string]$Sequence,
        [string]$ActionLogPath
    )

    Ensure-DesktopAutomationTypes
    if ($Sequence.Trim().Length -eq 0) {
        return
    }

    foreach ($entry in ($Sequence -split ';')) {
        $trimmed = $entry.Trim()
        if ($trimmed.Length -eq 0) {
            continue
        }
        if ($trimmed.StartsWith("wait:", [System.StringComparison]::OrdinalIgnoreCase)) {
            $milliseconds = [int]$trimmed.Substring(5).Trim()
            Start-Sleep -Milliseconds $milliseconds
            Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} prep_wait_ms {1}" -f (Get-Date), $milliseconds) -Encoding UTF8
            continue
        }
        if ($trimmed.StartsWith("keys:", [System.StringComparison]::OrdinalIgnoreCase)) {
            $keys = $trimmed.Substring(5)
            [System.Windows.Forms.SendKeys]::SendWait($keys)
            Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} prep_keys {1}" -f (Get-Date), $keys) -Encoding UTF8
            Start-Sleep -Milliseconds 500
            continue
        }
        if ($trimmed.StartsWith("nativekeys:", [System.StringComparison]::OrdinalIgnoreCase)) {
            $chord = $trimmed.Substring(11)
            $virtualKeys = Convert-RdpBenchmarkNativeKeyChord -Chord $chord
            Invoke-RdpBenchmarkNativeKeyChord -VirtualKeys $virtualKeys -ActionLogPath $ActionLogPath
            Start-Sleep -Milliseconds 700
            continue
        }

        $doubleClick = $false
        if ($trimmed.StartsWith("dbl:", [System.StringComparison]::OrdinalIgnoreCase)) {
            $doubleClick = $true
            $trimmed = $trimmed.Substring(4).Trim()
        }

        $parts = $trimmed -split ','
        if ($parts.Count -ne 2) {
            throw "Invalid prep click '$trimmed'. Expected x,y or dbl:x,y entries separated by semicolons."
        }
        $x = [int]$parts[0].Trim()
        $y = [int]$parts[1].Trim()
        $clickCount = if ($doubleClick) { 2 } else { 1 }
        [OmniRdpBenchmarkNative]::SetCursorPos($x, $y) | Out-Null
        Start-Sleep -Milliseconds 150
        for ($i = 0; $i -lt $clickCount; $i++) {
            [OmniRdpBenchmarkNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 70
            [OmniRdpBenchmarkNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
            if ($i -lt ($clickCount - 1)) {
                Start-Sleep -Milliseconds 120
            }
        }
        Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} prep_click {1},{2} count={3}" -f (Get-Date), $x, $y, $clickCount) -Encoding UTF8
        Start-Sleep -Milliseconds 700
    }
}

function Invoke-DesktopDragLoop {
    param(
        [int]$Seconds,
        [int]$StartX,
        [int]$StartY,
        [int]$EndX,
        [int]$EndY,
        [int]$Steps,
        [string]$ActionLogPath
    )

    Ensure-DesktopAutomationTypes
    if ($Steps -lt 1) {
        $Steps = 1
    }
    $deadline = (Get-Date).AddSeconds($Seconds)
    $round = 0
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} drag_loop_start seconds={1} start={2},{3} end={4},{5} steps={6}" -f (Get-Date), $Seconds, $StartX, $StartY, $EndX, $EndY, $Steps) -Encoding UTF8
    while ((Get-Date) -lt $deadline) {
        $round++
        [OmniRdpBenchmarkNative]::SetCursorPos($StartX, $StartY) | Out-Null
        Start-Sleep -Milliseconds 50
        [OmniRdpBenchmarkNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        for ($i = 1; $i -le $Steps; $i++) {
            $x = [int]($StartX + (($EndX - $StartX) * $i / $Steps))
            $y = [int]($StartY + (($EndY - $StartY) * $i / $Steps))
            [OmniRdpBenchmarkNative]::SetCursorPos($x, $y) | Out-Null
            Start-Sleep -Milliseconds 20
        }
        [OmniRdpBenchmarkNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
        [OmniRdpBenchmarkNative]::SetCursorPos($EndX, $EndY) | Out-Null
        [OmniRdpBenchmarkNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        for ($i = 1; $i -le $Steps; $i++) {
            $x = [int]($EndX + (($StartX - $EndX) * $i / $Steps))
            $y = [int]($EndY + (($StartY - $EndY) * $i / $Steps))
            [OmniRdpBenchmarkNative]::SetCursorPos($x, $y) | Out-Null
            Start-Sleep -Milliseconds 20
        }
        [OmniRdpBenchmarkNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
    }
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} drag_loop_end rounds={1}" -f (Get-Date), $round) -Encoding UTF8
}

function Save-DesktopScreenshot {
    param([string]$Path)

    Ensure-DesktopAutomationTypes
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } catch {
        Set-Content -LiteralPath ($Path + ".error.txt") -Value $_.Exception.Message -Encoding UTF8
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $ConfigPath)) {
    throw "Config file not found: $ConfigPath"
}

$runStamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runRoot = Join-Path $OutputRoot $runStamp
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$runRoot = (Resolve-Path -LiteralPath $runRoot).Path
Write-BinaryProvenance -Service $ServiceName -Path (Join-Path $runRoot "binary-provenance.txt")

$originalLines = Get-Content -LiteralPath $ConfigPath
$originalService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
$originalServiceStatus = if ($originalService) { [string]$originalService.Status } else { "NotFound" }
$backupPath = Join-Path $runRoot "config.original.ini"
$redactedBackupPath = Join-Path $runRoot "config.original.redacted.ini"
Set-Content -LiteralPath $backupPath -Value $originalLines -Encoding UTF8
Set-Content -LiteralPath $redactedBackupPath -Value (Redact-ConfigLines -Lines $originalLines) -Encoding UTF8

$section = "instance:$InstanceName"
$viewerHost = Get-IniValue -Lines $originalLines -Section $section -Key "viewer.bind_address" -Default "127.0.0.1"
$viewerPort = Get-IniValue -Lines $originalLines -Section $section -Key "viewer.port" -Default "3390"

$summaryPath = Join-Path $runRoot "summary.csv"
Set-Content -LiteralPath $summaryPath -Value (Format-RdpCodecMetricSummaryCsvHeader) -Encoding UTF8
$worktreeProcessIds = New-Object System.Collections.Generic.List[int]
$runtimeConfigPaths = New-Object System.Collections.Generic.List[string]
$viewerProcessIds = New-Object System.Collections.Generic.List[int]
$useWorktreeConsole = ($RuntimeMode -eq "worktree-console")
$normalizedRows = New-Object System.Collections.Generic.List[string]
foreach ($rowValue in $Rows) {
    foreach ($rowPart in ($rowValue -split ",")) {
        $trimmedRow = $rowPart.Trim()
        if ($trimmedRow.Length -gt 0) {
            $normalizedRows.Add($trimmedRow)
        }
    }
}
if ($useWorktreeConsole -and -not $KeepLastConfig -and (Test-Path -LiteralPath $backupPath)) {
    Remove-Item -LiteralPath $backupPath -Force
}

try {
    foreach ($row in $normalizedRows) {
        $rowName = $row.ToUpperInvariant()
        $rowRoot = Join-Path $runRoot ("row-" + $rowName)
        New-Item -ItemType Directory -Force -Path $rowRoot | Out-Null

        $settings = Get-RowSettings -Row $rowName -BestRow $BestBackendRow
        if ($BackendPasswordOverride.Length -gt 0) {
            $settings["backend.password"] = $BackendPasswordOverride
        }
        if ($BenchmarkViewerAuthNone) {
            $settings["viewer.auth.mode"] = "none"
        }
        $rowStart = Get-Date
        $settings.GetEnumerator() |
            Sort-Object Name |
            ForEach-Object {
                if ($_.Name -eq "backend.password") {
                    "$($_.Name) = <redacted>"
                } else {
                    "$($_.Name) = $($_.Value)"
                }
            } |
            Set-Content -LiteralPath (Join-Path $rowRoot "settings.txt") -Encoding UTF8

        if (-not $Apply) {
            Add-Content -LiteralPath $summaryPath -Value (Format-RdpCodecMetricSummaryCsvRow -Row $rowName -Status "planned" -Start $rowStart -End $rowStart -ThreadedRfx $settings["viewer.gfx.rfx_threading_enabled"] -Metrics $null -Notes "dry run only")
            continue
        }

        $updatedLines = Set-InstanceValues -Lines $originalLines -Instance $InstanceName -Values $settings
        $runtimeProcess = $null
        $viewerProcess = $null
        $actionLogPath = Join-Path $rowRoot "computer-use-action-log.txt"
        $activeLogRoot = "C:\ProgramData\OmniRDP\logs"
        if ($useWorktreeConsole) {
            $activeLogRoot = Join-Path (Resolve-Path -LiteralPath $rowRoot).Path "logs"
            $updatedLines = Set-IniValue -Lines $updatedLines -Section "service" -Key "log_dir" -Value $activeLogRoot
            if ($BenchmarkLogLevel.Length -gt 0) {
                $updatedLines = Set-IniValue -Lines $updatedLines -Section "service" -Key "log_level" -Value $BenchmarkLogLevel
            }
            $runtimeConfigPath = Join-Path $rowRoot "config.runtime.ini"
            Set-Content -LiteralPath $runtimeConfigPath -Value $updatedLines -Encoding UTF8
            Set-Content -LiteralPath (Join-Path $rowRoot "config.runtime.redacted.ini") -Value (Redact-ConfigLines -Lines $updatedLines) -Encoding UTF8
            $runtimeConfigPaths.Add($runtimeConfigPath)
            $runtimeProcess = Start-WorktreeRuntime -ReleaseDir $WorktreeReleaseDir -RuntimeConfigPath $runtimeConfigPath -RowRoot $rowRoot -FreeRdpDir $FreeRdpBuildDir -VcpkgDir $VcpkgBinDir
            $worktreeProcessIds.Add([int]$runtimeProcess.Id)
            "runtime_mode=worktree-console`nroot_pid=$($runtimeProcess.Id)`nconfig=$runtimeConfigPath`nlog_root=$activeLogRoot" |
                Set-Content -LiteralPath (Join-Path $rowRoot "runtime.txt") -Encoding UTF8
            [void](Wait-ViewerPort -HostName $viewerHost -Port ([int]$viewerPort) -TimeoutSeconds 20 -Path (Join-Path $rowRoot "viewer-port.txt"))
            Write-ProcessTreeSnapshotByRoot -RootProcessIds @([int]$runtimeProcess.Id) -Path (Join-Path $rowRoot "process-tree-before.txt")
        } else {
            Set-Content -LiteralPath $ConfigPath -Value $updatedLines -Encoding UTF8
            Set-Content -LiteralPath (Join-Path $rowRoot "config.applied.redacted.ini") -Value (Redact-ConfigLines -Lines $updatedLines) -Encoding UTF8

            if (-not $SkipRestart) {
                Restart-OrStartService -Name $ServiceName
                Start-Sleep -Seconds 5
            }

            Get-Service -Name $ServiceName | Format-List * | Out-File -LiteralPath (Join-Path $rowRoot "service-status.txt") -Encoding UTF8
            Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" |
                Select-Object Name,State,ProcessId,PathName |
                Format-List * |
                Out-File -LiteralPath (Join-Path $rowRoot "service-process.txt") -Encoding UTF8
            Write-InstanceProcessSnapshot -Service $ServiceName -Path (Join-Path $rowRoot "process-tree-before.txt")
            Test-NetConnection -ComputerName $viewerHost -Port ([int]$viewerPort) |
                Format-List * |
                Out-File -LiteralPath (Join-Path $rowRoot "viewer-port.txt") -Encoding UTF8
        }
        Copy-InstanceLogs -Instance $InstanceName -Destination (Join-Path $rowRoot "logs-before") -LogRoot $activeLogRoot

        $rdpPath = Join-Path $rowRoot "viewer.rdp"
        Set-Content -LiteralPath $rdpPath -Value @(
            "full address:s:$viewerHost`:$viewerPort",
            "prompt for credentials:i:1",
            "desktopwidth:i:1920",
            "desktopheight:i:1080",
            "session bpp:i:32"
        ) -Encoding ASCII

        if ($AutoWFreeRdp) {
            $viewerExe = Resolve-WFreeRdpPath -ExplicitPath $WFreeRdpPath -FreeRdpDir $FreeRdpBuildDir
            Add-Content -LiteralPath $actionLogPath -Value ("{0:o} starting_wfreerdp path={1} target={2}:{3}" -f (Get-Date), $viewerExe, $viewerHost, $viewerPort) -Encoding UTF8
            $viewerProcess = Start-WFreeRdpViewer -ViewerExe $viewerExe -HostName $viewerHost -Port ([int]$viewerPort) -RowRoot $rowRoot -FreeRdpDir $FreeRdpBuildDir -VcpkgDir $VcpkgBinDir -Fullscreen ([bool]$WFreeRdpFullscreen)
            $viewerProcessIds.Add([int]$viewerProcess.Id)
            Add-Content -LiteralPath $actionLogPath -Value ("{0:o} wfreerdp_pid={1}" -f (Get-Date), $viewerProcess.Id) -Encoding UTF8
            [void](Wait-ProcessWindow -Process $viewerProcess -TimeoutSeconds 20 -ActionLogPath $actionLogPath)
            Set-ProcessWindowForeground -Process $viewerProcess -ActionLogPath $actionLogPath
            if ($PrepBrowserAboutBlank) {
                Invoke-RemoteBrowserAboutBlankPrep -Process $viewerProcess -ClickX $PrepClickX -ClickY $PrepClickY -ActionLogPath $actionLogPath
            }
            Invoke-DesktopClickSequence -Sequence $PrepClickSequence -ActionLogPath $actionLogPath
        } elseif (-not $SkipMstsc) {
            Start-Process -FilePath "mstsc.exe" -ArgumentList "`"$rdpPath`""
        }

        Write-Host ""
        if ($AutoStartDelaySeconds -ge 0) {
            Write-Host "Row $rowName is active. Starting timed drag window in $AutoStartDelaySeconds seconds."
            Start-Sleep -Seconds $AutoStartDelaySeconds
        } else {
            Write-Host "Row $rowName is active. Connect the viewer if needed, open the same large opaque window, then press Enter to start the timed drag."
            [void](Read-Host)
        }

        if ($AutoWFreeRdp -or $AutoDrag) {
            Save-DesktopScreenshot -Path (Join-Path $rowRoot "desktop-before-drag.png")
        }

        $cpuPath = Join-Path $rowRoot "cpu-samples.csv"
        if ($useWorktreeConsole) {
            $job = Start-CpuSampleForRootProcessIds -RootProcessIds @([int]$runtimeProcess.Id) -Seconds $DragSeconds -Path $cpuPath
        } else {
            $job = Start-CpuSample -Service $ServiceName -Seconds $DragSeconds -Path $cpuPath
        }
        Write-Host "Start dragging now. Sampling for $DragSeconds seconds..."
        if ($AutoDrag) {
            Invoke-DesktopDragLoop -Seconds $DragSeconds -StartX $DragStartX -StartY $DragStartY -EndX $DragEndX -EndY $DragEndY -Steps $DragStepCount -ActionLogPath $actionLogPath
        } else {
            Start-Sleep -Seconds $DragSeconds
        }
        Wait-Job -Job $job | Out-Null
        Receive-Job -Job $job | Out-Null
        Remove-Job -Job $job

        if ($AutoWFreeRdp -or $AutoDrag) {
            Save-DesktopScreenshot -Path (Join-Path $rowRoot "desktop-after-drag.png")
        }

        Copy-InstanceLogs -Instance $InstanceName -Destination (Join-Path $rowRoot "logs-after") -LogRoot $activeLogRoot
        $rowMetrics = Get-RdpCodecLogMetrics -LogRoot (Join-Path $rowRoot "logs-after")
        if ($viewerProcess) {
            Stop-ProcessTree -RootProcessId ([int]$viewerProcess.Id)
            Start-Sleep -Milliseconds 500
            $viewerRemaining = Get-Process -Id ([int]$viewerProcess.Id) -ErrorAction SilentlyContinue
            if ($viewerRemaining) {
                Add-Content -LiteralPath (Join-Path $rowRoot "viewer-cleanup.txt") -Value "remaining_viewer_pid=$($viewerProcess.Id)" -Encoding UTF8
            } else {
                Add-Content -LiteralPath (Join-Path $rowRoot "viewer-cleanup.txt") -Value "remaining_viewer_processes=none" -Encoding UTF8
            }
        }
        if ($useWorktreeConsole) {
            Write-ProcessTreeSnapshotByRoot -RootProcessIds @([int]$runtimeProcess.Id) -Path (Join-Path $rowRoot "process-tree-after.txt")
            Stop-ProcessTree -RootProcessId ([int]$runtimeProcess.Id)
            Start-Sleep -Milliseconds 500
            $remaining = Get-Process -Id ([int]$runtimeProcess.Id) -ErrorAction SilentlyContinue
            if ($remaining) {
                "remaining_root_pid=$($runtimeProcess.Id)" |
                    Set-Content -LiteralPath (Join-Path $rowRoot "cleanup.txt") -Encoding UTF8
            } else {
                "remaining_processes=none" |
                    Set-Content -LiteralPath (Join-Path $rowRoot "cleanup.txt") -Encoding UTF8
            }
            if (-not $KeepLastConfig -and (Test-Path -LiteralPath $runtimeConfigPath)) {
                Remove-Item -LiteralPath $runtimeConfigPath -Force
                Add-Content -LiteralPath (Join-Path $rowRoot "cleanup.txt") -Value "removed_unredacted_config=true" -Encoding UTF8
            }
        } else {
            Write-InstanceProcessSnapshot -Service $ServiceName -Path (Join-Path $rowRoot "process-tree-after.txt")
        }

        $rowEnd = Get-Date
        Add-Content -LiteralPath $summaryPath -Value (Format-RdpCodecMetricSummaryCsvRow -Row $rowName -Status "captured" -Start $rowStart -End $rowEnd -ThreadedRfx $settings["viewer.gfx.rfx_threading_enabled"] -Metrics $rowMetrics -Notes "logs and cpu captured")
    }
} finally {
    if ($Apply -and -not $KeepLastConfig -and -not $useWorktreeConsole) {
        if (-not $SkipRestart) {
            Stop-ServiceIfPresent -Name $ServiceName
            Start-Sleep -Seconds 2
        }
        Set-Content -LiteralPath $ConfigPath -Value $originalLines -Encoding UTF8
        if (-not $SkipRestart) {
            Restore-ServiceState -Name $ServiceName -OriginalStatus $originalServiceStatus
        }
    }
    foreach ($trackedPid in $worktreeProcessIds) {
        Stop-ProcessTree -RootProcessId ([int]$trackedPid)
    }
    foreach ($trackedViewerPid in $viewerProcessIds) {
        Stop-ProcessTree -RootProcessId ([int]$trackedViewerPid)
    }
    if (-not $KeepLastConfig) {
        foreach ($path in $runtimeConfigPaths) {
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Force
            }
        }
    }
}

Write-Host "Benchmark artifacts: $runRoot"
