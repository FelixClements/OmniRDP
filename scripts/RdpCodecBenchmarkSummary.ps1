function Get-RdpCodecPercentile {
    param(
        [long[]]$Values,
        [double]$Percentile
    )

    if (-not $Values -or $Values.Count -eq 0) {
        return ""
    }

    $sorted = @($Values | Sort-Object)
    $index = [Math]::Ceiling($Percentile * $sorted.Count) - 1
    if ($index -lt 0) {
        $index = 0
    }
    if ($index -ge $sorted.Count) {
        $index = $sorted.Count - 1
    }
    return $sorted[$index]
}

function Get-RdpCodecLogMetrics {
    param([string]$LogRoot)

    $encodeUs = New-Object System.Collections.Generic.List[long]
    $sendUs = New-Object System.Collections.Generic.List[long]
    $shapedRects = New-Object System.Collections.Generic.List[long]
    $surfaceCommands = New-Object System.Collections.Generic.List[long]
    $payloadBytes = New-Object System.Collections.Generic.List[long]
    $batchAttemptCount = 0
    $batchUsedCount = 0
    $fallbackDelta = 0

    if ($LogRoot -and (Test-Path -LiteralPath $LogRoot)) {
        $files = Get-ChildItem -LiteralPath $LogRoot -Recurse -File -ErrorAction SilentlyContinue
        foreach ($file in $files) {
            foreach ($line in (Get-Content -LiteralPath $file.FullName -ErrorAction SilentlyContinue)) {
                if ($line -match 'RFX dirty encode:.*encode_us=(\d+)') {
                    [void]$encodeUs.Add([long]$Matches[1])
                }
                if ($line -match 'RDPEGFX dirty send complete:.*dirty_rects=(\d+).*surface_commands=(\d+).*payload_bytes=(\d+).*send_us=(\d+)') {
                    [void]$shapedRects.Add([long]$Matches[1])
                    [void]$surfaceCommands.Add([long]$Matches[2])
                    [void]$payloadBytes.Add([long]$Matches[3])
                    [void]$sendUs.Add([long]$Matches[4])
                } elseif ($line -match 'RDPEGFX dirty send complete:.*payload_bytes=(\d+).*send_us=(\d+)') {
                    [void]$payloadBytes.Add([long]$Matches[1])
                    [void]$sendUs.Add([long]$Matches[2])
                }
                if ($line -match 'RDPEGFX dirty send complete:.*batch_attempted=true') {
                    $batchAttemptCount++
                }
                if ($line -match 'RDPEGFX dirty send complete:.*batch_used=true') {
                    $batchUsedCount++
                }
                if ($line -match 'full-frame dirty fallback|pending area threshold|pending rectangle count threshold|consecutive deferred dirty sends') {
                    $fallbackDelta++
                }
            }
        }
    }

    [pscustomobject]@{
        EncodeP50Us = Get-RdpCodecPercentile -Values $encodeUs.ToArray() -Percentile 0.50
        EncodeP95Us = Get-RdpCodecPercentile -Values $encodeUs.ToArray() -Percentile 0.95
        SendP50Us = Get-RdpCodecPercentile -Values $sendUs.ToArray() -Percentile 0.50
        SendP95Us = Get-RdpCodecPercentile -Values $sendUs.ToArray() -Percentile 0.95
        ShapedRectsP50 = Get-RdpCodecPercentile -Values $shapedRects.ToArray() -Percentile 0.50
        ShapedRectsP95 = Get-RdpCodecPercentile -Values $shapedRects.ToArray() -Percentile 0.95
        SurfaceCommandsP50 = Get-RdpCodecPercentile -Values $surfaceCommands.ToArray() -Percentile 0.50
        SurfaceCommandsP95 = Get-RdpCodecPercentile -Values $surfaceCommands.ToArray() -Percentile 0.95
        PayloadP50Bytes = Get-RdpCodecPercentile -Values $payloadBytes.ToArray() -Percentile 0.50
        PayloadP95Bytes = Get-RdpCodecPercentile -Values $payloadBytes.ToArray() -Percentile 0.95
        BatchAttemptCount = $batchAttemptCount
        BatchUsedCount = $batchUsedCount
        FallbackDelta = $fallbackDelta
    }
}

function Format-RdpCodecMetricSummaryCsvHeader {
    return "row,status,start,end,threaded_rfx,encode_p50_us,encode_p95_us,send_p50_us,send_p95_us,shaped_rects_p50,shaped_rects_p95,surface_commands_p50,surface_commands_p95,payload_p50_bytes,payload_p95_bytes,batch_attempt_count,batch_used_count,fallback_delta,notes"
}

function Format-RdpCodecMetricSummaryCsvRow {
    param(
        [string]$Row,
        [string]$Status,
        [datetime]$Start,
        [datetime]$End,
        [string]$ThreadedRfx,
        [object]$Metrics,
        [string]$Notes
    )

    if (-not $Metrics) {
        $Metrics = [pscustomobject]@{
            EncodeP50Us = ""
            EncodeP95Us = ""
            SendP50Us = ""
            SendP95Us = ""
            ShapedRectsP50 = ""
            ShapedRectsP95 = ""
            SurfaceCommandsP50 = ""
            SurfaceCommandsP95 = ""
            PayloadP50Bytes = ""
            PayloadP95Bytes = ""
            BatchAttemptCount = ""
            BatchUsedCount = ""
            FallbackDelta = ""
        }
    }

    $escapedNotes = ($Notes -replace '"', '""')
    return ('{0},{1},{2:o},{3:o},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},"{18}"' -f
        $Row, $Status, $Start, $End, $ThreadedRfx,
        $Metrics.EncodeP50Us, $Metrics.EncodeP95Us,
        $Metrics.SendP50Us, $Metrics.SendP95Us,
        $Metrics.ShapedRectsP50, $Metrics.ShapedRectsP95,
        $Metrics.SurfaceCommandsP50, $Metrics.SurfaceCommandsP95,
        $Metrics.PayloadP50Bytes, $Metrics.PayloadP95Bytes,
        $Metrics.BatchAttemptCount, $Metrics.BatchUsedCount,
        $Metrics.FallbackDelta, $escapedNotes)
}
