rclone (https://rclone.org) - MIT licence, see LICENSE.txt (must ship with every copy).

Bundled version: 1.75.2, windows-amd64
Source: https://downloads.rclone.org/v1.75.2/rclone-v1.75.2-windows-amd64.zip
SHA-256 of the zip (matches the published SHA256SUMS): 8a9db2bad1dc79a65b3348fd857069598e2aab7cf4d118b77b1df23055aff818

rclone.exe (85 MB) is git-ignored. To (re)create it: download the zip above, verify the hash,
and copy rclone.exe next to this file. The CMake build copies this folder to <build>/rclone/
and deploy/Deploy.ps1 ships it in resources/bin/rclone/.

Motiva modification: the sign-in result page compiled into rclone.exe is replaced by a Motiva-branded
page of identical byte length: run  python -I scripts/tools/patch_rclone_page.py third_party/rclone/rclone.exe
after (re)creating rclone.exe. (The SHA-256 above is of the unmodified zip.)

Motiva branding of the executable itself (icon + version info shown in Explorer / Task Manager / taskbar):
  python -I scripts/tools/brand_rclone_exe.py Assets/application/motiva.ico third_party/rclone/rclone.exe
Run it after patch_rclone_page.py whenever rclone.exe is (re)created, and on every shipped copy
(build/rclone, deployment/resources/bin/rclone). Only PE resources change; rclone's copyright is kept (MIT).
