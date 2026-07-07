# build-demo.ps1 - build the tiny demo app and pack the SHD Systems demo installer.
# Run from anywhere:  .\demo\build-demo.ps1
# Produces demo\SHD-Systems-Demo-Installer.exe (and a versioned copy).
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $here "demo.json") -Raw | ConvertFrom-Json
$mingw = $cfg.qt.mingw
$gpp  = Join-Path $mingw "g++.exe"
if (-not (Test-Path -LiteralPath $gpp)) { throw "g++ not found at $gpp - fix qt.mingw in demo.json" }

Write-Host "Compiling demo-app.exe..."
& $gpp (Join-Path $here "demo-app.cpp") `
    -O2 -s -municode -mwindows -static -static-libgcc -static-libstdc++ `
    -o (Join-Path $here "demo-app.exe")
if ($LASTEXITCODE -ne 0) { throw "demo-app.exe failed to compile" }

Write-Host "Packing installer..."
& (Join-Path $here "..\pack.ps1") -Config (Join-Path $here "demo.json")
