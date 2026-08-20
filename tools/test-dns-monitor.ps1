[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18859,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18860
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$config = Resolve-Path -LiteralPath 'examples\dns-monitor.example.json'
$token = 'pixelstatus-dns-integration-test'
$quotedConfig = '"{0}"' -f $config.Path
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

try {
    $headers = @{ Authorization = "Bearer $token" }
    $stateUri = "http://127.0.0.1:$ApiPort/api/v1/status/localhost-dns"
    $state = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    while (-not $state -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before publishing the DNS monitor state'
        }
        try {
            $candidate = Invoke-RestMethod -Uri $stateUri -Headers $headers
            if ($candidate.status -eq 'ok' `
                -and $candidate.value -match '(^|,)127\.0\.0\.1(,|$)' `
                -and $candidate.message -match '^DNS resolved localhost') {
                $state = $candidate
            }
        } catch {
        }
    }
    if (-not $state) {
        throw 'DNS monitor did not publish the expected localhost address state'
    }

    Write-Output `
        "PixelStatus NX DNS monitor smoke test passed: id=$($state.id), status=$($state.status), addresses=$($state.value)."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
