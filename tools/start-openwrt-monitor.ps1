[CmdletBinding(DefaultParameterSetName = 'Serve')]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^https://')]
    [string]$Url,
    [Parameter(Mandatory)]
    [ValidatePattern('^(?:[0-9A-Fa-f]{2}:?){32}$')]
    [string]$CertificateSha256,
    [string]$Username = 'pixelstatus',
    [string]$SecretName = 'openwrt-api-password',
    [string]$UplinkInterface = 'wwan',
    [Parameter(ParameterSetName = 'Serve')]
    [ValidateRange(1, 65535)]
    [int]$Port = 18951,
    [Parameter(ParameterSetName = 'Serve')]
    [ValidateRange(1, 3600)]
    [int]$IntervalSeconds = 15,
    [Parameter(ParameterSetName = 'Probe')]
    [switch]$Probe
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$python = Get-Command python -ErrorAction Stop
$command = if ($Probe) { 'probe' } else { 'serve' }
$arguments = @(
    '-m', 'tools.openwrt', $command,
    '--url', $Url,
    '--certificate-sha256', $CertificateSha256,
    '--username', $Username,
    '--secret-name', $SecretName,
    '--uplink-interface', $UplinkInterface
)
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
        throw "OpenWrt monitor exited with code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
