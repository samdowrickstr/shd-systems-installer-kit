# configure.ps1 - PlannerDay Installer Kit setup wizard.
#
# Interactively builds an installer.json for a new project: product name,
# version, logo/icon, the app binaries to bundle and the Qt toolchain paths.
# When done, run  .\pack.ps1  to produce the installer.
#
#   .\configure.ps1                    # writes ./installer.json
#   .\configure.ps1 -Output foo.json   # write elsewhere
param(
    [string]$Output = "installer.json"
)
$ErrorActionPreference = "Stop"

function Ask([string]$prompt, [string]$default = "") {
    $suffix = if ($default) { " [$default]" } else { "" }
    $ans = Read-Host ("{0}{1}" -f $prompt, $suffix)
    if ([string]::IsNullOrWhiteSpace($ans)) { return $default }
    return $ans.Trim()
}
function AskYesNo([string]$prompt, [bool]$default = $true) {
    $d = if ($default) { "Y/n" } else { "y/N" }
    $ans = Read-Host ("{0} [{1}]" -f $prompt, $d)
    if ([string]::IsNullOrWhiteSpace($ans)) { return $default }
    return $ans.Trim().ToLower().StartsWith("y")
}

Write-Host ""
Write-Host "PlannerDay Installer Kit - setup wizard" -ForegroundColor Cyan
Write-Host "Answer the prompts; press Enter to accept the [default]." -ForegroundColor DarkGray
Write-Host ""

Write-Host "--- Product ---------------------------------" -ForegroundColor Cyan
$appName     = Ask "Product name (install folder, Add/Remove entry)"
while (-not $appName) { $appName = Ask "Product name (required)" }
$displayName = Ask "Display name shown in the header" $appName
$subtitle    = Ask "Subtitle under the title" "SETUP"
$publisher   = Ask "Publisher / company"
$regDefault  = ($appName -replace '[^A-Za-z0-9]','')
$registryKey = Ask "Registry key id (no spaces)" $regDefault
$accent      = Ask "Accent colour (hex)" "#1CA3C2"
$logo        = Ask "Path to logo (.svg)"
$icon        = Ask "Path to app icon (.ico)"

Write-Host ""
Write-Host "--- Version ---------------------------------" -ForegroundColor Cyan
$verFile = Ask "Path to version.txt (blank to type a literal version)"
$verLit  = ""
if (-not $verFile) { $verLit = Ask "Version string (e.g. 1.0.0)" "1.0.0" }
$appendMeta = AskYesNo "Append +build.<timestamp> metadata?" $true

Write-Host ""
Write-Host "--- Apps to bundle --------------------------" -ForegroundColor Cyan
Write-Host "Add each application. Leave the exe name blank to finish." -ForegroundColor DarkGray
$apps = @()
while ($true) {
    $exe = Ask "  App executable file name (e.g. myapp.exe)"
    if (-not $exe) { break }
    $name = Ask "    Display name" ($exe -replace '\.exe$','')
    $desc = Ask "    Short description (optional)"
    $def  = AskYesNo "    Checked by default?" $true
    $qml  = Ask "    QML source dir for windeployqt (optional)"
    $app  = [ordered]@{ exe = $exe; name = $name; description = $desc; default = $def }
    if ($qml) { $app.qmlDir = $qml }
    $apps += $app
}
if ($apps.Count -eq 0) { throw "No apps configured - at least one is required." }

Write-Host ""
Write-Host "--- Payload (already-built binaries) --------" -ForegroundColor Cyan
Write-Host "List files/folders to copy into the bundle. Blank line to finish." -ForegroundColor DarkGray
$sources = @()
while ($true) {
    $src = Ask "  Source file or folder"
    if (-not $src) { break }
    $sources += $src
}
if ($sources.Count -eq 0) {
    Write-Warning "No payload sources given - remember to add your built exes to installer.json."
}
$deploy = AskYesNo "Run windeployqt to bundle the Qt runtime?" $true
$autoDll = AskYesNo "Auto-detect & bundle native DLL dependencies (for non-Qt apps)?" (-not $deploy)
$dllDirs = @()
if ($autoDll) {
    Write-Host "Extra folders to resolve DLLs from (blank line to finish)." -ForegroundColor DarkGray
    while ($true) {
        $dd = Ask "  DLL search folder"
        if (-not $dd) { break }
        $dllDirs += $dd
    }
}

Write-Host ""
Write-Host "--- Qt toolchain ----------------------------" -ForegroundColor Cyan
$qtDir  = Ask "Qt kit dir (mingw)" "D:/Qt/6.8.3/mingw_64"
$mingw  = Ask "MinGW bin dir" "D:/Qt/Tools/mingw1310_64/bin"

Write-Host ""
Write-Host "--- Output ----------------------------------" -ForegroundColor Cyan
$prefix   = Ask "Installer file name prefix" ($appName -replace '\s+','-')
$distDir  = Ask "Output folder" "dist"
$portable = AskYesNo "Also build a portable .zip?" $true

Write-Host ""
Write-Host "--- Code signing (optional) -----------------" -ForegroundColor Cyan
$signEnabled = AskYesNo "Authenticode-sign the installer (needs Windows SDK signtool)?" $false
$signing = [ordered]@{ enabled = $signEnabled; signtool = ""; certFile = ""; certPasswordEnv = "INSTALLER_CERT_PASSWORD"; thumbprint = ""; timestampUrl = "http://timestamp.digicert.com"; signPayload = $true }
if ($signEnabled) {
    $useThumb = AskYesNo "Use a certificate from the Windows store (by thumbprint)?" $false
    if ($useThumb) {
        $signing.thumbprint = Ask "Certificate SHA1 thumbprint"
    } else {
        $signing.certFile = Ask "Path to .pfx certificate"
        $signing.certPasswordEnv = Ask "Env var holding the .pfx password" "INSTALLER_CERT_PASSWORD"
    }
    $signing.timestampUrl = Ask "Timestamp URL" "http://timestamp.digicert.com"
    $signing.signPayload = AskYesNo "Also sign the bundled app exes?" $true
}

$version = [ordered]@{ appendBuildMetadata = $appendMeta }
if ($verFile) { $version.file = $verFile } else { $version.literal = $verLit }

$installer = [ordered]@{
    product = [ordered]@{
        appName     = $appName
        displayName = $displayName
        subtitle    = $subtitle
        publisher   = $publisher
        registryKey = $registryKey
        accentColor = $accent
        logo        = $logo
        icon        = $icon
    }
    version = $version
    apps    = $apps
    payload = [ordered]@{ sources = $sources; windeployqt = $deploy; autoBundleDlls = $autoDll; dllSearchDirs = $dllDirs }
    qt      = [ordered]@{ dir = $qtDir; mingw = $mingw }
    output  = [ordered]@{ namePrefix = $prefix; distDir = $distDir; portableZip = $portable }
    signing = $signing
}

$outPath = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path (Get-Location) $Output }
$installer | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $outPath -Encoding UTF8

Write-Host ""
Write-Host "Wrote $outPath" -ForegroundColor Green
Write-Host "Review it, then run:  .\pack.ps1 -Config `"$outPath`"" -ForegroundColor Green
