<# Smoke-test MSI ownership/lifecycle in an isolated fixture, not real PotPlayer. #>
param(
    [Parameter(Mandatory = $true)][string]$MsiPath,
    [Parameter(Mandatory = $true)][string]$BuildInfoPath,
    [string]$LogDir = (Join-Path $env:RUNNER_TEMP 'potplayer-msi-test-logs')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$MsiPath = (Resolve-Path -LiteralPath $MsiPath).Path
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$info = Get-Content -LiteralPath $BuildInfoPath -Raw | ConvertFrom-Json
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('PotPlayer MSI fixture ' + [guid]::NewGuid().ToString('N'))
$target = Join-Path $fixture 'Extension\Subtitle\Translate'
$sentinel = Join-Path $target 'unrelated-translator.txt'
$installed = $false
New-Item -ItemType Directory -Path $target, $LogDir -Force | Out-Null

function Invoke-Msi([string]$Operation, [string]$LogName, [bool]$ExpectSuccess, [string]$Destination = '') {
    $log = Join-Path $LogDir ($LogName + '.log')
    $arguments = @($Operation, ('"' + $MsiPath + '"'), '/qn', '/norestart', '/l*v', ('"' + $log + '"'))
    if ($Destination) { $arguments += ('INSTALLFOLDER="' + $Destination + '"') }
    $process = Start-Process msiexec.exe -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit(180000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "MSI $LogName timed out; see $log"
    }
    $process.Refresh()
    $success = $process.ExitCode -in @(0, 3010)
    if ($success -ne $ExpectSuccess) {
        Get-Content -LiteralPath $log -Tail 80
        throw "MSI $LogName exit code $($process.ExitCode), expected success=$ExpectSuccess; see $log"
    }
    if (-not $ExpectSuccess -and $process.ExitCode -ne 1603) {
        throw "MSI $LogName failed with an unexpected infrastructure error $($process.ExitCode)"
    }
    Write-Host "PASS MSI $LogName (exit $($process.ExitCode))"
}

function Assert-Payload {
    foreach ($property in $info.plugin_sha256.PSObject.Properties) {
        $path = Join-Path $target $property.Name
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $property.Value) {
            throw "Installed script differs from the staged release: $($property.Name)"
        }
        $icon = [IO.Path]::ChangeExtension($property.Name, '.ico')
        if ((Get-FileHash -LiteralPath (Join-Path $target $icon)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $root $icon)).Hash) {
            throw "Installed icon differs from source: $icon"
        }
    }
    if ([IO.File]::ReadAllText($sentinel) -cne 'unrelated plugin must survive') { throw 'Unrelated file changed' }
    Write-Host 'PASS all four installed payload hashes and unrelated sentinel'
}

try {
    [IO.File]::WriteAllText($sentinel, 'unrelated plugin must survive')
    # A fake executable filename is only a path-validation fixture; never run it.
    Invoke-Msi '/i' 'reject-missing-potplayer' $false $target
    [IO.File]::WriteAllBytes((Join-Path $fixture 'PotPlayerMini64.exe'), [byte[]]@(77, 90, 0, 0))
    $foreign = Join-Path $target 'SubtitleTranslate - ChatGPT.as'
    [IO.File]::WriteAllText($foreign, 'existing unmanaged plugin')
    Invoke-Msi '/i' 'reject-unmanaged-file' $false $target
    if ([IO.File]::ReadAllText($foreign) -cne 'existing unmanaged plugin') { throw 'MSI changed an unmanaged plugin' }
    Remove-Item -LiteralPath $foreign
    Invoke-Msi '/i' 'install' $true $target
    $installed = $true
    Assert-Payload
    Remove-Item -LiteralPath (Join-Path $target 'SubtitleTranslate - ChatGPT.as')
    Invoke-Msi '/fa' 'repair' $true
    Assert-Payload
    Invoke-Msi '/x' 'uninstall' $true
    $installed = $false
    foreach ($name in @('SubtitleTranslate - ChatGPT.as', 'SubtitleTranslate - ChatGPT.ico', 'SubtitleTranslate - ChatGPT - Without Context.as', 'SubtitleTranslate - ChatGPT - Without Context.ico')) {
        if (Test-Path -LiteralPath (Join-Path $target $name)) { throw "Uninstall left owned payload: $name" }
    }
    if ([IO.File]::ReadAllText($sentinel) -cne 'unrelated plugin must survive') { throw 'Uninstall changed an unrelated file' }
    if (-not (Test-Path -LiteralPath (Join-Path $fixture 'PotPlayerMini64.exe'))) { throw 'Uninstall removed PotPlayer marker' }
    Write-Host 'PASS MSI lifecycle in synthetic PotPlayer folder; no real PotPlayer or live API was used'
}
finally {
    if ($installed) {
        $cleanup = Start-Process msiexec.exe -ArgumentList @('/x', ('"' + $MsiPath + '"'), '/qn', '/norestart') -Wait -PassThru
        if ($cleanup.ExitCode -notin @(0, 3010)) { Write-Warning "Cleanup uninstall failed: $($cleanup.ExitCode)" }
    }
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
