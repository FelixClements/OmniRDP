$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RdpBenchmarkAutomation.ps1")

$winE = Convert-RdpBenchmarkNativeKeyChord -Chord "WIN+E"
if (($winE -join ",") -ne "91,69") {
    throw "WIN+E expected 91,69, got $($winE -join ',')"
}

$ctrlShiftF5 = Convert-RdpBenchmarkNativeKeyChord -Chord "CTRL+SHIFT+F5"
if (($ctrlShiftF5 -join ",") -ne "17,16,116") {
    throw "CTRL+SHIFT+F5 expected 17,16,116, got $($ctrlShiftF5 -join ',')"
}

$threw = $false
try {
    [void](Convert-RdpBenchmarkNativeKeyChord -Chord "WIN+UNKNOWN")
} catch {
    $threw = ($_.Exception.Message -like "*Unsupported native key token*")
}
if (-not $threw) {
    throw "Unsupported key token did not throw the expected parser error."
}

Write-Host "RDP benchmark automation parser tests passed"
