# Generates res/nvlow.ico (PNG-compressed, 16-256 px) and res/nvlow.png (512 px preview).
# Design: green play triangle "compressed" into two chevrons (green + white) on a dark rounded tile.
# Run with Windows PowerShell.
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot '..\res'
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256

function New-IconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $f = [float]$s

    # Rounded dark tile with a subtle top-to-bottom gradient
    $r = $f * 0.22
    $tile = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $f - 1
    $tile.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $tile.AddArc($w - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $tile.AddArc($w - $r * 2, $w - $r * 2, $r * 2, $r * 2, 0, 90)
    $tile.AddArc(0, $w - $r * 2, $r * 2, $r * 2, 90, 90)
    $tile.CloseFigure()
    $tileBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF 0, $f), ([System.Drawing.Color]::FromArgb(255, 38, 40, 44)), ([System.Drawing.Color]::FromArgb(255, 20, 21, 24))
    $g.FillPath($tileBrush, $tile)

    # Small sizes get chunkier shapes so they stay readable.
    $small = $s -le 24
    $dx = $f * 0.035  # optical centring: the triangle's mass sits left of its bounding box

    # Play triangle with rounded corners (fill + round-joined stroke of the same colour)
    $tri = New-Object System.Drawing.Drawing2D.GraphicsPath
    $tri.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($f * 0.32 + $dx), ($f * 0.15)),
        (New-Object System.Drawing.PointF ($f * 0.32 + $dx), ($f * 0.61)),
        (New-Object System.Drawing.PointF ($f * 0.73 + $dx), ($f * 0.38))
    ))
    $green = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, ($f * 0.12)), (New-Object System.Drawing.PointF 0, ($f * 0.66)), ([System.Drawing.Color]::FromArgb(255, 170, 230, 0)), ([System.Drawing.Color]::FromArgb(255, 104, 170, 0))
    $g.FillPath($green, $tri)
    $round = New-Object System.Drawing.Pen ($green, [float]($f * 0.07))
    $round.LineJoin = 'Round'
    $g.DrawPath($round, $tri)

    # Two chevrons below: green, then white
    $penW = [float]($f * $(if ($small) { 0.10 } else { 0.075 }))
    $cx = $f * 0.50
    $half = $f * 0.17
    $depth = $f * 0.075
    $rows = @(
        @{ Y = $f * 0.715; Color = [System.Drawing.Color]::FromArgb(255, 140, 205, 0) },
        @{ Y = $f * 0.835; Color = [System.Drawing.Color]::FromArgb(255, 240, 240, 240) }
    )
    if ($small) { $rows[0].Y = $f * 0.72; $rows[1].Y = $f * 0.86 }
    foreach ($row in $rows) {
        $pen = New-Object System.Drawing.Pen ($row.Color, $penW)
        $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
        $pts = [System.Drawing.PointF[]]@(
            (New-Object System.Drawing.PointF ($cx - $half), $row.Y),
            (New-Object System.Drawing.PointF $cx, ($row.Y + $depth)),
            (New-Object System.Drawing.PointF ($cx + $half), $row.Y)
        )
        $g.DrawLines($pen, $pts)
        $pen.Dispose()
    }

    $g.Dispose()
    return $bmp
}

# Preview PNG
$preview = New-IconBitmap 512
$preview.Save((Join-Path $resDir 'nvlow.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()

# Classic 32-bit BGRA DIB entry (header + bottom-up pixels + empty AND mask), read by every Windows API.
function Get-DibBytes([System.Drawing.Bitmap]$bmp) {
    $s = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write([UInt32]40); $w.Write([Int32]$s); $w.Write([Int32]($s * 2)); $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]0); $w.Write([UInt32]0); $w.Write([Int32]0); $w.Write([Int32]0); $w.Write([UInt32]0); $w.Write([UInt32]0)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $s, $s
    $data = $bmp.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $row = New-Object byte[] ($s * 4)
    for ($y = $s - 1; $y -ge 0; $y--) {
        [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $row, 0, $s * 4)
        $w.Write($row)
    }
    $bmp.UnlockBits($data)
    $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $w.Write((New-Object byte[] ($maskStride * $s)))
    $w.Flush()
    return , $ms.ToArray()
}

# Multi-size ICO: DIB entries for small sizes, PNG for 256 px
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    if ($s -ge 256) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngs += , $ms.ToArray()
    } else {
        $pngs += , (Get-DibBytes $bmp)
    }
    $bmp.Dispose()
}

$out = Join-Path $resDir 'nvlow.ico'
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32); $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
Write-Host "Wrote $out"
