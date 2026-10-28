@echo off
rem Removes MKX Access from Mortal Kombat X.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
