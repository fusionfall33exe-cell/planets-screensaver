@echo off
rem Planets: runs the installed Linux program inside WSL (after "make install" there).
rem Put this folder on the Windows PATH and "Planets" works in cmd and PowerShell.
rem Quit with q or Esc (Ctrl-C makes cmd ask "Terminate batch job?").
wsl.exe -e bash -lc "exec Planets %*"
