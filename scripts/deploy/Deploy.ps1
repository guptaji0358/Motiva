<#
.SYNOPSIS
    Assembles Motiva's clean deployment directory (see the "Organize
    Final Deployment Output" task).

.DESCRIPTION
    Turns a normal Release build (build/Motiva.exe + build/MotivaLauncher.exe)
    into:

        deployment/
        |-- Motiva.exe                 (the launcher, renamed on copy)
        `-- resources/
            |-- bin/
            |   |-- Motiva.exe          (the real Qt-linked application)
            |   `-- qt.conf             (points Qt's plugin loader at ../plugins)
            |-- Qt/                     (Qt6*.dll, FFmpeg, MinGW runtime DLLs)
            `-- plugins/
                |-- platforms/ styles/ imageformats/ iconengines/ tls/
                    networkinformation/ generic/ multimedia/ ...

    windeployqt is run against a throwaway staged copy of the real exe
    (never against build/Motiva.exe itself, and never against the final
    resources/bin/Motiva.exe in place) purely so its output - a flat
    "exe + loose DLLs + plugin subfolders" layout - can be picked apart
    and moved into the tidier structure above without windeployqt ever
    touching (or needing to be re-run against) the actual deployment
    tree. This keeps the normal `build/` directory windeployqt already
    populates for day-to-day dev runs completely untouched - see
    CMakeLists.txt's MotivaLauncher/deploy target comments for why this
    whole thing is opt-in and separate from the normal dev build.

    App assets (Assets/*.svg, motiva.ico, ...) are NOT copied into
    resources/ - they are already embedded into Motiva.exe at build time
    via resources/app.qrc (Qt's own resource system), so there is
    nothing left on disk for the deployed app to load them from in the
    first place. Duplicating them into resources/Assets/ as well would
    just be dead weight with no runtime purpose.

.PARAMETER BuildDir
    The CMake build directory containing Motiva.exe and MotivaLauncher.exe
    (already built, e.g. via `cmake --build build --target Motiva MotivaLauncher`).

.PARAMETER QtBinDir
    The Qt "bin" directory containing windeployqt.exe (and the Qt DLLs
    windeployqt copies from).

.PARAMETER OutDir
    Where to (re)create the clean deployment tree. Removed and recreated
    from scratch on every run, so it never accumulates stale files from a
    previous deploy.
#>
param(
    [string]$BuildDir = "out/build",
    [string]$QtBinDir,
    [string]$OutDir = "out/deployment"
)

$ErrorActionPreference = "Stop"

function Resolve-RequiredPath([string]$Path, [string]$Description) {
    if (-not (Test-Path $Path)) {
        throw "$Description not found at: $Path"
    }
    return (Resolve-Path $Path).Path
}

$BuildDir = Resolve-RequiredPath $BuildDir "Build directory"
$realExe = Resolve-RequiredPath (Join-Path $BuildDir "Motiva.exe") "Real application binary (build Motiva first)"
$launcherExe = Resolve-RequiredPath (Join-Path $BuildDir "MotivaLauncher.exe") "Launcher binary (build MotivaLauncher first)"

if (-not $QtBinDir) {
    throw "QtBinDir was not provided - pass -QtBinDir '<Qt install>/bin' (CMake's deploy target passes this automatically)."
}
$windeployqt = Resolve-RequiredPath (Join-Path $QtBinDir "windeployqt.exe") "windeployqt.exe"

Write-Host "Building deployment tree at: $OutDir"

if (Test-Path $OutDir) {
    Remove-Item -Recurse -Force $OutDir
}
$resourcesDir = Join-Path $OutDir "resources"
$binDir = Join-Path $resourcesDir "bin"
$qtDllDir = Join-Path $resourcesDir "Qt"
$pluginsDir = Join-Path $resourcesDir "plugins"
foreach ($dir in @($OutDir, $resourcesDir, $binDir, $qtDllDir, $pluginsDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

# --- The two executables ---
Copy-Item $launcherExe (Join-Path $OutDir "Motiva.exe") -Force
Copy-Item $realExe (Join-Path $binDir "Motiva.exe") -Force
$shellDll = Join-Path $BuildDir "MotivaShell.dll"
if (Test-Path $shellDll) {
    Copy-Item $shellDll (Join-Path $binDir "MotivaShell.dll") -Force
}

# --- Browser extension: the Native Messaging helper and the unpacked extension ---
$hostExe = Join-Path $BuildDir "MotivaBrowserHost.exe"
if (Test-Path $hostExe) {
    Copy-Item $hostExe (Join-Path $binDir "MotivaBrowserHost.exe") -Force
}
$extSrc = Join-Path $PSScriptRoot "..\extension"
if (Test-Path (Join-Path $extSrc "manifest.json")) {
    $extDst = Join-Path $binDir "extension"
    New-Item -ItemType Directory -Force -Path $extDst | Out-Null
    Copy-Item (Join-Path $extSrc "*") $extDst -Recurse -Force -Exclude "tests" # not shipped
    $extTests = Join-Path $extDst "tests"
    if (Test-Path -LiteralPath $extTests) { Remove-Item -LiteralPath $extTests -Recurse -Force }
}

# --- rclone (MIT): the cloud-storage engine. Its licence must travel with it. ---
$rcloneSrc = Join-Path $PSScriptRoot "..\..\third_party\rclone"
$rcloneDst = Join-Path $binDir "rclone"
if (Test-Path (Join-Path $rcloneSrc "rclone.exe")) {
    New-Item -ItemType Directory -Force -Path $rcloneDst | Out-Null
    Copy-Item (Join-Path $rcloneSrc "rclone.exe") $rcloneDst -Force
    Copy-Item (Join-Path $rcloneSrc "LICENSE.txt") $rcloneDst -Force
    Copy-Item (Join-Path $rcloneSrc "README.txt") $rcloneDst -Force
    $ini = Join-Path $rcloneSrc "motiva-cloud.ini"
    if (Test-Path $ini) { Copy-Item $ini $rcloneDst -Force }
} else {
    Write-Warning "third_party/rclone/rclone.exe not found - cloud storage will not work in this deployment."
}

# Tells Qt's OWN plugin loader (independent of the OS DLL search PATH
# the launcher sets up) where to find platforms/styles/imageformats/...
# relative to the real exe's own directory - this is the standard,
# supported Qt mechanism for relocating plugins, not a workaround.
@"
[Paths]
Plugins = ../plugins
"@ | Set-Content -Path (Join-Path $binDir "qt.conf") -Encoding ascii

# --- Stage windeployqt's own output, then pick it apart ---
$stageDir = Join-Path $OutDir "_stage"
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
$stagedExe = Join-Path $stageDir "Motiva.exe"
Copy-Item $realExe $stagedExe -Force

Write-Host "Running windeployqt against a staged copy..."
# Motiva's playlist library (.mtv) only uses SQLite (qsqlite, which has
# SQLite compiled in). Linking Qt6Sql makes windeployqt add every SQL
# driver; the client-server ones are excluded - they'd be dead weight.
& $windeployqt --release --no-translations --no-system-d3d-compiler `
    --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql $stagedExe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}
# --no-system-d3d-compiler only skips windeployqt's bundled fallback copy.
# D3DCOMPILER_47.dll is NOT copied: Motiva links the system d3dcompiler import
# library (CMakeLists.txt) and Windows 8.1+ ships that DLL in System32.

Get-ChildItem $stageDir -Filter "*.dll" | ForEach-Object {
    Move-Item $_.FullName (Join-Path $qtDllDir $_.Name) -Force
}
Get-ChildItem $stageDir -Directory | ForEach-Object {
    Move-Item $_.FullName (Join-Path $pluginsDir $_.Name) -Force
}
Remove-Item -Recurse -Force $stageDir

# --- Fail if the tree is incomplete: a missing DLL or plugin otherwise only
# shows up on the user's machine (0xC0000135 "DLL not found", no video, ...).
# Keep in step with the Qt modules in CMakeLists.txt. FFmpeg and style file
# names carry versions, hence the wildcards. rclone stays optional (see above).
$required = @(
    "Motiva.exe",
    "resources\bin\Motiva.exe",
    "resources\bin\qt.conf",
    "resources\bin\MotivaShell.dll",
    "resources\bin\MotivaBrowserHost.exe",
    "resources\bin\extension\manifest.json",
    "resources\Qt\Qt6Core.dll",
    "resources\Qt\Qt6Gui.dll",
    "resources\Qt\Qt6Widgets.dll",
    "resources\Qt\Qt6OpenGL.dll",
    "resources\Qt\Qt6OpenGLWidgets.dll",
    "resources\Qt\Qt6Multimedia.dll",
    "resources\Qt\Qt6MultimediaWidgets.dll",
    "resources\Qt\Qt6Network.dll",
    "resources\Qt\Qt6Sql.dll",
    "resources\Qt\Qt6Svg.dll",
    "resources\Qt\libgcc_s_seh-1.dll",
    "resources\Qt\libstdc++-6.dll",
    "resources\Qt\libwinpthread-1.dll",
    "resources\Qt\avcodec-*.dll",
    "resources\Qt\avformat-*.dll",
    "resources\Qt\avutil-*.dll",
    "resources\Qt\swresample-*.dll",
    "resources\Qt\swscale-*.dll",
    "resources\plugins\platforms\qwindows.dll",
    "resources\plugins\styles\*.dll",
    "resources\plugins\multimedia\ffmpegmediaplugin.dll",
    "resources\plugins\imageformats\qgif.dll",
    "resources\plugins\imageformats\qjpeg.dll",
    "resources\plugins\imageformats\qsvg.dll",
    "resources\plugins\imageformats\qwebp.dll",
    "resources\plugins\iconengines\qsvgicon.dll",
    "resources\plugins\sqldrivers\qsqlite.dll",
    "resources\plugins\tls\qschannelbackend.dll"
)
$missing = @($required | Where-Object {
    $dir = Join-Path $OutDir (Split-Path $_ -Parent)
    -not ((Test-Path -LiteralPath $dir) -and
          (Get-ChildItem -LiteralPath $dir -Filter (Split-Path $_ -Leaf) -File))
})
if ($missing.Count -gt 0) {
    throw "Deployment tree is incomplete, missing: $($missing -join ', ')"
}

Write-Host ""
Write-Host "Deployment tree:"
Get-ChildItem -Recurse $OutDir | ForEach-Object {
    $rel = $_.FullName.Substring((Resolve-Path $OutDir).Path.Length + 1)
    $depth = ($rel -split '\\').Length - 1
    Write-Host ("  " * $depth + $_.Name)
}

Write-Host ""
Write-Host "Deployment root contents (should be ONLY Motiva.exe and resources/):"
Get-ChildItem $OutDir | ForEach-Object { Write-Host "  $($_.Name)" }
