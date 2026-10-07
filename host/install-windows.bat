@echo off
rem Double-click to install the NetMeter PC app (runs install-windows.ps1
rem without changing your PowerShell execution policy).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-windows.ps1"
pause
