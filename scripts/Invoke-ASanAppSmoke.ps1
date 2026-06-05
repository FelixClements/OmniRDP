[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$FreeRdpBuildDir,
    [string]$Configuration = "Debug",
    [string]$ArtifactDir,
    [int]$TimeoutSeconds = 20,
    [switch]$DetectStackUseAfterReturn,
    [switch]$NoDump
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot "OmniRDP\build-asan"
}
if (-not $FreeRdpBuildDir) {
    $FreeRdpBuildDir = Join-Path $RepoRoot "freerdp-3.26.0\build-debug"
    if (-not (Test-Path -LiteralPath $FreeRdpBuildDir)) {
        $FreeRdpBuildDir = Join-Path $RepoRoot "freerdp-3.26.0\build"
    }
}
if (-not $ArtifactDir) {
    $ArtifactDir = Join-Path $RepoRoot "artifacts\asan\app-smoke"
}

function Resolve-AsanRuntimeDir {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $searchRoots = @()

    if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
        $vsPaths = & $vsWhere -products * -requires Microsoft.VisualStudio.Component.VC.ASAN -property installationPath 2>$null
        if (-not $vsPaths) {
            $vsPaths = & $vsWhere -latest -property installationPath 2>$null
        }
        foreach ($path in $vsPaths) {
            if ($path) { $searchRoots += (Join-Path $path "VC\Tools\MSVC") }
        }
    }

    $fallback = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio"
    if (Test-Path -LiteralPath $fallback) { $searchRoots += $fallback }

    foreach ($root in ($searchRoots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $dll = Get-ChildItem -LiteralPath $root -Recurse -Filter "clang_rt.asan_dynamic-x86_64.dll" -File -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($dll) { return $dll.DirectoryName }
    }

    throw "Unable to locate clang_rt.asan_dynamic-x86_64.dll. Install MSVC AddressSanitizer support in Visual Studio Build Tools."
}

function Add-ExistingPath {
    param(
        [System.Collections.Generic.List[string]]$Paths,
        [string]$Path
    )
    if ($Path -and (Test-Path -LiteralPath $Path -PathType Container)) {
        $resolved = (Resolve-Path -LiteralPath $Path).Path
        if (-not $Paths.Contains($resolved)) { [void]$Paths.Add($resolved) }
    }
}

function Invoke-SmokeTarget {
    param(
        [string]$Name,
        [string]$ExePath,
        [string[]]$Arguments,
        [int[]]$ExpectedExitCodes,
        [string]$OutputDir,
        [int]$Timeout
    )

    if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        throw "Missing ASan executable for ${Name}: $ExePath"
    }

    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    $stdout = Join-Path $OutputDir "$Name.stdout.txt"
    $stderr = Join-Path $OutputDir "$Name.stderr.txt"
    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $OutputDir -Filter "$Name.asan*.dmp" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    if (-not $NoDump) {
        $env:ASAN_SAVE_DUMPS = Join-Path $OutputDir "$Name.asan.dmp"
    }

    Write-Host "Running $Name under ASan: $ExePath $($Arguments -join ' ')"
    $processInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $processInfo.FileName = $ExePath
    $processInfo.WorkingDirectory = Split-Path -Parent $ExePath
    $processInfo.UseShellExecute = $false
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    if ($Arguments.Count -gt 0) {
        $processInfo.Arguments = (($Arguments | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' ')
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $processInfo
    [void]$process.Start()
    if (-not $process.WaitForExit($Timeout * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "$Name timed out after $Timeout seconds. Output: $OutputDir"
    }

    $stdoutText = $process.StandardOutput.ReadToEnd()
    $stderrText = $process.StandardError.ReadToEnd()
    Set-Content -LiteralPath $stdout -Value $stdoutText -NoNewline
    Set-Content -LiteralPath $stderr -Value $stderrText -NoNewline
    $exitCode = $process.ExitCode
    $combined = "$stdoutText`n$stderrText"

    if ($combined -match "ERROR: AddressSanitizer|AddressSanitizer:|==\d+==ERROR") {
        Write-Host $combined
        throw "$Name reported an AddressSanitizer error. Output: $OutputDir"
    }

    if ($ExpectedExitCodes -notcontains $exitCode) {
        Write-Host $combined
        throw "$Name exited with $exitCode; expected one of: $($ExpectedExitCodes -join ', '). Output: $OutputDir"
    }

    Write-Host "$Name completed with expected exit code $exitCode. Output: $OutputDir"
}

$buildRoot = (Resolve-Path -LiteralPath $BuildDir).Path
$artifactRoot = New-Item -ItemType Directory -Force -Path $ArtifactDir
$artifactRoot = $artifactRoot.FullName
$asanRuntimeDir = Resolve-AsanRuntimeDir

$pathEntries = [System.Collections.Generic.List[string]]::new()
Add-ExistingPath $pathEntries $asanRuntimeDir
Add-ExistingPath $pathEntries (Join-Path $buildRoot $Configuration)
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "libfreerdp\$Configuration")
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "winpr\libwinpr\$Configuration")
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "client\common\$Configuration")
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "server\common\$Configuration")
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "libfreerdp\Release")
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "winpr\libwinpr\Release")
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "client\common\Release")
Add-ExistingPath $pathEntries (Join-Path $FreeRdpBuildDir "server\common\Release")
Add-ExistingPath $pathEntries (Join-Path $RepoRoot "artifacts\windows-Release\bin")
Add-ExistingPath $pathEntries (Join-Path $RepoRoot "OmniRDP\build\Release")

$env:PATH = (($pathEntries.ToArray() + @($env:PATH)) -join ";")
$asanOptions = @("halt_on_error=1", "windows_fast_fail_on_error=0")
if ($DetectStackUseAfterReturn) { $asanOptions += "detect_stack_use_after_return=1" }
$env:ASAN_OPTIONS = ($asanOptions -join ":")

Write-Host "ASan runtime: $asanRuntimeDir"
Write-Host "Build dir:    $buildRoot"
Write-Host "Artifacts:    $artifactRoot"

$binDir = Join-Path $buildRoot $Configuration
Invoke-SmokeTarget -Name "OmniRDP" -ExePath (Join-Path $binDir "OmniRDP.exe") -Arguments @() -ExpectedExitCodes @(1) -OutputDir (Join-Path $artifactRoot "OmniRDP") -Timeout $TimeoutSeconds
Invoke-SmokeTarget -Name "OmniRDP-svc" -ExePath (Join-Path $binDir "OmniRDP-svc.exe") -Arguments @("--help") -ExpectedExitCodes @(0) -OutputDir (Join-Path $artifactRoot "OmniRDP-svc") -Timeout $TimeoutSeconds
Invoke-SmokeTarget -Name "OmniRDP-tray" -ExePath (Join-Path $binDir "OmniRDP-tray.exe") -Arguments @("--help") -ExpectedExitCodes @(0) -OutputDir (Join-Path $artifactRoot "OmniRDP-tray") -Timeout $TimeoutSeconds

Write-Host "ASan app smoke checks completed."
