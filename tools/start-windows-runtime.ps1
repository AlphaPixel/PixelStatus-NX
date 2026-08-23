[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$releaseRoot = Split-Path -Parent $PSScriptRoot
$profilePath = Join-Path $releaseRoot 'runtime.local.json'
if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
    throw "Published runtime profile is missing: $profilePath"
}

$profile = Get-Content -Raw -LiteralPath $profilePath | ConvertFrom-Json
$launcher = Join-Path $PSScriptRoot 'run-operations-demo.ps1'
$runtimeRoot = Split-Path -Parent (Split-Path -Parent $releaseRoot)
$arguments = @{
    BuildDirectory = Join-Path $releaseRoot 'bin'
    Config = Join-Path $releaseRoot 'config\operations.local.json'
    MiMode = $profile.mi_mode
    PythonExecutable = $profile.python_executable
    UnifiBaseUrl = $profile.unifi_base_url
    UnifiCertificateSha256 = $profile.unifi_certificate_sha256
    OpenWrtUrl = $profile.openwrt_url
    OpenWrtCertificateSha256 = $profile.openwrt_certificate_sha256
    MiStatusFile = Join-Path $runtimeRoot 'logs\mi-ble-status.json'
}

& $launcher @arguments
