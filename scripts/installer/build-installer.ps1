# Release pipeline: Release build -> deployment tree (scripts/deploy/Deploy.ps1) -> clean staging -> Inno Setup.
#
# Defaults: build out\build with the CMake `deploy` target (recreates out\deployment), installer in out\release.
# -BuildDir / -OutputDir (absolute, or relative to the repo root) validate elsewhere: if either differs from its
# default, out\deployment is not touched and the deployment tree is built in <OutputDir>\deployment.
# <OutputDir>\staging and that deployment folder are deleted and recreated.
# Never kills processes: refuses if one runs from a directory that would be overwritten.
param(
    [string]$BuildDir,
    [string]$OutputDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent

function Resolve-Dir([string]$Path, [string]$Default) {
    if (-not $Path) { $Path = $Default }
    return [IO.Path]::GetFullPath([IO.Path]::Combine($root, $Path)).TrimEnd('\')
}

function Stop-WithRefusal([string]$Reason) {
    Write-Output "REFUSED: $Reason"
    exit 2
}

function Assert-NothingRunningFrom([string[]]$Dirs) {
    $hits = @(foreach ($p in (Get-CimInstance Win32_Process)) {
        $exe = $p.ExecutablePath
        $inside = $exe -and @($Dirs | Where-Object {
            $exe.StartsWith("$_\", [StringComparison]::OrdinalIgnoreCase) }).Count
        if ($inside -or (-not $exe -and $p.Name -like 'Motiva*')) { "$($p.Name) (PID $($p.ProcessId)) $exe" }
    })
    if ($hits.Count) {
        Stop-WithRefusal ("processes are running from a directory that would be overwritten; close them " +
            "manually and retry (nothing was killed): $($hits -join '; ')")
    }
}

$defaultBuild = Resolve-Dir '' 'out\build'
$defaultOutput = Resolve-Dir '' 'out\release'
$BuildDir = Resolve-Dir $BuildDir $defaultBuild
$OutputDir = Resolve-Dir $OutputDir $defaultOutput
$custom = ($BuildDir -ne $defaultBuild) -or ($OutputDir -ne $defaultOutput)
$deployDir = if ($custom) { "$OutputDir\deployment" } else { "$root\out\deployment" }
$stage = "$OutputDir\staging"

$cache = "$BuildDir\CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cache)) { Stop-WithRefusal "build directory is not configured: $BuildDir" }
$source = (Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:\w+=(.*)$').Matches.Groups[1].Value
if (-not $source -or [IO.Path]::GetFullPath($source).TrimEnd('\') -ne $root) {
    Stop-WithRefusal "build directory was configured for another source tree: $source"
}
$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $iscc)) { Stop-WithRefusal 'Inno Setup 6 compiler (ISCC.exe) not found' }
Assert-NothingRunningFrom $BuildDir, $deployDir, $OutputDir

if ($custom) {
    cmake --build $BuildDir --target Motiva MotivaLauncher MotivaBrowserHost MotivaShell
    if ($LASTEXITCODE) { throw 'build failed' }
    $qt6 = (Select-String -LiteralPath $cache -Pattern '^Qt6_DIR:\w+=(.*)$').Matches.Groups[1].Value
    $qtBin = [IO.Path]::GetFullPath("$qt6\..\..\..\bin")
    Assert-NothingRunningFrom $BuildDir, $deployDir, $OutputDir
    & "$root\scripts\deploy\Deploy.ps1" -BuildDir $BuildDir -QtBinDir $qtBin -OutDir $deployDir
} else {
    cmake --build $BuildDir --target deploy
    if ($LASTEXITCODE) { throw 'deploy failed' }
}

Assert-NothingRunningFrom $deployDir, $OutputDir
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$deployDir\*" $stage -Recurse
& $iscc "/DStagingDir=$stage" "/O$OutputDir" "$PSScriptRoot\MotivaSetup.iss"
if ($LASTEXITCODE) { throw 'ISCC failed' }
Remove-Item -LiteralPath $stage -Recurse -Force
Get-Item -LiteralPath "$OutputDir\MotivaSetup.exe"
