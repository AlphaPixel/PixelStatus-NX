[CmdletBinding()]
param(
    [string]$DisplayUrl = 'http://127.0.0.1:8788/api/v1/display',
    [ValidateSet('pixel', 'block')]
    [string]$Mode = 'pixel',
    [string]$Address,
    [switch]$Response,
    [switch]$Once,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$python = Get-Command python -ErrorAction Stop
$arguments = @(
    '-m', 'tools.mi_ble',
    'bridge',
    '--url', $DisplayUrl,
    '--mode', $Mode
)
if ($Address) {
    $arguments += @('--address', $Address)
}
if ($Response) {
    $arguments += '--response'
}
if ($Once) {
    $arguments += '--once'
}
if ($DryRun) {
    $arguments += '--dry-run'
}

& $python.Source @arguments
if ($LASTEXITCODE -ne 0) {
    throw "MI BLE bridge exited with code $LASTEXITCODE"
}
