[CmdletBinding(DefaultParameterSetName = 'Serve')]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^https://')]
    [string]$BaseUrl,
    [Parameter(Mandatory)]
    [ValidatePattern('^(?:[0-9A-Fa-f]{2}:?){32}$')]
    [string]$CertificateSha256,
    [string]$SecretName = 'unifi-api-key',
    [string]$SiteId,
    [string]$DeviceId,
    [Parameter(ParameterSetName = 'Serve')]
    [ValidateRange(1, 65535)]
    [int]$Port = 18950,
    [Parameter(ParameterSetName = 'Serve')]
    [ValidateRange(1, 3600)]
    [int]$IntervalSeconds = 30,
    [Parameter(ParameterSetName = 'Probe')]
    [switch]$Probe
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$python = Get-Command python -ErrorAction Stop
$command = if ($Probe) { 'probe' } else { 'serve' }
$arguments = @(
    '-m', 'tools.unifi', $command,
    '--base-url', $BaseUrl,
    '--certificate-sha256', $CertificateSha256,
    '--secret-name', $SecretName
)
if ($SiteId) {
    $arguments += @('--site-id', $SiteId)
}
if ($DeviceId) {
    $arguments += @('--device-id', $DeviceId)
}
if (-not $Probe) {
    $arguments += @(
        '--port', $Port.ToString(),
        '--interval-seconds', $IntervalSeconds.ToString()
    )
}

Push-Location $repositoryRoot
try {
    & $python.Source @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "UniFi monitor exited with code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
