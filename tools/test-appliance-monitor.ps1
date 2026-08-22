[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18939,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18940
)

$ErrorActionPreference = 'Stop'
$UpstreamPort = 18080

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$mockServer = Resolve-Path -LiteralPath 'tools\mock-health-server.py'
$config = Resolve-Path -LiteralPath 'examples\appliance-monitor.example.json'
$python = Get-Command python -ErrorAction Stop
$token = 'pixelstatus-appliance-integration-test'
$secretVariables = @{
    PIXELSTATUS_SECRET_TRUENAS_API_KEY = 'fixture-truenas-token'
    PIXELSTATUS_SECRET_UNIFI_API_KEY = 'fixture-unifi-token'
}
$previousSecrets = @{}
foreach ($entry in $secretVariables.GetEnumerator()) {
    $previousSecrets[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, 'Process')
    [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
}

$quotedMockServer = '"{0}"' -f $mockServer.Path
$quotedConfig = '"{0}"' -f $config.Path
$mockProcess = Start-Process `
    -FilePath $python.Source `
    -ArgumentList @($quotedMockServer, '--port', $UpstreamPort.ToString()) `
    -WindowStyle Hidden `
    -PassThru
$simulatorProcess = $null

try {
    $mockReady = $false
    $mockDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $mockReady -and [DateTime]::UtcNow -lt $mockDeadline) {
        Start-Sleep -Milliseconds 100
        if ($mockProcess.HasExited) {
            throw 'Mock appliance endpoint exited before becoming ready'
        }
        try {
            $headers = @{Authorization = 'Bearer fixture-truenas-token'}
            $health = Invoke-RestMethod `
                -Uri "http://127.0.0.1:$UpstreamPort/appliance/truenas" `
                -Headers $headers
            $mockReady = $health.pools[0].status -eq 'ONLINE'
        } catch {
        }
    }
    if (-not $mockReady) {
        throw "Mock appliance endpoint did not become ready on port $UpstreamPort"
    }

    $arguments = @(
        $quotedConfig,
        '--run-for-ms', '12000',
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

    $apiHeaders = @{Authorization = "Bearer $token"}
    $statusUri = "http://127.0.0.1:$ApiPort/api/v1/status"
    $displayUri = "http://127.0.0.1:$WebPort/api/v1/display"
    $states = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    while (-not $states -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before publishing appliance states'
        }
        try {
            $candidate = Invoke-RestMethod -Uri $statusUri -Headers $apiHeaders
            if ($candidate.statuses.Count -eq 5) {
                $byId = @{}
                foreach ($state in $candidate.statuses) {
                    $byId[$state.id] = $state
                }
                if ($byId['truenas-pool-health'].status -eq 'ok' `
                    -and $byId['truenas-pool-health'].value -eq 'ONLINE' `
                    -and $byId['truenas-alert-count'].status -eq 'warn' `
                    -and $byId['truenas-alert-count'].value -eq 1 `
                    -and $byId['truenas-storage-used'].status -eq 'warn' `
                    -and $byId['truenas-storage-used'].value -eq 75 `
                    -and $byId['unifi-device-state'].status -eq 'ok' `
                    -and $byId['unifi-device-state'].value -eq 'ONLINE' `
                    -and $byId['unifi-wan-utilization'].status -eq 'ok' `
                    -and $byId['unifi-wan-utilization'].value -eq 30) {
                    $states = $candidate.statuses
                }
            }
        } catch {
        }
    }
    if (-not $states) {
        throw 'Appliance monitors did not publish the five expected values and statuses'
    }

    $frame = Invoke-RestMethod -Uri $displayUri
    if ($frame.pixels.Count -ne 256 `
        -or $frame.pixels[0] -ne 0x00C853 `
        -or $frame.pixels[64] -ne 0xFFD600 `
        -or $frame.pixels[36] -ne 0xFFD600 `
        -or $frame.pixels[104] -ne 0x00C853 `
        -or $frame.pixels[44] -ne 0x02050A `
        -or $frame.pixels[108] -ne 0x00C853 `
        -or -not ($frame.pixels[144..255] -contains 0xFFD600)) {
        throw 'Appliance states did not render into the expected grid, bars, indicator, and UTC clock'
    }

    Write-Output (
        'PixelStatus NX appliance smoke test passed: ' +
        'pool=ONLINE/ok, alerts=1/warn, storage=75%/warn, ' +
        'UniFi=ONLINE/ok, WAN=30%/ok; browser frame verified.'
    )
} finally {
    if ($simulatorProcess -and -not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
    if (-not $mockProcess.HasExited) {
        Stop-Process -Id $mockProcess.Id
        $mockProcess.WaitForExit()
    }
    foreach ($entry in $previousSecrets.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
}
