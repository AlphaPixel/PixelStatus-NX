[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pixelStatusRepository = Split-Path -Parent $PSScriptRoot
$pixelStatusSavedPath = $env:PATH

Push-Location $pixelStatusRepository
try {
    # Some process launchers provide both Path and PATH. MSBuild treats those as
    # duplicate dictionary keys, so normalize the spelling in this child shell.
    Remove-Item Env:PATH
    $env:Path = $pixelStatusSavedPath

    & cmake --preset windows-debug
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & cmake --build --preset windows-debug
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & ctest --preset windows-debug
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
