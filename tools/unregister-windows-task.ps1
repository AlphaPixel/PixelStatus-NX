[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$TaskName = 'AlphaPixel PixelStatus-NX'
)

$ErrorActionPreference = 'Stop'
$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($null -eq $task) {
    Write-Output "Scheduled task is not registered: $TaskName"
    return
}
if ($PSCmdlet.ShouldProcess($TaskName, 'stop and unregister scheduled task')) {
    if ($task.State -eq 'Running') {
        Stop-ScheduledTask -TaskName $TaskName
    }
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}
