param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\UnlockIt-icon.png')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$source = [System.Drawing.Image]::FromFile(
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SourcePath))
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$frames = [System.Collections.Generic.List[byte[]]]::new()
try {
    foreach ($size in $sizes) {
        $bitmap = [System.Drawing.Bitmap]::new($size, $size,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $stream = [System.IO.MemoryStream]::new()
        try {
            $graphics.Clear([System.Drawing.Color]::Transparent)
            $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $scale = [Math]::Min($size / $source.Width, $size / $source.Height)
            $width = [int][Math]::Round($source.Width * $scale)
            $height = [int][Math]::Round($source.Height * $scale)
            $graphics.DrawImage($source, [System.Drawing.Rectangle]::new(
                [int](($size - $width) / 2), [int](($size - $height) / 2), $width, $height))
            if ($size -eq 256) {
                $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            }
            else {
                # Small shell icons use uncompressed DIB data with an AND mask.
                # Reserve PNG compression for the 256-pixel image.
                $frameWriter = [System.IO.BinaryWriter]::new($stream,
                    [System.Text.Encoding]::UTF8, $true)
                try {
                    $maskStride = [int]([Math]::Floor(($size + 31) / 32) * 4)
                    $frameWriter.Write([uint32]40) # BITMAPINFOHEADER
                    $frameWriter.Write([int]$size)
                    $frameWriter.Write([int]($size * 2)) # Color and mask height.
                    $frameWriter.Write([uint16]1)
                    $frameWriter.Write([uint16]32)
                    $frameWriter.Write([uint32]0) # BI_RGB
                    $frameWriter.Write([uint32]0)
                    $frameWriter.Write([int]0)
                    $frameWriter.Write([int]0)
                    $frameWriter.Write([uint32]0)
                    $frameWriter.Write([uint32]0)
                    $mask = [byte[]]::new($maskStride * $size)
                    for ($row = 0; $row -lt $size; $row++) {
                        for ($column = 0; $column -lt $size; $column++) {
                            $pixel = $bitmap.GetPixel($column, $size - 1 - $row)
                            $frameWriter.Write([byte]$pixel.B)
                            $frameWriter.Write([byte]$pixel.G)
                            $frameWriter.Write([byte]$pixel.R)
                            $frameWriter.Write([byte]$pixel.A)
                            if ($pixel.A -eq 0) {
                                $maskIndex = $row * $maskStride + [int][Math]::Floor($column / 8)
                                $mask[$maskIndex] = [byte]($mask[$maskIndex] -bor (0x80 -shr ($column % 8)))
                            }
                        }
                    }
                    $frameWriter.Write($mask)
                    $frameWriter.Flush()
                }
                finally { $frameWriter.Dispose() }
            }
            $frames.Add($stream.ToArray())
        }
        finally {
            $stream.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
}
finally { $source.Dispose() }

# ICO directory followed by DIB/PNG frames; zero encodes 256 pixels.
$iconStream = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($iconStream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($index = 0; $index -lt $sizes.Count; $index++) {
        $dimension = if ($sizes[$index] -eq 256) { 0 } else { $sizes[$index] }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$index].Length)
        $writer.Write([uint32]$offset)
        $offset += $frames[$index].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
    $writer.Flush()
    foreach ($name in @('UnlockIt.ico', 'small.ico')) {
        $path = Join-Path $PSScriptRoot ('..\' + $name)
        [System.IO.File]::WriteAllBytes($path, $iconStream.ToArray())
        Write-Output "Icono actualizado: $path"
    }
}
finally { $writer.Dispose(); $iconStream.Dispose() }
