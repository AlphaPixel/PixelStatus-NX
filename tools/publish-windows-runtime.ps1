[CmdletBinding()]
param(
    [string]$RuntimeRoot = (Join-Path $env:LOCALAPPDATA 'AlphaPixel\PixelStatus-NX'),
    [string]$BuildDirectory = 'out\build\windows-debug\Debug',
    [string]$Config = 'examples\operations.local.json',
    [Parameter(Mandatory)]
    [ValidatePattern('^https://')]
    [string]$UnifiBaseUrl,
    [Parameter(Mandatory)]
    [ValidatePattern('^(?:[0-9A-Fa-f]{2}:?){32}$')]
    [string]$UnifiCertificateSha256,
    [Parameter(Mandatory)]
    [ValidatePattern('^https://')]
    [string]$OpenWrtUrl,
    [Parameter(Mandatory)]
    [ValidatePattern('^(?:[0-9A-Fa-f]{2}:?){32}$')]
    [string]$OpenWrtCertificateSha256,
    [ValidateSet('pixel', 'block')]
    [string]$MiMode = 'block',
    [string]$TaskName = 'AlphaPixel PixelStatus-NX',
    [switch]$StartTask
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepositoryPath {
    param([Parameter(Mandatory)] [string]$Path)

    if ([IO.Path]::IsPathRooted($Path)) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $repositoryRoot $Path)).Path
}

function Copy-PythonPackage {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$DestinationRoot
    )

    $source = Join-Path $PSScriptRoot $Name
    $destination = Join-Path $DestinationRoot $Name
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath $source -File -Filter '*.py' |
        Copy-Item -Destination $destination
}

$simulator = Join-Path (Resolve-RepositoryPath $BuildDirectory) 'pixelstatus_simulator.exe'
if (-not (Test-Path -LiteralPath $simulator -PathType Leaf)) {
    throw "Simulator is missing: $simulator"
}
$configPath = Resolve-RepositoryPath $Config
$python = (Get-Command python -ErrorAction Stop).Source
& $python -c 'import bleak'
if ($LASTEXITCODE -ne 0) {
    throw 'The selected Python runtime cannot import Bleak'
}

$runtimeFullPath = [IO.Path]::GetFullPath($RuntimeRoot)
$localAppDataFullPath = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\') + '\'
if (-not $runtimeFullPath.StartsWith(
    $localAppDataFullPath,
    [StringComparison]::OrdinalIgnoreCase
)) {
    throw 'RuntimeRoot must remain below the current user LocalAppData directory'
}

$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
$taskWasRunning = $null -ne $task -and $task.State -eq 'Running'
if ($taskWasRunning) {
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

$releaseId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$releasesRoot = Join-Path $runtimeFullPath 'releases'
$releaseRoot = Join-Path $releasesRoot $releaseId
$bin = Join-Path $releaseRoot 'bin'
$configuration = Join-Path $releaseRoot 'config'
$runtimeTools = Join-Path $releaseRoot 'tools'
New-Item -ItemType Directory -Path $bin, $configuration, $runtimeTools -Force | Out-Null

Copy-Item -LiteralPath $simulator -Destination $bin
Copy-Item -LiteralPath $configPath -Destination (Join-Path $configuration 'operations.local.json')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run-operations-demo.ps1') -Destination $runtimeTools
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'start-windows-runtime.ps1') -Destination $runtimeTools
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'requirements-mi-ble.txt') -Destination $runtimeTools
Copy-PythonPackage -Name 'mi_ble' -DestinationRoot $runtimeTools
Copy-PythonPackage -Name 'unifi' -DestinationRoot $runtimeTools
Copy-PythonPackage -Name 'openwrt' -DestinationRoot $runtimeTools
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.md') -Destination $releaseRoot

$profile = [ordered]@{
    schema_version = 1
    published_at = [DateTime]::UtcNow.ToString('o')
    python_executable = $python
    mi_mode = $MiMode
    unifi_base_url = $UnifiBaseUrl
    unifi_certificate_sha256 = $UnifiCertificateSha256
    openwrt_url = $OpenWrtUrl
    openwrt_certificate_sha256 = $OpenWrtCertificateSha256
}
$profile | ConvertTo-Json | Set-Content `
    -LiteralPath (Join-Path $releaseRoot 'runtime.local.json') `
    -Encoding utf8

New-Item -ItemType Directory -Path $runtimeFullPath -Force | Out-Null
Copy-Item `
    -LiteralPath (Join-Path $PSScriptRoot 'run-published-runtime.ps1') `
    -Destination (Join-Path $runtimeFullPath 'run-published-runtime.ps1') `
    -Force
$pointerPath = Join-Path $runtimeFullPath 'current.json'
$previousPointerPath = Join-Path $runtimeFullPath 'current.previous.json'
if (Test-Path -LiteralPath $pointerPath) {
    Copy-Item -LiteralPath $pointerPath -Destination $previousPointerPath -Force
}
$temporaryPointer = Join-Path $runtimeFullPath 'current.next.json'
@{
    schema_version = 1
    release = "releases/$releaseId"
} | ConvertTo-Json | Set-Content -LiteralPath $temporaryPointer -Encoding utf8
Move-Item -LiteralPath $temporaryPointer -Destination $pointerPath -Force

if (($taskWasRunning -or $StartTask) -and $null -ne $task) {
    Start-ScheduledTask -TaskName $TaskName
}

Write-Output ([pscustomobject]@{
    RuntimeRoot = $runtimeFullPath
    Release = $releaseId
    TaskRegistered = $null -ne $task
    TaskStarted = ($taskWasRunning -or $StartTask) -and $null -ne $task
})
