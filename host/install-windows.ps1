# NetMeter PC app installer for Windows 10/11.
#
# In PowerShell (no administrator needed):
#   irm https://raw.githubusercontent.com/dcryptoRS/netmeter-4848s040/main/host/install-windows.ps1 | iex
#
# or double-click install-windows.bat from a downloaded copy of the repository.
#
# Installs into your user account only: a private Python environment, the
# `netmeter` command, and an entry that starts NetMeter (with no window) every
# time you log in. It finds the screen by itself whenever it is plugged in.

$ErrorActionPreference = 'Stop'
$RepoRaw = 'https://raw.githubusercontent.com/dcryptoRS/netmeter-4848s040/main'
$AppDir  = Join-Path $env:LOCALAPPDATA 'NetMeter'

function Say($msg)  { Write-Host $msg -ForegroundColor Cyan }
function Warn($msg) { Write-Host "! $msg" -ForegroundColor Yellow }

# ---- Python 3.8+ ----
function Find-Python {
    $candidates = @(
        @('py', '-3'),
        @('python'),
        @("$env:LOCALAPPDATA\Programs\Python\Python313\python.exe"),
        @("$env:LOCALAPPDATA\Programs\Python\Python312\python.exe")
    )
    foreach ($c in $candidates) {
        try {
            $exe = $c[0]; $rest = @($c | Select-Object -Skip 1)
            # The Microsoft Store "python" alias prints nothing and exits 9009;
            # asking Python for its own path tells a real install apart.
            $out = & $exe @rest -c "import sys; print(sys.executable) if sys.version_info >= (3, 8) else None" 2>$null
            if ($LASTEXITCODE -eq 0 -and $out -and (Test-Path $out)) { return $out.Trim() }
        } catch { }
    }
    return $null
}

$Py = Find-Python
if (-not $Py) {
    Say 'Python 3 was not found. Installing it with winget (current user only)...'
    try {
        winget install -e --id Python.Python.3.12 --scope user --silent `
            --accept-package-agreements --accept-source-agreements | Out-Null
    } catch { }
    $Py = Find-Python
    if (-not $Py) {
        Warn 'Could not install Python automatically.'
        Write-Host 'Install it from https://www.python.org/downloads/ (tick "Add python.exe to PATH"),'
        Write-Host 'then run this installer again. Or use NetMeter.exe from the Releases page instead.'
        exit 1
    }
}
Say "Using Python: $Py"

# ---- Private environment ----
Say "Installing NetMeter into: $AppDir"
New-Item -ItemType Directory -Force -Path $AppDir | Out-Null
& $Py -m venv "$AppDir\venv"
if ($LASTEXITCODE -ne 0) { Warn 'Could not create the Python environment.'; exit 1 }
$VPy = "$AppDir\venv\Scripts\python.exe"
& $VPy -m pip install --quiet --upgrade pip
& $VPy -m pip install --quiet --upgrade 'pyserial>=3.5' 'psutil>=5.9'
if ($LASTEXITCODE -ne 0) { Warn 'Could not install pyserial/psutil (no internet?).'; exit 1 }

# ---- The app itself: from this folder if present, else from GitHub ----
$Local = if ($PSScriptRoot) { Join-Path $PSScriptRoot 'netmeter.py' } else { $null }
if ($Local -and (Test-Path $Local)) {
    Copy-Item $Local "$AppDir\netmeter.py" -Force
} else {
    Invoke-WebRequest -UseBasicParsing "$RepoRaw/host/netmeter.py" -OutFile "$AppDir\netmeter.py"
}

# `netmeter` command for the terminal
Set-Content -Encoding ASCII -Path "$AppDir\netmeter.cmd" -Value "@`"$VPy`" `"$AppDir\netmeter.py`" %*"
$UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not ($UserPath -split ';' | Where-Object { $_ -eq $AppDir })) {
    [Environment]::SetEnvironmentVariable('Path', ($UserPath.TrimEnd(';') + ";$AppDir"), 'User')
}

# ---- Autostart + start now ----
# NETMETER_NO_AUTOSTART=1 skips this step (used by CI).
if ($env:NETMETER_NO_AUTOSTART -ne '1') {
    Say 'Setting up autostart...'
    & $VPy "$AppDir\netmeter.py" install
}

# ---- USB driver hint ----
$ch340 = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
         Where-Object { $_.InstanceId -match 'VID_1A86' }
if ($ch340 -and ($ch340 | Where-Object { $_.Status -ne 'OK' })) {
    Warn 'The screen is plugged in but its USB driver (CH340) is not working.'
    Write-Host 'Install the driver from https://www.wch-ic.com/downloads/CH341SER_EXE.html and unplug/replug the screen.'
}

Say 'Done.'
Write-Host @'

Next:
  1. Plug the screen into this computer with a USB data cable.
  2. On the screen, tap the gear icon and enter your internet plan
     (or, in a new terminal: netmeter config --plan-down 600 --plan-up 600).

Useful commands (open a NEW terminal first):
  netmeter status        what it is doing right now
  netmeter config        show / change the screen's settings
  netmeter uninstall     stop it starting with Windows
'@
