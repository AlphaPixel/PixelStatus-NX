[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18839,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18840
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$mockServer = Resolve-Path -LiteralPath 'tools\mock-health-server.py'
$config = Resolve-Path -LiteralPath 'examples\concurrent-monitors.example.json'
$python = Get-Command python -ErrorAction Stop
$token = 'pixelstatus-concurrency-integration-test'
$upstreamPort = 18080
$quotedMockServer = '"{0}"' -f $mockServer.Path
$quotedConfig = '"{0}"' -f $config.Path

$mockProcess = Start-Process `
    -FilePath $python.Source `
    -ArgumentList @($quotedMockServer, '--port', $upstreamPort.ToString(), '--slow-delay-ms', '2500') `
    -WindowStyle Hidden `
    -PassThru
$simulatorProcess = $null

try {
    $mockReady = $false
    $mockDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $mockReady -and [DateTime]::UtcNow -lt $mockDeadline) {
        Start-Sleep -Milliseconds 100
        if ($mockProcess.HasExited) {
            throw 'Mock HTTP health server exited before becoming ready'
        }
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$upstreamPort/health"
            $mockReady = $health.database.replication_lag -eq 12
        } catch {
        }
    }
    if (-not $mockReady) {
        throw "Mock HTTP health server did not become ready on port $upstreamPort"
    }

    $arguments = @(
        $quotedConfig,
        '--run-for-ms', '10000',
        '--no-window',
        '--api-token', $token,
        '--api-port', $ApiPort.ToString(),
        '--web-display-port', $WebPort.ToString(),
        '--monitor-workers', '2'
    )
    $simulatorProcess = Start-Process `
        -FilePath $simulator.Path `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -PassThru

    $slowStarted = $false
    $startDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $slowStarted -and [DateTime]::UtcNow -lt $startDeadline) {
        Start-Sleep -Milliseconds 50
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before starting the slow monitor request'
        }
        try {
            $fixtureStats = Invoke-RestMethod -Uri "http://127.0.0.1:$upstreamPort/stats"
            $slowStarted = $fixtureStats.slow_started -eq $true
        } catch {
        }
    }
    if (-not $slowStarted) {
        throw 'Simulator did not start the deliberately slow monitor request'
    }

    $headers = @{ Authorization = "Bearer $token" }
    $fastStateUri = "http://127.0.0.1:$ApiPort/api/v1/status/b-fast"
    $fastState = $null
    $fastDeadline = [DateTime]::UtcNow.AddMilliseconds(1500)
    while (-not $fastState -and [DateTime]::UtcNow -lt $fastDeadline) {
        Start-Sleep -Milliseconds 50
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before publishing the fast monitor state'
        }
        try {
            $candidate = Invoke-RestMethod -Uri $fastStateUri -Headers $headers
            if ($candidate.status -eq 'ok' -and $candidate.value -eq 2) {
                $fastState = $candidate
            }
        } catch {
        }
    }
    if (-not $fastState) {
        throw 'Fast monitor was blocked behind the deliberately slow monitor'
    }

    $frame = $null
    $frameDeadline = $fastDeadline
    while (-not $frame -and [DateTime]::UtcNow -lt $frameDeadline) {
        try {
            $frame = Invoke-RestMethod -Uri "http://127.0.0.1:$WebPort/api/v1/display"
        } catch {
            Start-Sleep -Milliseconds 50
        }
    }
    if (-not $frame) {
        throw 'Browser display did not respond during the slow monitor request'
    }
    if ($frame.width -ne 2 -or $frame.height -ne 1 -or $frame.pixels.Count -ne 2) {
        throw 'Browser display did not remain responsive during the slow monitor request'
    }

    $slowStateUri = "http://127.0.0.1:$ApiPort/api/v1/status/a-slow"
    $slowState = $null
    $slowDeadline = [DateTime]::UtcNow.AddSeconds(4)
    while (-not $slowState -and [DateTime]::UtcNow -lt $slowDeadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before publishing the slow monitor state'
        }
        try {
            $candidate = Invoke-RestMethod -Uri $slowStateUri -Headers $headers
            if ($candidate.status -eq 'ok' -and $candidate.value -eq 1) {
                $slowState = $candidate
            }
        } catch {
        }
    }
    if (-not $slowState) {
        throw 'Slow monitor did not complete after the configured fixture delay'
    }

    Write-Output `
        "PixelStatus NX concurrency smoke test passed: fast value=$($fastState.value), slow value=$($slowState.value), display=$($frame.width)x$($frame.height)."
} finally {
    if ($simulatorProcess -and -not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
    if (-not $mockProcess.HasExited) {
        Stop-Process -Id $mockProcess.Id
        $mockProcess.WaitForExit()
    }
}
