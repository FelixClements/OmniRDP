[CmdletBinding()]
param(
    [string]$TargetExe,
    [string[]]$TargetArgs = @(),
    [string[]]$DrMemoryArgs = @("-batch"),
    [string]$DrMemoryRoot,
    [string]$InstallDir,
    [string]$LogDir,
    [string]$DownloadUrl = "https://github.com/DynamoRIO/drmemory/releases/download/cronbuild-2.6.20434/DrMemory-Windows-2.6.20434.zip",
    [switch]$SkipDownload
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $InstallDir) {
    $InstallDir = Join-Path $RepoRoot "artifacts\drmemory-20434"
}

if (-not $LogDir) {
    $LogDir = Join-Path $RepoRoot "artifacts\drmemory-20434\logs"
}

if (-not $TargetExe) {
    $TargetExe = Join-Path $RepoRoot "OmniRDP\build\tests\Debug\test_viewer_state.exe"
}

function Get-DrMemoryCandidates {
    param([string]$Root)

    @(
        (Join-Path $Root "bin64\drmemory.exe"),
        (Join-Path $Root "bin\drmemory.exe"),
        (Join-Path $Root "drmemory.exe")
    )
}

function Resolve-DrMemoryExe {
    param(
        [string]$Root,
        [string]$InstallPath
    )

    if ($Root) {
        foreach ($candidate in (Get-DrMemoryCandidates -Root $Root)) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }

        throw "Could not find drmemory.exe under '$Root'."
    }

    if (Test-Path -LiteralPath $InstallPath) {
        $local = Get-ChildItem -LiteralPath $InstallPath -Recurse -Filter drmemory.exe -File |
            Where-Object { $_.FullName -match "[\\/]bin64?[\\/]drmemory\.exe$" } |
            Sort-Object @{ Expression = { if ($_.FullName -match "[\\/]bin64[\\/]") { 0 } else { 1 } } }, FullName |
            Select-Object -First 1

        if ($local) {
            return $local.FullName
        }
    }

    $command = Get-Command drmemory.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    return $null
}

function Install-DrMemoryZip {
    param(
        [string]$Url,
        [string]$Destination
    )

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null

    $zipPath = Join-Path $Destination (Split-Path -Leaf ([Uri]$Url).AbsolutePath)
    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        Write-Host "Downloading Dr. Memory from $Url"
        Invoke-WebRequest -Uri $Url -OutFile $zipPath
    }

    Write-Host "Extracting Dr. Memory into $Destination"
    Expand-Archive -LiteralPath $zipPath -DestinationPath $Destination -Force
}

if (-not (Test-Path -LiteralPath $TargetExe -PathType Leaf)) {
    throw "Target executable not found: $TargetExe. Build Debug first or pass -TargetExe."
}

$TargetExe = (Resolve-Path -LiteralPath $TargetExe).Path
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogDir = (Resolve-Path -LiteralPath $LogDir).Path

$drMemoryExe = Resolve-DrMemoryExe -Root $DrMemoryRoot -InstallPath $InstallDir
if (-not $drMemoryExe) {
    if ($SkipDownload) {
        throw "drmemory.exe was not found on PATH or under '$InstallDir', and -SkipDownload was supplied."
    }

    Install-DrMemoryZip -Url $DownloadUrl -Destination $InstallDir
    $drMemoryExe = Resolve-DrMemoryExe -Root $DrMemoryRoot -InstallPath $InstallDir
}

if (-not $drMemoryExe) {
    throw "Dr. Memory installation completed, but drmemory.exe was not found under '$InstallDir'."
}

$runStamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stdoutPath = Join-Path $LogDir "drmemory-$runStamp.stdout.txt"
$stderrPath = Join-Path $LogDir "drmemory-$runStamp.stderr.txt"
$arguments = @($DrMemoryArgs + @("-logdir", $LogDir, "--", $TargetExe) + $TargetArgs)

Write-Host "Dr. Memory: $drMemoryExe"
Write-Host "Target:     $TargetExe"
Write-Host "Log dir:    $LogDir"
Write-Host "Command:    `"$drMemoryExe`" $($arguments -join ' ')"

$process = Start-Process -FilePath $drMemoryExe -ArgumentList $arguments -NoNewWindow -Wait -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
$exitCode = $process.ExitCode

Write-Host "Dr. Memory process exit code: $exitCode"
Write-Host "stdout: $stdoutPath"
Write-Host "stderr: $stderrPath"

$latestResults = Get-ChildItem -LiteralPath $LogDir -Recurse -Filter results.txt -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $latestResults) {
    Write-Warning "No results.txt file was found under $LogDir. See stdout/stderr above."
    if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
        Get-Content -LiteralPath $stderrPath -Tail 40 | ForEach-Object { Write-Host $_ }
    }
    exit $exitCode
}

Write-Host "results: $($latestResults.FullName)"
Write-Host ""
Write-Host "Dr. Memory summary:"
$summaryLines = Get-Content -LiteralPath $latestResults.FullName |
    Where-Object {
        $_ -match "^Dr\. Memory" -or
        $_ -match "^~~Dr\.M~~" -or
        $_ -match "^ERRORS FOUND:" -or
        $_ -match "^ERRORS IGNORED:" -or
        $_ -match "^ *[0-9]+ .*error" -or
        $_ -match "^ *[0-9]+ .*leak" -or
        $_ -match "^ *[0-9]+ .*warning" -or
        $_ -match "^NO ERRORS FOUND"
    }

if ($summaryLines) {
    $summaryLines | ForEach-Object { Write-Host $_ }
} else {
    Get-Content -LiteralPath $latestResults.FullName -Tail 40 | ForEach-Object { Write-Host $_ }
}

if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
    $stderrText = Get-Content -LiteralPath $stderrPath -Raw
    if ($stderrText -match "Dr\. Memory internal crash|failed to start the target application|Unable to load client library") {
        Write-Host ""
        Write-Warning "Dr. Memory reported a tool/runtime startup failure. Recent stderr follows."
        Get-Content -LiteralPath $stderrPath -Tail 40 | ForEach-Object { Write-Host $_ }
    }
}

exit $exitCode
