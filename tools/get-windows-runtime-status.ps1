[CmdletBinding()]
param(
    [string]$RuntimeRoot = (Join-Path $env:LOCALAPPDATA 'AlphaPixel\PixelStatus-NX'),
    [string]$TaskName = 'AlphaPixel PixelStatus-NX',
    [ValidateRange(5, 3600)]
    [int]$MiHeartbeatMaximumAgeSeconds = 60
)

$ErrorActionPreference = 'Stop'
$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
$taskInfo = if ($null -ne $task) {
    Get-ScheduledTaskInfo -TaskName $TaskName
} else {
    $null
}
$pointerPath = Join-Path $RuntimeRoot 'current.json'
$pointer = if (Test-Path -LiteralPath $pointerPath) {
    Get-Content -Raw -LiteralPath $pointerPath | ConvertFrom-Json
} else {
    $null
}
$miStatusPath = Join-Path $RuntimeRoot 'logs\mi-ble-status.json'
$miStatus = if (Test-Path -LiteralPath $miStatusPath) {
    try {
        Get-Content -Raw -LiteralPath $miStatusPath | ConvertFrom-Json
    } catch {
        $null
    }
} else {
    $null
}
$miUpdatedAt = $null
$miHeartbeatAgeSeconds = $null
$miHeartbeatFresh = $false
if ($null -ne $miStatus -and $miStatus.updated_at) {
    try {
        if ($miStatus.updated_at -is [DateTime]) {
            $miUpdatedAt = [DateTimeOffset]$miStatus.updated_at
        } else {
            $miUpdatedAt = [DateTimeOffset]::Parse([string]$miStatus.updated_at)
        }
        $miHeartbeatAgeSeconds = [Math]::Max(
            0,
            [Math]::Floor(
                ([DateTimeOffset]::UtcNow - $miUpdatedAt.ToUniversalTime()).TotalSeconds
            )
        )
        $miHeartbeatFresh = (
            $miHeartbeatAgeSeconds -le $MiHeartbeatMaximumAgeSeconds
        )
    } catch {
        $miUpdatedAt = $null
    }
}

function Get-EndpointState {
    param([Parameter(Mandatory)] [string]$Uri)
    try {
        $response = Invoke-RestMethod -Uri $Uri -TimeoutSec 2
        if ($response.collector.status) {
            return $response.collector.status
        }
        if ($response.sequence) {
            return "frame $($response.sequence)"
        }
        return 'responding'
    } catch {
        return 'unavailable'
    }
}

[pscustomobject]@{
    TaskName = $TaskName
    TaskState = if ($null -ne $task) { $task.State } else { 'NotRegistered' }
    LastRunTime = if ($null -ne $taskInfo) { $taskInfo.LastRunTime } else { $null }
    LastTaskResult = if ($null -ne $taskInfo) { $taskInfo.LastTaskResult } else { $null }
    CurrentRelease = if ($null -ne $pointer) { $pointer.release } else { $null }
    Browser = Get-EndpointState 'http://127.0.0.1:18908/api/v1/display'
    UniFiCollector = Get-EndpointState 'http://127.0.0.1:18950/health'
    OpenWrtCollector = Get-EndpointState 'http://127.0.0.1:18951/health'
    MiDisplay = if ($null -eq $miStatus) {
        'unavailable'
    } elseif (-not $miHeartbeatFresh) {
        'stale'
    } else {
        $miStatus.status
    }
    MiReportedStatus = if ($null -ne $miStatus) { $miStatus.status } else { $null }
    MiSourceFrame = if ($null -ne $miStatus) { $miStatus.source_frame } else { $null }
    MiUpdatedAt = $miUpdatedAt
    MiHeartbeatAgeSeconds = $miHeartbeatAgeSeconds
    MiHeartbeatFresh = $miHeartbeatFresh
    Log = Join-Path $RuntimeRoot 'logs\runtime.log'
}
