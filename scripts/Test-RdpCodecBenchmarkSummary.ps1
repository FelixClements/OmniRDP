$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "RdpCodecBenchmarkSummary.ps1")

$tempRoot = Join-Path $repoRoot ".omo\evidence\rfx-threaded-dirty-batching\summary-parser-fixture"
if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null

$logPath = Join-Path $tempRoot "viewer.log"
Set-Content -LiteralPath $logPath -Encoding UTF8 -Value @(
    "Viewer 1 RDPEGFX RFX dirty encode: generation=10 surface_id=0 rect=(0,0)-(63,63) payload_bytes=1000 encode_us=10",
    "Viewer 1 RDPEGFX dirty send complete: generation=10 frame_id=1 dirty_rects=2 dirty_area=8192 surface_commands=2 batch_attempted=true batch_used=false payload_bytes=2000 send_us=20",
    "Viewer 1 RDPEGFX RFX dirty encode: generation=11 surface_id=0 rect=(64,0)-(127,63) payload_bytes=3000 encode_us=30",
    "Viewer 1 RDPEGFX dirty send complete: generation=11 frame_id=2 dirty_rects=1 dirty_area=4096 surface_commands=1 batch_attempted=true batch_used=true payload_bytes=3000 send_us=40",
    "Viewer 1 RDPEGFX threshold full-frame dirty fallback reason=pending area threshold"
)

$metrics = Get-RdpCodecLogMetrics -LogRoot $tempRoot
if ($metrics.EncodeP50Us -ne 10) { throw "EncodeP50Us expected 10, got $($metrics.EncodeP50Us)" }
if ($metrics.EncodeP95Us -ne 30) { throw "EncodeP95Us expected 30, got $($metrics.EncodeP95Us)" }
if ($metrics.SendP50Us -ne 20) { throw "SendP50Us expected 20, got $($metrics.SendP50Us)" }
if ($metrics.SendP95Us -ne 40) { throw "SendP95Us expected 40, got $($metrics.SendP95Us)" }
if ($metrics.ShapedRectsP50 -ne 1) { throw "ShapedRectsP50 expected 1, got $($metrics.ShapedRectsP50)" }
if ($metrics.ShapedRectsP95 -ne 2) { throw "ShapedRectsP95 expected 2, got $($metrics.ShapedRectsP95)" }
if ($metrics.SurfaceCommandsP95 -ne 2) { throw "SurfaceCommandsP95 expected 2, got $($metrics.SurfaceCommandsP95)" }
if ($metrics.PayloadP95Bytes -ne 3000) { throw "PayloadP95Bytes expected 3000, got $($metrics.PayloadP95Bytes)" }
if ($metrics.FallbackDelta -ne 1) { throw "FallbackDelta expected 1, got $($metrics.FallbackDelta)" }
if ($metrics.BatchAttemptCount -ne 2) { throw "BatchAttemptCount expected 2, got $($metrics.BatchAttemptCount)" }
if ($metrics.BatchUsedCount -ne 1) { throw "BatchUsedCount expected 1, got $($metrics.BatchUsedCount)" }

$header = Format-RdpCodecMetricSummaryCsvHeader
foreach ($column in @("threaded_rfx", "encode_p50_us", "encode_p95_us", "send_p50_us", "send_p95_us", "shaped_rects_p50", "shaped_rects_p95", "surface_commands_p95", "payload_p95_bytes", "batch_attempt_count", "batch_used_count", "fallback_delta")) {
    if ($header -notmatch "(^|,)$([regex]::Escape($column))(,|$)") {
        throw "Missing summary column $column"
    }
}

Remove-Item -LiteralPath $tempRoot -Recurse -Force
Write-Host "RDP codec benchmark summary parser tests passed"
