param(
    [string]$ConfigPath = "C:\ProgramData\OmniRDP\config.ini",
    [string]$InstanceName = "vm2",
    [string]$OutputRoot = ".omo\ulw-loop\evidence\task6-multi-viewer-isolation",
    [ValidateSet("decode-only", "backend-rfx")]
    [string]$BackendMode = "decode-only",
    [int]$DragSeconds = 60,
    [int]$CrashAfterSeconds = 15,
    [int]$InitialWaitSeconds = 60,
    [string]$WorktreeReleaseDir = "OmniRDP\build\Release",
    [string]$FreeRdpBuildDir = "freerdp-3.26.0\build-release",
    [string]$VcpkgBinDir = "C:\tools\vcpkg\installed\x64-windows\bin",
    [string]$WFreeRdpPath = "",
    [int]$DragStartX = 900,
    [int]$DragStartY = 270,
    [int]$DragEndX = 520,
    [int]$DragEndY = 220,
    [int]$DragStepCount = 24
)

$ErrorActionPreference = "Stop"

function Get-IniValue {
    param([string[]]$Lines, [string]$Section, [string]$Key, [string]$Default = "")

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
    param([string[]]$Lines, [string]$Section, [string]$Key, [string]$Value)

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
    param([string[]]$Lines, [string]$Instance, [hashtable]$Values)

    $updated = $Lines
    $section = "instance:$Instance"
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

function Get-FreeRdpPathEntries {
    param([string]$BuildDir, [string]$VcpkgDir)

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
        [string]$RunRoot,
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
            -RedirectStandardOutput (Join-Path $RunRoot "worktree-svc.out.txt") `
            -RedirectStandardError (Join-Path $RunRoot "worktree-svc.err.txt")
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
    param([string]$HostName, [int]$Port, [int]$TimeoutSeconds, [string]$Path)

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
    if (-not ("OmniRdpIsolationNative" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class OmniRdpIsolationNative {
  [DllImport("user32.dll")]
  public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")]
  public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")]
  public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@
    }
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
}

function Resolve-WFreeRdpPath {
    param([string]$ExplicitPath, [string]$FreeRdpDir)

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
        [string]$RunRoot,
        [string]$Name,
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
        } else {
            $viewerArgs += @("/w:1280", "/h:720")
        }
        return Start-Process -FilePath $ViewerExe `
            -ArgumentList $viewerArgs `
            -PassThru `
            -WindowStyle Normal `
            -RedirectStandardOutput (Join-Path $RunRoot "$Name.wfreerdp.out.txt") `
            -RedirectStandardError (Join-Path $RunRoot "$Name.wfreerdp.err.txt")
    } finally {
        $env:PATH = $oldPath
    }
}

function Wait-ProcessWindow {
    param([System.Diagnostics.Process]$Process, [int]$TimeoutSeconds, [string]$ActionLogPath, [string]$Name)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $fresh = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
        if (-not $fresh) {
            Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} {1}_process_exited_before_window" -f (Get-Date), $Name) -Encoding UTF8
            return $false
        }
        if ($fresh.MainWindowHandle -ne 0) {
            Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} {1}_window_handle={2}" -f (Get-Date), $Name, $fresh.MainWindowHandle) -Encoding UTF8
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} {1}_window_timeout" -f (Get-Date), $Name) -Encoding UTF8
    return $false
}

function Set-ProcessWindowForeground {
    param([System.Diagnostics.Process]$Process, [string]$ActionLogPath, [string]$Name)

    Ensure-DesktopAutomationTypes
    $fresh = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if (-not $fresh -or $fresh.MainWindowHandle -eq 0) {
        Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} {1}_foreground_skipped" -f (Get-Date), $Name) -Encoding UTF8
        return
    }
    [OmniRdpIsolationNative]::ShowWindow($fresh.MainWindowHandle, 9) | Out-Null
    [OmniRdpIsolationNative]::SetForegroundWindow($fresh.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} {1}_foreground handle={2}" -f (Get-Date), $Name, $fresh.MainWindowHandle) -Encoding UTF8
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

function Invoke-DragLoopWithCrash {
    param(
        [int]$Seconds,
        [int]$CrashAfter,
        [int]$CrashTargetPid,
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
    $start = Get-Date
    $deadline = $start.AddSeconds($Seconds)
    $crashAt = $start.AddSeconds($CrashAfter)
    $crashed = $false
    $round = 0
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} drag_loop_start seconds={1} crash_after={2} crash_pid={3}" -f $start, $Seconds, $CrashAfter, $CrashTargetPid) -Encoding UTF8
    while ((Get-Date) -lt $deadline) {
        if (-not $crashed -and (Get-Date) -ge $crashAt) {
            $crashTs = Get-Date
            Stop-ProcessTree -RootProcessId $CrashTargetPid
            $crashed = $true
            Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} crash_target_terminated pid={1}" -f $crashTs, $CrashTargetPid) -Encoding UTF8
            Start-Sleep -Milliseconds 500
        }

        $round++
        [OmniRdpIsolationNative]::SetCursorPos($StartX, $StartY) | Out-Null
        Start-Sleep -Milliseconds 50
        [OmniRdpIsolationNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        for ($i = 1; $i -le $Steps; $i++) {
            $x = [int]($StartX + (($EndX - $StartX) * $i / $Steps))
            $y = [int]($StartY + (($EndY - $StartY) * $i / $Steps))
            [OmniRdpIsolationNative]::SetCursorPos($x, $y) | Out-Null
            Start-Sleep -Milliseconds 20
        }
        [OmniRdpIsolationNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
        [OmniRdpIsolationNative]::SetCursorPos($EndX, $EndY) | Out-Null
        [OmniRdpIsolationNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        for ($i = 1; $i -le $Steps; $i++) {
            $x = [int]($EndX + (($StartX - $EndX) * $i / $Steps))
            $y = [int]($EndY + (($StartY - $EndY) * $i / $Steps))
            [OmniRdpIsolationNative]::SetCursorPos($x, $y) | Out-Null
            Start-Sleep -Milliseconds 20
        }
        [OmniRdpIsolationNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
    }
    Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} drag_loop_end rounds={1} crash_sent={2}" -f (Get-Date), $round, $crashed) -Encoding UTF8
}

function Get-MsSinceMidnight {
    param([datetime]$Value)

    return [int64]$Value.TimeOfDay.TotalMilliseconds
}

function Get-LogMsSinceMidnight {
    param([string]$Line)

    if ($Line -match '^\[(\d{2}):(\d{2}):(\d{2}):(\d{3})\]') {
        return ((([int]$Matches[1] * 60L + [int]$Matches[2]) * 60L + [int]$Matches[3]) * 1000L + [int]$Matches[4])
    }
    return $null
}

function Analyze-IsolationLog {
    param([string]$ViewerLogPath, [string]$ActionLogPath, [string]$SummaryPath)

    $action = Get-Content -LiteralPath $ActionLogPath
    $crashLine = $action | Where-Object { $_ -match 'crash_target_terminated pid=' } | Select-Object -First 1
    if (-not $crashLine -or $crashLine -notmatch '^([^ ]+) ') {
        throw "No crash marker found in action log."
    }
    $crashTime = [datetime]::Parse($Matches[1])
    $crashMs = Get-MsSinceMidnight -Value $crashTime
    $postCrashThreshold = $crashMs + 1000

    $lines = Get-Content -LiteralPath $ViewerLogPath
    $healthyDirty = 0
    $healthyAck = 0
    $healthyFatal = 0
    $crashViewerDirtyAfterCrash = 0
    $activated = @()

    foreach ($line in $lines) {
        if ($line -match 'Viewer ([0-9]+) activated') {
            $activated += $Matches[1]
        }
        $ms = Get-LogMsSinceMidnight -Line $line
        if ($null -eq $ms -or $ms -lt $postCrashThreshold) {
            continue
        }
        if ($line -match 'Viewer 1 RDPEGFX dirty send complete') {
            $healthyDirty++
        }
        if ($line -match 'Viewer 1 RDPEGFX frame ack pdu') {
            $healthyAck++
        }
        if ($line -match 'Viewer 2 RDPEGFX dirty send complete') {
            $crashViewerDirtyAfterCrash++
        }
        if ($line -match 'Viewer 1 .*RDPEGFX message handling failed|Viewer 1 .*post-activation RDPEGFX failure|Viewer 1 .*disconnecting|Viewer 1 .*classic_fallback|Viewer 1 .*ack_policy_suspended=true|Viewer 1 .*suspended=true') {
            $healthyFatal++
        }
    }

    $status = "FAIL"
    if ($healthyDirty -gt 0 -and $healthyAck -gt 0 -and $healthyFatal -eq 0) {
        $status = "PASS"
    }

    $summary = @(
        "status=$status",
        "backend_mode=$BackendMode",
        "viewer_rfx_threading_enabled=true",
        "healthy_viewer_assumed_id=1",
        "crash_target_assumed_id=2",
        "activation_order=$($activated -join ',')",
        "crash_marker=$($crashTime.ToString('o'))",
        "post_crash_threshold_ms_since_midnight=$postCrashThreshold",
        "post_crash_healthy_dirty_sends=$healthyDirty",
        "post_crash_healthy_frame_acks=$healthyAck",
        "post_crash_healthy_fatal_or_suspended_lines=$healthyFatal",
        "post_crash_crash_viewer_dirty_sends=$crashViewerDirtyAfterCrash",
        "viewer_log=$ViewerLogPath",
        "action_log=$ActionLogPath"
    )
    Set-Content -LiteralPath $SummaryPath -Value $summary -Encoding UTF8
    return $status
}

if (-not (Test-Path -LiteralPath $ConfigPath)) {
    throw "Config file not found: $ConfigPath"
}

$runStamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runRoot = Join-Path $OutputRoot $runStamp
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$runRoot = (Resolve-Path -LiteralPath $runRoot).Path
$actionLogPath = Join-Path $runRoot "computer-use-action-log.txt"
$runtimeConfigPath = Join-Path $runRoot "config.runtime.ini"
$runtimeProcess = $null
$healthyViewer = $null
$crashViewer = $null

try {
    $originalLines = Get-Content -LiteralPath $ConfigPath
    $section = "instance:$InstanceName"
    $viewerHost = Get-IniValue -Lines $originalLines -Section $section -Key "viewer.bind_address" -Default "127.0.0.1"
    $viewerPort = [int](Get-IniValue -Lines $originalLines -Section $section -Key "viewer.port" -Default "3390")

    $settings = @{
        "backend.gfx.decode_only_enabled" = if ($BackendMode -eq "decode-only") { "true" } else { "false" }
        "backend.gfx.rfx_enabled" = if ($BackendMode -eq "backend-rfx") { "true" } else { "false" }
        "viewer.gfx.enabled" = "true"
        "viewer.gfx.codec" = "rfx"
        "viewer.gfx.rfx_threading_enabled" = "true"
        "viewer.gfx.dirty_max_in_flight_frames" = "1"
        "viewer.gfx.dirty_max_in_flight_bytes" = "4194304"
        "viewer.auth.mode" = "none"
    }
    $updatedLines = Set-InstanceValues -Lines $originalLines -Instance $InstanceName -Values $settings
    $activeLogRoot = Join-Path $runRoot "logs"
    $updatedLines = Set-IniValue -Lines $updatedLines -Section "service" -Key "log_dir" -Value $activeLogRoot
    $updatedLines = Set-IniValue -Lines $updatedLines -Section "service" -Key "log_level" -Value "debug"
    Set-Content -LiteralPath $runtimeConfigPath -Value $updatedLines -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $runRoot "config.runtime.redacted.ini") -Value (Redact-ConfigLines -Lines $updatedLines) -Encoding UTF8

    $settings.GetEnumerator() |
        Sort-Object Name |
        ForEach-Object { "$($_.Name) = $($_.Value)" } |
        Set-Content -LiteralPath (Join-Path $runRoot "settings.txt") -Encoding UTF8

    $runtimeProcess = Start-WorktreeRuntime -ReleaseDir $WorktreeReleaseDir -RuntimeConfigPath $runtimeConfigPath -RunRoot $runRoot -FreeRdpDir $FreeRdpBuildDir -VcpkgDir $VcpkgBinDir
    "runtime_mode=worktree-console`nroot_pid=$($runtimeProcess.Id)`nconfig=$runtimeConfigPath`nlog_root=$activeLogRoot" |
        Set-Content -LiteralPath (Join-Path $runRoot "runtime.txt") -Encoding UTF8
    [void](Wait-ViewerPort -HostName $viewerHost -Port $viewerPort -TimeoutSeconds 30 -Path (Join-Path $runRoot "viewer-port.txt"))

    $viewerExe = Resolve-WFreeRdpPath -ExplicitPath $WFreeRdpPath -FreeRdpDir $FreeRdpBuildDir
    Add-Content -LiteralPath $actionLogPath -Value ("{0:o} starting_healthy_viewer target={1}:{2}" -f (Get-Date), $viewerHost, $viewerPort) -Encoding UTF8
    $healthyViewer = Start-WFreeRdpViewer -ViewerExe $viewerExe -HostName $viewerHost -Port $viewerPort -RunRoot $runRoot -Name "healthy" -FreeRdpDir $FreeRdpBuildDir -VcpkgDir $VcpkgBinDir -Fullscreen $true
    Add-Content -LiteralPath $actionLogPath -Value ("{0:o} healthy_viewer_pid={1}" -f (Get-Date), $healthyViewer.Id) -Encoding UTF8
    [void](Wait-ProcessWindow -Process $healthyViewer -TimeoutSeconds 30 -ActionLogPath $actionLogPath -Name "healthy")
    Set-ProcessWindowForeground -Process $healthyViewer -ActionLogPath $actionLogPath -Name "healthy"
    Start-Sleep -Seconds 10

    Add-Content -LiteralPath $actionLogPath -Value ("{0:o} starting_crash_viewer target={1}:{2}" -f (Get-Date), $viewerHost, $viewerPort) -Encoding UTF8
    $crashViewer = Start-WFreeRdpViewer -ViewerExe $viewerExe -HostName $viewerHost -Port $viewerPort -RunRoot $runRoot -Name "crash-target" -FreeRdpDir $FreeRdpBuildDir -VcpkgDir $VcpkgBinDir -Fullscreen $false
    Add-Content -LiteralPath $actionLogPath -Value ("{0:o} crash_viewer_pid={1}" -f (Get-Date), $crashViewer.Id) -Encoding UTF8
    [void](Wait-ProcessWindow -Process $crashViewer -TimeoutSeconds 30 -ActionLogPath $actionLogPath -Name "crash")
    Start-Sleep -Seconds 10
    Set-ProcessWindowForeground -Process $healthyViewer -ActionLogPath $actionLogPath -Name "healthy"

    Add-Content -LiteralPath $actionLogPath -Value ("{0:o} initial_wait_start seconds={1}" -f (Get-Date), $InitialWaitSeconds) -Encoding UTF8
    Start-Sleep -Seconds $InitialWaitSeconds
    Add-Content -LiteralPath $actionLogPath -Value ("{0:o} initial_wait_end" -f (Get-Date)) -Encoding UTF8

    Save-DesktopScreenshot -Path (Join-Path $runRoot "desktop-before-drag.png")
    Invoke-DragLoopWithCrash -Seconds $DragSeconds -CrashAfter $CrashAfterSeconds -CrashTargetPid ([int]$crashViewer.Id) -StartX $DragStartX -StartY $DragStartY -EndX $DragEndX -EndY $DragEndY -Steps $DragStepCount -ActionLogPath $actionLogPath
    Save-DesktopScreenshot -Path (Join-Path $runRoot "desktop-after-drag.png")

    $viewerLogPath = Join-Path $activeLogRoot $InstanceName "viewer.log"
    if (-not (Test-Path -LiteralPath $viewerLogPath)) {
        $viewerLogPath = Join-Path $activeLogRoot "viewer.log"
    }
    $status = Analyze-IsolationLog -ViewerLogPath $viewerLogPath -ActionLogPath $actionLogPath -SummaryPath (Join-Path $runRoot "summary.txt")
    if ($status -ne "PASS") {
        throw "Isolation analysis failed. See $runRoot"
    }
} finally {
    if ($healthyViewer) {
        Stop-ProcessTree -RootProcessId ([int]$healthyViewer.Id)
    }
    if ($crashViewer) {
        Stop-ProcessTree -RootProcessId ([int]$crashViewer.Id)
    }
    if ($runtimeProcess) {
        Stop-ProcessTree -RootProcessId ([int]$runtimeProcess.Id)
    }
    Start-Sleep -Milliseconds 500

    $cleanup = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @(
            @("healthy_viewer", $healthyViewer),
            @("crash_viewer", $crashViewer),
            @("runtime", $runtimeProcess))) {
        if ($entry[1]) {
            $remaining = Get-Process -Id ([int]$entry[1].Id) -ErrorAction SilentlyContinue
            $cleanup.Add(("{0}_remaining={1}" -f $entry[0], $(if ($remaining) { "pid_$($entry[1].Id)" } else { "none" })))
        }
    }
    if (Test-Path -LiteralPath $runtimeConfigPath) {
        Remove-Item -LiteralPath $runtimeConfigPath -Force
        $cleanup.Add("removed_unredacted_config=true")
    }
    $cleanup.Add("run_root=$runRoot")
    Set-Content -LiteralPath (Join-Path $runRoot "cleanup.txt") -Value $cleanup -Encoding UTF8
}

$planEvidenceDir = ".omo\evidence\rdp-codec-fps"
New-Item -ItemType Directory -Force -Path $planEvidenceDir | Out-Null
Copy-Item -LiteralPath (Join-Path $runRoot "summary.txt") -Destination (Join-Path $planEvidenceDir "task-4-rfx-threaded-multi-viewer-isolation.txt") -Force

Write-Host "Isolation artifacts: $runRoot"
