# Release pipeline: Release build -> existing `deploy` target -> clean staging -> Inno Setup.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Get-Process Motiva -ErrorAction SilentlyContinue | Stop-Process -Force
cmake --build "$root\out\build" --target deploy
if ($LASTEXITCODE) { throw 'deploy failed' }
$stage = "$root\out\release\staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
Copy-Item "$root\out\deployment\*" $stage -Recurse
$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
& $iscc "$PSScriptRoot\MotivaSetup.iss"
if ($LASTEXITCODE) { throw 'ISCC failed' }
Remove-Item $stage -Recurse -Force
Get-Item "$root\out\release\MotivaSetup.exe"
