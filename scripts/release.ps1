<#
.SYNOPSIS
  Builds a release: the C++ launcher (Release, tested) goes inside the installer, next to the window, where
  src-tauri/src/engine.rs looks for it; the installer is signed for the app's updater. Nothing is published: the
  files to attach to the GitHub release land in release\v<version>\:

    MultiplayerEverything-setup.exe    the installer (what the app's updater and the C++ hand-over download)
    latest.json                        the updater's feed: version, notes, the installer's URL and signature
    MultiplayerEverythingLauncher.exe  the C++ launcher alone, for players on 1.0.41 and older: their launcher
                                       updates itself to this file, which then installs the app (launcher.json
                                       "updates": "app", "appExe")

  The updater's private key is read from $env:TAURI_SIGNING_PRIVATE_KEY (the key or its path), else from
  ~\.tauri\multiplayer-everything.key. It never goes into the repository. Its public half is in
  src-tauri/tauri.conf.json; a release signed with any other key is refused by every installed app.
.PARAMETER Notes
  The release notes shown in latest.json (and to paste into the GitHub release).
.EXAMPLE
  .\scripts\release.ps1 -Notes 'The new launcher, and it updates itself.'
#>
param(
  [string]$Engine = (Join-Path $PSScriptRoot '..\old'),
  [string]$Notes = '',
  [string]$Repo = 'multiplayereverything/multiplayer-mods-launcher'
)
$ErrorActionPreference = 'Stop'
$tauri = Split-Path $PSScriptRoot

# One version for both: build.rs refuses a build where they differ, so read it once here.
$conf = Get-Content (Join-Path $tauri 'src-tauri\tauri.conf.json') -Raw | ConvertFrom-Json
$version = $conf.version
$cmake = Select-String -Path (Join-Path $Engine 'CMakeLists.txt') -Pattern 'project\(\S+ VERSION ([0-9.]+)' | Select-Object -First 1
if (-not $cmake -or $cmake.Matches[0].Groups[1].Value -ne $version) { throw "tauri.conf.json says $version but old/CMakeLists.txt doesn't; give both the same version" }

if (-not $env:TAURI_SIGNING_PRIVATE_KEY) {
  $key = Join-Path $HOME '.tauri\multiplayer-everything.key'
  if (-not (Test-Path $key)) { throw "No updater key: set TAURI_SIGNING_PRIVATE_KEY or put the key at $key" }
  $env:TAURI_SIGNING_PRIVATE_KEY = $key
}
if ($null -eq $env:TAURI_SIGNING_PRIVATE_KEY_PASSWORD) { $env:TAURI_SIGNING_PRIVATE_KEY_PASSWORD = '' }

& (Join-Path $Engine 'build.ps1') -Test   # the engine ships only when its own tests pass
if ($LASTEXITCODE) { throw "The C++ launcher's build or tests failed" }
$exe = Join-Path $Engine 'build\Release\MultiplayerEverythingLauncher.exe'
$stage = Join-Path $tauri 'src-tauri\engine'
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage -Force
Push-Location $tauri
try {
  npm ci
  if ($LASTEXITCODE) { throw 'npm ci failed' }
  npm test
  if ($LASTEXITCODE) { throw 'npm test failed' }
  npx tauri build --config src-tauri/tauri.release.json
  if ($LASTEXITCODE) { throw 'tauri build failed' }
} finally { Pop-Location }

$nsis = Join-Path $tauri 'src-tauri\target\release\bundle\nsis'
$setup = Get-ChildItem $nsis -Filter "*_${version}_x64-setup.exe" | Select-Object -First 1
if (-not $setup) { throw "No installer for $version in $nsis" }
$sig = "$($setup.FullName).sig"
if (-not (Test-Path $sig)) { throw "No signature next to $($setup.Name) (createUpdaterArtifacts in tauri.release.json?)" }

# GitHub turns spaces in file names into dots, and the hand-over looks for one fixed name; the signature is of the
# file's bytes, so the copy keeps it.
$out = Join-Path $tauri "release\v$version"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $setup.FullName (Join-Path $out 'MultiplayerEverything-setup.exe')
Copy-Item $exe (Join-Path $out 'MultiplayerEverythingLauncher.exe')
$feed = [ordered]@{
  version   = $version
  notes     = $Notes
  pub_date  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  platforms = [ordered]@{
    'windows-x86_64' = [ordered]@{
      signature = (Get-Content $sig -Raw).Trim()
      url       = "https://github.com/$Repo/releases/download/v$version/MultiplayerEverything-setup.exe"
    }
  }
}
[IO.File]::WriteAllText((Join-Path $out 'latest.json'), ($feed | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
Write-Host "Release files for v$version in $out (not published):"
Get-ChildItem $out | ForEach-Object { Write-Host "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
