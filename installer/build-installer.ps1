# Release pipeline: Release build -> existing `deploy` target -> clean staging -> Inno Setup.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Get-Process Motiva -ErrorAction SilentlyContinue | Stop-Process -Force
cmake --build "$root\build" --target deploy
if ($LASTEXITCODE) { throw 'deploy failed' }
$stage = "$root\release\staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
Copy-Item "$root\deployment\*" $stage -Recurse
$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
& $iscc "$PSScriptRoot\MotivaSetup.iss"
if ($LASTEXITCODE) { throw 'ISCC failed' }
Remove-Item $stage -Recurse -Force
Get-Item "$root\release\MotivaSetup.exe"
