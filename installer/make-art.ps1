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

# ================= Custom control artwork (installer/assets/ui/*.bmp) =================
# Drawn at 2x; the installer scales them to the wizard's DPI with halftone stretching.
# BMP has no alpha, so they are drawn on the exact wizard background RGB(12,14,30).
$ui = "$PSScriptRoot\assets\ui"; New-Item -ItemType Directory $ui -Force | Out-Null
$BG = [System.Drawing.Color]::FromArgb(12,14,30)
function RR($x,$y,$w,$h,$r){
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d=$r*2; $p.AddArc($x,$y,$d,$d,180,90); $p.AddArc($x+$w-$d,$y,$d,$d,270,90)
  $p.AddArc($x+$w-$d,$y+$h-$d,$d,$d,0,90); $p.AddArc($x,$y+$h-$d,$d,$d,90,90); $p.CloseFigure(); return $p
}
function C($r,$g,$b,$a=255){ [System.Drawing.Color]::FromArgb($a,$r,$g,$b) }
function New-UiBmp($w,$h){
  $b = New-Object System.Drawing.Bitmap $w,$h,([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  $g = [System.Drawing.Graphics]::FromImage($b); $g.SmoothingMode='AntiAlias'; $g.Clear($BG)
  return @($b,$g)
}
function Draw-Button($name,$w,$h,$fillA,$fillB,$border,$glow){
  $b,$g = New-UiBmp $w $h
  if($glow){ $pw = New-Object System.Drawing.Pen (C 90 190 255 70),6; $g.DrawPath($pw,(RR 1 1 ($w-3) ($h-3) 24)) }
  $path = RR 5 5 ($w-11) ($h-11) 20
  $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Rectangle]::new(0,0,$w,$h)),$fillA,$fillB,0.0
  $g.FillPath($br,$path)
  if($border){ $g.DrawPath((New-Object System.Drawing.Pen $border,2.4),$path) }
  $b.Save("$ui\$name.bmp",[System.Drawing.Imaging.ImageFormat]::Bmp)
}
# primary 300x80 (150x40 @1x) ; states 0 normal,1 hover,2 pressed,3 disabled
Draw-Button 'btn-primary-0' 300 80 (C 52 140 255) (C 130 88 255) (C 150 200 255 120) $false
Draw-Button 'btn-primary-1' 300 80 (C 84 172 255) (C 160 116 255) (C 210 235 255 200) $true
Draw-Button 'btn-primary-2' 300 80 (C 36 108 220) (C 100 62 210) (C 120 170 240 120) $false
Draw-Button 'btn-primary-3' 300 80 (C 26 30 58) (C 26 30 58) (C 44 50 90) $false
# secondary 220x80 (110x40 @1x)
Draw-Button 'btn-secondary-0' 220 80 (C 24 28 56) (C 24 28 56) (C 66 76 128) $false
Draw-Button 'btn-secondary-1' 220 80 (C 32 38 76) (C 32 38 76) (C 90 190 255) $true
Draw-Button 'btn-secondary-2' 220 80 (C 17 20 42) (C 17 20 42) (C 70 150 230) $false
Draw-Button 'btn-secondary-3' 220 80 (C 14 16 34) (C 14 16 34) (C 34 38 70) $false

function Draw-Check($name,$on,$hover){
  $b,$g = New-UiBmp 48 48
  if($hover){ $g.DrawPath((New-Object System.Drawing.Pen (C 90 190 255 80),6),(RR 2 2 42 42 14)) }
  $path = RR 6 6 35 35 10
  if($on){
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Rectangle]::new(0,0,48,48)),(C 52 140 255),(C 140 90 255),45.0
    $g.FillPath($br,$path)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White),5; $pen.StartCap='Round'; $pen.EndCap='Round'; $pen.LineJoin='Round'
    $g.DrawLines($pen,@([System.Drawing.PointF]::new(14.5,24.5),[System.Drawing.PointF]::new(21.5,31.5),[System.Drawing.PointF]::new(34,17)))
  } else {
    $g.FillPath((New-Object System.Drawing.SolidBrush (C 18 21 44)),$path)
    $bc = if($hover){ C 90 190 255 } else { C 112 126 190 }
    $g.DrawPath((New-Object System.Drawing.Pen $bc,3.6),$path)
  }
  $b.Save("$ui\$name.bmp",[System.Drawing.Imaging.ImageFormat]::Bmp)
}
Draw-Check 'check-off' $false $false; Draw-Check 'check-off-hover' $false $true
Draw-Check 'check-on'  $true  $false; Draw-Check 'check-on-hover'  $true  $true

# progress track 1280x28 and fill 1280x16 (fill's 16px-high caps are drawn separately by the installer)
$b,$g = New-UiBmp 1280 28
$tp = RR 1 1 1277 25 13; $g.FillPath((New-Object System.Drawing.SolidBrush (C 20 24 50)),$tp); $g.DrawPath((New-Object System.Drawing.Pen (C 50 60 108),2),$tp)
$b.Save("$ui\progress-track.bmp",[System.Drawing.Imaging.ImageFormat]::Bmp)
$b,$g = New-UiBmp 1280 16
$fp = RR 0 0 1279 15 7.5
$fb = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Rectangle]::new(0,0,1280,16)),(C 0 200 255),(C 150 90 255),0.0
$blend = New-Object System.Drawing.Drawing2D.ColorBlend 3
$blend.Colors = @((C 0 205 255),(C 80 140 255),(C 160 90 255)); $blend.Positions=@(0.0,0.55,1.0); $fb.InterpolationColors=$blend
$g.FillPath($fb,$fp)
$g.FillPath((New-Object System.Drawing.SolidBrush (C 255 255 255 40)),(RR 2 2 1276 6 3))
$fill = $b
function Crop-Save($bmp,$x,$w,$name){ $r=[System.Drawing.Rectangle]::new($x,0,$w,$bmp.Height); $c=$bmp.Clone($r,$bmp.PixelFormat); $c.Save("$ui\$name.bmp",[System.Drawing.Imaging.ImageFormat]::Bmp) }
Crop-Save $fill 0 8 'progress-fill-l'; Crop-Save $fill 8 1264 'progress-fill-m'; Crop-Save $fill 1272 8 'progress-fill-r'


# ================= Uninstaller heroes (installer/assets/uninstall-*.bmp) =================
# Same aurora language as the installer. Installed next to the app (see MotivaSetup.iss) because
# the uninstaller has no access to the installer's embedded files. Aspect matches the dialog's hero area.
$UW = 1600; $UH = 1034; $dy = 64
$bmp,$g = New-Canvas $UW $UH; Aurora $g $UW $UH
Glow $g ($UW/2) (250+$dy) 260 @(150,90,255) 110
$g.DrawImage($logo,($UW/2-100),(150+$dy),200,200)
Center $g 'Uninstall Motiva' (New-Object System.Drawing.Font 'Segoe UI Semibold',58) $white (392+$dy) $UW
Center $g 'Remove Motiva from this computer?' (New-Object System.Drawing.Font 'Segoe UI Light',30) $soft (526+$dy) $UW
Center $g 'Your settings and videos will be kept.' (New-Object System.Drawing.Font 'Segoe UI',20) $dim (612+$dy) $UW
Center $g 'Robin Gupta Studios' (New-Object System.Drawing.Font 'Segoe UI',15) $dim ($UH-76) $UW
Save-Scaled $bmp 800 "$PSScriptRoot\assets\uninstall-welcome.bmp" ([System.Drawing.Imaging.ImageFormat]::Bmp)

$bmp,$g = New-Canvas $UW $UH; Aurora $g $UW $UH
Glow $g ($UW/2) (260+$dy) 240 @(80,220,190) 100
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255,120,235,210)),8
$g.DrawEllipse($pen,($UW/2-80),(190+$dy),160,160)
$pen2 = New-Object System.Drawing.Pen ([System.Drawing.Color]::White),13; $pen2.StartCap='Round'; $pen2.EndCap='Round'; $pen2.LineJoin='Round'
$g.DrawLines($pen2,@([System.Drawing.PointF]::new($UW/2-34,(272+$dy)),[System.Drawing.PointF]::new($UW/2-8,(298+$dy)),[System.Drawing.PointF]::new($UW/2+38,(240+$dy))))
Center $g 'Motiva has been removed' (New-Object System.Drawing.Font 'Segoe UI Semibold',54) $white (440+$dy) $UW
Center $g 'You can reinstall Motiva at any time.' (New-Object System.Drawing.Font 'Segoe UI Light',28) $soft (560+$dy) $UW
Center $g 'Robin Gupta Studios' (New-Object System.Drawing.Font 'Segoe UI',15) $dim ($UH-76) $UW
Save-Scaled $bmp 800 "$PSScriptRoot\assets\uninstall-done.bmp" ([System.Drawing.Imaging.ImageFormat]::Bmp)
