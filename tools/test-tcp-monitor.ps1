[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18849,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18850
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$mockServer = Resolve-Path -LiteralPath 'tools\mock-health-server.py'
$config = Resolve-Path -LiteralPath 'examples\tcp-connect.example.json'
$python = Get-Command python -ErrorAction Stop
$token = 'pixelstatus-tcp-integration-test'
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
            throw 'Mock TCP listener exited before becoming ready'
        }
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$upstreamPort/health"
            $mockReady = $health.healthy -eq $true
        } catch {
        }
    }
    if (-not $mockReady) {
        throw "Mock TCP listener did not become ready on port $upstreamPort"
    }

    $arguments = @(
        $quotedConfig,
        '--run-for-ms', '10000',
        '--no-window',
        '--api-token', $token,
        '--api-port', $ApiPort.ToString(),
        '--web-display-port', $WebPort.ToString()
    )
    $simulatorProcess = Start-Process `
        -FilePath $simulator.Path `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -PassThru

    $headers = @{ Authorization = "Bearer $token" }
    $stateUri = "http://127.0.0.1:$ApiPort/api/v1/status/tcp-health"
    $state = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    while (-not $state -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before publishing the TCP monitor state'
        }
        try {
            $candidate = Invoke-RestMethod -Uri $stateUri -Headers $headers
            if ($candidate.status -eq 'ok' `
                -and $candidate.value -ge 0 `
                -and $candidate.message -match '^TCP connection established') {
                $state = $candidate
            }
        } catch {
        }
    }
    if (-not $state) {
        throw 'TCP-connect monitor did not publish the expected successful state'
    }

    Write-Output `
        "PixelStatus NX TCP monitor smoke test passed: id=$($state.id), status=$($state.status), latency=$($state.value)ms."
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
