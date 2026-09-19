# Regenerates installer-only artwork (not part of the app runtime). Output: installer/assets/*.bmp
# Hero images are drawn at 2x the wizard page size and stretched by the installer.
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$logo = [System.Drawing.Image]::FromFile("$root\Assets\application\motiva_256.png")
$HW = 1600; $HH = 900   # welcome/finish hero (matches WizardForm page aspect)

function New-Canvas($w,$h){
  $bmp = New-Object System.Drawing.Bitmap $w,$h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode='AntiAlias'; $g.InterpolationMode='HighQualityBicubic'; $g.TextRenderingHint='AntiAliasGridFit'
  $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0,0)),([System.Drawing.Point]::new($w,$h)),([System.Drawing.Color]::FromArgb(8,10,22)),([System.Drawing.Color]::FromArgb(24,14,54))
  $g.FillRectangle($bg,0,0,$w,$h)
  return @($bmp,$g)
}
function Glow($g,$cx,$cy,$r,$rgb,$alpha){
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath; $path.AddEllipse($cx-$r,$cy-$r,2*$r,2*$r)
  $pb = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
  $pb.CenterColor=[System.Drawing.Color]::FromArgb($alpha,$rgb[0],$rgb[1],$rgb[2])
  $pb.SurroundColors=@([System.Drawing.Color]::FromArgb(0,$rgb[0],$rgb[1],$rgb[2]))
  $g.FillEllipse($pb,$cx-$r,$cy-$r,2*$r,2*$r)
}
function Aurora($g,$w,$h){
  Glow $g ($w*0.10) ($h*0.95) ($w*0.55) @(0,190,230) 80
  Glow $g ($w*0.92) ($h*0.10) ($w*0.50) @(140,90,255) 85
  Glow $g ($w*0.55) ($h*0.55) ($w*0.42) @(50,120,255) 50
  # faint aurora ribbons
  for($i=0;$i -lt 3;$i++){
    $pts = @(); for($x=0;$x -le $w;$x+=40){ $pts += [System.Drawing.PointF]::new($x, $h*(0.62+0.05*$i) + 60*[Math]::Sin($x/260.0+$i*1.7)) }
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(28,120+40*$i,180,255)),(6+4*$i)
    $g.DrawCurve($pen,[System.Drawing.PointF[]]$pts)
  }
}
function Save-Scaled($src,$w,$path,$fmt=[System.Drawing.Imaging.ImageFormat]::Png){
  $h=[int]($src.Height*$w/$src.Width); $o=New-Object System.Drawing.Bitmap $w,$h
  $g=[System.Drawing.Graphics]::FromImage($o); $g.InterpolationMode='HighQualityBicubic'; $g.DrawImage($src,0,0,$w,$h)
  $o.Save($path,$fmt)
}
function Center($g,$text,$font,$brush,$y,$w){
  $sf = New-Object System.Drawing.StringFormat; $sf.Alignment='Center'
  $g.DrawString($text,$font,$brush,[System.Drawing.RectangleF]::new(0,$y,$w,200),$sf)
}
$white = [System.Drawing.Brushes]::White
$soft  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(200,205,225,255))
$dim   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(140,170,185,225))

# --- Welcome hero
$bmp,$g = New-Canvas $HW $HH; Aurora $g $HW $HH
Glow $g ($HW/2) 250 260 @(90,170,255) 110
$g.DrawImage($logo,($HW/2-100),150,200,200)
$f = New-Object System.Drawing.Font 'Segoe UI Semibold',66
Center $g 'M O T I V A' $f $white 390 $HW
Center $g 'Bring motion to your desktop.' (New-Object System.Drawing.Font 'Segoe UI Light',30) $soft 530 $HW
Center $g "Beautiful animated wallpapers,`ndesigned for your Windows desktop." (New-Object System.Drawing.Font 'Segoe UI',20) $dim 610 $HW
Center $g 'Robin Gupta Studios' (New-Object System.Drawing.Font 'Segoe UI',15) $dim 820 $HW
Save-Scaled $bmp 1200 "$PSScriptRoot\assets\motiva-installer-welcome.png"

# --- Finish hero (shorter: the launch checkbox sits below it)
$FH = 640
$bmp,$g = New-Canvas $HW $FH; Aurora $g $HW $FH
Glow $g ($HW/2) 170 230 @(80,220,190) 100
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255,120,235,210)),8
$g.DrawEllipse($pen,($HW/2-70),100,140,140)
$pen2 = New-Object System.Drawing.Pen ([System.Drawing.Color]::White),12; $pen2.StartCap='Round'; $pen2.EndCap='Round'; $pen2.LineJoin='Round'
$g.DrawLines($pen2,@([System.Drawing.PointF]::new($HW/2-30,172),[System.Drawing.PointF]::new($HW/2-8,196),[System.Drawing.PointF]::new($HW/2+34,146)))
Center $g 'Motiva is ready' (New-Object System.Drawing.Font 'Segoe UI Semibold',52) $white 280 $HW
Center $g 'Your desktop is ready for motion.' (New-Object System.Drawing.Font 'Segoe UI Light',26) $soft 400 $HW
Center $g 'Robin Gupta Studios' (New-Object System.Drawing.Font 'Segoe UI',15) $dim 560 $HW
Save-Scaled $bmp 800 "$PSScriptRoot\assets\motiva-installer-finish.bmp" ([System.Drawing.Imaging.ImageFormat]::Bmp)

# --- Header badge (small image on inner pages)
$bmp,$g = New-Canvas 440 464; Aurora $g 440 464
Glow $g 220 232 150 @(90,170,255) 90
$g.DrawImage($logo,110,122,220,220)
Save-Scaled $bmp 220 "$PSScriptRoot\assets\motiva-installer-banner.png"
