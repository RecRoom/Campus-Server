# ============================================================
#  Build + package the 2025 Rec Room server (E12354 base, retargeted at
#  gameauth.ddns.net + our Photon IDs) into campus-deploy.tar.gz.
#
#  This is the SUCCESSOR to Campus2025Server\PackageServer.ps1 (which
#  built the 2023 server). Uses the same tarball name so the existing
#  DeployCampusServer.bat / GoLive.bat upload pipeline works unchanged;
#  just point them at this folder.
# ============================================================
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$src = "Server\2025-RecRoom-Server-E12354-main"        # C# project folder inside this repo
$csproj = "$src\Rec_Rewild_Test_Server.csproj"
$out = Join-Path $env:TEMP "campus-deploy-2025"
$pkg = "campus-deploy.tar.gz"

Write-Host ""
Write-Host "  ==========================================="
Write-Host "    Campus 2025 (E12354 base) - build + package"
Write-Host "  ==========================================="
Write-Host ""

if (-not (Test-Path $csproj)) { Write-Host "  [X] $csproj not found."; pause; exit 1 }

# Unique stamp for this package so DeployCampusServer.bat can confirm the VPS is
# actually running THIS package (not some stale prior build).
$tag     = -join ((1..6) | ForEach-Object { "0123456789abcdef"[(Get-Random -Max 16)] })
$buildId = "{0} #{1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $tag

Write-Host "  [1/4] Publishing (linux-x64, self-contained) ..."
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
dotnet publish $csproj -c Release -r linux-x64 --self-contained true -o $out -v quiet -clp:ErrorsOnly
if ($LASTEXITCODE -ne 0) { Write-Host "  [X] Build failed - see errors above."; pause; exit 1 }

Write-Host "  [2/4] Adding data + web + build stamp ..."
# The 2025 server reads Data / Web from ContentRoot (Environment.CurrentDirectory on
# the VPS = /opt/campus). Publish doesn't automatically copy either, so we do it here.
foreach ($d in @("Data", "Web", "AdminPanel")) {
    if (Test-Path "$src\$d") {
        Copy-Item "$src\$d" "$out\" -Recurse -Force
        $n = (Get-ChildItem "$src\$d" -Recurse -File -ErrorAction SilentlyContinue).Count
        Write-Host ("        + $d ({0} files)" -f $n)
    }
}
# Build stamp - written verbatim, no trailing newline; the installer compares this
# byte-for-byte against LastBuild.txt to prove the deploy landed.
[IO.File]::WriteAllText("$out\Data\BUILD.txt", $buildId)
[IO.File]::WriteAllText("LastBuild.txt", $buildId)

Write-Host "  [3/4] Writing installer ..."
# MUST be LF-only. A CRLF shebang makes bash fail with "bad interpreter".
$install = @'
#!/usr/bin/env bash
set -e
APP=/opt/campus
UNIT=/etc/systemd/system/campus.service
BIN_NAME=CampusServer

echo ""
echo "  [1/6] stopping the server ..."
systemctl stop campus 2>/dev/null || true
sleep 2

echo "  [2/6] copying files ..."
mkdir -p "$APP"

# Preserve live DBs across deploys — burner accounts, dorm state, heartbeats etc.
# live in Data/DBs/*.db and would otherwise be clobbered by the tarball's seed copies.
DB_BACKUP=$(mktemp -d /tmp/campus-dbs-XXXXXX)
if [ -d "$APP/Data/DBs" ] && [ "$(ls -A "$APP/Data/DBs" 2>/dev/null)" ]; then
    echo "        preserving existing $APP/Data/DBs -> $DB_BACKUP"
    cp -a "$APP/Data/DBs/." "$DB_BACKUP/"
    HAVE_LIVE_DBS=1
else
    HAVE_LIVE_DBS=0
fi

cp -r ./* "$APP"/
chmod +x "$APP/$BIN_NAME" 2>/dev/null || true

if [ "$HAVE_LIVE_DBS" = "1" ]; then
    mkdir -p "$APP/Data/DBs"
    cp -a "$DB_BACKUP/." "$APP/Data/DBs/"
    echo "        restored live DBs (kept burner accounts, etc.)"
fi
rm -rf "$DB_BACKUP"

echo "  [3/6] writing / refreshing systemd unit ..."
# Same unit name (campus.service) as the 2023 server so nothing else on the VPS
# needs to change - Caddy, journalctl -u campus, our watch script all keep working.
# ExecStart points at the 2025 binary (CampusServer, forced via AssemblyName in the csproj).
cat > "$UNIT" <<UNITEOF
[Unit]
Description=Campus 2025 (E12354 base) Rec Room server
After=network.target

[Service]
Type=simple
WorkingDirectory=$APP
# Kestrel binds inside the container. Caddy on the same box handles TLS + proxies
# https://gameauth.ddns.net/* to localhost:8080, so only HTTP-on-8080 is needed.
Environment=ASPNETCORE_URLS=http://0.0.0.0:8080
Environment=DOTNET_gcServer=1
ExecStart=$APP/$BIN_NAME
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNITEOF
systemctl daemon-reload

echo "  [4/6] starting the server ..."
systemctl enable campus >/dev/null 2>&1 || true
systemctl start campus
sleep 3

echo "  [5/6] status ..."
systemctl is-active campus || true

echo "  [6/6] smoke test - GET / ..."
# The 2025 server routes "/" to the NS response (WebController.NsRoot). If the
# server is up and answering right, this prints 200. On any other code it dumps
# the tail of the journal so the operator can see WHY it's not answering.
code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 http://127.0.0.1:8080/ || echo 000)
echo "        GET http://127.0.0.1:8080/  ->  HTTP $code"
if [ "$code" != "200" ]; then
    echo ""
    echo "  [!] Non-200 - last 20 lines of journal:"
    journalctl -u campus --no-pager -n 20 || true
fi
'@
# Force LF line endings + UTF-8 no BOM (bash rejects CRLF shebang / BOM).
$installLF = $install -replace "`r`n","`n"
[IO.File]::WriteAllBytes("$out\install.sh", [Text.UTF8Encoding]::new($false).GetBytes($installLF))

Write-Host "  [4/4] Packing $pkg ..."
if (Test-Path $pkg) { Remove-Item $pkg -Force }
# tar is native on Windows 10+; -C to switch into the publish dir so paths in the
# tarball are relative ("./install.sh" etc.) - what the installer expects.
tar -czf $pkg -C $out .
if ($LASTEXITCODE -ne 0) { Write-Host "  [X] tar failed"; pause; exit 1 }

$szMB = [math]::Round((Get-Item $pkg).Length / 1MB, 1)
Write-Host ""
Write-Host "  ==========================================="
Write-Host "    PACKAGED OK"
Write-Host ("    file  : $pkg  ({0} MB)" -f $szMB)
Write-Host "    build : $buildId"
Write-Host ""
Write-Host "    Next:  DeployCampusServer.bat"
Write-Host "  ==========================================="
