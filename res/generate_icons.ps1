param(
    [string]$ChromePath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not $ChromePath) {
    $chromeCandidates = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
    )
    $ChromePath = $chromeCandidates | Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}
if (-not $ChromePath -or -not (Test-Path -LiteralPath $ChromePath)) {
    throw 'Chrome or Edge is required to rasterize the SVG icon sources.'
}

function Write-Ico {
    param(
        [string]$PngPath,
        [string]$IcoPath
    )

    $sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    $source = [System.Drawing.Image]::FromFile($PngPath)
    try {
        $images = foreach ($size in $sizes) {
            $bitmap = [System.Drawing.Bitmap]::new($size, $size,
                [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
                try {
                    $graphics.Clear([System.Drawing.Color]::Transparent)
                    $graphics.CompositingMode =
                        [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
                    $graphics.CompositingQuality =
                        [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                    $graphics.InterpolationMode =
                        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $graphics.PixelOffsetMode =
                        [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                    $graphics.DrawImage($source, 0, 0, $size, $size)
                } finally {
                    $graphics.Dispose()
                }
                $stream = [System.IO.MemoryStream]::new()
                if ($size -le 64) {
                    # Windows' older HICON loaders expect BMP/DIB frames for
                    # small sizes. The height includes the XOR and AND masks.
                    $frameWriter = [System.IO.BinaryWriter]::new($stream)
                    $frameWriter.Write([uint32]40)
                    $frameWriter.Write([int32]$size)
                    $frameWriter.Write([int32]($size * 2))
                    $frameWriter.Write([uint16]1)
                    $frameWriter.Write([uint16]32)
                    $frameWriter.Write([uint32]0)
                    $frameWriter.Write([uint32]($size * $size * 4))
                    $frameWriter.Write([int32]0)
                    $frameWriter.Write([int32]0)
                    $frameWriter.Write([uint32]0)
                    $frameWriter.Write([uint32]0)
                    for ($y = $size - 1; $y -ge 0; --$y) {
                        for ($x = 0; $x -lt $size; ++$x) {
                            $pixel = $bitmap.GetPixel($x, $y)
                            $frameWriter.Write([byte]$pixel.B)
                            $frameWriter.Write([byte]$pixel.G)
                            $frameWriter.Write([byte]$pixel.R)
                            $frameWriter.Write([byte]$pixel.A)
                        }
                    }
                    $maskBytes = [int]([math]::Ceiling($size / 32.0) * 4 * $size)
                    $frameWriter.Write([byte[]]::new($maskBytes))
                    $frameWriter.Flush()
                    $frameWriter.Dispose()
                } else {
                    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
                }
                ,$stream.ToArray()
                $stream.Dispose()
            } finally {
                $bitmap.Dispose()
            }
        }

        $output = [System.IO.File]::Open($IcoPath, [System.IO.FileMode]::Create)
        $writer = [System.IO.BinaryWriter]::new($output)
        try {
            $writer.Write([uint16]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]$images.Count)
            $offset = 6 + 16 * $images.Count
            for ($index = 0; $index -lt $images.Count; ++$index) {
                $size = $sizes[$index]
                $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
                $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
                $writer.Write([byte]0)
                $writer.Write([byte]0)
                $writer.Write([uint16]1)
                $writer.Write([uint16]32)
                $writer.Write([uint32]$images[$index].Length)
                $writer.Write([uint32]$offset)
                $offset += $images[$index].Length
            }
            foreach ($image in $images) { $writer.Write($image) }
        } finally {
            $writer.Dispose()
            $output.Dispose()
        }
    } finally {
        $source.Dispose()
    }
}

$iconSets = Join-Path $PSScriptRoot 'IconSets'
$definitions = @(
    @{ Svg = Join-Path $PSScriptRoot 'EasyTunnel.svg'; Ico = Join-Path $PSScriptRoot 'EasyTunnel.ico' },
    @{ Svg = Join-Path $iconSets 'EasyTunnelDisconnected.svg'; Ico = Join-Path $iconSets 'EasyTunnelDisconnected.ico' },
    @{ Svg = Join-Path $iconSets 'EasyTunnelRx.svg'; Ico = Join-Path $iconSets 'EasyTunnelRx.ico' },
    @{ Svg = Join-Path $iconSets 'EasyTunnelTx.svg'; Ico = Join-Path $iconSets 'EasyTunnelTx.ico' },
    @{ Svg = Join-Path $iconSets 'EasyTunnelRxTx.svg'; Ico = Join-Path $iconSets 'EasyTunnelRxTx.ico' }
)

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$tempDirectory = Join-Path $tempRoot ("EasyTunnel-icons-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tempDirectory | Out-Null
try {
    foreach ($definition in $definitions) {
        $pngName = ([System.IO.Path]::GetFileNameWithoutExtension($definition.Svg)) + '.png'
        $pngPath = Join-Path $tempDirectory $pngName
        $profileDirectory = Join-Path $tempDirectory ($pngName + '-profile')
        $svgUri = [System.Uri]::new([System.IO.Path]::GetFullPath($definition.Svg)).AbsoluteUri
        & $ChromePath --headless=new --disable-gpu --hide-scrollbars `
            --default-background-color=00000000 --window-size=512,512 `
            "--user-data-dir=$profileDirectory" "--screenshot=$pngPath" $svgUri
        for ($attempt = 0; $attempt -lt 50 -and
            -not (Test-Path -LiteralPath $pngPath); ++$attempt) {
            Start-Sleep -Milliseconds 100
        }
        if (-not (Test-Path -LiteralPath $pngPath)) {
            throw "Failed to rasterize $($definition.Svg)"
        }
        Write-Ico -PngPath $pngPath -IcoPath $definition.Ico
        Write-Host "Generated $($definition.Ico)"
    }
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($tempDirectory)
    if ($resolvedTemp.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        $resolvedTemp -ne $tempRoot) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
