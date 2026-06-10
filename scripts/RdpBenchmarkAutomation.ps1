$script:RdpBenchmarkVirtualKeyMap = @{
    "ALT" = 0x12
    "CTRL" = 0x11
    "CONTROL" = 0x11
    "ENTER" = 0x0D
    "ESC" = 0x1B
    "ESCAPE" = 0x1B
    "SHIFT" = 0x10
    "TAB" = 0x09
    "WIN" = 0x5B
    "WINDOWS" = 0x5B
}

function Convert-RdpBenchmarkNativeKeyChord {
    param([string]$Chord)

    if ($null -eq $Chord -or $Chord.Trim().Length -eq 0) {
        throw "Native key chord cannot be empty."
    }

    $keys = New-Object System.Collections.Generic.List[int]
    foreach ($part in ($Chord -split '\+')) {
        $token = $part.Trim().ToUpperInvariant()
        if ($token.Length -eq 0) {
            throw "Native key chord '$Chord' contains an empty key token."
        }

        if ($script:RdpBenchmarkVirtualKeyMap.ContainsKey($token)) {
            $keys.Add([int]$script:RdpBenchmarkVirtualKeyMap[$token])
            continue
        }

        if ($token.Length -eq 1 -and $token[0] -ge [char]'A' -and $token[0] -le [char]'Z') {
            $keys.Add([int][char]$token[0])
            continue
        }

        if ($token.Length -eq 1 -and $token[0] -ge [char]'0' -and $token[0] -le [char]'9') {
            $keys.Add([int][char]$token[0])
            continue
        }

        if ($token -match '^F([1-9]|1[0-2])$') {
            $keys.Add(0x70 + ([int]$Matches[1]) - 1)
            continue
        }

        throw "Unsupported native key token '$part' in chord '$Chord'."
    }

    return $keys.ToArray()
}

function Invoke-RdpBenchmarkNativeKeyChord {
    param(
        [int[]]$VirtualKeys,
        [string]$ActionLogPath
    )

    if ($null -eq $VirtualKeys -or $VirtualKeys.Count -eq 0) {
        throw "Native key chord has no keys to send."
    }

    foreach ($key in $VirtualKeys) {
        [OmniRdpBenchmarkNative]::keybd_event([byte]$key, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 50
    }
    for ($i = $VirtualKeys.Count - 1; $i -ge 0; $i--) {
        [OmniRdpBenchmarkNative]::keybd_event([byte]$VirtualKeys[$i], 0, 0x0002, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 50
    }

    if ($ActionLogPath) {
        Add-Content -LiteralPath $ActionLogPath -Value ("{0:o} prep_nativekeys vk={1}" -f (Get-Date), ($VirtualKeys -join "+")) -Encoding UTF8
    }
}
