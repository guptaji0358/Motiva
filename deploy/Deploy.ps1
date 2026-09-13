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
    [string]$BuildDir = "build",
    [string]$QtBinDir,
    [string]$OutDir = "deployment"
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
& $windeployqt --release --no-translations --no-system-d3d-compiler $stagedExe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}
# --no-system-d3d-compiler above only skips windeployqt's OWN bundled
# fallback copy - D3Dcompiler_47.dll is still required (D3DWallpaperRenderer
# uses it) and is picked up as a normal loose DLL below since Motiva.exe
# already links it directly (see CMakeLists.txt's target_link_libraries).

Get-ChildItem $stageDir -Filter "*.dll" | ForEach-Object {
    Move-Item $_.FullName (Join-Path $qtDllDir $_.Name) -Force
}
Get-ChildItem $stageDir -Directory | ForEach-Object {
    Move-Item $_.FullName (Join-Path $pluginsDir $_.Name) -Force
}
Remove-Item -Recurse -Force $stageDir

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
