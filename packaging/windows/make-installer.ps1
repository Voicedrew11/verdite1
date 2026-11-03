# Makes dist\Verdite1-VERSION-win-x64-setup.exe from dist\Verdite1-VERSION-win-x64.zip, with Inno
# Setup's iscc (on a Windows machine or under wine; release.yml's windows job).
#   packaging\windows\make-installer.ps1 -Version 0.1.0
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$name = "Verdite1-$Version-win-x64"
$unpacked = Join-Path $root "build\installer"
if (Test-Path $unpacked) { Remove-Item $unpacked -Recurse -Force }
Expand-Archive (Join-Path $root "dist\$name.zip") $unpacked

$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $iscc) { $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $iscc)) {
    choco install innosetup --no-progress -y
    if ($LASTEXITCODE) { throw "cannot install Inno Setup" }
}

$env:VERDITE_VERSION = $Version
$env:VERDITE_STAGE = Join-Path $unpacked $name
$env:VERDITE_OUT = Join-Path $root "dist"
& $iscc /Q (Join-Path $PSScriptRoot "verdite1.iss")
if ($LASTEXITCODE) { throw "iscc failed" }
