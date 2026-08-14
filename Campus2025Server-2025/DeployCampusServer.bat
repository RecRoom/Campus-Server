@echo off
setlocal EnableDelayedExpansion
REM ============================================================
REM   Deploy the Campus 2025 (Campus) server to the VPS
REM   Reads the root password from password.txt (same folder).
REM   Uploads campus-deploy.tar.gz, extracts it, runs install.sh
REM ============================================================

cd /d "%~dp0"

set HOST=50.114.179.17
set DOMAIN=gameauth.ddns.net
set USER=root
set PKG=campus-deploy.tar.gz
set PWFILE=password.txt

echo.
echo  ===========================================
echo    Campus 2025 - server deploy
echo    target: %USER%@%HOST%
echo    domain: https://%DOMAIN%
echo  ===========================================
echo.

REM --- checks -------------------------------------------------
if not exist "%PWFILE%" (
  echo  [X] %PWFILE% not found.
  echo      Create it next to this .bat and put ONLY the root
  echo      password on the first line.
  goto :fail
)

if not exist "%PKG%" (
  echo  [X] %PKG% not found in this folder.
  goto :fail
)

where pscp >nul 2>&1
if errorlevel 1 (
  echo  [X] PuTTY tools not found ^(pscp/plink^).
  echo      Install them first, then re-run this:
  echo.
  echo          winget install PuTTY.PuTTY
  echo.
  echo      Then close and reopen this window ^(PATH refresh^).
  goto :fail
)

REM --- read the password (first line only), never echo it ------
set PW=
for /f "usebackq delims=" %%p in ("%PWFILE%") do (
  if not defined PW set "PW=%%p"
)
if not defined PW (
  echo  [X] %PWFILE% is empty.
  goto :fail
)

echo  [1/3] Uploading %PKG% ...
echo        ^(~49 MB - pscp shows a live progress bar below^)
echo.
echo y | pscp -pw "!PW!" "%PKG%" %USER%@%HOST%:/root/
if errorlevel 1 (
  echo  [X] Upload failed - wrong password, or host unreachable.
  goto :fail
)
echo        upload complete.
echo.

echo  [2/3] Extracting + installing on the VPS ...
echo        Steps 2 and 3 on the server take ^~1 minute total.
echo        The apt step is silent for ^~30-60s - a spinner will show
echo        it's alive. Do NOT close this window.
echo.
REM 2025 server binary is Rec_Rewild_Test_Server (not Campus2025). install.sh
REM chmods it itself, so we don't need to touch the binary here. Our tar packs
REM contents flat (tar -czf ... -C $out .), so extract INTO a fresh campus-deploy/
REM rather than expecting the tar to contain a campus-deploy/ prefix.
echo y | plink -ssh -pw "!PW!" %USER%@%HOST% "cd /root && rm -rf campus-deploy && mkdir campus-deploy && echo '  extracting archive ...' && tar -xzf %PKG% -C campus-deploy && cd campus-deploy && chmod +x install.sh && ./install.sh"
if errorlevel 1 (
  echo  [X] Install failed - see the output above.
  goto :fail
)
echo.

echo  [3/3] Checking the server is answering ...
REM 2025 server: no direct :2059 endpoint any more (Kestrel listens on :8080 only,
REM Caddy terminates TLS + proxies from :443). Smoke test via the public domain.
timeout /t 3 /nobreak >nul
curl -sk --ssl-no-revoke -o nul -w "        https://%DOMAIN%/  ->  HTTP %%{http_code}%%\n" --max-time 10 https://%DOMAIN%/
echo.

REM 2025 server currently has no /buildinfo endpoint - skip the build-stamp check.
REM The install.sh already prints a local GET / smoke test result, and above we
REM checked the public URL, so any failure is visible there.
echo.

echo  ===========================================
echo    DONE!
echo.
echo    Game server : https://%DOMAIN%
echo.
echo    Admin panel is NOT public (on purpose).
echo    Reach it with a tunnel, in another window:
echo        ssh -L 8080:localhost:8080 %USER%@%HOST%
echo    then open  http://localhost:8080/admin
echo.
echo    Useful VPS commands:
echo        systemctl status campus
echo        journalctl -u campus -f      (live log)
echo        systemctl restart campus
echo.
echo    Server IP: %HOST%
echo    Domain:    https://%DOMAIN%
echo  ===========================================
echo.
echo   TIP: delete %PWFILE% when you're done - it's
echo        your root password sitting in plain text.
echo.
goto :end

:fail
echo.
echo  Deploy aborted.
echo.

:end
set PW=
pause