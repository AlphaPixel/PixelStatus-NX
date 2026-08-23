[CmdletBinding()]
param(
    [string]$RuntimeRoot = (Join-Path $env:LOCALAPPDATA 'AlphaPixel\PixelStatus-NX'),
    [string]$TaskName = 'AlphaPixel PixelStatus-NX',
    [ValidateRange(0, 3600)]
    [int]$LogonDelaySeconds = 15,
    [switch]$Start
)

$ErrorActionPreference = 'Stop'
$runtimeFullPath = [IO.Path]::GetFullPath($RuntimeRoot)
$bootstrap = Join-Path $runtimeFullPath 'run-published-runtime.ps1'
if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) {
    throw "Publish a Windows runtime before registering the task: $bootstrap"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$powershell = Join-Path `
    $env:SystemRoot `
    'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    throw "System Windows PowerShell is missing: $powershell"
}
$existingTask = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($null -ne $existingTask -and $existingTask.State -eq 'Running') {
    Stop-ScheduledTask -TaskName $TaskName
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 200
        $existingTask = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
    } while ($existingTask.State -eq 'Running' -and [DateTime]::UtcNow -lt $deadline)
    if ($existingTask.State -eq 'Running') {
        throw "Scheduled task $TaskName did not stop within 15 seconds"
    }
}
$actionArguments = '-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden ' +
    '-ExecutionPolicy Bypass -File "{0}"' -f $bootstrap
$action = New-ScheduledTaskAction `
    -Execute $powershell `
    -Argument $actionArguments `
    -WorkingDirectory $runtimeFullPath
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity
if ($LogonDelaySeconds -gt 0) {
    $trigger.Delay = "PT${LogonDelaySeconds}S"
}
$principal = New-ScheduledTaskPrincipal `
    -UserId $identity `
    -LogonType Interactive `
    -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -RestartCount 20 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero)
$definition = New-ScheduledTask `
    -Action $action `
    -Trigger $trigger `
    -Principal $principal `
    -Settings $settings `
    -Description 'Runs the published PixelStatus NX display and monitoring runtime.'

Register-ScheduledTask `
    -TaskName $TaskName `
    -InputObject $definition `
    -Force | Out-Null
if ($Start) {
    Start-ScheduledTask -TaskName $TaskName
}

Get-ScheduledTask -TaskName $TaskName
