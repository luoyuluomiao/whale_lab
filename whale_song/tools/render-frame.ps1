# 把 `whale_song --dump` 导出的着色分段渲染成 PNG。
#
# 为什么不用截图？因为控制台里的真彩 ANSI 没法直接存成图，
# 但 dump 出来的分段数据本来就是"哪一格什么颜色"，画起来最省事。
#
# 用法：
#   pwsh -File render-frame.ps1 -Exe ..\build\bin\whale_song.exe -Out frame.png

param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\build\bin\whale_song.exe"),
    [string]$Out = (Join-Path $PSScriptRoot "..\whale_song_frame.png")
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $Exe)) { throw "找不到可执行文件: $Exe" }

$lines = & $Exe --dump
if ($LASTEXITCODE -ne 0) { throw "whale_song --dump 退出码 $LASTEXITCODE" }

$head  = $lines[0] -split "`t"
$cols  = [int]$head[0]
$rows  = [int]$head[1]

$runs = @()
foreach ($line in $lines[1..($lines.Count - 1)]) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $f = $line -split "`t", 5
    $runs += [pscustomobject]@{
        Y     = [int]$f[0]
        X     = [int]$f[1]
        Len   = [int]$f[2]
        Cat   = [int]$f[3]
        Text  = if ($f.Count -ge 5) { $f[4] } else { '' }
    }
}

# 字体与格子尺寸：等宽字体量一下 'M' 的宽度，保证 100 列不会错位
$drawFont = [System.Drawing.Font]::new("Consolas", [single]15, [System.Drawing.FontStyle]::Regular)
$sf       = [System.Drawing.StringFormat]::GenericTypographic
$probe    = [System.Drawing.Bitmap]::new(1, 1)
$g0       = [System.Drawing.Graphics]::FromImage($probe)
$cellW    = [math]::Ceiling($g0.MeasureString("M", $drawFont, 1000, $sf).Width)
$g0.Dispose()
$probe.Dispose()
$cellH = 20

$img = New-Object System.Drawing.Bitmap([int]($cols * $cellW), [int]($rows * $cellH))
$g   = [System.Drawing.Graphics]::FromImage($img)
$g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::None
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

$g.Clear([System.Drawing.Color]::FromArgb(10, 18, 32))

# 类别 -> 颜色（和 C++ 里 colorFor 的意图保持一致）
function Get-CatColor([int]$cat) {
    switch ($cat) {
        1 { return [System.Drawing.Color]::FromArgb(127, 224, 255) }   # 鲸鱼：亮青
        2 { return [System.Drawing.Color]::FromArgb(47, 143, 224) }    # 波浪：海水蓝
        3 { return [System.Drawing.Color]::FromArgb(207, 230, 255) }   # 气泡：淡白蓝
        default { return [System.Drawing.Color]::FromArgb(90, 150, 205) }
    }
}

foreach ($cat in 1, 2, 3) {
    $brush = New-Object System.Drawing.SolidBrush((Get-CatColor $cat))
    foreach ($r in $runs | Where-Object { $_.Cat -eq $cat -and $_.Text.Trim().Length -gt 0 }) {
        $g.DrawString($r.Text, $drawFont, $brush, [float]($r.X * $cellW), [float]($r.Y * $cellH), $sf)
    }
    $brush.Dispose()
}

$g.Dispose()
$full = [System.IO.Path]::GetFullPath($Out)
$pw = $img.Width
$ph = $img.Height
$img.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$img.Dispose()
$drawFont.Dispose()

"已生成: $full (${pw}x${ph})"
