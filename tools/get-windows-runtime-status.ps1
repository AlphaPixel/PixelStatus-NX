[CmdletBinding()]
param(
    [string]$RuntimeRoot = (Join-Path $env:LOCALAPPDATA 'AlphaPixel\PixelStatus-NX'),
    [string]$TaskName = 'AlphaPixel PixelStatus-NX'
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
    MiDisplay = if ($null -ne $miStatus) { $miStatus.status } else { 'unavailable' }
    MiSourceFrame = if ($null -ne $miStatus) { $miStatus.source_frame } else { $null }
    MiUpdatedAt = if ($null -ne $miStatus) { $miStatus.updated_at } else { $null }
    Log = Join-Path $RuntimeRoot 'logs\runtime.log'
}
