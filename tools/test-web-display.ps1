[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18790
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$config = Resolve-Path -LiteralPath 'examples\pixelstatus.sample.json'
$quotedConfig = '"{0}"' -f $config.Path
$arguments = @(
    $quotedConfig,
    '--run-for-ms', '10000',
    '--no-window',
    '--no-api',
    '--web-display-port', $WebPort.ToString(),
    '--web-refresh-ms', '33'
)

$simulatorProcess = Start-Process `
    -FilePath $simulator.Path `
    -ArgumentList $arguments `
    -WindowStyle Hidden `
    -PassThru

try {
    $baseUri = "http://127.0.0.1:$WebPort"
    $frameResponse = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    while (-not $frameResponse -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Headless simulator exited before the browser display became ready'
        }
        try {
            $frameResponse = Invoke-WebRequest -Uri "$baseUri/api/v1/display"
        } catch {
        }
    }
    if (-not $frameResponse) {
        throw "Browser display did not become ready on port $WebPort"
    }

    $frame = $frameResponse.Content | ConvertFrom-Json
    if ($frame.schema_version -ne 1 `
        -or $frame.width -ne 16 `
        -or $frame.height -ne 16 `
        -or $frame.format -ne 'rgb888' `
        -or $frame.default_refresh_ms -ne 33 `
        -or $frame.pixels.Count -ne 256 `
        -or $frame.sequence -lt 1) {
        throw 'Browser display frame API returned unexpected data'
    }

    $page = Invoke-WebRequest -Uri "$baseUri/"
    if ($page.Content -notmatch 'id="pixelstatus-display"' `
        -or $page.Content -notmatch 'id="refresh-rate"' `
        -or $page.Content -notmatch 'Open minimal display window') {
        throw 'Browser display page is missing expected display controls'
    }

    $manifest = Invoke-RestMethod -Uri "$baseUri/manifest.webmanifest"
    if ($manifest.display -ne 'standalone') {
        throw 'Browser display manifest is not configured for standalone display'
    }

    Write-Output `
        "PixelStatus NX browser display smoke test passed: $($frame.width)x$($frame.height), frame $($frame.sequence), $($frame.pixels.Count) pixels."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
