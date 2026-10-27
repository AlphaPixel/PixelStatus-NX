[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [string]$Config = 'examples\operations.local.json',
    [ValidateRange(1, 65535)]
    [int]$ApiPort = 18907,
    [ValidateRange(1, 65535)]
    [int]$WebPort = 18908,
    [ValidateSet('pixel', 'block')]
    [string]$MiMode = 'block',
    [string]$Address,
    [ValidatePattern('^https://')]
    [string]$UnifiBaseUrl,
    [ValidatePattern('^(?:[0-9A-Fa-f]{2}:?){32}$')]
    [string]$UnifiCertificateSha256,
    [ValidatePattern('^https://')]
    [string]$OpenWrtUrl,
    [ValidatePattern('^(?:[0-9A-Fa-f]{2}:?){32}$')]
    [string]$OpenWrtCertificateSha256,
    [switch]$ClaudeUsage,
    [string]$PythonExecutable,
    [string]$MiStatusFile,
    [string]$ProcessFile,
    [switch]$RestartMiBridge,
    [switch]$BrowserOnly
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepositoryPath {
    param([Parameter(Mandatory)] [string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return Resolve-Path -LiteralPath $Path
    }
    return Resolve-Path -LiteralPath (Join-Path $repositoryRoot $Path)
}

function Stop-RecordedRuntimeProcesses {
    if (-not $ProcessFile -or -not (Test-Path -LiteralPath $ProcessFile)) {
        return
    }
    try {
        $manifest = Get-Content -Raw -LiteralPath $ProcessFile | ConvertFrom-Json
        foreach ($entry in @($manifest.processes)) {
            if ($entry.role -notin @('simulator', 'unifi', 'openwrt', 'claude_usage')) {
                continue
            }
            $process = Get-Process -Id ([int]$entry.pid) -ErrorAction SilentlyContinue
            if ($null -eq $process) {
                continue
            }
            $actualPath = $process.Path
            $recordedPath = [IO.Path]::GetFullPath([string]$entry.executable)
            $actualStart = $process.StartTime.ToUniversalTime()
            $recordedStart = [DateTimeOffset]::Parse(
                [string]$entry.started_at
            ).UtcDateTime
            if (
                $actualPath -and
                [IO.Path]::GetFullPath($actualPath).Equals(
                    $recordedPath,
                    [StringComparison]::OrdinalIgnoreCase
                ) -and
                [Math]::Abs(($actualStart - $recordedStart).TotalSeconds) -lt 1
            ) {
                Stop-Process -Id $process.Id
                $process.WaitForExit()
            } else {
                Write-Warning (
                    "Skipped stale runtime PID $($entry.pid); " +
                    'the executable or start time no longer matches'
                )
            }
        }
    } catch {
        Write-Warning "Unable to clean recorded runtime processes: $_"
    } finally {
        Remove-Item -LiteralPath $ProcessFile -Force -ErrorAction SilentlyContinue
    }
}

function Write-RuntimeProcessManifest {
    if (-not $ProcessFile) {
        return
    }
    $entries = @()
    foreach ($item in @(
        @{Role = 'simulator'; Process = $simulatorProcess},
        @{Role = 'unifi'; Process = $unifiProcess},
        @{Role = 'openwrt'; Process = $openWrtProcess},
        @{Role = 'claude_usage'; Process = $claudeUsageProcess}
    )) {
        $process = $item.Process
        if ($null -ne $process -and -not $process.HasExited) {
            $entries += [ordered]@{
                role = $item.Role
                pid = $process.Id
                executable = $process.Path
                started_at = $process.StartTime.ToUniversalTime().ToString('o')
            }
        }
    }
    $directory = Split-Path -Parent $ProcessFile
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $temporary = "$ProcessFile.$PID.next"
    [ordered]@{
        schema_version = 1
        owner_pid = $PID
        processes = $entries
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $temporary -Encoding utf8
    Move-Item -LiteralPath $temporary -Destination $ProcessFile -Force
}

$buildPath = Resolve-RepositoryPath -Path $BuildDirectory
$simulator = Resolve-Path -LiteralPath (Join-Path $buildPath.Path 'pixelstatus_simulator.exe')
$configPath = Resolve-RepositoryPath -Path $Config
$useUnifiCollector = $UnifiBaseUrl -or $UnifiCertificateSha256
if ([bool]$UnifiBaseUrl -ne [bool]$UnifiCertificateSha256) {
    throw 'UnifiBaseUrl and UnifiCertificateSha256 must be supplied together'
}
if ([bool]$OpenWrtUrl -ne [bool]$OpenWrtCertificateSha256) {
    throw 'OpenWrtUrl and OpenWrtCertificateSha256 must be supplied together'
}
$useOpenWrtCollector = $OpenWrtUrl -or $OpenWrtCertificateSha256
$pythonPath = $null
if (-not $BrowserOnly -or $useUnifiCollector -or $useOpenWrtCollector -or $ClaudeUsage) {
    if ($PythonExecutable) {
        $pythonPath = (Get-Item -LiteralPath $PythonExecutable -ErrorAction Stop).FullName
    } else {
        $pythonPath = (Get-Command python -ErrorAction Stop).Source
    }
}

$token = 'pixelstatus-operations-demo'
$quotedConfig = '"{0}"' -f $configPath.Path
$simulatorArguments = @(
    $quotedConfig,
    '--no-window',
    '--api-token', $token,
    '--api-port', $ApiPort.ToString(),
    '--web-display-port', $WebPort.ToString(),
    '--web-refresh-ms', '33',
    '--monitor-workers', '8'
)
$simulatorProcess = $null
$unifiProcess = $null
$openWrtProcess = $null
$claudeUsageProcess = $null

Push-Location $repositoryRoot
try {
    Stop-RecordedRuntimeProcesses
    if ($useUnifiCollector) {
        $unifiArguments = @(
            '-m', 'tools.unifi', 'serve',
            '--base-url', $UnifiBaseUrl,
            '--certificate-sha256', $UnifiCertificateSha256,
            '--port', '18950',
            '--interval-seconds', '30'
        )
        $unifiProcess = Start-Process `
            -FilePath $pythonPath `
            -ArgumentList $unifiArguments `
            -WorkingDirectory $repositoryRoot `
            -WindowStyle Hidden `
            -PassThru
        Write-RuntimeProcessManifest

        $unifiHealthUrl = 'http://127.0.0.1:18950/health'
        $unifiDeadline = [DateTime]::UtcNow.AddSeconds(20)
        $unifiReady = $false
        while ([DateTime]::UtcNow -lt $unifiDeadline) {
            if ($unifiProcess.HasExited) {
                throw "UniFi collector exited during startup with code $($unifiProcess.ExitCode)"
            }
            try {
                $unifiHealth = Invoke-RestMethod -Uri $unifiHealthUrl -TimeoutSec 1
                if ($unifiHealth.collector.status -in @('healthy', 'error')) {
                    $unifiReady = $true
                    break
                }
            } catch {
            }
            Start-Sleep -Milliseconds 200
        }
        if (-not $unifiReady) {
            throw "UniFi collector did not respond at $unifiHealthUrl within 20 seconds"
        }
        Write-Host "Official UniFi monitoring is responding through $unifiHealthUrl"
    }

    if ($useOpenWrtCollector) {
        $openWrtArguments = @(
            '-m', 'tools.openwrt', 'serve',
            '--url', $OpenWrtUrl,
            '--certificate-sha256', $OpenWrtCertificateSha256,
            '--port', '18951',
            '--interval-seconds', '15'
        )
        $openWrtProcess = Start-Process `
            -FilePath $pythonPath `
            -ArgumentList $openWrtArguments `
            -WorkingDirectory $repositoryRoot `
            -WindowStyle Hidden `
            -PassThru
        Write-RuntimeProcessManifest

        $openWrtHealthUrl = 'http://127.0.0.1:18951/health'
        $openWrtDeadline = [DateTime]::UtcNow.AddSeconds(20)
        $openWrtReady = $false
        while ([DateTime]::UtcNow -lt $openWrtDeadline) {
            if ($openWrtProcess.HasExited) {
                throw "OpenWrt collector exited during startup with code $($openWrtProcess.ExitCode)"
            }
            try {
                $openWrtHealth = Invoke-RestMethod -Uri $openWrtHealthUrl -TimeoutSec 1
                if ($openWrtHealth.collector.status -in @('healthy', 'error')) {
                    $openWrtReady = $true
                    break
                }
            } catch {
            }
            Start-Sleep -Milliseconds 200
        }
        if (-not $openWrtReady) {
            throw "OpenWrt collector did not respond at $openWrtHealthUrl within 20 seconds"
        }
        Write-Host "OpenWrt bridge and Starlink-path monitoring is responding through $openWrtHealthUrl"
    }

    if ($ClaudeUsage) {
        $claudeUsageProcess = Start-Process `
            -FilePath $pythonPath `
            -ArgumentList @('-m', 'tools.claude_usage', 'serve', '--port', '18952') `
            -WorkingDirectory $repositoryRoot `
            -WindowStyle Hidden `
            -PassThru
        Write-RuntimeProcessManifest

        $claudeUsageHealthUrl = 'http://127.0.0.1:18952/health'
        $claudeUsageDeadline = [DateTime]::UtcNow.AddSeconds(20)
        $claudeUsageReady = $false
        while ([DateTime]::UtcNow -lt $claudeUsageDeadline) {
            if ($claudeUsageProcess.HasExited) {
                throw "Claude usage collector exited during startup with code $($claudeUsageProcess.ExitCode)"
            }
            try {
                $claudeUsageHealth = Invoke-RestMethod -Uri $claudeUsageHealthUrl -TimeoutSec 1
                if ($claudeUsageHealth.collector.status -in @('healthy', 'error')) {
                    $claudeUsageReady = $true
                    break
                }
            } catch {
            }
            Start-Sleep -Milliseconds 200
        }
        if (-not $claudeUsageReady) {
            throw "Claude usage collector did not respond at $claudeUsageHealthUrl within 20 seconds"
        }
        Write-Host "Claude plan usage monitoring is responding through $claudeUsageHealthUrl"
    }

    $simulatorProcess = Start-Process `
        -FilePath $simulator.Path `
        -ArgumentList $simulatorArguments `
        -WindowStyle Hidden `
        -PassThru
    Write-RuntimeProcessManifest

    $displayUrl = "http://127.0.0.1:$WebPort/api/v1/display"
    $browserUrl = "http://127.0.0.1:$WebPort/"
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $ready = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($simulatorProcess.HasExited) {
            throw "Simulator exited during startup with code $($simulatorProcess.ExitCode). Are ports $ApiPort and $WebPort already in use?"
        }
        try {
            $frame = Invoke-RestMethod -Uri $displayUrl -TimeoutSec 1
            if ($frame.width -eq 16 -and $frame.height -eq 16) {
                $ready = $true
                break
            }
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $ready) {
        throw "Simulator did not become ready at $browserUrl within 15 seconds"
    }

    Write-Host "PixelStatus NX operations deck is running at $browserUrl"
    Write-Host 'Leave this terminal open. Press Ctrl+C to stop it.'

    if ($BrowserOnly) {
        Write-Host 'Bluetooth mirroring is disabled for this run.'
        while (-not $simulatorProcess.WaitForExit(1000)) {
        }
        if ($simulatorProcess.ExitCode -ne 0) {
            throw "Simulator exited with code $($simulatorProcess.ExitCode)"
        }
    } else {
        $bridgeArguments = @(
            '-m', 'tools.mi_ble',
            'bridge',
            '--url', $displayUrl,
            '--mode', $MiMode
        )
        if ($Address) {
            $bridgeArguments += @('--address', $Address)
        }
        if ($MiStatusFile) {
            $bridgeArguments += @('--status-file', $MiStatusFile)
        }

        Write-Host "Mirroring to MI Matrix Display in $MiMode mode."
        do {
            & $pythonPath @bridgeArguments
            $bridgeExitCode = $LASTEXITCODE
            if (-not $RestartMiBridge) {
                if ($bridgeExitCode -ne 0) {
                    throw "MI BLE bridge exited with code $bridgeExitCode"
                }
                break
            }
            Write-Warning (
                "MI BLE bridge exited with code $bridgeExitCode; " +
                'restarting in five seconds'
            )
            Start-Sleep -Seconds 5
        } while ($true)
    }
} finally {
    if ($null -ne $simulatorProcess -and -not $simulatorProcess.HasExited) {
        Stop-Process -Id $simulatorProcess.Id
        $simulatorProcess.WaitForExit()
    }
    if ($null -ne $unifiProcess -and -not $unifiProcess.HasExited) {
        Stop-Process -Id $unifiProcess.Id
        $unifiProcess.WaitForExit()
    }
    if ($null -ne $openWrtProcess -and -not $openWrtProcess.HasExited) {
        Stop-Process -Id $openWrtProcess.Id
        $openWrtProcess.WaitForExit()
    }
    if ($null -ne $claudeUsageProcess -and -not $claudeUsageProcess.HasExited) {
        Stop-Process -Id $claudeUsageProcess.Id
        $claudeUsageProcess.WaitForExit()
    }
    if ($ProcessFile) {
        Remove-Item -LiteralPath $ProcessFile -Force -ErrorAction SilentlyContinue
    }
    Pop-Location
}
