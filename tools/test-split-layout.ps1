[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18929,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18930
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$config = Resolve-Path -LiteralPath 'examples\split-layout.example.json'
$token = 'pixelstatus-split-layout-test'
$quotedConfig = '"{0}"' -f $config.Path
$arguments = @(
    $quotedConfig,
    '--run-for-ms', '15000',
    '--no-window',
    '--api-token', $token,
    '--api-port', $ApiPort.ToString(),
    '--web-display-port', $WebPort.ToString(),
    '--web-refresh-ms', '33'
)
$simulatorProcess = Start-Process `
    -FilePath $simulator.Path `
    -ArgumentList $arguments `
    -WindowStyle Hidden `
    -PassThru

try {
    $headers = @{ Authorization = "Bearer $token" }
    $statusUri = "http://127.0.0.1:$ApiPort/api/v1/status"
    $displayUri = "http://127.0.0.1:$WebPort/api/v1/display"
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $ready = $false
    while (-not $ready -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before the split-layout endpoints became ready'
        }
        try {
            $null = Invoke-RestMethod -Uri $statusUri -Headers $headers
            $null = Invoke-RestMethod -Uri $displayUri
            $ready = $true
        } catch {
        }
    }
    if (-not $ready) {
        throw 'Split-layout status and display APIs did not become ready'
    }

    function Push-State {
        param(
            [Parameter(Mandatory)] [string]$Id,
            [Parameter(Mandatory)] [string]$Status,
            [AllowNull()] $Value
        )
        $body = @{status = $Status; value = $Value} | ConvertTo-Json -Compress
        $null = Invoke-RestMethod `
            -Uri "$statusUri/$Id" `
            -Headers $headers `
            -Method Post `
            -ContentType 'application/json' `
            -Body $body
    }

    Push-State -Id 'disk-1' -Status 'ok' -Value 50
    Push-State -Id 'disk-2' -Status 'warn' -Value 75
    Push-State -Id 'disk-3' -Status 'fail' -Value 100
    Push-State -Id 'wan-primary' -Status 'ok' -Value 25
    Push-State -Id 'wan-secondary' -Status 'warn' -Value 75

    $statuses = @('ok', 'warn', 'fail')
    foreach ($prefix in @('drive', 'vps')) {
        foreach ($number in 1..16) {
            $id = '{0}-{1:D2}' -f $prefix, $number
            Push-State -Id $id -Status $statuses[($number - 1) % $statuses.Count] -Value $null
        }
    }
    # The simulator's demonstration transition targets its first source. Updating it
    # last gives this test a deterministic frame before the next four-second tick.
    Push-State -Id 'disk-0' -Status 'ok' -Value 25

    $deadline = [DateTime]::UtcNow.AddSeconds(4)
    $verified = $false
    $lastSequence = 0
    while (-not $verified -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        $frame = Invoke-RestMethod -Uri $displayUri
        if ($frame.pixels.Count -ne 256) {
            continue
        }
        $lastSequence = $frame.sequence
        $clockRegion = $frame.pixels[144..255]
        $verified = $frame.pixels[0] -eq 0x00C853 `
            -and $frame.pixels[1] -eq 0x02050A `
            -and $frame.pixels[32] -eq 0x00C853 `
            -and $frame.pixels[34] -eq 0x02050A `
            -and $frame.pixels[4] -eq 0x00C853 `
            -and $frame.pixels[5] -eq 0xFFD600 `
            -and $frame.pixels[6] -eq 0xFF1744 `
            -and $frame.pixels[8] -eq 0x02050A `
            -and $frame.pixels[104] -eq 0x00C853 `
            -and $frame.pixels[26] -eq 0x02050A `
            -and $frame.pixels[42] -eq 0xFFD600 `
            -and $frame.pixels[12] -eq 0x00C853 `
            -and $frame.pixels[13] -eq 0xFFD600 `
            -and $frame.pixels[14] -eq 0xFF1744 `
            -and $frame.pixels[128] -eq 0x02050A `
            -and $clockRegion -contains 0xFFD600
    }
    if (-not $verified) {
        throw 'The expected split bars, grids, gap, and bounded UTC clock were not observed'
    }
    Write-Output "PixelStatus NX split-layout browser-frame test passed through frame $lastSequence."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
