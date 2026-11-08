# 生成 res/app.ico：16/32/48 三尺寸 PNG 图标（蓝色圆底 + 白色放大镜）
Add-Type -AssemblyName System.Drawing

function Make-Png([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $blue = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 30, 110, 220))
    $g.FillEllipse($blue, [int]($s * 0.02), [int]($s * 0.02), [int]($s * 0.96), [int]($s * 0.96))

    $penW = [Math]::Max(1, [int]($s * 0.13))
    $white = New-Object System.Drawing.Pen([System.Drawing.Color]::White, $penW)
    $white.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $white.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

    $r = [int]($s * 0.22)                       # 镜片半径
    $cx = [int]($s * 0.20); $cy = [int]($s * 0.20)
    $g.DrawEllipse($white, $cx, $cy, $r * 2, $r * 2)
    $g.DrawLine($white,
        [int]($cx + $r * 1.42), [int]($cy + $r * 1.42),
        [int]($s * 0.78),       [int]($s * 0.78))

    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = @(16, 32, 48)
$pngs = @()
foreach ($s in $sizes) { $pngs += ,(Make-Png $s) }

$fs = New-Object System.IO.FileStream("D:\data\qx\z.ai\search_tool\res\app.ico", [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $bw.Write([byte]$sizes[$i]); $bw.Write([byte]$sizes[$i])
    $bw.Write([byte]0);          $bw.Write([byte]0)
    $bw.Write([uint16]1);        $bw.Write([uint16]32)
    $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close(); $fs.Close()
Write-Output "ICO-CREATED"
