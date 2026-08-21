[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18919
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$config = Resolve-Path -LiteralPath 'examples\layout-card.example.json'
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
    $displayUri = "http://127.0.0.1:$WebPort/api/v1/display"
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $verified = $false
    $lastSequence = 0
    while ([DateTime]::UtcNow -lt $deadline -and -not $verified) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before the layout card was observed'
        }
        try {
            $frame = Invoke-RestMethod -Uri $displayUri
            if ($frame.pixels.Count -ne 256) {
                continue
            }
            $lastSequence = $frame.sequence
            $bottom = $frame.pixels[144..255]
            $verified = $frame.pixels[0] -eq 0x00C853 `
                -and $frame.pixels[8] -eq 0x6200EA `
                -and $frame.pixels[12] -eq 0x00C853 `
                -and $bottom -contains 0xFFD600
        } catch {
        }
    }
    if (-not $verified) {
        throw 'Composite indicators and bounded UTC clock were not observed'
    }
    Write-Output `
        "PixelStatus NX AABB layout-card test passed through frame $lastSequence."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
