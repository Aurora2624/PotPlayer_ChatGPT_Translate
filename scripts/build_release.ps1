param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\dist')
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
if ((Test-Path -LiteralPath $OutputDir) -and @(Get-ChildItem -LiteralPath $OutputDir -Force).Count -gt 0) {
    throw "Output directory must be empty; choose a new directory: $OutputDir"
}
if ($Version -notmatch '^v?\d+\.\d+\.\d+(?:[-+][A-Za-z0-9][A-Za-z0-9.+-]*)?$') {
    throw 'All-format builds require a version such as v1.9.5 or v1.9.5-rc.1.'
}
$normalizedVersion = $Version -replace '^v', ''
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('potplayer-release-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
try {
    & python (Join-Path $PSScriptRoot 'package_release.py') stage --root $root --stage-dir $stage --version $Version
    if ($LASTEXITCODE -ne 0) { throw 'Release staging failed.' }
    & (Join-Path $stage 'releases\build\build_installer.ps1') -VersionOverride $normalizedVersion -OutputDir $OutputDir
    Copy-Item -LiteralPath (Join-Path $OutputDir 'installer.exe') -Destination (Join-Path $OutputDir 'installer-cpp.exe')
    & python (Join-Path $PSScriptRoot 'verify_native_payload.py') --installer (Join-Path $OutputDir 'installer-cpp.exe') --source-dir $stage
    if ($LASTEXITCODE -ne 0) { throw 'Native installer payload verification failed.' }
    & (Join-Path $stage 'releases\build\build_python_installer.ps1') -Version $normalizedVersion -OutputDir $OutputDir
    & (Join-Path $stage 'installer\build_installer.ps1') -VersionOverride $normalizedVersion -OutputDir $OutputDir
    & (Join-Path $stage 'releases\build\build_msi_installer.ps1') -Version $normalizedVersion -OutputDir $OutputDir
    & python (Join-Path $PSScriptRoot 'package_release.py') finalize --stage-dir $stage --output-dir $OutputDir --require-installers
    if ($LASTEXITCODE -ne 0) { throw 'Release asset validation failed.' }
    Write-Host "All release formats built and checksummed in $OutputDir"
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
