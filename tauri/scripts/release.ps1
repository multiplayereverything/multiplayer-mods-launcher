<#
.SYNOPSIS
  Builds the installer: the C++ launcher (Release, tested) goes inside it, next to the window, where
  src-tauri/src/engine.rs looks for it. Nothing is published; the installer lands in
  src-tauri/target/release/bundle/nsis.
.EXAMPLE
  .\scripts\release.ps1 -Engine ..\..\flash-launcher-multiplayer
#>
param([string]$Engine = (Join-Path $PSScriptRoot '..\..\..\flash-launcher-multiplayer'))
$ErrorActionPreference = 'Stop'
$tauri = Split-Path $PSScriptRoot
& (Join-Path $Engine 'build.ps1') -Test   # the engine ships only when its own tests pass
$exe = Join-Path $Engine 'build\Release\MultiplayerEverythingLauncher.exe'
$stage = Join-Path $tauri 'src-tauri\engine'
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage -Force
Push-Location $tauri
try {
  npm ci
  npm test
  npx tauri build --config src-tauri/tauri.release.json
} finally { Pop-Location }
