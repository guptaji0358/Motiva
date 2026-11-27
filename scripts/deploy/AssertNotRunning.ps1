# Run by the CMake `deploy` target before Deploy.ps1: exit 1 if any Motiva.exe is running.
# Only looks - never stops or restarts a process. A failed process query also aborts (non-zero exit).
$ErrorActionPreference = 'Stop'

$running = @(Get-Process | Where-Object { $_.ProcessName -eq 'Motiva' })
if ($running.Count -gt 0) {
    $list = ($running | ForEach-Object { "PID $($_.Id) $($_.Path)".TrimEnd() }) -join '; '
    [Console]::Error.WriteLine("DEPLOY ABORTED: Motiva is running ($list). " +
        "No deployment files were changed. Exit Motiva (including from the system tray), then run the deploy target again.")
    exit 1
}
