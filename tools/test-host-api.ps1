[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$Port = 18787
)

$ErrorActionPreference = 'Stop'

$simulator = Join-Path $BuildDirectory 'pixelstatus_simulator.exe'
if (-not (Test-Path -LiteralPath $simulator -PathType Leaf)) {
    throw "Simulator executable not found: $simulator"
}

$token = 'pixelstatus-host-integration-test'
$arguments = @(
    '--run-for-ms', '10000',
    '--api-token', $token,
    '--api-port', $Port.ToString(),
    '--no-web-display'
)
$simulatorProcess = Start-Process `
    -FilePath (Resolve-Path -LiteralPath $simulator) `
    -ArgumentList $arguments `
    -WindowStyle Hidden `
    -PassThru

try {
    $headers = @{ Authorization = "Bearer $token" }
    $baseUri = "http://127.0.0.1:$Port/api/v1/status"
    $ready = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $ready -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        try {
            $null = Invoke-RestMethod -Uri $baseUri -Headers $headers -Method Get
            $ready = $true
        } catch {
            if ($simulatorProcess.HasExited) {
                throw "Simulator exited before its status API became ready"
            }
        }
    }
    if (-not $ready) {
        throw "Simulator status API did not become ready on port $Port"
    }

    $body = @{
        status = 'fail'
        value = 7
        message = 'Host integration smoke test'
        ttl = 5
    } | ConvertTo-Json
    $created = Invoke-RestMethod `
        -Uri "$baseUri/build" `
        -Headers $headers `
        -Method Post `
        -ContentType 'application/json' `
        -Body $body
    $readBack = Invoke-RestMethod -Uri "$baseUri/build" -Headers $headers -Method Get

    if ($created.status -ne 'fail' `
        -or $readBack.value -ne 7 `
        -or $readBack.message -ne 'Host integration smoke test') {
        throw 'Status API round trip returned unexpected data'
    }

    Write-Output "PixelStatus NX host API smoke test passed on port $Port."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
