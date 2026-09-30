[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# MSI only compares three numeric fields. Keep the full version in ProductName
# and ProductCode, and explicitly block different releases with the same base.
$versionMatch = [regex]::Match($Version, '^v?(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\z')
if (-not $versionMatch.Success) {
    throw 'MSI Version must be MAJOR.MINOR.PATCH with an optional SemVer prerelease label and optional v prefix. Build metadata, leading-zero numeric fields and fourth fields are unsupported.'
}
if ($Version.Length -gt 128) { throw 'The MSI release version must not exceed 128 characters.' }
foreach ($identifier in ($versionMatch.Groups[4].Value -split '\.')) {
    if ($identifier -match '^0[0-9]+$') { throw 'Numeric SemVer prerelease identifiers cannot have leading zeros.' }
}
$major = [int]$versionMatch.Groups[1].Value
$minor = [int]$versionMatch.Groups[2].Value
$patch = [int]$versionMatch.Groups[3].Value
if ($major -gt 255 -or $minor -gt 255 -or $patch -gt 65535) {
    throw 'MSI ProductVersion requires MAJOR <= 255, MINOR <= 255 and PATCH <= 65535.'
}
$productVersion = "$major.$minor.$patch"
$releaseVersion = $Version -creplace '^v', ''
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'MSI packaging requires Windows, a .NET SDK, and Visual Studio x64 C++ Build Tools.'
}
if (-not [IO.Path]::IsPathFullyQualified($OutputDir)) {
    throw 'OutputDir must be an absolute path.'
}
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$msiSource = Join-Path $projectRoot 'installer\msi'
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$outputMsi = Join-Path $OutputDir 'installer.msi'
$wixVersion = '5.0.2'

$requiredFiles = @(
    'SubtitleTranslate - ChatGPT.as',
    'SubtitleTranslate - ChatGPT.ico',
    'SubtitleTranslate - ChatGPT - Without Context.as',
    'SubtitleTranslate - ChatGPT - Without Context.ico',
    'icon.ico', 'LICENSE',
    'installer\msi\Package.wxs', 'installer\msi\Package.en-us.wxl',
    'installer\msi\TargetValidation.cpp'
)
foreach ($relative in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $relative) -PathType Leaf)) {
        throw "Missing staged MSI input: $relative"
    }
}
foreach ($scriptName in @('SubtitleTranslate - ChatGPT.as', 'SubtitleTranslate - ChatGPT - Without Context.as')) {
    $scriptText = [IO.File]::ReadAllText((Join-Path $projectRoot $scriptName))
    $scriptVersion = [regex]::Match($scriptText, 'string\s+GetVersion\s*\(\s*\)\s*\{\s*return\s+"([^"]+)"\s*;')
    if (-not $scriptVersion.Success -or $scriptVersion.Groups[1].Value -cne $releaseVersion) {
        throw "Stage and version-stamp $scriptName to $releaseVersion before building MSI. Source files are never rewritten by this builder."
    }
}
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw 'dotnet SDK was not found. CI should use actions/setup-dotnet (8.0.x or newer).'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "Visual Studio Installer was not found: $vswhere"
}
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($vsInstall -join ''))) {
    throw 'Visual Studio x64 C++ Build Tools were not found.'
}
$vcvars = Join-Path (($vsInstall -join '').Trim()) 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "Missing $vcvars" }

function Get-ProductCode([string]$FullVersion) {
    # RFC 4122 UUID v5 of the full version in our UpgradeCode namespace. A rebuild of
    # one release must not silently become a second same-version MSI product.
    [byte[]]$namespace = ([guid]'6c47a458-771e-5b7d-9cdf-880cdec12830').ToByteArray()
    [Array]::Reverse($namespace, 0, 4)
    [Array]::Reverse($namespace, 4, 2)
    [Array]::Reverse($namespace, 6, 2)
    $sha = [Security.Cryptography.SHA1]::Create()
    try {
        [byte[]]$hash = $sha.ComputeHash([byte[]]($namespace + [Text.Encoding]::UTF8.GetBytes($FullVersion)))
    } finally { $sha.Dispose() }
    [byte[]]$guidBytes = $hash[0..15]
    $guidBytes[6] = ($guidBytes[6] -band 0x0f) -bor 0x50
    $guidBytes[8] = ($guidBytes[8] -band 0x3f) -bor 0x80
    [Array]::Reverse($guidBytes, 0, 4)
    [Array]::Reverse($guidBytes, 4, 2)
    [Array]::Reverse($guidBytes, 6, 2)
    return ([guid]::new($guidBytes)).ToString('D').ToUpperInvariant()
}

function Write-LicenseRtf([string]$Text, [string]$Destination) {
    # Embed the repository's complete, unmodified license wording in the MSI UI.
    $body = [Text.StringBuilder]::new()
    foreach ($character in $Text.Replace("`r`n", "`n").Replace("`r", "`n").ToCharArray()) {
        $code = [int]$character
        if ($code -eq 10) { [void]$body.Append('\par' + "`r`n") }
        elseif ($code -eq 9) { [void]$body.Append('\tab ') }
        elseif ($character -eq '\' -or $character -eq '{' -or $character -eq '}') {
            [void]$body.Append('\').Append($character)
        }
        elseif ($code -ge 32 -and $code -le 126) { [void]$body.Append($character) }
        else {
            if ($code -gt 32767) { $code -= 65536 }
            [void]$body.Append('\u').Append($code).Append('?')
        }
    }
    [IO.File]::WriteAllText($Destination, '{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\f0\fs18 ' + $body.ToString() + '}', [Text.Encoding]::ASCII)
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('potplayer-msi-' + [guid]::NewGuid().ToString('N'))
$previousRollForward = $env:DOTNET_ROLL_FORWARD
$previousNugetPackages = $env:NUGET_PACKAGES
New-Item -ItemType Directory -Path $work -Force | Out-Null
Push-Location $work
try {
    # Restrict restore to official NuGet. Both CLI and UI extension are pinned;
    # caches/tools live only in this temporary build, never the user's globals.
    $config = Join-Path $work 'NuGet.Config'
    [IO.File]::WriteAllText($config, '<?xml version="1.0" encoding="utf-8"?><configuration><packageSources><clear/><add key="nuget.org" value="https://api.nuget.org/v3/index.json"/></packageSources></configuration>')
    $env:DOTNET_ROLL_FORWARD = 'Major'
    $env:NUGET_PACKAGES = Join-Path $work 'nuget-packages'
    $toolDir = Join-Path $work 'tools'
    & dotnet tool install wix --version $wixVersion --tool-path $toolDir --configfile $config --no-cache
    if ($LASTEXITCODE -ne 0) { throw 'Installing the pinned WiX build tool failed.' }
    $wix = Join-Path $toolDir 'wix.exe'
    & $wix extension add "WixToolset.UI.wixext/$wixVersion"
    if ($LASTEXITCODE -ne 0) { throw 'Restoring the pinned WiX UI extension failed.' }

    # The DLL is embedded in the MSI Binary table; it is not an installed file.
    $validationDll = Join-Path $work 'TargetValidation.dll'
    $compileScript = Join-Path $work 'compile.cmd'
    $compileText = @"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /utf-8 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /LD "$msiSource\TargetValidation.cpp" /Fo"$work\TargetValidation.obj" /link /MACHINE:X64 /OUT:"$validationDll" /IMPLIB:"$work\TargetValidation.lib" msi.lib
if errorlevel 1 exit /b 1
"@
    # UTF-8 code page lets a Unicode checkout path survive the temporary cmd file.
    [IO.File]::WriteAllText($compileScript, "@chcp 65001 >nul`r`n" + $compileText, [Text.UTF8Encoding]::new($false))
    & cmd.exe /d /c $compileScript
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $validationDll)) {
        throw 'Building the read-only x64 MSI target validator failed.'
    }

    $licenseRtf = Join-Path $work 'License.rtf'
    Write-LicenseRtf ([IO.File]::ReadAllText((Join-Path $projectRoot 'LICENSE'))) $licenseRtf
    $temporaryMsi = Join-Path $work 'installer.msi'
    $arguments = @(
        'build', (Join-Path $msiSource 'Package.wxs'),
        '-loc', (Join-Path $msiSource 'Package.en-us.wxl'),
        '-arch', 'x64', '-culture', 'en-US',
        '-ext', "WixToolset.UI.wixext/$wixVersion",
        '-d', "ProjectRoot=$projectRoot",
        '-d', "ProductVersion=$productVersion",
        '-d', "ReleaseVersion=$releaseVersion",
        '-d', "ProductCode=$(Get-ProductCode $releaseVersion)",
        '-d', "ValidationDll=$validationDll",
        '-d', "LicenseRtf=$licenseRtf",
        '-pdbtype', 'none', '-out', $temporaryMsi
    )
    & $wix @arguments
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $temporaryMsi -PathType Leaf)) {
        throw 'WiX MSI build/validation failed.'
    }
    # Query the actual package as an MSI database; a renamed EXE cannot pass.
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase($temporaryMsi, 0)
    $view = $database.OpenView('SELECT `FileName` FROM `File`')
    $view.Execute()
    $installedNames = @()
    while ($null -ne ($record = $view.Fetch())) {
        $installedNames += ($record.StringData(1) -split '\|')[-1]
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
    }
    $view.Close()
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    $expectedNames = $requiredFiles[0..3]
    if ($installedNames.Count -ne 4 -or @(Compare-Object $expectedNames $installedNames).Count -ne 0) {
        throw "The MSI File table does not contain exactly the four expected plugin files: $installedNames"
    }
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    Copy-Item -LiteralPath $temporaryMsi -Destination $outputMsi -Force
    Write-Host "Built offline x64 MSI $releaseVersion (ProductVersion $productVersion): $outputMsi"
}
finally {
    Pop-Location
    $env:DOTNET_ROLL_FORWARD = $previousRollForward
    $env:NUGET_PACKAGES = $previousNugetPackages
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
