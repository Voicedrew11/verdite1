# Builds the Windows release zip from the "windows" CMake preset build.
#   powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1 -Version 0.1.0-pre.1
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = "build\windows",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$name = "Verdite1-v$Version-windows"
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null

foreach ($file in "verdite1.exe", "SDL3.dll") {
    $path = Join-Path $BuildDir $file
    if (-not (Test-Path $path)) { throw "Missing $path - build the 'windows' preset first." }
    Copy-Item $path $stage
}
Copy-Item "docs\release\README.txt" $stage
Copy-Item "docs\MODDING.md" $stage
Copy-Item "mods" (Join-Path $stage "mods") -Recurse

$licenses = New-Item -ItemType Directory (Join-Path $stage "licenses")
Copy-Item "LICENSE" (Join-Path $licenses "Verdite1-MIT.txt")
Copy-Item "third_party\imgui\LICENSE.txt" (Join-Path $licenses "DearImGui.txt")
Copy-Item "third_party\lua\LICENSE" (Join-Path $licenses "Lua.txt")
Copy-Item "third_party\cjson\LICENSE" (Join-Path $licenses "cJSON.txt")
Copy-Item "third_party\libchdr\LICENSE.txt" (Join-Path $licenses "libchdr.txt")
Copy-Item "third_party\libchdr\deps\zstd-1.5.7\LICENSE" (Join-Path $licenses "zstd.txt")
$sdl = Get-ChildItem $BuildDir -Directory -Filter "SDL3-*" | Select-Object -First 1
if ($sdl) { Copy-Item (Join-Path $sdl.FullName "LICENSE.txt") (Join-Path $licenses "SDL3.txt") }
else { Write-Warning "SDL3 license not found in $BuildDir; add it to licenses\SDL3.txt by hand." }

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
# Windows' bsdtar writes portable zip paths (Compress-Archive in PowerShell 5 uses backslashes).
$tar = Join-Path $env:SystemRoot "System32/tar.exe"   # not Git's GNU tar, which cannot write zips
$zipFull = Join-Path (Resolve-Path -LiteralPath $OutDir) "$name.zip"
& $tar -a -c -f $zipFull -C $OutDir $name
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
Write-Host "Created $zip"
