# Build a port's Windows x64 package.
#
# Run through the game's packaging/windows/build-windows.ps1, or directly; the game
# is the checkout this subtree sits in (tools/verdite-core), or $env:VERDITE_GAME_ROOT.
# Its packaging/package.env names it (NAME, APP_ID, INNO_APP_ID, and INNO_PRIVILEGES
# when it is not "admin"), and packaging/shared/ holds its $APP_ID.ico if it ships one.
#
# Needs the RecompOne subtree built (the game's scripts/setup_tools.sh) and the
# .NET 10 SDK. It does NOT need the disc: the launcher carries the inputs to a
# build and makes the game on the player's machine.
#
# Produces dist/<NAME>-<version>-win-x64.zip, and the Inno Setup installer too if
# iscc is on PATH. The zip's layout is <NAME>.exe (a tiny stub) + bin/ (the
# self-contained runtime) + content/ + licenses/; see "What the release contains"
# in the game's docs/PACKAGING.md.
$ErrorActionPreference = 'Stop'

if ($env:VERDITE_GAME_ROOT) { $root = Resolve-Path $env:VERDITE_GAME_ROOT }
else { $root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..') }

$envFile = Join-Path $root 'packaging\package.env'
if (-not (Test-Path $envFile)) { throw "no packaging\package.env under $root" }
$pkg = @{}
foreach ($line in Get-Content $envFile) {
    if ($line -match '^\s*([A-Z_]+)=(.*)$') { $pkg[$Matches[1]] = $Matches[2].Trim().Trim('"') }
}
$name = $pkg['NAME']; $appId = $pkg['APP_ID']
if (-not $name -or -not $appId) { throw "package.env must set NAME and APP_ID" }

$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'win-x64'
$csproj = Join-Path $root "$name.Launcher\$name.Launcher.csproj"
$icon = Join-Path $root "packaging\shared\$appId.ico"

# One rule for the number, for everything that names a build, as scripts/version.sh
# gives it and the launcher's csproj resolves it: VERSION, else $env:VERDITE_VERSION,
# else the newest vMAJOR.MINOR.PATCH tag reachable from HEAD, else 0.0.0. So the
# zip and the installer cannot be named something other than what is inside them.
$versionFile = Join-Path $root 'VERSION'
if (Test-Path $versionFile) { $version = (Get-Content $versionFile -Raw).Trim() }
elseif ($env:VERDITE_VERSION) { $version = $env:VERDITE_VERSION -replace '^v', '' }
else {
    $tag = $null
    try {
        $tag = git -C "$root" tag --merged HEAD --list 'v*' --sort=-v:refname 2>$null |
            Where-Object { $_ -match '^v\d+\.\d+\.\d+$' } | Select-Object -First 1
    } catch { }
    $version = if ($tag) { $tag.Substring(1) } else { '0.0.0' }
}
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "not MAJOR.MINOR.PATCH: $version" }

Write-Host "==> publishing win-x64 ($version)"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# The self-contained apphost has to sit next to its DLLs, so the whole publish
# lands in bin/ and a tiny Framework stub at the stage root is what Explorer
# and the installer shortcuts launch. content/ is lifted next to that stub so
# the payload is not mixed in with the runtime.
$bin = Join-Path $stage 'bin'
dotnet publish $csproj -c Release -r win-x64 --self-contained `
    -p:DebugType=none -p:DebugSymbols=false -o $bin
if ($LASTEXITCODE -ne 0) { throw "publish failed" }

$content = Join-Path $bin 'content'
if (-not (Test-Path $content)) { throw "publish did not stage content/" }
Move-Item $content (Join-Path $stage 'content')

# The csproj also copies LICENSE next to the published exe; the licence tree
# below is the one the zip carries, so do not leave a second copy in bin/.
$publishedLicense = Join-Path $bin 'LICENSE'
if (Test-Path $publishedLicense) { Remove-Item $publishedLicense }

Write-Host "==> stub"
$stubProj = Join-Path $PSScriptRoot 'Stub\Verdite.Stub.csproj'
$stubOut = Join-Path $dist 'stub'
if (Test-Path $stubOut) { Remove-Item -Recurse -Force $stubOut }
# A port that ships no mark gets a stub with none: Windows draws its plain
# program icon until the game points the shortcuts at the card icon it read.
$stubArgs = @("-p:StubName=$name")
if (Test-Path $icon) { $stubArgs += "-p:StubIcon=$icon" }
dotnet publish $stubProj -c Release -o $stubOut @stubArgs
if ($LASTEXITCODE -ne 0) { throw "stub publish failed" }
Copy-Item (Join-Path $stubOut "$name.exe") (Join-Path $stage "$name.exe")
$stubConfig = Join-Path $stubOut "$name.exe.config"
if (Test-Path $stubConfig) {
    Copy-Item $stubConfig (Join-Path $stage "$name.exe.config")
}
Remove-Item -Recurse -Force $stubOut

# Third-party licences the artifact is obliged to carry. Noto Sans is embedded in
# RecompOne.Runtime.dll (tools/RecompOne/patches/0033) and is SIL OFL 1.1, which
# requires its licence to travel with the font; the port's own MIT terms go beside
# it rather than only in the source tree.
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $licenses 'LICENSE')
Copy-Item (Join-Path $root 'tools\RecompOne\patches\assets\NotoSans-OFL.txt') `
    (Join-Path $licenses 'NotoSans-OFL.txt')

$zip = Join-Path $dist "$name-$version-win-x64.zip"
Write-Host "==> $zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip

# The installer takes every name from here; it has no literal of its own.
$env:VERDITE_VERSION = $version
$env:VERDITE_NAME = $name
$env:VERDITE_INNO_APP_ID = $pkg['INNO_APP_ID']
$env:VERDITE_INNO_PRIVILEGES = $pkg['INNO_PRIVILEGES']
$env:VERDITE_ROOT = "$root"

if (Get-Command iscc -ErrorAction SilentlyContinue) {
    Write-Host "==> installer"
    iscc (Join-Path $PSScriptRoot 'verdite.iss')
    if ($LASTEXITCODE -ne 0) { throw "iscc failed" }
} else {
    Write-Host "==> iscc not on PATH; skipping the installer (the zip is built)"
}

Write-Host "done."
