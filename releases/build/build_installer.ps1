param(
    [string]$BuildDir = $PSScriptRoot,
    [string]$ReleaseSuffix = '',
    [string]$VersionOverride = '',
    [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $BuildDir '..\..')).Path
$generatedDir = Join-Path $BuildDir 'generated'
$outputExe = if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    Join-Path $projectRoot 'releases\latest\installer.exe'
} else {
    Join-Path ([System.IO.Path]::GetFullPath($OutputDir)) 'installer.exe'
}
$objFile = Join-Path $BuildDir 'installer.obj'
$resFile = Join-Path $BuildDir 'installer.res'

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Installer not found: $vswhere"
}

$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsInstall)) {
    throw 'Visual Studio Build Tools not found.'
}
$vsInstall = $vsInstall.Trim()

$vcvars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}

Write-Host 'Preparing generated installer headers...'
& (Join-Path $BuildDir 'prepare_installer.ps1') -ProjectRoot $projectRoot -BuildDir $BuildDir -ReleaseSuffix $ReleaseSuffix -VersionOverride $VersionOverride
if ($LASTEXITCODE -ne 0) {
    throw 'Header generation failed.'
}

New-Item -ItemType Directory -Path (Split-Path -Parent $outputExe) -Force | Out-Null

$compileScript = @"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
cd /d "$BuildDir"
if errorlevel 1 exit /b 1
rc /nologo /fo "$resFile" "$BuildDir\installer.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /utf-8 /MT /DUNICODE /D_UNICODE /I "$generatedDir" /I "$BuildDir" /c /Fo"$objFile" "$BuildDir\installer.cpp"
if errorlevel 1 exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /OUT:"$outputExe" "$objFile" "$resFile" comctl32.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib winhttp.lib user32.lib gdi32.lib comdlg32.lib crypt32.lib dwmapi.lib
if errorlevel 1 exit /b 1
"@

$compileBat = Join-Path $BuildDir 'build_native_tmp.cmd'
Set-Content -LiteralPath $compileBat -Value $compileScript -Encoding ASCII

try {
    Write-Host 'Building native C++ installer...'
    & cmd.exe /c $compileBat
    if ($LASTEXITCODE -ne 0) {
        throw 'Native build failed.'
    }
}
finally {
    Remove-Item -LiteralPath $compileBat -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $objFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $resFile -Force -ErrorAction SilentlyContinue
}

Write-Host "Native installer build finished: $outputExe"
