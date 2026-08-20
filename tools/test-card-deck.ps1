[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18899,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18900
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$config = Resolve-Path -LiteralPath 'examples\card-deck.example.json'
$token = 'pixelstatus-card-deck-integration-test'
$quotedConfig = '"{0}"' -f $config.Path
$arguments = @(
    $quotedConfig,
    '--run-for-ms', '22000',
    '--no-window',
    '--api-token', $token,
    '--api-port', $ApiPort.ToString(),
    '--web-display-port', $WebPort.ToString(),
    '--web-refresh-ms', '33',
    '--monitor-workers', '4'
)
$simulatorProcess = Start-Process `
    -FilePath $simulator.Path `
    -ArgumentList $arguments `
    -WindowStyle Hidden `
    -PassThru

try {
    $baseUri = "http://127.0.0.1:$WebPort"
    $initialFrame = $null
    $logoBlue = 0x0050C8
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $initialFrame -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before the card-deck display became ready'
        }
        try {
            $candidate = Invoke-RestMethod -Uri "$baseUri/api/v1/display"
            if ($candidate.pixels.Count -eq 256 `
                -and $candidate.pixels[18] -eq $logoBlue) {
                $initialFrame = $candidate
            }
        } catch {
        }
    }
    if (-not $initialFrame -or $initialFrame.pixels.Count -ne 256) {
        throw 'Card-deck display did not publish a 16x16 initial frame'
    }

    $initialPixels = $initialFrame.pixels -join ','
    $changedFrame = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    while (-not $changedFrame -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before the deck advanced'
        }
        try {
            $candidate = Invoke-RestMethod -Uri "$baseUri/api/v1/display"
            if (($candidate.pixels -join ',') -ne $initialPixels) {
                $changedFrame = $candidate
            }
        } catch {
        }
    }
    if (-not $changedFrame) {
        throw 'Rendered frame did not change when the deck transition was due'
    }

    $headers = @{ Authorization = "Bearer $token" }
    foreach ($source in @('lan-dns', 'wan-web')) {
        $state = $null
        $lastCandidate = $null
        $deadline = [DateTime]::UtcNow.AddSeconds(8)
        while (-not $state -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
            if ($simulatorProcess.HasExited) {
                throw "Simulator exited before publishing $source"
            }
            try {
                $candidate = Invoke-RestMethod `
                    -Uri "http://127.0.0.1:$ApiPort/api/v1/status/$source" `
                    -Headers $headers
                $lastCandidate = $candidate
                if ($candidate.status -eq 'ok') {
                    $state = $candidate
                }
            } catch {
            }
        }
        if (-not $state) {
            if ($lastCandidate) {
                throw "$source ended as $($lastCandidate.status): $($lastCandidate.message)"
            }
            throw "$source did not publish a healthy state"
        }
    }

    $networkFrame = $null
    $healthyGreen = 0x00C853
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    while (-not $networkFrame -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before rendering the live network card'
        }
        try {
            $candidate = Invoke-RestMethod -Uri "$baseUri/api/v1/display"
            if ($candidate.pixels[0] -eq $healthyGreen `
                -and $candidate.pixels[15] -eq $healthyGreen) {
                $networkFrame = $candidate
            }
        } catch {
        }
    }
    if (-not $networkFrame) {
        throw 'Live network card did not render both healthy monitor regions'
    }

    Write-Output `
        "PixelStatus NX card-deck smoke test passed: logo advanced at frame $($changedFrame.sequence); live LAN/WAN card rendered at frame $($networkFrame.sequence)."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
