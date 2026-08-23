[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18939,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18940
)

$ErrorActionPreference = 'Stop'

$simulator = Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'pixelstatus_simulator.exe')
$config = Resolve-Path -LiteralPath 'examples\layered-layout.example.json'
$token = 'pixelstatus-layered-layout-test'
$quotedConfig = '"{0}"' -f $config.Path
$arguments = @(
    $quotedConfig,
    '--run-for-ms', '12000',
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
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($simulatorProcess.HasExited) {
            throw 'Simulator exited before the layered-layout endpoints became ready'
        }
        try {
            $null = Invoke-RestMethod -Uri $statusUri -Headers $headers
            $null = Invoke-RestMethod -Uri $displayUri
            break
        } catch {
            Start-Sleep -Milliseconds 50
        }
    }

    function Push-State {
        param(
            [Parameter(Mandatory)] [string]$Id,
            [Parameter(Mandatory)] [string]$Status
        )
        $body = @{status = $Status} | ConvertTo-Json -Compress
        $null = Invoke-RestMethod `
            -Uri "$statusUri/$Id" `
            -Headers $headers `
            -Method Post `
            -ContentType 'application/json' `
            -Body $body
    }

    function Wait-ForFrame {
        param(
            [Parameter(Mandatory)] [int]$Background,
            [Parameter(Mandatory)] [int]$ServiceA,
            [Parameter(Mandatory)] [int]$ServiceB,
            [Parameter(Mandatory)] [int]$ServiceC
        )
        $deadline = [DateTime]::UtcNow.AddSeconds(2)
        while ([DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 40
            $frame = Invoke-RestMethod -Uri $displayUri
            if ($frame.pixels.Count -eq 256 `
                -and $frame.pixels[3] -eq $Background `
                -and $frame.pixels[128] -eq $Background `
                -and $frame.pixels[0] -eq $ServiceA `
                -and $frame.pixels[4] -eq $ServiceB `
                -and $frame.pixels[8] -eq $ServiceC) {
                return $frame.sequence
            }
        }
        throw ('Expected layered frame was not observed: background=0x{0:X6}' -f $Background)
    }

    Push-State -Id 'service-a' -Status 'ok'
    Push-State -Id 'service-b' -Status 'ok'
    Push-State -Id 'service-c' -Status 'ok'
    Push-State -Id 'service-d' -Status 'unknown'
    $healthySequence = Wait-ForFrame `
        -Background 0x000000 `
        -ServiceA 0x00C853 `
        -ServiceB 0x00C853 `
        -ServiceC 0x00C853

    Push-State -Id 'service-b' -Status 'warn'
    $warningSequence = Wait-ForFrame `
        -Background 0x332B00 `
        -ServiceA 0x00C853 `
        -ServiceB 0xFFD600 `
        -ServiceC 0x00C853

    Push-State -Id 'service-c' -Status 'fail'
    $failureSequence = Wait-ForFrame `
        -Background 0x38050F `
        -ServiceA 0x00C853 `
        -ServiceB 0xFFD600 `
        -ServiceC 0xFF1744

    Write-Output `
        "PixelStatus NX layered-layout browser test passed: unknown ignored and black healthy background at frame $healthySequence, dull yellow warning at frame $warningSequence, dull red failure with bright foreground status at frame $failureSequence."
} finally {
    if (-not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
}
