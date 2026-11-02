<#
.SYNOPSIS
  Converts a square PNG into a multi-size .ico (16-256 px, PNG-compressed).
  Use it to make assets/app.ico, the icon compiled into Setup.exe.
.EXAMPLE
  .\scripts\png-to-ico.ps1 -Png examples\sample-mod\assets\icon.png -Ico assets\app.ico
#>
param([Parameter(Mandatory)] [string]$Png, [Parameter(Mandatory)] [string]$Ico)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile((Resolve-Path $Png))
$sizes = 16, 24, 32, 48, 64, 128, 256
$images = foreach ($s in $sizes) {
  $bmp = New-Object System.Drawing.Bitmap $s, $s
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.InterpolationMode = 'HighQualityBicubic'; $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
  $g.DrawImage($src, 0, 0, $s, $s); $g.Dispose()
  $ms = New-Object IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  , $ms.ToArray()
}
$src.Dispose()
$out = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $len = $images[$i].Length
  $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$len); $w.Write([UInt32]$offset)
  $offset += $len
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Ico))) | Out-Null
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($Ico), $out.ToArray())
Write-Host "Wrote $Ico"
