[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18789
)

$ErrorActionPreference = 'Stop'

$simulator = Join-Path $BuildDirectory 'pixelstatus_simulator.exe'
if (-not (Test-Path -LiteralPath $simulator -PathType Leaf)) {
    throw "Simulator executable not found: $simulator"
}
$mockServer = Resolve-Path -LiteralPath 'tools\mock-health-server.py'
$config = Resolve-Path -LiteralPath 'examples\http-monitor.example.json'
$python = Get-Command python -ErrorAction Stop
$token = 'pixelstatus-monitor-integration-test'
$upstreamPort = 18080
$quotedMockServer = '"{0}"' -f $mockServer.Path
$quotedConfig = '"{0}"' -f $config.Path

$mockProcess = Start-Process `
    -FilePath $python.Source `
    -ArgumentList @($quotedMockServer, '--port', $upstreamPort.ToString()) `
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
        '--api-token', $token,
        '--api-port', $ApiPort.ToString(),
        '--no-web-display'
    )
    $simulatorProcess = Start-Process `
        -FilePath (Resolve-Path -LiteralPath $simulator) `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -PassThru

    $headers = @{ Authorization = "Bearer $token" }
    $stateUri = "http://127.0.0.1:$ApiPort/api/v1/status/replication-lag"
    $matched = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    while (-not $matched -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before publishing the configured monitor state'
        }
        try {
            $state = Invoke-RestMethod -Uri $stateUri -Headers $headers
            $matched = $state.status -eq 'ok' -and $state.value -eq 12
        } catch {
        }
    }
    if (-not $matched) {
        throw 'Configured HTTP monitor did not publish the expected state'
    }

    Write-Output `
        "PixelStatus NX monitor smoke test passed: id=$($state.id), status=$($state.status), value=$($state.value)."
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
