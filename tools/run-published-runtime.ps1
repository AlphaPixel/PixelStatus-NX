[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$runtimeRoot = $PSScriptRoot
$logs = Join-Path $runtimeRoot 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$logPath = Join-Path $logs 'runtime.log'
$previousLogPath = Join-Path $logs 'runtime.previous.log'
if ((Test-Path -LiteralPath $logPath) `
    -and (Get-Item -LiteralPath $logPath).Length -gt 5MB) {
    Move-Item -LiteralPath $logPath -Destination $previousLogPath -Force
}

$transcriptStarted = $false
try {
    Start-Transcript -LiteralPath $logPath -Append | Out-Null
    $transcriptStarted = $true

    $pointerPath = Join-Path $runtimeRoot 'current.json'
    $pointer = Get-Content -Raw -LiteralPath $pointerPath | ConvertFrom-Json
    if ($pointer.release -notmatch '^releases/[0-9A-Za-z._-]+$') {
        throw 'Published runtime pointer is invalid'
    }

    $releaseRoot = [IO.Path]::GetFullPath((Join-Path $runtimeRoot $pointer.release))
    $releasesRoot = [IO.Path]::GetFullPath((Join-Path $runtimeRoot 'releases'))
    $releasesPrefix = $releasesRoot.TrimEnd('\') + '\'
    if (-not $releaseRoot.StartsWith(
        $releasesPrefix,
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw 'Published runtime pointer escapes the releases directory'
    }

    $entryPoint = Join-Path $releaseRoot 'tools\start-windows-runtime.ps1'
    & $entryPoint
    throw 'PixelStatus NX runtime exited unexpectedly'
} catch {
    Write-Error $_
    exit 1
} finally {
    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
}
