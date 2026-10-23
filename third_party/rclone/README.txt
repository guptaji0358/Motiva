rclone (https://rclone.org) - MIT licence, see LICENSE.txt (must ship with every copy).

Bundled version: 1.75.2, windows-amd64
Source: https://downloads.rclone.org/v1.75.2/rclone-v1.75.2-windows-amd64.zip
SHA-256 of the zip (matches the published SHA256SUMS): 8a9db2bad1dc79a65b3348fd857069598e2aab7cf4d118b77b1df23055aff818

rclone.exe (85 MB) is git-ignored. To (re)create it: download the zip above, verify the hash,
and copy rclone.exe next to this file. The CMake build copies this folder to <build>/rclone/
and deploy/Deploy.ps1 ships it in resources/bin/rclone/.
