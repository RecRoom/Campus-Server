@echo off
REM ============================================================
REM   Build + package the server into campus-deploy.tar.gz
REM   (thin wrapper - the real work is in PackageServer.ps1)
REM ============================================================
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0PackageServer.ps1"
pause
