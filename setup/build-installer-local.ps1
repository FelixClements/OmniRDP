param(
    [switch]$SkipClean
)

$ErrorActionPreference = 'Stop'

function Get-ScriptDirectory {
    if ($PSScriptRoot) {
        return $PSScriptRoot
    }

    return Split-Path -Parent $PSCommandPath
}

function Format-PathList {
    param([string[]]$Paths)

    return (($Paths | Select-Object -Unique | ForEach-Object { "  - $_" }) -join [Environment]::NewLine)
}

function Resolve-FirstExistingFile {
    param(
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string[]]$Directories
    )

    foreach ($directory in $Directories) {
        if ([string]::IsNullOrWhiteSpace($directory)) {
            continue
        }

        $candidate = Join-Path $directory $FileName
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Get-Item -LiteralPath $candidate).FullName
        }
    }

    return $null
}

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string[]]$CandidateDirectories,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory
    )

    $source = Resolve-FirstExistingFile -FileName $FileName -Directories $CandidateDirectories
    if (-not $source) {
        return [pscustomobject]@{
            FileName             = $FileName
            Found                = $false
            CandidateDirectories = $CandidateDirectories
        }
    }

    Copy-Item -LiteralPath $source -Destination (Join-Path $DestinationDirectory $FileName) -Force
    Write-Host "Staged $FileName from $source"

    return [pscustomobject]@{
        FileName             = $FileName
        Found                = $true
        Source               = $source
        CandidateDirectories = $CandidateDirectories
    }
}

function Get-PathDirectories {
    return (($env:Path -split ';') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Get-VcpkgBinDirectoriesFromCMakeCache {
    param([string[]]$CachePaths)

    $result = @()

    foreach ($cachePath in $CachePaths) {
        if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
            continue
        }

        $installedDirs = @()
        $triplets = @()

        foreach ($line in Get-Content -LiteralPath $cachePath) {
            if ($line -match '^_?VCPKG_INSTALLED_DIR:[^=]*=(.+)$') {
                $installedDirs += $Matches[1].Trim().Trim('"')
            }
            elseif ($line -match '^VCPKG_TARGET_TRIPLET:[^=]*=(.+)$') {
                $triplets += $Matches[1].Trim().Trim('"')
            }
        }

        if (($installedDirs.Count -gt 0) -and ($triplets.Count -eq 0)) {
            $triplets += 'x64-windows'
        }

        foreach ($installedDir in ($installedDirs | Select-Object -Unique)) {
            if ([string]::IsNullOrWhiteSpace($installedDir)) {
                continue
            }

            foreach ($triplet in ($triplets | Select-Object -Unique)) {
                if ([string]::IsNullOrWhiteSpace($triplet)) {
                    continue
                }

                $result += Join-Path (Join-Path $installedDir $triplet) 'bin'
            }
        }
    }

    return ($result | Select-Object -Unique)
}

$scriptDir = Get-ScriptDirectory
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..')).Path
$setupDir = Join-Path $repoRoot 'setup'
$issPath = Join-Path $setupDir 'OmniRDP.iss'

if (-not (Test-Path -LiteralPath $issPath -PathType Leaf)) {
    throw "Unable to locate setup\OmniRDP.iss from script path '$scriptDir'. Expected repo root: '$repoRoot'."
}

$artifactBin = Join-Path $repoRoot 'artifacts\windows-Release\bin'

# ── Build OmniRDP Release ─────────────────────────────────────────
$omniBuildDir = Join-Path $repoRoot 'OmniRDP\build'
if (Test-Path -LiteralPath $omniBuildDir -PathType Container) {
    Write-Host "Building OmniRDP Release..."
    & cmake --build $omniBuildDir --config Release -j
    if ($LASTEXITCODE -ne 0) {
        throw "OmniRDP Release build failed with exit code $LASTEXITCODE."
    }
}
else {
    Write-Host "WARNING: OmniRDP build directory not found at '$omniBuildDir', skipping build." -ForegroundColor Yellow
}

# ── Stage artifacts ────────────────────────────────────────────────
if ((Test-Path -LiteralPath $artifactBin) -and -not $SkipClean) {
    Remove-Item -LiteralPath $artifactBin -Recurse -Force
}

New-Item -ItemType Directory -Path $artifactBin -Force | Out-Null

$omniReleaseDirs = @(
    (Join-Path $repoRoot 'OmniRDP\build\Release'),
    (Join-Path $repoRoot 'OmniRDP\build-release\Release')
)

$freerdpRoot = Join-Path $repoRoot 'freerdp-3.26.0'
$freerdpDllDirs = @{
    'freerdp3.dll'        = @(
        (Join-Path $freerdpRoot 'build\libfreerdp\Release'),
        (Join-Path $freerdpRoot 'build-release\libfreerdp\Release')
    )
    'freerdp-client3.dll' = @(
        (Join-Path $freerdpRoot 'build\client\common\Release'),
        (Join-Path $freerdpRoot 'build-release\client\common\Release')
    )
    'freerdp-server3.dll' = @(
        (Join-Path $freerdpRoot 'build\server\common\Release'),
        (Join-Path $freerdpRoot 'build-release\server\common\Release')
    )
    'winpr3.dll'          = @(
        (Join-Path $freerdpRoot 'build\winpr\libwinpr\Release'),
        (Join-Path $freerdpRoot 'build-release\winpr\libwinpr\Release')
    )
}

$pathDirs = Get-PathDirectories
$vcpkgCacheDirs = Get-VcpkgBinDirectoriesFromCMakeCache -CachePaths @(
    (Join-Path $freerdpRoot 'build\CMakeCache.txt'),
    (Join-Path $freerdpRoot 'build-release\CMakeCache.txt')
)
$runtimeDirs = @(
    (Join-Path $repoRoot 'vcpkg\installed\x64-windows\bin'),
    (Join-Path $repoRoot 'vcpkg_installed\x64-windows\bin'),
    $vcpkgCacheDirs,
    'C:\tools\vcpkg\installed\x64-windows\bin',
    (Join-Path $freerdpRoot 'build\install\bin'),
    (Join-Path $freerdpRoot 'build-release\install\bin'),
    (Join-Path $freerdpRoot 'install\bin'),
    (Join-Path $freerdpRoot 'build\bin\Release'),
    (Join-Path $freerdpRoot 'build-release\bin\Release'),
    (Join-Path $freerdpRoot 'build\Release'),
    (Join-Path $freerdpRoot 'build-release\Release')
) + $pathDirs

$copyResults = @()

foreach ($exe in @('OmniRDP.exe', 'OmniRDP-svc.exe', 'OmniRDP-tray.exe')) {
    $copyResults += Copy-RequiredFile -FileName $exe -CandidateDirectories $omniReleaseDirs -DestinationDirectory $artifactBin
}

foreach ($dll in @('freerdp3.dll', 'freerdp-client3.dll', 'freerdp-server3.dll', 'winpr3.dll')) {
    $copyResults += Copy-RequiredFile -FileName $dll -CandidateDirectories $freerdpDllDirs[$dll] -DestinationDirectory $artifactBin
}

foreach ($dll in @('libcrypto-3-x64.dll', 'libssl-3-x64.dll', 'libusb-1.0.dll', 'zlib1.dll')) {
    $copyResults += Copy-RequiredFile -FileName $dll -CandidateDirectories $runtimeDirs -DestinationDirectory $artifactBin
}

$requiredPayload = @(
    'OmniRDP.exe',
    'OmniRDP-svc.exe',
    'OmniRDP-tray.exe',
    'freerdp3.dll',
    'freerdp-client3.dll',
    'freerdp-server3.dll',
    'winpr3.dll',
    'libcrypto-3-x64.dll',
    'libssl-3-x64.dll',
    'libusb-1.0.dll',
    'zlib1.dll'
)

$missing = @()
foreach ($file in $requiredPayload) {
    if (-not (Test-Path -LiteralPath (Join-Path $artifactBin $file) -PathType Leaf)) {
        $missing += $file
    }
}

if ($missing.Count -gt 0) {
    Write-Host "ERROR: Missing required installer payload file(s) in '$artifactBin': $($missing -join ', ')" -ForegroundColor Red
    foreach ($file in $missing) {
        $result = $copyResults | Where-Object { $_.FileName -eq $file } | Select-Object -First 1
        Write-Host "Candidate locations for ${file}:"
        Write-Host (Format-PathList -Paths $result.CandidateDirectories)
    }
    exit 1
}

function Resolve-Iscc {
    $command = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    if (-not $command) {
        $command = Get-Command 'iscc.exe' -ErrorAction SilentlyContinue
    }
    if ($command) {
        return $command.Source
    }

    $standardPaths = @(
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe'
    )

    foreach ($path in $standardPaths) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            return $path
        }
    }

    return $null
}

$iscc = Resolve-Iscc
if (-not $iscc) {
    throw "Unable to locate ISCC.exe. Add Inno Setup 6 to PATH or install it in C:\Program Files (x86)\Inno Setup 6 or C:\Program Files\Inno Setup 6."
}

Write-Host "Running Inno Setup: $iscc $issPath"
Push-Location $repoRoot
try {
    & $iscc $issPath
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC.exe failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$installerPath = Join-Path $setupDir 'Output\OmniRDP-Setup.exe'
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "ISCC.exe completed, but expected installer was not found: $installerPath"
}

Write-Host "Installer built successfully: $installerPath"
