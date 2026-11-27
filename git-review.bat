@echo off
rem Read-only final Git review (LIAS automation "git-review"). Changes nothing in the repository.
rem Usage: git-review.bat [base-ref]     e.g.  git-review.bat origin/main
setlocal
cd /d "%~dp0"
set "ARGS={\"report\":true}"
if not "%~1"=="" set "ARGS={\"report\":true,\"base\":\"%~1\"}"
python .claude\lias\cli.py automation run git-review --args "%ARGS%"
set "CODE=%ERRORLEVEL%"
rem Keep the window open only when the file was double-clicked.
echo %cmdcmdline% | find /i "/c """"%~f0""" >nul && pause
exit /b %CODE%
