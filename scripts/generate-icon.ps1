[CmdletBinding()]
param(
    [string]$OutputDirectory
)

# Windows-only asset authoring tool, independent of the application's build.
# Render the simple rect/polygon SVG at each size with pixel-snapped edges.
# No Python, image editor, downloaded package or font is required.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets'
}
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\QuadDeck.svg'
[xml]$svg = Get-Content -LiteralPath $source -Raw
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$supersample = 8
$culture = [Globalization.CultureInfo]::InvariantCulture
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

function Read-Number([string]$Value) {
    return [float]::Parse($Value, $culture)
}

function Render-Icon([int]$Size) {
    $large = New-Object Drawing.Bitmap ($Size * $supersample), ($Size * $supersample)
    $graphics = [Drawing.Graphics]::FromImage($large)
    try {
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.ScaleTransform($supersample, $supersample)
        $scale = $Size / 256.0
        foreach ($node in $svg.DocumentElement.ChildNodes) {
            if ($node.LocalName -eq 'title') { continue }
            $brush = New-Object Drawing.SolidBrush ([Drawing.ColorTranslator]::FromHtml($node.fill))
            $path = New-Object Drawing.Drawing2D.GraphicsPath
            try {
                if ($node.LocalName -eq 'rect') {
                    $x = [float][Math]::Round((Read-Number $node.x) * $scale)
                    $y = [float][Math]::Round((Read-Number $node.y) * $scale)
                    $right = [float][Math]::Round(((Read-Number $node.x) + (Read-Number $node.width)) * $scale)
                    $bottom = [float][Math]::Round(((Read-Number $node.y) + (Read-Number $node.height)) * $scale)
                    $diameter = [float](2 * (Read-Number $node.rx) * $scale)
                    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
                    $path.AddArc($right - $diameter, $y, $diameter, $diameter, 270, 90)
                    $path.AddArc($right - $diameter, $bottom - $diameter, $diameter, $diameter, 0, 90)
                    $path.AddArc($x, $bottom - $diameter, $diameter, $diameter, 90, 90)
                    $path.CloseFigure()
                } elseif ($node.LocalName -eq 'polygon') {
                    [Drawing.PointF[]]$points = foreach ($pair in ($node.points -split '\s+')) {
                        $xy = $pair -split ','
                        # Half-pixel vertices preserve the play triangle at 16px.
                        $px = [float]([Math]::Round((Read-Number $xy[0]) * $scale * 2) / 2)
                        $py = [float]([Math]::Round((Read-Number $xy[1]) * $scale * 2) / 2)
                        New-Object Drawing.PointF $px, $py
                    }
                    $path.AddPolygon($points)
                } else {
                    throw "Unsupported SVG element: $($node.LocalName)"
                }
                $graphics.FillPath($brush, $path)
            } finally {
                $path.Dispose()
                $brush.Dispose()
            }
        }
        $bitmap = New-Object Drawing.Bitmap $Size, $Size
        $downsample = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $downsample.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $downsample.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $downsample.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $downsample.DrawImage($large, (New-Object Drawing.Rectangle 0, 0, $Size, $Size),
                0, 0, $large.Width, $large.Height, [Drawing.GraphicsUnit]::Pixel)
        } finally { $downsample.Dispose() }
        return $bitmap
    } finally {
        $graphics.Dispose()
        $large.Dispose()
    }
}

# 32-bit DIB frames plus a 1-bit transparency mask work in both the native
# resource loader and Explorer. Each frame is rendered independently.
$frames = @()
foreach ($size in $sizes) {
    $bitmap = Render-Icon $size
    $stream = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter $stream
    try {
        $maskStride = [int]([Math]::Ceiling($size / 32.0) * 4)
        $writer.Write([uint32]40)
        $writer.Write([int32]$size)
        $writer.Write([int32]($size * 2))
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]0)
        $writer.Write([uint32]($size * $size * 4 + $maskStride * $size))
        1..4 | ForEach-Object { $writer.Write([uint32]0) }
        for ($y = $size - 1; $y -ge 0; --$y) {
            for ($x = 0; $x -lt $size; ++$x) {
                $pixel = $bitmap.GetPixel($x, $y)
                $writer.Write([byte]$pixel.B)
                $writer.Write([byte]$pixel.G)
                $writer.Write([byte]$pixel.R)
                $writer.Write([byte]$pixel.A)
            }
        }
        for ($y = $size - 1; $y -ge 0; --$y) {
            $mask = New-Object byte[] $maskStride
            for ($x = 0; $x -lt $size; ++$x) {
                if ($bitmap.GetPixel($x, $y).A -eq 0) {
                    $index = [int][Math]::Floor($x / 8)
                    $mask[$index] = $mask[$index] -bor (128 -shr ($x % 8))
                }
            }
            $writer.Write($mask)
        }
        $frames += ,$stream.ToArray()
        if ($size -eq 256) {
            $bitmap.Save((Join-Path $OutputDirectory 'QuadDeck.png'), [Drawing.Imaging.ImageFormat]::Png)
        }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
        $bitmap.Dispose()
    }
}

$iconPath = Join-Path $OutputDirectory 'QuadDeck.ico'
$file = [IO.File]::Create($iconPath)
$output = New-Object IO.BinaryWriter $file
try {
    $output.Write([uint16]0)
    $output.Write([uint16]1)
    $output.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; ++$i) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $output.Write([byte]$dimension)
        $output.Write([byte]$dimension)
        $output.Write([uint16]0)
        $output.Write([uint16]1)
        $output.Write([uint16]32)
        $output.Write([uint32]$frames[$i].Length)
        $output.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $output.Write([byte[]]$frame) }
} finally {
    $output.Dispose()
    $file.Dispose()
}
Write-Host "Generated $iconPath ($($sizes -join ', ') px) and QuadDeck.png"
