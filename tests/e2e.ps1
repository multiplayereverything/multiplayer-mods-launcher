<#
.SYNOPSIS
  End-to-end checks against the real MultiplayerEverythingLauncher.exe: command line, launching
  with exact arguments, the built-in relay, and the window driven through UI
  Automation (as a screen reader would). Uses a fake player (launcher_tests.exe
  --fake-player) and a fake game folder in %TEMP%; never touches real games.
#>
param(
  [Parameter(Mandatory)] [string]$Launcher,
  [Parameter(Mandatory)] [string]$Root
)
$ErrorActionPreference = 'Stop'
# Animations off, whatever Windows says: checks must not depend on frame timing.
$env:FL_MOTION = '0'
# The first-run name prompt would wait for a person; the lobby section asks for it on purpose.
$env:FL_NO_NAME_PROMPT = '1'
# The website's games would add cards to every test window; the tests use their own games only.
$env:FL_NO_SITE = '1'
$failures = 0
function Check([bool]$ok, [string]$what) {
  if ($ok) { Write-Host "[ OK ] $what" } else { Write-Host "[FAIL] $what"; $script:failures++ }
}
function Run([string[]]$argList) {
  $p = Start-Process -FilePath $Launcher -ArgumentList $argList -Wait -PassThru -NoNewWindow `
         -RedirectStandardOutput "$work\out.txt" -RedirectStandardError "$work\err.txt"
  return @{ Code = $p.ExitCode; Out = (Get-Content "$work\out.txt" -Raw) + (Get-Content "$work\err.txt" -Raw) }
}
function PortOpen([int]$port) {
  $c = New-Object System.Net.Sockets.TcpClient
  try { $c.Connect('127.0.0.1', $port); return $true } catch { return $false } finally { $c.Close() }
}
# Another program can hold the clipboard for a moment; try again instead of failing.
function SetClip([string]$text) {
  for ($i = 0; $i -lt 20; $i++) {
    try { Set-Clipboard -Value $text; return } catch { Start-Sleep -Milliseconds 100 }
  }
  Set-Clipboard -Value $text
}
function WaitFor([scriptblock]$cond, [int]$ms = 5000) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $ms) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 100 }
  return $false
}

# MSAA access to the window's controls (the API screen readers use).
Add-Type -ReferencedAssemblies Accessibility -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Accessibility;

public class AccControl { public IntPtr Hwnd; public string Name; public string Class; }

public static class Acc {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out IAccessible acc);
    static Guid IID = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");

    public static List<AccControl> Controls(IntPtr top) {
        var list = new List<AccControl>();
        EnumChildWindows(top, (h, l) => {
            if (!IsWindowVisible(h)) return true;
            IAccessible a;
            if (AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref IID, out a) != 0 || a == null) return true;
            var c = new AccControl { Hwnd = h };
            try { c.Name = a.get_accName(0) ?? ""; } catch { c.Name = ""; }
            var sb = new StringBuilder(64); GetClassName(h, sb, 64); c.Class = sb.ToString();
            list.Add(c);
            return true;
        }, IntPtr.Zero);
        return list;
    }
    public static void Do(IntPtr h) {
        IAccessible a;
        AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref IID, out a);
        a.accDoDefaultAction(0);
    }
}
'@

# The library is only the cards; a game's buttons live on its page, which Enter on the selected card opens.
function OpenPage([IntPtr]$top) {
  $script:pageGrid = $null
  WaitFor { $script:pageGrid = [Acc]::Controls($top) | Where-Object { $_.Class -eq 'SysListView32' } | Select-Object -First 1; $null -ne $script:pageGrid } 10000 | Out-Null
  if (-not $script:pageGrid) { Check $false 'the game grid is found'; return }
  [Acc]::PostMessage($script:pageGrid.Hwnd, 0x0100, [IntPtr]0x0D, [IntPtr]::Zero) | Out-Null  # WM_KEYDOWN, VK_RETURN
  Check (WaitFor { $null -ne ([Acc]::Controls($top) | Where-Object { $_.Name -eq 'Back to games' } | Select-Object -First 1) }) 'Enter on a card opens its page'
}

$fake = Join-Path (Split-Path $Launcher) 'launcher_tests.exe'
$work = Join-Path $env:TEMP "fl-e2e-$PID"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$work\games", "$work\my games" | Out-Null
Set-Content "$work\games\demo.swf" 'not a real swf'
Set-Content "$work\my games\online demo.swf" 'not a real swf'
$port = 47300 + ($PID % 500)
$config = @"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players",
             "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"], "args": ["--fake-player"] } ],
  "categories": [ { "id": "online", "name": "Online multiplayer" } ],
  "games": [
    { "id": "demo", "name": "Demo Game", "players": "1-2 players", "categories": ["online"],
      "actions": [
        { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\demo.swf" },
        { "id": "host", "label": "Host online", "swf": "`${LAUNCHER_DIR}\\my games\\online demo.swf",
          "args": ["--tcp-connections", "allow"], "relay": { "port": $port } }
      ] },
    { "id": "missing", "name": "Missing Game", "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\nope.swf" } ] }
  ]
}
"@
Set-Content "$work\launcher.json" $config -Encoding UTF8
$env:FL_FAKE_OUT = "$work\args.txt"

# ---- command line ---------------------------------------------------------
$r = Run @('--help'); Check ($r.Code -eq 0 -and $r.Out -match '--launch') '--help prints usage'
$r = Run @('--version'); Check ($r.Code -eq 0 -and $r.Out -match 'Multiplayer Everything Launcher \d+\.\d+\.\d+') "--version prints the version ($($r.Out.Trim()))"
$r = Run @('--bogus'); Check ($r.Code -eq 3) 'unknown option exits 3'
$r = Run @('--validate', '--config', "`"$Root\launcher\launcher.json`""); Check ($r.Code -eq 0 -and $r.Out -match 'OK') 'shipped launcher.json validates'
$r = Run @('--validate', '--config', "`"$work\launcher.json`""); Check ($r.Code -eq 0) 'test config validates'
$r = Run @('--list', '--config', "`"$work\launcher.json`"")
Check ($r.Out -match 'play \(Play\): ready' -and $r.Out -match 'game file missing') '--list shows ready and missing games'
# --json: the same for programs (the Tauri front end reads the library this way).
$r = Run @('--list', '--json', '--config', "`"$work\launcher.json`"")
$lj = try { $r.Out | ConvertFrom-Json } catch { $null }
Check ($null -ne $lj) '--list --json prints JSON'
if ($lj) {
  $demo = $lj.games | Where-Object id -eq 'demo'
  $miss = $lj.games | Where-Object id -eq 'missing'
  Check ((($demo.actions | Where-Object id -eq 'play').state -eq 'ready') -and (($miss.actions | Where-Object id -eq 'play').state -eq 'noFile')) '--list --json gives each action its state'
  Check (($demo.actions | Where-Object id -eq 'host').relayPort -eq $port) '--list --json tells a host action from its relay'
  Check ($lj.text.'status.ready' -eq 'Ready' -and $null -ne $lj.user -and $lj.players[0].id -eq 'fake') '--list --json carries the texts, the user and the players'
}

$env:FL_FAKE_MS = '0'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$r = Run @('--launch', 'demo', '--config', "`"$work\launcher.json`"")
Check ($r.Code -eq 0) '--launch demo starts the player'
Check (WaitFor { Test-Path "$work\args.txt" }) 'player ran'
$args1 = @(Get-Content "$work\args.txt")
Check ($args1.Count -eq 1 -and $args1[0] -like '*\games\demo.swf') "player got the SWF path ($($args1 -join ' | '))"

$r = Run @('--launch', 'missing', '--config', "`"$work\launcher.json`"")
Check ($r.Code -eq 4 -and $r.Out -match 'missing') 'missing game file exits 4 with a reason'

# Host: relay must be up while the game runs, and gone after it exits.
$env:FL_FAKE_MS = '3000'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$host1 = Start-Process -FilePath $Launcher -ArgumentList @('--launch', 'demo:host', '--config', "`"$work\launcher.json`"") -PassThru
Check (WaitFor { PortOpen $port }) "--launch demo:host opens the relay on $port"
$args2 = @()
if (WaitFor { Test-Path "$work\args.txt" }) { $args2 = @(Get-Content "$work\args.txt") }
Check ($args2.Count -eq 3 -and $args2[0] -eq '--tcp-connections' -and $args2[2] -like '*\my games\online demo.swf') "host passes args and a path with spaces ($($args2 -join ' | '))"
$host1.WaitForExit(15000) | Out-Null
Check ($host1.HasExited -and -not (PortOpen $port)) 'relay stops when the hosted game exits'

$relay = Start-Process -FilePath $Launcher -ArgumentList @('--relay', ($port + 1)) -PassThru
Check (WaitFor { PortOpen ($port + 1) }) '--relay runs a bare relay'
Stop-Process -Id $relay.Id -Force

# ---- installing a missing player (served from 127.0.0.1, no internet) -----
$pkg = Join-Path $work 'pkg\FakePlayer'
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item $fake (Join-Path $pkg 'fakeplay.exe')
Compress-Archive -Path $pkg -DestinationPath "$work\fake.zip"  # the archive wraps a top folder
$sha = (Get-FileHash "$work\fake.zip" -Algorithm SHA256).Hash.ToLower()
$httpPort = $port + 2
# A tiny web server: GET /<path> answers with the file www\<path>.
$www = Join-Path $work 'www'
New-Item -ItemType Directory -Force $www | Out-Null
Copy-Item "$work\fake.zip" $www
$server = Start-Job -ArgumentList $httpPort, $www -ScriptBlock {
  param($p, $root)
  $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $p)
  $l.Start()
  for ($i = 0; $i -lt 60; $i++) {
    $c = $l.AcceptTcpClient(); $s = $c.GetStream()
    $buf = New-Object byte[] 8192; $n = $s.Read($buf, 0, $buf.Length)
    $path = ([Text.Encoding]::ASCII.GetString($buf, 0, $n) -split ' ')[1].TrimStart('/') -replace '/', '\'
    $file = Join-Path $root $path
    if (Test-Path $file -PathType Leaf) {
      $bytes = [IO.File]::ReadAllBytes($file)
      $h = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
      $s.Write($h, 0, $h.Length); $s.Write($bytes, 0, $bytes.Length)
    } else {
      $h = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 404 Not Found`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
      $s.Write($h, 0, $h.Length)
    }
    $s.Flush(); $c.Close()
  }
  $l.Stop()
}
Start-Sleep -Milliseconds 800
function InstallConfig([string]$hash) {
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["`${PLAYERS_DIR}\\fake\\fakeplay.exe"],
                 "install": { "url": "http://127.0.0.1:$httpPort/fake.zip", "sha256": "$hash",
                              "dir": "`${PLAYERS_DIR}\\fake", "exe": "fakeplay.exe" } } ],
  "games": [ { "id": "demo", "name": "Demo Game", "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\demo.swf" } ] } ]
}
"@
}
Set-Content "$work\install.json" (InstallConfig $sha) -Encoding UTF8
$r = Run @('--list', '--config', "`"$work\install.json`"")
Check ($r.Out -match 'Fake Player: not found') 'player starts out missing'
$r = Run @('--install-player', 'fake', '--config', "`"$work\install.json`"")
Check ($r.Code -eq 0 -and (Test-Path "$work\players\fake\fakeplay.exe")) "--install-player downloads, checks and unpacks ($($r.Out.Trim() -replace '\s+', ' '))"
$r = Run @('--list', '--config', "`"$work\install.json`"")
Check ($r.Out -match 'play \(Play\): ready, Fake Player') 'the installed player is used right away'
Check (-not (Test-Path "$work\players\fake.installing") -and -not (Test-Path "$work\players\fake.old")) 'no leftovers after installing'

Set-Content "$work\install.json" (InstallConfig ('0' * 64)) -Encoding UTF8
$r = Run @('--install-player', 'fake', '--config', "`"$work\install.json`"")
Check ($r.Code -eq 1 -and $r.Out -match 'checksum') 'a download with the wrong checksum is refused'
Check (Test-Path "$work\players\fake\fakeplay.exe") 'a failed reinstall keeps the working player'
$r = Run @('--install-player', 'nope', '--config', "`"$work\install.json`"")
Check ($r.Code -eq 3) 'unknown player id exits 3'

# First run with no player: the window offers the install; accept it.
Set-Content "$work\install.json" (InstallConfig $sha) -Encoding UTF8
Remove-Item -Recurse -Force "$work\players\fake"
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Top {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static IntPtr Find(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
            if (p == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
$first = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\install.json`"") -PassThru
$dlg = [IntPtr]::Zero
WaitFor { $script:dlg = [Top]::Find([uint32]$first.Id, '#32770'); $script:dlg -ne [IntPtr]::Zero } 10000 | Out-Null
Check ($dlg -ne [IntPtr]::Zero) 'first run with no player asks to install one'
if ($dlg -ne [IntPtr]::Zero) {
  [Top]::SendMessage($dlg, 0x0466, [IntPtr]6, [IntPtr]::Zero) | Out-Null  # TDM_CLICK_BUTTON, IDYES
  Check (WaitFor { Test-Path "$work\players\fake\fakeplay.exe" } 15000) 'accepting the prompt installs the player'
}
Stop-Process -Id $first.Id -Force -ErrorAction SilentlyContinue
# ---- the launcher updating itself from a (fake) GitHub release -------------
# A copy of the launcher stands in for the installed one; the "new version" is
# another exe, so the swap is easy to see.
$upd = Join-Path $work 'upd'
New-Item -ItemType Directory -Force $upd, "$www\dl", "$www\repos\test\launcher\releases" | Out-Null
Copy-Item $Launcher "$upd\MultiplayerEverythingLauncher.exe"
Copy-Item $fake "$www\dl\MultiplayerEverythingLauncher.exe"
$newSha = (Get-FileHash "$www\dl\MultiplayerEverythingLauncher.exe" -Algorithm SHA256).Hash.ToLower()
$newSize = (Get-Item "$www\dl\MultiplayerEverythingLauncher.exe").Length
function ReleaseJson([string]$tag, [string]$digest) {
  '{ "tag_name": "' + $tag + '", "assets": [ { "name": "MultiplayerEverythingLauncher.exe", "size": ' + $newSize +
  ', "digest": "sha256:' + $digest + '", "browser_download_url": "http://127.0.0.1:' + $httpPort + '/dl/MultiplayerEverythingLauncher.exe" } ] }'
}
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "updates": { "github": "test/launcher", "asset": "MultiplayerEverythingLauncher.exe", "api": "http://127.0.0.1:$httpPort" },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["x.exe"] } ],
  "games": [ { "id": "demo", "name": "Demo Game", "actions": [ { "id": "play", "label": "Play", "swf": "demo.swf" } ] } ]
}
"@ | Set-Content "$upd\update.json" -Encoding UTF8
$before = (Get-FileHash "$upd\MultiplayerEverythingLauncher.exe" -Algorithm SHA256).Hash
function RunUpd() {
  $p = Start-Process -FilePath "$upd\MultiplayerEverythingLauncher.exe" -ArgumentList @('--update', '--config', "`"$upd\update.json`"") -Wait -PassThru -NoNewWindow `
         -RedirectStandardOutput "$work\uout.txt" -RedirectStandardError "$work\uerr.txt"
  return @{ Code = $p.ExitCode; Out = (Get-Content "$work\uout.txt" -Raw) + (Get-Content "$work\uerr.txt" -Raw) }
}
Set-Content "$www\repos\test\launcher\releases\latest" (ReleaseJson 'v0.9.0' $newSha) -Encoding ASCII
$r = RunUpd
Check ($r.Code -eq 0 -and $r.Out -match 'Up to date' -and (Get-FileHash "$upd\MultiplayerEverythingLauncher.exe" -Algorithm SHA256).Hash -eq $before) "an older release changes nothing ($($r.Out.Trim()))"
Set-Content "$www\repos\test\launcher\releases\latest" (ReleaseJson 'v99.0.0' ('0' * 64)) -Encoding ASCII
$r = RunUpd
Check ($r.Code -ne 0 -and (Get-FileHash "$upd\MultiplayerEverythingLauncher.exe" -Algorithm SHA256).Hash -eq $before -and -not (Test-Path "$upd\MultiplayerEverythingLauncher.exe.new")) 'a newer release with the wrong checksum is refused'
Set-Content "$www\repos\test\launcher\releases\latest" (ReleaseJson 'v99.0.0' $newSha) -Encoding ASCII
$r = RunUpd
Check ($r.Code -eq 0 -and $r.Out -match 'Updated to v99.0.0') "a newer release is installed ($($r.Out.Trim()))"
Check ((Get-FileHash "$upd\MultiplayerEverythingLauncher.exe" -Algorithm SHA256).Hash.ToLower() -eq $newSha) 'the new exe is in place'
Check ((Test-Path "$upd\MultiplayerEverythingLauncher.exe.old") -and (Get-FileHash "$upd\MultiplayerEverythingLauncher.exe.old" -Algorithm SHA256).Hash -eq $before) 'the running exe was set aside as .old'

# ---- downloading missing game files from the catalog -----------------------
New-Item -ItemType Directory -Force "$www\games\demo2" | Out-Null
Set-Content "$www\games\demo2\dl.swf" 'pretend game file' -NoNewline
$gsha = (Get-FileHash "$www\games\demo2\dl.swf" -Algorithm SHA256).Hash.ToLower()
$gsize = (Get-Item "$www\games\demo2\dl.swf").Length
function CatalogJson([string]$hash) {
  "{ `"schemaVersion`": 1, `"files`": [ { `"name`": `"dl.swf`", `"game`": `"demo2`", `"path`": `"games/demo2/dl.swf`", `"size`": $gsize, `"sha256`": `"$hash`" } ] }"
}
Set-Content "$www\catalog.json" (CatalogJson $gsha) -Encoding ASCII
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\dlgames", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "downloads": { "catalog": "http://127.0.0.1:$httpPort/catalog.json" },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"] } ],
  "games": [ { "id": "demo2", "name": "Downloadable Game", "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\dl.swf" } ] } ]
}
"@ | Set-Content "$work\dl.json" -Encoding UTF8
$r = Run @('--list', '--config', "`"$work\dl.json`"")
Check ($r.Out -match 'game file missing') 'downloadable game starts out missing'
$r = Run @('--download-game', 'demo2', '--config', "`"$work\dl.json`"")
Check ($r.Code -eq 0 -and (Test-Path "$work\dlgames\dl.swf")) '--download-game saves the file from the catalog'
Check ((Get-FileHash "$work\dlgames\dl.swf" -Algorithm SHA256).Hash.ToLower() -eq $gsha) 'downloaded file matches the catalog checksum'
$r = Run @('--list', '--config', "`"$work\dl.json`"")
Check ($r.Out -match 'play \(Play\): ready') 'downloaded game is ready to play'
Remove-Item "$work\dlgames\dl.swf"
Set-Content "$www\catalog.json" (CatalogJson ('0' * 64)) -Encoding ASCII
$r = Run @('--download-game', 'demo2', '--config', "`"$work\dl.json`"")
Check ($r.Code -eq 1 -and -not (Test-Path "$work\dlgames\dl.swf") -and -not (Test-Path "$work\dlgames\dl.swf.part")) 'a file with the wrong checksum is not saved'
$r = Run @('--download-game', 'nosuchgame', '--config', "`"$work\dl.json`"")
Check ($r.Code -eq 3) 'unknown game id exits 3'

# The window: the catalog arrives in the background, then Download game files,
# accept the prompt, and the file lands in the games folder.
Set-Content "$www\catalog.json" (CatalogJson $gsha) -Encoding ASCII
$dlwin = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\dl.json`"") -PassThru
$dlHwnd = [IntPtr]::Zero
WaitFor { $dlwin.Refresh(); $dlwin.MainWindowHandle -ne 0 } 10000 | Out-Null
$btn = $null
if ($dlwin.MainWindowHandle -ne 0) {
  OpenPage $dlwin.MainWindowHandle
  WaitFor { $script:btn = [Acc]::Controls($dlwin.MainWindowHandle) | Where-Object { $_.Name -like 'Download game files*' } | Select-Object -First 1; $null -ne $script:btn } 10000 | Out-Null
}
Check ($null -ne $btn) "window offers Download game files ($($btn.Name))"
if ($btn) {
  [Acc]::Do($btn.Hwnd)
  $dlg = [IntPtr]::Zero
  WaitFor { $script:dlg = [Top]::Find([uint32]$dlwin.Id, '#32770'); $script:dlg -ne [IntPtr]::Zero } 10000 | Out-Null
  if ($dlg -ne [IntPtr]::Zero) { [Top]::SendMessage($dlg, 0x0466, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
  Check (WaitFor { Test-Path "$work\dlgames\dl.swf" } 15000) 'accepting the prompt downloads the game file'
}
Stop-Process -Id $dlwin.Id -Force -ErrorAction SilentlyContinue
# ---- open rooms, game updates and Settings --------------------------------
$centralPort = $port + 3
$central = Start-Job -ArgumentList $centralPort -ScriptBlock {
  param($p)
  $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $p)
  $l.Start()
  for ($i = 0; $i -lt 30; $i++) {
    $c = $l.AcceptTcpClient(); $s = $c.GetStream()
    $buf = New-Object byte[] 1024; $null = $s.Read($buf, 0, $buf.Length)
    $bytes = [Text.Encoding]::UTF8.GetBytes("LIST|ABCDE,1,4,Test room;FULL1,4,4,Full room" + [char]0)
    $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $c.Close()
  }
  $l.Stop()
}
Start-Sleep -Milliseconds 800
# The catalog now claims a different build of dl.swf, which is already downloaded.
Set-Content "$www\catalog.json" (CatalogJson ('1' * 64)) -Encoding ASCII
Remove-Item -Recurse -Force "$work\rdata" -ErrorAction SilentlyContinue
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\dlgames", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\rdata", "flashpoint": [] },
  "downloads": { "catalog": "http://127.0.0.1:$httpPort/catalog.json" },
  "rooms": { "server": "127.0.0.1:$centralPort", "refreshSeconds": 10 },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"], "args": ["--fake-player"], "flashvarArg": "-P`${KEY}=`${VALUE}" } ],
  "games": [ { "id": "demo2", "name": "Downloadable Game", "online": { "id": "demo2", "join": "play" },
               "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\dl.swf" } ] } ]
}
"@ | Set-Content "$work\rooms.json" -Encoding UTF8
$r = Run @('--rooms', 'demo2', '--config', "`"$work\rooms.json`"")
Check ($r.Code -eq 0 -and $r.Out -match '2 open' -and $r.Out -match 'ABCDE\s+1/4\s+Test room') "--rooms lists the central server's rooms"
$r = Run @('--rooms', 'demo2', '--json', '--config', "`"$work\rooms.json`"")
$rj = try { $r.Out | ConvertFrom-Json } catch { $null }
Check ($null -ne $rj -and @($rj.rooms).Count -eq 2 -and $rj.rooms[0].code -eq 'ABCDE' -and $rj.rooms[0].max -eq 4) '--rooms --json lists the rooms for programs'
# --join: a room code without the window (lower case is tidied the way the Join box does).
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$env:FL_FAKE_MS = '0'
$r = Run @('--launch', 'demo2:play', '--join', 'abcde', '--config', "`"$work\rooms.json`"")
Check ($r.Code -eq 0 -and (WaitFor { (Test-Path "$work\args.txt") -and ((Get-Content "$work\args.txt") -contains '-Pjoin=ABCDE') })) '--launch --join passes the room code to the player'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Win2 {
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
$env:FL_FAKE_MS = '500'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
# A saved player name: opening Settings must not wipe it (its name box once saved an empty name while being created).
New-Item -ItemType Directory -Force "$work\rdata" | Out-Null
'{ "playerName": "E2E Tester" }' | Set-Content "$work\rdata\settings.json" -Encoding ASCII
$rw = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\rooms.json`"") -PassThru
try {
  WaitFor { $rw.Refresh(); $rw.MainWindowHandle -ne 0 } 10000 | Out-Null
  $h = $rw.MainWindowHandle
  $join = $null; $full = $null; $roomsTab = $null
  $upd = $null
  OpenPage $h
  WaitFor { $script:upd = [Acc]::Controls($h) | Where-Object { $_.Name -like 'Update game files*' } | Select-Object -First 1; $null -ne $script:upd } 10000 | Out-Null
  Check ($null -ne $upd) 'a newer build in the catalog shows Update game files'
  # Rooms live under the details panel's Rooms tab, named with their count.
  WaitFor { $script:roomsTab = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Rooms (2)' } | Select-Object -First 1; $null -ne $script:roomsTab } 10000 | Out-Null
  Check ($null -ne $roomsTab) 'the details panel has a Rooms tab with the room count'
  if ($roomsTab) { [Acc]::Do($roomsTab.Hwnd) }
  WaitFor { $script:join = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Join room ABCDE' } | Select-Object -First 1; $null -ne $script:join } 10000 | Out-Null
  $full = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Join room FULL1' } | Select-Object -First 1
  Check ($null -ne $join) 'the Rooms tab lists open rooms with Join buttons'
  Check ($null -ne $full -and -not [Win2]::IsWindowEnabled($full.Hwnd)) "a full room's Join button is disabled"
  if ($join) {
    [Acc]::Do($join.Hwnd)
    Check (WaitFor { (Test-Path "$work\args.txt") -and ((Get-Content "$work\args.txt") -contains '-Pjoin=ABCDE') }) 'Join passes the room code to the player (-Pjoin=ABCDE)'
    $args3 = @(Get-Content "$work\args.txt" -ErrorAction SilentlyContinue)
    Check ($args3.Count -ge 2 -and $args3[-1] -like '*dl.swf') "the game file stays last ($($args3 -join ' | '))"
    Check (WaitFor { ($(try { Get-Clipboard } catch { '' })) -eq 'ABCDE' }) 'Join copies the room code'
  }
  # Quick play joins the busiest room that still has space (ABCDE; FULL1 is full).
  Start-Sleep -Milliseconds 800
  Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
  $playTab = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Play' } | Select-Object -First 1
  if ($playTab) { [Acc]::Do($playTab.Hwnd) }
  $quick = $null
  WaitFor { $script:quick = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Quick play' } | Select-Object -First 1; $null -ne $script:quick } | Out-Null
  if ($quick) {
    [Acc]::Do($quick.Hwnd)
    Check (WaitFor { (Test-Path "$work\args.txt") -and ((Get-Content "$work\args.txt") -contains '-Pjoin=ABCDE') }) 'Quick play joins the busiest open room'
  } else { Check $false 'Quick play button found' }
  # The top bar's Rooms tab opens a page of every game's rooms with their own Join buttons; Library goes back.
  $roomsTop = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Rooms' } | Select-Object -First 1
  $libTop = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Library' } | Select-Object -First 1
  if ($roomsTop -and $libTop) {
    [Acc]::Do($roomsTop.Hwnd)
    # The Play tab is open again, so a visible Join button is the page's own.
    Check (WaitFor { @([Acc]::Controls($h) | Where-Object { $_.Name -eq 'Join room ABCDE' }).Count -ge 1 }) 'the Rooms page lists the open rooms'
    Check ($null -ne ([Acc]::Controls($h) | Where-Object { $_.Name -eq 'Join by code' } | Select-Object -First 1)) 'the Rooms page has Join by code'
    [Acc]::Do($libTop.Hwnd)
  } else { Check $false 'the top bar has Library and Rooms tabs' }
  $setBtn = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Settings' } | Select-Object -First 1
  if ($setBtn) {
    [Acc]::Do($setBtn.Hwnd)
    $setWnd = [IntPtr]::Zero
    WaitFor { $script:setWnd = [Top]::Find([uint32]$rw.Id, 'MultiplayerEverythingLauncherSettings'); $script:setWnd -ne [IntPtr]::Zero } 5000 | Out-Null
    Check ($setWnd -ne [IntPtr]::Zero) 'Settings opens'
    if ($setWnd -ne [IntPtr]::Zero) {
      # The window exists a moment before its controls do.
      $min = $null
      WaitFor { $script:min = [Acc]::Controls($setWnd) | Where-Object { $_.Name -eq 'Minimize the launcher while a game runs' } | Select-Object -First 1; $null -ne $script:min } 5000 | Out-Null
      if ($min) { [Acc]::Do($min.Hwnd) }
      Check (WaitFor { (Get-Content "$work\rdata\settings.json" -Raw -ErrorAction SilentlyContinue) -match '"minimize": 1' }) 'a Settings checkbox applies and saves at once'
      Check ((Get-Content "$work\rdata\settings.json" -Raw -ErrorAction SilentlyContinue) -match '"playerName": "E2E Tester"') 'opening Settings keeps the saved player name'
      [Win2]::PostMessage($setWnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
      Check (WaitFor { [Top]::Find([uint32]$rw.Id, 'MultiplayerEverythingLauncherSettings') -eq [IntPtr]::Zero } 5000) 'Settings closes'
      Check ([Win2]::IsWindowEnabled($h)) 'the library is usable again after Settings closes'
    }
  } else { Check $false 'Settings button found' }
} finally {
  Stop-Process -Id $rw.Id -Force -ErrorAction SilentlyContinue
  Stop-Job $central -ErrorAction SilentlyContinue; Remove-Job $central -Force -ErrorAction SilentlyContinue
}
Stop-Job $server -ErrorAction SilentlyContinue; Remove-Job $server -Force -ErrorAction SilentlyContinue

# ---- PC game mods: install, update, repair, uninstall through the real exe ----
# A local web server stands in for mods-info and the package download; the "game"
# is a fake folder. The game folder must end up exactly as it started.
$modPort = 48200 + ($PID % 300)
$mw = Join-Path $work 'modsite'
New-Item -ItemType Directory -Force "$mw\mods", "$mw\dl" | Out-Null
$modServer = Start-Job -ArgumentList $modPort, $mw -ScriptBlock {
  param($p, $root)
  $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $p)
  $l.Start()
  for ($i = 0; $i -lt 80; $i++) {
    $c = $l.AcceptTcpClient(); $s = $c.GetStream()
    $buf = New-Object byte[] 8192; $n = $s.Read($buf, 0, $buf.Length)
    $path = ([Text.Encoding]::ASCII.GetString($buf, 0, $n) -split ' ')[1].TrimStart('/') -replace '/', '\'
    $file = Join-Path $root $path
    if (Test-Path $file -PathType Leaf) {
      $bytes = [IO.File]::ReadAllBytes($file)
      $h = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
      $s.Write($h, 0, $h.Length); $s.Write($bytes, 0, $bytes.Length)
    } else {
      $h = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 404 Not Found`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
      $s.Write($h, 0, $h.Length)
    }
    $s.Flush(); $c.Close()
  }
  $l.Stop()
}

function MakePackage([string]$version, [string]$name) {
  $pkg = Join-Path $work "pkg-$name"
  Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force "$pkg\core\config", "$pkg\core\data", "$pkg\override\Data" | Out-Null
  Set-Content "$pkg\core\Mod.dll" "mod dll $version" -NoNewline
  Set-Content "$pkg\core\config\settings.ini" "default=$version" -NoNewline
  Set-Content "$pkg\core\data\content-$version.txt" "content $version" -NoNewline
  Set-Content "$pkg\override\Data\original.dat" "MODDED $version" -NoNewline
  $zip = Join-Path $mw "dl\$name.zip"
  Remove-Item $zip -ErrorAction SilentlyContinue
  Compress-Archive -Path "$pkg\*" -DestinationPath $zip
  return (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
}
function Publish([string]$version, [string]$name, [string]$sha, [bool]$published = $true) {
  $url = if ($published) { "http://127.0.0.1:$modPort/dl/$name.zip" } else { '' }
  $json = '{ "schema": 1, "id": "modtest", "name": "Mod Test", "game": "Fake Game", "engine": "none", "status": "testing", ' +
          '"latest_version": "' + $version + '", "min_version": "' + $version + '", "protocol": "x", "download_url": "' + $url + '", ' +
          '"download_sha256": "' + $sha + '", "changelog_url": "", "relay": { "mode": "tunnel", "max_players": 4, "server": "main" }, ' +
          '"notice": "", "updated": "2026-10-25" }'
  Set-Content "$mw\mods\modtest.json" $json -Encoding ASCII
}

$gameDir = Join-Path $work 'FakeGame'
New-Item -ItemType Directory -Force "$gameDir\Data" | Out-Null
Set-Content "$gameDir\Game.exe" 'game binary' -NoNewline
Set-Content "$gameDir\Data\original.dat" 'ORIGINAL' -NoNewline
function Snapshot() { (Get-ChildItem $gameDir -Recurse -File | Sort-Object FullName | ForEach-Object { "$($_.FullName.Substring($gameDir.Length)) $((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }) -join "`n" }
$modBefore = Snapshot

@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\moddata", "flashpoint": [] },
  "downloads": { "modsInfo": "http://127.0.0.1:$modPort/" },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"] } ],
  "games": [ {
    "id": "modtest", "name": "Mod Test",
    "mod": {
      "id": "mod-test",
      "game": { "name": "Fake Game", "executable": "Game.exe", "processNames": ["FakeGame-not-running.exe"],
                "detection": [ { "type": "path", "path": "C:\\no\\such\\folder" } ] },
      "install": { "preserve": ["config/*.ini"], "lockedFileRetries": 1 },
      "components": [
        { "id": "core", "name": "Core", "source": "core", "required": true },
        { "id": "override", "name": "Override", "source": "override", "target": "`${GAME_DIR}", "required": true }
      ],
      "package": { "modsInfo": "modtest" }
    } } ]
}
"@ | Set-Content "$work\mods.json" -Encoding UTF8
function ModRun([string[]]$extra) {
  $p = Start-Process -FilePath $Launcher -ArgumentList (@('--config', "`"$work\mods.json`"") + $extra) -Wait -PassThru -NoNewWindow `
         -RedirectStandardOutput "$work\mout.txt" -RedirectStandardError "$work\merr.txt"
  return @{ Code = $p.ExitCode; Out = (Get-Content "$work\mout.txt" -Raw) + (Get-Content "$work\merr.txt" -Raw) }
}
$installDir = "$gameDir\Mods\mod-test"
Start-Sleep -Milliseconds 800

$r = ModRun @('--validate'); Check ($r.Code -eq 0) "the mod test config validates $($r.Out.Trim())"
$sha1 = MakePackage '1.0.0' 'v1'
Publish '1.0.0' 'v1' $sha1 $false
$r = ModRun @('--mods', '--game-dir', "`"$gameDir`"")
Check ($r.Out -match 'game: .*FakeGame' -and $r.Out -match 'installed: no' -and $r.Out -match 'not published yet') '--mods shows the game found, not installed, not published'
$r = ModRun @('--mods', '--json', '--game-dir', "`"$gameDir`"")
$mj = try { @($r.Out | ConvertFrom-Json) } catch { @() }
$mt = $mj | Where-Object id -eq 'modtest'
Check ($null -ne $mt -and $mt.gameFound -and -not $mt.installed -and -not $mt.published -and $mt.gameDir -like '*FakeGame*') '--mods --json reports the same for programs'
$r = ModRun @('--install-mod', 'modtest', '--game-dir', "`"$gameDir`"")
Check ($r.Code -ne 0 -and $r.Out -match "isn't published" -and (Snapshot) -eq $modBefore) 'an unpublished mod is not installed'

Publish '1.0.0' 'v1' ('0' * 64)
$r = ModRun @('--install-mod', 'modtest', '--game-dir', "`"$gameDir`"")
Check ($r.Code -ne 0 -and (Snapshot) -eq $modBefore) 'a package with the wrong checksum is refused and nothing changes'

Publish '1.0.0' 'v1' $sha1
$r = ModRun @('--install-mod', 'modtest', '--game-dir', "`"$gameDir`"")
Check ($r.Code -eq 0 -and $r.Out -match 'Done: Mod Test 1.0.0') "--install-mod downloads, checks and installs ($($r.Out.Trim() -replace '\s+', ' '))"
Check ((Get-Content "$installDir\Mod.dll" -Raw -ErrorAction SilentlyContinue) -eq 'mod dll 1.0.0') 'the mod file is in the install folder'
Check ((Get-Content "$gameDir\Data\original.dat" -Raw) -eq 'MODDED 1.0.0') 'a game file the mod replaces now has the modded content'
Check (Test-Path "$installDir\.installer\manifest.json") 'the install record is written next to the mod'
$r = ModRun @('--mods', '--game-dir', "`"$gameDir`"")
Check ($r.Out -match 'installed: 1\.0\.0' -and $r.Out -match 'newest: 1\.0\.0') '--mods shows the installed and newest version'

# The player edits a preserved setting; an update keeps it and swaps the rest.
Set-Content "$installDir\config\settings.ini" 'user=42' -NoNewline
$sha2 = MakePackage '2.0.0' 'v2'
Publish '2.0.0' 'v2' $sha2
$r = ModRun @('--install-mod', 'modtest', '--game-dir', "`"$gameDir`"")
Check ($r.Code -eq 0 -and $r.Out -match 'Done: Mod Test 2.0.0') 'a newer version is installed as an update'
Check ((Get-Content "$installDir\Mod.dll" -Raw) -eq 'mod dll 2.0.0' -and (Test-Path "$installDir\data\content-2.0.0.txt") -and -not (Test-Path "$installDir\data\content-1.0.0.txt")) 'the update replaced the files and removed the obsolete one'
Check ((Get-Content "$installDir\config\settings.ini" -Raw) -eq 'user=42') "the player's edited settings file was kept"

# Repair puts back a deleted file.
Remove-Item "$installDir\Mod.dll"
$r = ModRun @('--repair-mod', 'modtest', '--game-dir', "`"$gameDir`"")
Check ($r.Code -eq 0 -and (Get-Content "$installDir\Mod.dll" -Raw) -eq 'mod dll 2.0.0') '--repair-mod restores a missing file'

# Uninstall: the game folder is exactly as it was before the mod.
Remove-Item "$installDir\config\settings.ini"   # a preserved file the player removed is simply gone
$r = ModRun @('--uninstall-mod', 'modtest', '--game-dir', "`"$gameDir`"")
Check ($r.Code -eq 0 -and $r.Out -match 'Uninstalled Mod Test') '--uninstall-mod succeeds'
Check ((Snapshot) -eq $modBefore -and -not (Test-Path "$gameDir\Mods")) 'the game folder is byte-for-byte what it was before the mod'
$r = ModRun @('--mods', '--game-dir', "`"$gameDir`"")
Check ($r.Out -match 'installed: no') '--mods says it is not installed any more'

# The same through the window's buttons: Install, Launch, Uninstall (with its question).
Publish '1.0.0' 'v1' $sha1
$env:FL_FAKE_OUT = "$work\args.txt"
$env:FL_FAKE_MS = '300'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$winCfg = Get-Content "$work\mods.json" -Raw
$winCfg = $winCfg.Replace('"path": "C:\\no\\such\\folder"', '"path": "' + ($gameDir -replace '\\', '\\') + '"')
$winCfg = $winCfg.Replace('"package": { "modsInfo": "modtest" }', '"launch": { "executable": "' + ($fake -replace '\\', '\\') + '", "arguments": "--fake-player" }, "package": { "modsInfo": "modtest" }')
$winCfg | Set-Content "$work\modswin.json" -Encoding UTF8
$mwin = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\modswin.json`"") -PassThru
try {
  WaitFor { $mwin.Refresh(); $mwin.MainWindowHandle -ne 0 } 10000 | Out-Null
  $mh = $mwin.MainWindowHandle
  OpenPage $mh
  $btn = $null
  WaitFor { $script:btn = [Acc]::Controls($mh) | Where-Object { $_.Name -eq 'Install Fake Game' } | Select-Object -First 1; $null -ne $script:btn } 10000 | Out-Null
  Check ($null -ne $btn) 'the window offers Install for a mod whose game it found'
  if ($btn) {
    [Acc]::Do($btn.Hwnd)
    Check (WaitFor { Test-Path "$installDir\Mod.dll" } 20000) 'Install puts the mod into the game folder'
    $launch = $null
    WaitFor { $script:launch = [Acc]::Controls($mh) | Where-Object { $_.Name -eq 'Launch game' } | Select-Object -First 1; $null -ne $script:launch } 10000 | Out-Null
    Check ($null -ne $launch) 'the Launch button appears once the mod is installed'
    if ($launch) {
      [Acc]::Do($launch.Hwnd)
      Check (WaitFor { Test-Path "$work\args.txt" } 10000) 'Launch starts the game'
    }
    $un = $null
    WaitFor { $script:un = [Acc]::Controls($mh) | Where-Object { $_.Name -eq 'Uninstall' } | Select-Object -First 1; $null -ne $script:un } 10000 | Out-Null
    if ($un) {
      [Acc]::Do($un.Hwnd)
      $dlg = [IntPtr]::Zero
      WaitFor { $script:dlg = [Top]::Find([uint32]$mwin.Id, '#32770'); $script:dlg -ne [IntPtr]::Zero } 10000 | Out-Null
      Check ($dlg -ne [IntPtr]::Zero) 'Uninstall asks before removing anything'
      if ($dlg -ne [IntPtr]::Zero) { [Top]::SendMessage($dlg, 0x0466, [IntPtr]6, [IntPtr]::Zero) | Out-Null }  # TDM_CLICK_BUTTON, IDYES
      Check (WaitFor { -not (Test-Path "$installDir\Mod.dll") } 20000) 'Uninstall removes the mod'
      Check (WaitFor { (Snapshot) -eq $modBefore } 5000) 'the game folder is as it was after the window uninstalls the mod'
    } else { Check $false 'the Uninstall button is found' }
  }
} finally {
  Stop-Process -Id $mwin.Id -Force -ErrorAction SilentlyContinue
}
Stop-Job $modServer -ErrorAction SilentlyContinue; Remove-Job $modServer -Force -ErrorAction SilentlyContinue

# ---- window, through MSAA (the API screen readers use for Win32 controls) ----
function Ctl([IntPtr]$top, [string]$name) { [Acc]::Controls($top) | Where-Object { $_.Name -eq $name } | Select-Object -First 1 }
function OfClass([IntPtr]$top, [string]$cls) { [Acc]::Controls($top) | Where-Object { $_.Class -eq $cls } | Select-Object -First 1 }
function GridCount([IntPtr]$top) {
  $g = [Acc]::Controls($top) | Where-Object { $_.Class -eq 'SysListView32' } | Select-Object -First 1
  if (-not $g) { return 0 }  # hidden when nothing matches
  return [int][Acc]::SendMessage($g.Hwnd, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)  # LVM_GETITEMCOUNT
}

$env:FL_FAKE_MS = '1500'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$gui = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\launcher.json`"") -PassThru
try {
$win = [IntPtr]::Zero
WaitFor { $gui.Refresh(); $gui.MainWindowHandle -ne 0 } 10000 | Out-Null
$win = $gui.MainWindowHandle
Check ($win -ne [IntPtr]::Zero) 'window opens'
if ($win -ne [IntPtr]::Zero) {
  OpenPage $win
  $play = Ctl $win 'Play'
  Check ($null -ne $play) 'Play button has an accessible name'
  if ($play) {
    [Acc]::Do($play.Hwnd)
    Check (WaitFor { Test-Path "$work\args.txt" }) 'Play (default action) starts the player'
  }
  $hostBtn = Ctl $win 'Host online'
  if ($hostBtn) {
    [Acc]::Do($hostBtn.Hwnd)
    Check (WaitFor { PortOpen $port }) 'Host online starts the relay'
    $stop = $null
    WaitFor { $script:stop = Ctl $win 'Stop hosting'; $null -ne $script:stop } | Out-Null
    Check ($null -ne $stop) 'hosting box shows Stop hosting'
    if ($stop) {
      [Acc]::Do($stop.Hwnd)
      Check (WaitFor { -not (PortOpen $port) }) 'Stop hosting closes the relay'
    }
  } else { Check $false 'Host online button found' }

  $back = Ctl $win 'Back to games'
  if ($back) { [Acc]::Do($back.Hwnd) }
  Check (WaitFor { $null -eq (Ctl $win 'Back to games') }) 'Back to games returns to the library'
  Check (WaitFor { (GridCount $win) -eq 2 }) 'grid lists both games'
  $search = OfClass $win 'Edit'
  if ($search) {
    [Acc]::SendMessage($search.Hwnd, 0x000C, [IntPtr]::Zero, 'missing') | Out-Null  # WM_SETTEXT
    Check (WaitFor { (GridCount $win) -eq 1 }) 'search filters the grid'
    [Acc]::SendMessage($search.Hwnd, 0x000C, [IntPtr]::Zero, 'zzzz') | Out-Null
    Check (WaitFor { (GridCount $win) -eq 0 }) 'search with no match empties the grid'
    [Acc]::SendMessage($search.Hwnd, 0x000C, [IntPtr]::Zero, '') | Out-Null
    Check (WaitFor { (GridCount $win) -eq 2 }) 'clearing the search shows everything again'
  } else { Check $false 'search box found' }
  [Acc]::PostMessage($win, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
  $gui.WaitForExit(5000) | Out-Null
}
Check ($gui.HasExited) 'window closes'
$settings = Get-Content "$work\data\settings.json" -Raw -ErrorAction SilentlyContinue
Check ($settings -match '"plays": [1-9]') 'play count is saved'
} catch {
  Check $false "window automation: $($_.Exception.Message)"
} finally {
  if (-not $gui.HasExited) { Stop-Process -Id $gui.Id -Force }
}

# ---- The lobby: name prompt, hosting by direct IP, a friend, start, the bridge ----
# The friend is this script speaking the launchers' lobby protocol; the "game" is
# the fake player, which plays the game's side of the bridge from its FlashVars.
$lport = 48900 + ($PID % 90)
New-Item -ItemType Directory -Force "$work\lobby\games" | Out-Null
Set-Content "$work\lobby\games\lob.swf" 'lobby game file'
$lobHash = (Get-FileHash "$work\lobby\games\lob.swf" -Algorithm SHA256).Hash.ToLower().Substring(0, 16)
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "rooms": { "server": "127.0.0.1:1" },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"], "args": ["--fake-player"], "flashvarArg": "-P`${KEY}=`${VALUE}" } ],
  "games": [ { "id": "lob", "name": "Lobby Game", "players": "1-4 players",
               "online": { "id": "lob", "join": "online", "lobby": { "version": "2.0", "port": $lport } },
               "actions": [ { "id": "online", "label": "Play online", "swf": "`${GAMES_DIR}\\lob.swf", "flashvars": { "online": "1" } } ] } ]
}
"@ | Set-Content "$work\lobby\launcher.json" -Encoding UTF8
Copy-Item $Launcher "$work\lobby\"
$lobbyExe = Join-Path "$work\lobby" (Split-Path $Launcher -Leaf)
$r = Run @('--validate', '--config', "`"$work\lobby\launcher.json`"")
Check ($r.Code -eq 0) "lobby test config validates $($r.Out.Trim())"

function LobbySend($w, [string]$json) { $b = [Text.Encoding]::UTF8.GetBytes($json + [char]0); $w.Write($b, 0, $b.Length); $w.Flush() }
# Next message whose "k" is $kind (others skipped), parsed; $null after $ms.
function LobbyExpect($c, [string]$kind, [int]$ms = 8000) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $ms) {
    $i = $c.Buf.IndexOf([char]0)
    if ($i -ge 0) {
      $m = $c.Buf.Substring(0, $i); $c.Buf = $c.Buf.Substring($i + 1)
      try { $o = $m | ConvertFrom-Json } catch { continue }
      if ($o.k -eq $kind) { return $o }
      continue
    }
    if ($c.Tcp.Available -gt 0) {
      $b = New-Object byte[] 65536; $n = $c.Stream.Read($b, 0, $b.Length)
      $c.Buf += [Text.Encoding]::UTF8.GetString($b, 0, $n)
    } else { Start-Sleep -Milliseconds 50 }
  }
  return $null
}

$env:FL_FAKE_OUT = "$work\lobby\args.txt"
$env:FL_FAKE_MS = '6000'
Remove-Item Env:\FL_NO_NAME_PROMPT
$lw = Start-Process -FilePath $lobbyExe -PassThru
$env:FL_NO_NAME_PROMPT = '1'
$bot = $null
try {
  # First start: the launcher asks for a player name before anything else.
  $nameWnd = [IntPtr]::Zero
  WaitFor { $script:nameWnd = [Top]::Find([uint32]$lw.Id, 'MultiplayerEverythingLauncherName'); $script:nameWnd -ne [IntPtr]::Zero } 10000 | Out-Null
  Check ($nameWnd -ne [IntPtr]::Zero) 'the first start asks for a player name'
  if ($nameWnd -ne [IntPtr]::Zero) {
    $nameEdit = $null
    WaitFor { $script:nameEdit = OfClass $nameWnd 'Edit'; $null -ne $script:nameEdit } | Out-Null
    [Acc]::SendMessage($nameEdit.Hwnd, 0x000C, [IntPtr]::Zero, 'Tester') | Out-Null  # WM_SETTEXT
    $cont = Ctl $nameWnd 'Continue'
    if ($cont) { [Acc]::Do($cont.Hwnd) }
    Check (WaitFor { (Get-Content "$work\lobby\data\settings.json" -Raw -ErrorAction SilentlyContinue) -match '"playerName": "Tester"' }) 'the name is saved'
  }
  WaitFor { $lw.Refresh(); $lw.MainWindowHandle -ne 0 } 10000 | Out-Null
  $lh = $lw.MainWindowHandle
  OpenPage $lh
  $playOnline = $null
  WaitFor { $script:playOnline = Ctl $lh 'Play online'; $null -ne $script:playOnline } | Out-Null
  if ($playOnline) { [Acc]::Do($playOnline.Hwnd) }
  $lobWnd = [IntPtr]::Zero
  WaitFor { $script:lobWnd = [Top]::Find([uint32]$lw.Id, 'MultiplayerEverythingLauncherLobby'); $script:lobWnd -ne [IntPtr]::Zero } 5000 | Out-Null
  Check ($lobWnd -ne [IntPtr]::Zero) 'Play online opens the lobby window for a lobby game'
  if ($lobWnd -ne [IntPtr]::Zero) {
    $direct = $null
    WaitFor { $script:direct = [Acc]::Controls($lobWnd) | Where-Object { $_.Name -like 'Direct IP*' } | Select-Object -First 1; $null -ne $script:direct } | Out-Null
    if ($direct) { [Acc]::Do($direct.Hwnd) }
    $portEdit = $null
    WaitFor { $script:portEdit = Ctl $lobWnd 'Port'; $null -ne $script:portEdit } | Out-Null
    Check ($null -ne $portEdit) 'Direct IP shows the port field'
    $hostRoom = Ctl $lobWnd 'Host room'
    if ($hostRoom) { [Acc]::Do($hostRoom.Hwnd) }
    Check (WaitFor { PortOpen $lport }) "Host room listens for friends on port $lport"
    Check ($null -ne (Ctl $lobWnd 'Start game')) 'the room page has Start game'

    # A friend joins, gets a slot, gets ready.
    $tcp = New-Object System.Net.Sockets.TcpClient
    $tcp.Connect('127.0.0.1', $lport)
    $bot = @{ Tcp = $tcp; Stream = $tcp.GetStream(); Buf = '' }
    LobbySend $bot.Stream ('{"k":"hello","proto":1,"name":"Bot","token":"bot-token-0123456789","version":"2.0#' + $lobHash + '"}')
    $welcome = LobbyExpect $bot 'welcome'
    Check ($null -ne $welcome -and $welcome.slot -eq 1) "the friend is welcomed as P2 (slot $($welcome.slot))"
    $lob = LobbyExpect $bot 'lobby'
    Check ($null -ne $lob -and @($lob.players).Count -eq 2 -and $lob.players[0].name -eq 'Tester') 'the lobby lists the host by name and the friend'
    $players = $null
    WaitFor { $script:players = [Acc]::Controls($lobWnd) | Where-Object { $_.Class -eq 'SysListView32' } | Select-Object -First 1; $null -ne $script:players } | Out-Null
    Check (WaitFor { [int][Acc]::SendMessage($players.Hwnd, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) -eq 2 }) 'the host sees two players'
    LobbySend $bot.Stream '{"k":"ready","v":true}'
    LobbySend $bot.Stream '{"k":"chat","text":"ready!"}'
    Check ($null -ne (LobbyExpect $bot 'chat')) 'chat goes round'
    Start-Sleep -Milliseconds 500

    # The host starts: the launcher opens the game with the room's FlashVars, and the
    # game gets "go" only once the friend's game is loaded too.
    [Acc]::Do((Ctl $lobWnd 'Start game').Hwnd)
    $start = LobbyExpect $bot 'start'
    Check ($null -ne $start -and $start.seed -ge 0) "the friend gets start with a seed ($($start.seed))"
    Check (WaitFor { (Get-Content "$work\lobby\args.txt" -ErrorAction SilentlyContinue) -contains '-Prole=host' }) 'the launcher starts the game as host'
    # The arguments, without the fake game's notes from the bridge that follow them.
    $largs = @(Get-Content "$work\lobby\args.txt" -ErrorAction SilentlyContinue | Where-Object { $_ -notlike 'BRIDGE *' })
    Check (($largs -contains '-Pslot=0') -and ($largs | Where-Object { $_ -match '^-PbridgePort=\d+$' }) -and ($largs | Where-Object { $_ -match '^-PsessionToken=[0-9a-f]{32}$' }) -and ($largs -contains "-Pseed=$($start.seed)")) "FlashVars carry slot, bridgePort, sessionToken and seed ($($largs -join ' '))"
    Check ($largs[-1] -like '*lob.swf') 'the game file stays last'
    Check (WaitFor { (Get-Content "$work\lobby\args.txt" -Raw -ErrorAction SilentlyContinue) -match '"t":"welcome","slot":0,"role":"host"' }) 'the game is welcomed on the bridge as slot 0, host'
    Start-Sleep -Milliseconds 400
    Check (-not ((Get-Content "$work\lobby\args.txt" -Raw) -match 'BRIDGE \{"t":"go"')) 'no go while the friend is still loading'
    LobbySend $bot.Stream '{"k":"loaded"}'
    Check (WaitFor { (Get-Content "$work\lobby\args.txt" -Raw) -match 'BRIDGE \{"t":"go"' }) 'go once everyone is loaded'
    $g = LobbyExpect $bot 'g'
    Check ($null -ne $g -and $g.from -eq 0 -and $g.d -eq 'hello from the game') 'the game reaches the friend through the bridge'
    LobbySend $bot.Stream '{"k":"g","dst":0,"d":{"x":7}}'
    Check (WaitFor { (Get-Content "$work\lobby\args.txt" -Raw) -match '"t":"msg","from":1,"d":\{"x":7\}' }) "the friend's message reaches the game"

    $diag = Ctl $lobWnd 'Copy diagnostics'
    if ($diag) {
      [Acc]::Do($diag.Hwnd)
      Check (WaitFor { ($(try { Get-Clipboard -Raw } catch { '' })) -match 'Lobby: direct, host' }) 'Copy diagnostics puts a report on the clipboard'
      Check (-not (($(try { Get-Clipboard -Raw } catch { '' })) -match 'Tester')) 'the report leaves out the player name'
    } else { Check $false 'Copy diagnostics button found' }

    # The game closes (the fake player's time is up): everyone is back in the lobby.
    Check ($null -ne (LobbyExpect $bot 'end' 12000)) 'the host game closing ends the round for everyone'
    $leave = Ctl $lobWnd 'Leave room'
    if ($leave) { [Acc]::Do($leave.Hwnd) }
    Check ($null -ne (LobbyExpect $bot 'closed')) 'Leave room tells the friend the room closed'
    Check (WaitFor { -not (PortOpen $lport) }) 'and stops listening'
  }
} catch {
  Check $false "lobby automation: $($_.Exception.Message)"
} finally {
  if ($bot) { $bot.Tcp.Close() }
  Stop-Process -Id $lw.Id -Force -ErrorAction SilentlyContinue
  $env:FL_FAKE_OUT = "$work\args.txt"
}

# ---- HTML5: a browser player, a local page, a game server and a friend's link ----
# The "browser" is the fake player too; the "server" is launcher_tests.exe
# --fake-server, which prints a tunnel link the way the PB2 co-op server does.
$wport = 48700 + ($PID % 200)
New-Item -ItemType Directory -Force "$work\games\webpage", "$work\games\webgame" | Out-Null
Set-Content "$work\games\webpage\index.html" '<h1>web demo</h1>'
Set-Content "$work\games\webgame\server.js" '// stand-in'
$webConfig = @"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players",
             "data": "`${LAUNCHER_DIR}\\wdata", "flashpoint": [] },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"], "args": ["--fake-player"] },
               { "id": "browser", "name": "Fake Browser", "kind": "browser", "paths": ["$($fake -replace '\\','\\')"],
                 "args": ["--fake-player", "--app=`${URL}"] } ],
  "categories": [ { "id": "online", "name": "Online multiplayer" } ],
  "games": [
    { "id": "web", "name": "Web Demo", "players": "1-8 players", "categories": ["online"],
      "actions": [
        { "id": "host", "label": "Host co-op", "file": "`${GAMES_DIR}\\webgame\\server.js",
          "server": { "name": "Fake server", "exe": ["$($fake -replace '\\','\\')"], "args": ["--fake-server", "$wport"],
                      "url": "http://127.0.0.1:$wport/", "share": "https://[a-z0-9-]+\\.trycloudflare\\.com" } },
        { "id": "page", "label": "Play page", "page": "`${GAMES_DIR}\\webpage\\index.html" },
        { "id": "join", "label": "Join with a link", "joinLink": true }
      ] }
  ]
}
"@
Set-Content "$work\web.json" $webConfig -Encoding UTF8
$r = Run @('--validate', '--config', "`"$work\web.json`""); Check ($r.Code -eq 0) "HTML5 test config validates $($r.Out.Trim())"

# A local page: served from 127.0.0.1 while the browser runs, gone after.
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$env:FL_FAKE_MS = '4000'
$pg = Start-Process -FilePath $Launcher -ArgumentList @('--launch', 'web:page', '--config', "`"$work\web.json`"") -PassThru
$app = $null
WaitFor { $script:app = Get-Content "$work\args.txt" -ErrorAction SilentlyContinue | Where-Object { $_ -like '--app=*' } | Select-Object -First 1; $null -ne $script:app } | Out-Null
Check ($app -match '^--app=http://127\.0\.0\.1:\d+/index\.html$') "a local page opens in the browser player ($app)"
$pageUrl = if ($app) { $app.Substring(6) } else { 'http://127.0.0.1:1/' }
$body = try { (Invoke-WebRequest -UseBasicParsing $pageUrl -TimeoutSec 5).Content } catch { '' }
Check ($body -match 'web demo') 'the launcher serves the page while the game runs'
$pg.WaitForExit(15000) | Out-Null
Check ($pg.HasExited -and $pg.ExitCode -eq 0) 'the page action ends with the browser'
$pagePort = if ($pageUrl -match ':(\d+)/') { [int]$Matches[1] } else { 1 }
Check (-not (PortOpen $pagePort)) 'the page server stops with it'

# A game server: started first, the browser opens its URL, and it stops after.
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$env:FL_FAKE_MS = '1500'
$r = Run @('--launch', 'web:host', '--config', "`"$work\web.json`"")
Check ($r.Code -eq 0) "Host co-op runs headless (exit $($r.Code))"
Check ((Get-Content "$work\args.txt" -ErrorAction SilentlyContinue) -contains "--app=http://127.0.0.1:$wport/") 'the browser opens the server URL'
Check (WaitFor { -not (PortOpen $wport) }) 'the game server stops when the browser closes'
$r = Run @('--launch', 'web:join', '--config', "`"$work\web.json`"")
Check ($r.Code -eq 3) 'Join with a link needs the window'

# The window: share link, Copy, Stop hosting, and joining from the clipboard.
$env:FL_FAKE_MS = '60000'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$ww = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\web.json`"") -PassThru
try {
  WaitFor { $ww.Refresh(); $ww.MainWindowHandle -ne 0 } 10000 | Out-Null
  $wh = $ww.MainWindowHandle
  OpenPage $wh
  $hostCoop = $null
  WaitFor { $script:hostCoop = Ctl $wh 'Host co-op'; $null -ne $script:hostCoop } | Out-Null
  if ($hostCoop) {
    [Acc]::Do($hostCoop.Hwnd)
    Check (WaitFor { PortOpen $wport } 10000) 'Host co-op starts the game server'
    Check (WaitFor { (Get-Content "$work\args.txt" -ErrorAction SilentlyContinue) -contains "--app=http://127.0.0.1:$wport/" } 10000) 'then opens the browser on it'
    $copy = $null
    WaitFor { $script:copy = Ctl $wh 'Copy address'; $null -ne $script:copy } | Out-Null
    if ($copy) {
      SetClip 'nothing yet'
      [Acc]::Do($copy.Hwnd)
      # The launcher may hold the clipboard open for a moment; retry instead of failing.
      Check (WaitFor { ($(try { Get-Clipboard } catch { '' })) -eq 'https://fake-test-link.trycloudflare.com' }) 'Copy gives friends the internet link the server printed'
    } else { Check $false 'hosting box shows Copy for an HTML5 server' }
    $stopWeb = Ctl $wh 'Stop hosting'
    if ($stopWeb) {
      [Acc]::Do($stopWeb.Hwnd)
      Check (WaitFor { -not (PortOpen $wport) }) 'Stop hosting stops the game server'
    } else { Check $false 'hosting box shows Stop hosting for an HTML5 server' }
  } else { Check $false 'Host co-op button found' }

  Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
  SetClip 'https://friend-link.trycloudflare.com/'
  $joinBtn = Ctl $wh 'Join with a link'
  if ($joinBtn) {
    [Acc]::Do($joinBtn.Hwnd)
    Check (WaitFor { (Get-Content "$work\args.txt" -ErrorAction SilentlyContinue) -contains '--app=https://friend-link.trycloudflare.com/' }) 'Join with a link opens the copied link'
  } else { Check $false 'Join with a link button found' }
} finally {
  Stop-Process -Id $ww.Id -Force -ErrorAction SilentlyContinue
  # Browsers started by the window outlive it on purpose; end the fake ones.
  Get-CimInstance Win32_Process -Filter "Name = 'launcher_tests.exe'" |
    Where-Object { $_.CommandLine -like '*--fake-*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if ($failures) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'All end-to-end checks passed'
