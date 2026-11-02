<#
.SYNOPSIS
  Configures, builds and (optionally) tests the launcher with MSVC + Ninja.
.EXAMPLE
  .\build.ps1                 # Release build
  .\build.ps1 -Test           # build and run all tests
  .\build.ps1 -Config Debug
  .\build.ps1 -Dist           # build, then make dist\MultiplayerEverythingLauncher\ and its .zip
#>
param(
  [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
  [switch]$Test,
  [switch]$Clean,
  [switch]$Dist
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root "build\$Config"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (Build Tools) 2019+ with the C++ workload is required.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with MSVC x64 tools was found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$steps = @(
  "cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config",
  "cmake --build `"$build`""
)
if ($Test) { $steps += "ctest --test-dir `"$build`" --output-on-failure" }
$cmd = "`"$vcvars`" >nul 2>nul && " + ($steps -join ' && ')
# Native tools write progress to stderr; don't let PowerShell treat that as fatal.
$ErrorActionPreference = 'Continue'
cmd /c "$cmd 2>&1"
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($code -ne 0) { throw "Build or tests failed (exit $code)" }

$exe = Join-Path $build 'MultiplayerEverythingLauncher.exe'
Write-Host "Built $exe"

if ($Dist) {
  # The exe carries launcher.json and the covers inside it, so it is all people
  # need; the folder also gets the editable copies next to it.
  # Game SWFs and player binaries are never shipped.
  $out = Join-Path $root 'dist\MultiplayerEverythingLauncher'
  if (Test-Path $out) { Remove-Item -Recurse -Force $out }
  New-Item -ItemType Directory -Force $out | Out-Null
  Copy-Item $exe $out
  Copy-Item (Join-Path $root '..\launcher\*') $out -Recurse  # launcher\ is shared with the Tauri app, one level up
  Get-ChildItem $out -Recurse -Include *.swf, *.exe | Where-Object { $_.Name -ne 'MultiplayerEverythingLauncher.exe' } | ForEach-Object {
    throw "Refusing to package $($_.FullName): game files and player binaries must not be shipped."
  }
  $zip = Join-Path $root 'dist\MultiplayerEverythingLauncher.zip'
  if (Test-Path $zip) { Remove-Item $zip }
  Compress-Archive -Path $out -DestinationPath $zip
  Copy-Item $exe (Join-Path $root 'dist\MultiplayerEverythingLauncher.exe')  # the one-file version
  Write-Host "Packaged $zip and dist\MultiplayerEverythingLauncher.exe"
}
