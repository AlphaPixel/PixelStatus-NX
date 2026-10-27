[CmdletBinding()]
param(
    [string]$RuntimeRoot = (Join-Path $env:LOCALAPPDATA 'AlphaPixel\PixelStatus-NX'),
    [string]$TaskName = 'AlphaPixel PixelStatus-NX',
    [switch]$CleanLegacyProcesses,
    # Enables the Claude usage collector; once published it stays on in later updates.
    [switch]$ClaudeUsage
)

$ErrorActionPreference = 'Stop'
$runtimeFullPath = [IO.Path]::GetFullPath($RuntimeRoot)
$localAppDataPrefix = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\') + '\'
if (-not $runtimeFullPath.StartsWith(
    $localAppDataPrefix,
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw 'RuntimeRoot must remain below the current user LocalAppData directory'
}

$pointerPath = Join-Path $runtimeFullPath 'current.json'
$pointer = Get-Content -Raw -LiteralPath $pointerPath | ConvertFrom-Json
if ($pointer.release -notmatch '^releases/[0-9A-Za-z._-]+$') {
    throw 'Published runtime pointer is invalid'
}
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $runtimeFullPath $pointer.release))
$releasesRoot = [IO.Path]::GetFullPath((Join-Path $runtimeFullPath 'releases'))
$releasesPrefix = $releasesRoot.TrimEnd('\') + '\'
if (-not $releaseRoot.StartsWith(
    $releasesPrefix,
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw 'Published runtime pointer escapes the releases directory'
}
$profilePath = Join-Path $releaseRoot 'runtime.local.json'
$profile = Get-Content -Raw -LiteralPath $profilePath | ConvertFrom-Json

$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($null -ne $task -and $task.State -eq 'Running') {
    Stop-ScheduledTask -TaskName $TaskName
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 200
        $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
    } while ($task.State -eq 'Running' -and [DateTime]::UtcNow -lt $deadline)
    if ($task.State -eq 'Running') {
        throw "Scheduled task $TaskName did not stop within 15 seconds"
    }
}

if ($CleanLegacyProcesses) {
    $candidates = Get-CimInstance Win32_Process | Where-Object {
        if ($_.ProcessId -eq $PID) {
            return $false
        }
        $executable = [string]$_.ExecutablePath
        $commandLine = [string]$_.CommandLine
        $publishedSimulator = (
            $_.Name -eq 'pixelstatus_simulator.exe' -and
            $executable.StartsWith(
                $releasesPrefix,
                [StringComparison]::OrdinalIgnoreCase
            )
        )
        $hostAdapter = (
            $_.Name -match '^python(?:w)?\.exe$' -and
            $commandLine -match '(?i)-m\s+tools\.(?:mi_ble|unifi|openwrt|claude_usage)(?:\s|$)'
        )
        $publishedPowerShell = (
            $_.Name -match '^(?:powershell|pwsh)\.exe$' -and
            $commandLine.IndexOf(
                $runtimeFullPath,
                [StringComparison]::OrdinalIgnoreCase
            ) -ge 0
        )
        $publishedSimulator -or $hostAdapter -or $publishedPowerShell
    }
    foreach ($candidate in $candidates) {
        Stop-Process -Id $candidate.ProcessId -Force
    }
    if ($candidates) {
        Start-Sleep -Seconds 1
    }
}

$publishArguments = @{
    RuntimeRoot = $runtimeFullPath
    TaskName = $TaskName
    UnifiBaseUrl = [string]$profile.unifi_base_url
    UnifiCertificateSha256 = [string]$profile.unifi_certificate_sha256
    OpenWrtUrl = [string]$profile.openwrt_url
    OpenWrtCertificateSha256 = [string]$profile.openwrt_certificate_sha256
    MiMode = [string]$profile.mi_mode
    ClaudeUsage = $ClaudeUsage -or [bool]$profile.claude_usage
}
& (Join-Path $PSScriptRoot 'publish-windows-runtime.ps1') @publishArguments
& (Join-Path $PSScriptRoot 'register-windows-task.ps1') `
    -RuntimeRoot $runtimeFullPath `
    -TaskName $TaskName `
    -Start

