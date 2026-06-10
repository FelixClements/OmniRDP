[CmdletBinding()]
param(
    [string]$Repo = "FelixClements/OmniRDP",
    [string]$InputPath,
    [ValidateSet("Auto", "GitHub", "Sarif")]
    [string]$InputKind = "Auto",
    [string]$OutputSarif = ".omo\evidence\flawfinder-local.sarif",
    [string]$BaselineCsv = ".omo\evidence\code-scanning-open-before.csv",
    [string[]]$RuleIds = @("FF1013", "FF1016", "FF1017", "FF1023", "FF1044"),
    [switch]$NoGitHubFallback
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

function Read-JsonFile {
    param([string]$Path)

    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    } catch {
        Write-Error "Failed to parse alert data from '$Path': $($_.Exception.Message)"
        exit 2
    }
}

function Get-SarifAlerts {
    param([object]$Sarif)

    $alerts = New-Object System.Collections.Generic.List[object]
    if (-not $Sarif.runs) {
        return $alerts
    }

    foreach ($run in $Sarif.runs) {
        foreach ($result in @($run.results)) {
            $ruleId = [string]$result.ruleId
            if ($RuleIds -notcontains $ruleId) {
                continue
            }

            $location = $result.locations[0].physicalLocation
            $path = [string]$location.artifactLocation.uri
            $line = [int]$location.region.startLine
            $message = [string]$result.message.text
            $alerts.Add([pscustomobject]@{
                RuleId = $ruleId
                Path = $path
                Line = $line
                Message = $message
                Source = "sarif"
            })
        }
    }

    return $alerts
}

function Get-GitHubAlerts {
    param([object[]]$Items)

    $alerts = New-Object System.Collections.Generic.List[object]
    foreach ($item in @($Items)) {
        $ruleId = [string]$item.rule.id
        if ($RuleIds -notcontains $ruleId) {
            continue
        }

        $location = $item.most_recent_instance.location
        $alerts.Add([pscustomobject]@{
            RuleId = $ruleId
            Path = [string]$location.path
            Line = [int]$location.start_line
            Message = [string]$item.most_recent_instance.message.text
            Source = "github"
            Number = [int]$item.number
            Url = [string]$item.html_url
        })
    }

    return $alerts
}

function Invoke-Flawfinder {
    param([string]$SarifPath)

    $flawfinder = Get-Command flawfinder -ErrorAction SilentlyContinue
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $flawfinder -and -not $python) {
        return $false
    }

    $fullSarifPath = if ([System.IO.Path]::IsPathRooted($SarifPath)) {
        $SarifPath
    } else {
        Join-Path $RepoRoot $SarifPath
    }
    $sarifDir = Split-Path -Parent $fullSarifPath
    if ($sarifDir) {
        New-Item -ItemType Directory -Force -Path $sarifDir | Out-Null
    }

    Push-Location $RepoRoot
    try {
        if ($flawfinder) {
            $env:PYTHONUTF8 = "1"
            & $flawfinder.Source --sarif --minlevel 3 OmniRDP/src OmniRDP/include OmniRDP/tests > $fullSarifPath
        } else {
            & $python.Source -X utf8 -m flawfinder --sarif --minlevel 3 OmniRDP/src OmniRDP/include OmniRDP/tests > $fullSarifPath
        }
    } finally {
        Pop-Location
    }

    return $true
}

$alerts = $null

if ($InputPath) {
    $fullInputPath = if ([System.IO.Path]::IsPathRooted($InputPath)) {
        $InputPath
    } else {
        Join-Path $RepoRoot $InputPath
    }
    $json = Read-JsonFile -Path $fullInputPath
    if ($InputKind -eq "Sarif" -or ($InputKind -eq "Auto" -and $json.runs)) {
        $alerts = Get-SarifAlerts -Sarif $json
    } else {
        $alerts = Get-GitHubAlerts -Items @($json)
    }
} else {
    if (Invoke-Flawfinder -SarifPath $OutputSarif) {
        $json = Read-JsonFile -Path (Join-Path $RepoRoot $OutputSarif)
        $alerts = Get-SarifAlerts -Sarif $json
    } elseif (-not $NoGitHubFallback) {
        $raw = & gh api --paginate "repos/$Repo/code-scanning/alerts?state=open&per_page=100"
        try {
            $json = $raw | ConvertFrom-Json
        } catch {
            Write-Error "Failed to parse GitHub Code Scanning alerts: $($_.Exception.Message)"
            exit 2
        }
        $alerts = Get-GitHubAlerts -Items @($json)
    } else {
        Write-Error "flawfinder is not installed and GitHub fallback is disabled."
        exit 2
    }
}

$sorted = @($alerts) | Sort-Object Path, Line, RuleId
$fullBaselineCsv = if ([System.IO.Path]::IsPathRooted($BaselineCsv)) {
    $BaselineCsv
} else {
    Join-Path $RepoRoot $BaselineCsv
}
if (Test-Path -LiteralPath $fullBaselineCsv -PathType Leaf) {
    $baseline = Import-Csv -LiteralPath $fullBaselineCsv
    $baselineKeys = @{}
    foreach ($item in $baseline) {
        $baselineKeys["$($item.rule)|$($item.path)|$($item.line)"] = $true
    }
    $sorted = @($sorted) | Where-Object {
        $baselineKeys.ContainsKey("$($_.RuleId)|$($_.Path)|$($_.Line)")
    } | Sort-Object Path, Line, RuleId
}

if ($sorted.Count -gt 0) {
    Write-Output "Open matching Code Scanning alerts: $($sorted.Count)"
    $sorted | Select-Object RuleId, Path, Line, Message, Number, Url | Format-Table -AutoSize | Out-String -Width 240 | Write-Output
    exit 1
}

Write-Output "Open matching Code Scanning alerts: 0"
exit 0
