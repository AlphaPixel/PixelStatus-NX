[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18889,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18890
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$config = Resolve-Path -LiteralPath 'examples\https-monitor.example.json'
$token = 'pixelstatus-https-integration-test'
$quotedConfig = '"{0}"' -f $config.Path
$arguments = @(
    $quotedConfig,
    '--run-for-ms', '15000',
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
    $stateUri = "http://127.0.0.1:$ApiPort/api/v1/status/public-https"
    $state = $null
    $lastCandidate = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not $state -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before publishing the HTTPS state'
        }
        try {
            $candidate = Invoke-RestMethod -Uri $stateUri -Headers $headers
            $lastCandidate = $candidate
            if ($candidate.status -eq 'ok' `
                -and $candidate.value -eq 200 `
                -and $candidate.message -eq 'HTTP 200') {
                $state = $candidate
            }
        } catch {
        }
    }
    if (-not $state) {
        if ($lastCandidate) {
            throw "HTTPS monitor ended as $($lastCandidate.status): $($lastCandidate.message)"
        }
        throw 'HTTPS monitor did not publish a state'
    }

    Write-Output `
        "PixelStatus NX HTTPS smoke test passed: id=$($state.id), status=$($state.status), HTTP=$($state.value)."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
