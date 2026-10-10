# Generates the tray icons: a Claude-style spark with a microphone cut into its
# centre (transparent gap around the mic), in orange (idle) and red (recording).
# Also writes a preview PNG for each.
# Run: powershell -ExecutionPolicy Bypass -File make_icon.ps1
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path

$M      = 1024                                  # master canvas size
$orange = [System.Drawing.Color]::FromArgb(255, 217, 119, 87)   # #D97757
$red    = [System.Drawing.Color]::FromArgb(255, 220, 50, 47)    # recording
$clear  = [System.Drawing.Color]::FromArgb(0, 0, 0, 0)

$variants = @(
    @{ Color = $orange; Ico = 'claude_dictate.ico';     Preview = 'icon_preview.png' },
    @{ Color = $red;    Ico = 'claude_dictate_rec.ico'; Preview = 'icon_preview_rec.png' }
)

function New-MicPath {
    # Microphone: capsule head, U-shaped holder, stem and base.
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $cx = 512; $w = 220; $top = 207; $h = 352
    $r = $w / 2
    $p.AddArc($cx - $r, $top, $w, $w, 180, 180)
    $p.AddArc($cx - $r, $top + $h - $w, $w, $w, 0, 180)
    $p.CloseFigure()
    return $p
}

function New-HolderPath {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $cx = 512; $R = 176; $cy = 418
    $p.AddArc($cx - $R, $cy - $R, 2 * $R, 2 * $R, 0, 180)   # U under the head
    $p.StartFigure(); $p.AddLine($cx, $cy + $R, $cx, 710)    # stem
    $p.StartFigure(); $p.AddLine($cx - 116, 710, $cx + 116, 710)  # base
    return $p
}

foreach ($v in $variants) {
$color = $v.Color
$bmp = New-Object System.Drawing.Bitmap($M, $M, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.Clear($clear)

# 1. Spark: rays from the centre with round caps and varying lengths.
$lengths = 510, 480, 500, 470, 510, 485, 500, 475, 510, 480, 500, 470
$n = $lengths.Count
for ($i = 0; $i -lt $n; $i++) {
    $a = (2 * [Math]::PI * $i / $n) - [Math]::PI / 2 + 0.12
    $pen = New-Object System.Drawing.Pen($color, 92)
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $L = $lengths[$i] - 40
    $g.DrawLine($pen, 512, 512, 512 + $L * [Math]::Cos($a), 512 + $L * [Math]::Sin($a))
    $pen.Dispose()
}

# 2. Erase a negative-space border around the microphone.
$mic = New-MicPath; $holder = New-HolderPath
$g.CompositingMode = 'SourceCopy'
$gap = 62
$eraseBrush = New-Object System.Drawing.SolidBrush($clear)
$erasePen = New-Object System.Drawing.Pen($clear, (2 * $gap)); $erasePen.LineJoin = 'Round'
$erasePen.StartCap = 'Round'; $erasePen.EndCap = 'Round'
$g.FillPath($eraseBrush, $mic); $g.DrawPath($erasePen, $mic)
$erasePen2 = New-Object System.Drawing.Pen($clear, (59 + 2 * $gap))
$erasePen2.StartCap = 'Round'; $erasePen2.EndCap = 'Round'; $erasePen2.LineJoin = 'Round'
$g.DrawPath($erasePen2, $holder)
# Clear the inside of the U too, so the mic reads as one shape.
$g.FillRectangle($eraseBrush, 512 - 176, 290, 352, 130)
$g.CompositingMode = 'SourceOver'

# 3. Draw the microphone.
$micBrush = New-Object System.Drawing.SolidBrush($color)
$g.FillPath($micBrush, $mic)
$micPen = New-Object System.Drawing.Pen($color, 59)
$micPen.StartCap = 'Round'; $micPen.EndCap = 'Round'
$g.DrawPath($micPen, $holder)
$g.Dispose()

# Downscale to each icon size and pack PNGs into an .ico.
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()
foreach ($s in $sizes) {
    $small = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $sg = [System.Drawing.Graphics]::FromImage($small)
    $sg.InterpolationMode = 'HighQualityBicubic'
    $sg.PixelOffsetMode = 'HighQuality'
    $sg.CompositingQuality = 'HighQuality'
    $sg.DrawImage($bmp, 0, 0, $s, $s)
    $sg.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $small.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $small.Dispose()
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
[System.IO.File]::WriteAllBytes((Join-Path $dir $v.Ico), $out.ToArray())

# Preview: each size on light and dark backgrounds, scaled up for inspection.
$pv = New-Object System.Drawing.Bitmap(1100, 560)
$pg = [System.Drawing.Graphics]::FromImage($pv)
$pg.InterpolationMode = 'NearestNeighbor'; $pg.PixelOffsetMode = 'Half'
$pg.FillRectangle([System.Drawing.Brushes]::WhiteSmoke, 0, 0, 1100, 280)
$pg.FillRectangle((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(32, 32, 32))), 0, 280, 1100, 280)
$pg.DrawImage($bmp, 10, 10, 260, 260)
$pg.DrawImage($bmp, 10, 290, 260, 260)
$x = 290
foreach ($i in 0, 2, 3) {          # 16, 24, 32 px, each at 1x and 6x
    $img = [System.Drawing.Image]::FromStream((New-Object System.IO.MemoryStream(, $pngs[$i])))
    $s = $sizes[$i]
    foreach ($y in 20, 300) {
        $pg.DrawImage($img, $x, $y, $s, $s)
        $pg.DrawImage($img, $x, $y + 50, $s * 6, $s * 6)
    }
    $x += $s * 6 + 30
}
$pg.Dispose()
$pv.Save((Join-Path $dir $v.Preview), [System.Drawing.Imaging.ImageFormat]::Png)
"wrote $($v.Ico) ($($out.Length) bytes)"
}
