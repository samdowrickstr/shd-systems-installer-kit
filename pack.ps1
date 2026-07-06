# pack.ps1 — PlannerDay Installer Kit packager.
#
# Reads a project's installer.json, embeds its runtime config + branding into the
# generic AppSetup.exe, stages the payload (your already-built app exes + the Qt
# runtime), and produces ONE self-contained Installer.exe (a native Win32 stub
# with the whole bundle embedded as a tar.gz), plus an optional portable .zip.
#
#   pwsh pack.ps1                       # uses ./installer.json
#   pwsh pack.ps1 -Config path.json     # explicit config
#
# windeployqt prints harmless warnings to stderr; don't let them abort the run.
param(
    [string]$Config = "installer.json"
)
$ErrorActionPreference = "Stop"

$kitRoot = $PSScriptRoot
$cfgPath = if ([System.IO.Path]::IsPathRooted($Config)) { $Config } else { Join-Path (Get-Location) $Config }
if (-not (Test-Path -LiteralPath $cfgPath)) {
    throw "Config not found: $cfgPath  (copy installer.example.json to installer.json and edit it)"
}
$cfgDir = Split-Path -Parent (Resolve-Path -LiteralPath $cfgPath)
$cfg = Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json

# Resolve a path from installer.json relative to the config file's folder.
function Resolve-CfgPath([string]$p) {
    if ([string]::IsNullOrWhiteSpace($p)) { return $null }
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return [System.IO.Path]::GetFullPath((Join-Path $cfgDir $p))
}

# --- Qt / MinGW toolchain ---------------------------------------------------
$qt    = $cfg.qt.dir
$mingw = $cfg.qt.mingw
if (-not (Test-Path -LiteralPath $qt))    { throw "Qt dir not found: $qt" }
if (-not (Test-Path -LiteralPath $mingw)) { throw "MinGW dir not found: $mingw" }
$env:Path = "$qt\bin;$mingw;" + $env:Path

# --- version ----------------------------------------------------------------
$baseVer = ""
if ($cfg.version.literal) {
    $baseVer = "$($cfg.version.literal)"
} elseif ($cfg.version.file) {
    $vf = Resolve-CfgPath $cfg.version.file
    if (-not (Test-Path -LiteralPath $vf)) { throw "version file not found: $vf" }
    $baseVer = (Get-Content -LiteralPath $vf -Raw).Trim()
}
if (-not $baseVer) { throw "No version: set version.literal or version.file in installer.json" }
$baseVer = ($baseVer -replace '\+.*$','')
$appendMeta = -not ($cfg.version.appendBuildMetadata -eq $false)
$buildMetadata = "build.$((Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmss'))"
$ver = if ($appendMeta) { "$baseVer+$buildMetadata" } else { $baseVer }

# --- folders ----------------------------------------------------------------
$appName  = $cfg.product.appName
$prefix   = if ($cfg.output.namePrefix) { $cfg.output.namePrefix } else { ($appName -replace '\s+','-') }
$build    = Join-Path $kitRoot "build"
$stage    = Join-Path $build "stage\$appName"
$distDir  = if ($cfg.output.distDir) { Resolve-CfgPath $cfg.output.distDir } else { Join-Path $cfgDir "dist" }
$res      = Join-Path $kitRoot "resources"
# Build the installer GUI (uses windres) in a path WITHOUT spaces.
$instBuild = Join-Path $env:TEMP "plannerday-installer-build"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
New-Item -ItemType Directory -Force -Path $build | Out-Null

Write-Host "[1/7] Writing branding + runtime config..."
# Logo (SVG only) and icon into the resources the GUI embeds.
$logo = Resolve-CfgPath $cfg.product.logo
$icon = Resolve-CfgPath $cfg.product.icon
if ($logo) {
    if ([System.IO.Path]::GetExtension($logo).ToLower() -ne ".svg") {
        Write-Warning "Logo should be an .svg — got '$logo'. Using it anyway as logo.svg."
    }
    Copy-Item -LiteralPath $logo -Destination (Join-Path $res "logo.svg") -Force
}
if ($icon) {
    Copy-Item -LiteralPath $icon -Destination (Join-Path $res "app.ico") -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $res "app.ico"))) {
    throw "resources/app.ico missing — set product.icon in installer.json"
}

# Runtime config.json (only the fields the GUI reads at runtime).
$apps = @()
foreach ($a in $cfg.apps) {
    $apps += [ordered]@{
        exe         = $a.exe
        name        = if ($a.name) { $a.name } else { $a.exe }
        description = "$($a.description)"
        default     = -not ($a.default -eq $false)
    }
}
$runtime = [ordered]@{
    appName     = $appName
    displayName = if ($cfg.product.displayName) { $cfg.product.displayName } else { $appName }
    subtitle    = if ($cfg.product.subtitle) { "$($cfg.product.subtitle)" } else { "SETUP" }
    publisher   = "$($cfg.product.publisher)"
    version     = $ver
    registryKey = $cfg.product.registryKey
    accentColor = if ($cfg.product.accentColor) { $cfg.product.accentColor } else { "#1CA3C2" }
    options     = [ordered]@{
        desktopShortcut  = -not ($cfg.output.desktopShortcut -eq $false)
        startMenuShortcut = -not ($cfg.output.startMenuShortcut -eq $false)
    }
    apps        = $apps
}
$runtime | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $res "config.json") -Encoding UTF8

Write-Host "[2/7] Building installer GUI (AppSetup.exe)..."
cmake -S $kitRoot -B $instBuild -G "MinGW Makefiles" "-DCMAKE_PREFIX_PATH=$qt" -DCMAKE_BUILD_TYPE=Release | Out-Null
cmake --build $instBuild -j | Out-Null
$setupExe = Join-Path $instBuild "AppSetup.exe"
if (-not (Test-Path -LiteralPath $setupExe)) { throw "AppSetup.exe was not built" }

Write-Host "[3/7] Staging payload + deploying Qt..."
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($src in $cfg.payload.sources) {
    $s = Resolve-CfgPath $src
    if (-not (Test-Path -LiteralPath $s)) { throw "payload source not found: $s" }
    if ((Get-Item -LiteralPath $s).PSIsContainer) {
        Copy-Item -LiteralPath $s -Destination $stage -Recurse -Force
    } else {
        Copy-Item -LiteralPath $s -Destination $stage -Force
    }
}
Copy-Item -LiteralPath $setupExe -Destination $stage -Force

if (-not ($cfg.payload.windeployqt -eq $false)) {
    foreach ($a in $cfg.apps) {
        $exePath = Join-Path $stage $a.exe
        if (-not (Test-Path -LiteralPath $exePath)) {
            Write-Warning "app exe not in payload, skipping windeployqt: $($a.exe)"
            continue
        }
        if ($a.qmlDir) {
            $qml = Resolve-CfgPath $a.qmlDir
            & "$qt\bin\windeployqt.exe" --release --compiler-runtime --qmldir "$qml" "$exePath" | Out-Null
        } else {
            & "$qt\bin\windeployqt.exe" --release --compiler-runtime "$exePath" | Out-Null
        }
    }
    & "$qt\bin\windeployqt.exe" --release (Join-Path $stage "AppSetup.exe") | Out-Null
}

Write-Host "[4/7] Archiving payload (tar.gz preserves folders)..."
$zip = Join-Path $build "payload.tgz"
if (Test-Path $zip) { Remove-Item $zip -Force }
& tar.exe -czf $zip -C $stage "."

Write-Host "[5/7] Generating + compiling bootstrap resource..."
$manifest = (Join-Path $res "setup.manifest") -replace '\\','/'
$iconFwd  = (Join-Path $res "app.ico")         -replace '\\','/'
$zipFwd   = $zip                                -replace '\\','/'
$rc = @"
#include <windows.h>
CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST "$manifest"
IDI_ICON1 ICON "$iconFwd"
101 RCDATA "$zipFwd"
"@
$rcPath = Join-Path $build "bootstrap.rc"
Set-Content -LiteralPath $rcPath -Value $rc -Encoding ASCII
$rcObj = Join-Path $build "bootstrap_rc.o"
& "$mingw\windres.exe" $rcPath -O coff -o $rcObj

# Bake the product title into the stub (avoids command-line quoting of spaces).
$genH = Join-Path $build "bootstrap_gen.h"
$titleEsc = ($appName -replace '"','\"')
Set-Content -LiteralPath $genH -Encoding ASCII -Value @"
#define SETUP_TITLE L"$titleEsc Setup"
#define SETUP_EXE   L"AppSetup.exe"
"@

Write-Host "[6/7] Linking standalone installer stub..."
$out       = Join-Path $distDir "$prefix-v$ver-Installer.exe"
$latestOut = Join-Path $distDir "$prefix-Installer.exe"
if (Test-Path $out) { Remove-Item $out -Force }
& "$mingw\g++.exe" (Join-Path $kitRoot "src\bootstrap.cpp") $rcObj `
    "-I$build" `
    -DUNICODE -D_UNICODE -O2 -s -mwindows -static -static-libgcc -static-libstdc++ `
    -o $out
Copy-Item -LiteralPath $out -Destination $latestOut -Force

$makePortable = -not ($cfg.output.portableZip -eq $false)
$portZip = $null
if ($makePortable) {
    Write-Host "[7/7] Building portable zip (app only, no installer)..."
    $portRoot = Join-Path $build "portable"
    if (Test-Path $portRoot) { Remove-Item $portRoot -Recurse -Force }
    $portApp = Join-Path $portRoot $appName
    New-Item -ItemType Directory -Force -Path $portApp | Out-Null
    Copy-Item "$stage\*" $portApp -Recurse
    Remove-Item (Join-Path $portApp "AppSetup.exe") -Force -ErrorAction SilentlyContinue
    $portZip = Join-Path $distDir "$prefix-v$ver-portable.zip"
    if (Test-Path $portZip) { Remove-Item $portZip -Force }
    Compress-Archive -Path $portApp -DestinationPath $portZip
} else {
    Write-Host "[7/7] Skipping portable zip (output.portableZip = false)."
}

$mb = [math]::Round((Get-Item $out).Length/1MB,1)
Write-Host ""
Write-Host "Done."
Write-Host "Standalone installer: $out  ($mb MB)"
Write-Host "Latest installer:     $latestOut"
if ($portZip) {
    $pmb = [math]::Round((Get-Item $portZip).Length/1MB,1)
    Write-Host "Portable zip:         $portZip  ($pmb MB)"
}
