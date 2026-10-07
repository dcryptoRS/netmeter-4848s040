#!/usr/bin/env bash
# NetMeter PC app installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/dcryptoRS/netmeter-4848s040/main/host/install.sh | bash
#
# or, from a clone of the repository:  bash host/install.sh
#
# Installs into your user account only (no sudo): a private Python
# environment, the `netmeter` command, and an autostart entry so the app runs
# at every login and finds the screen by itself whenever it is plugged in.
set -euo pipefail

REPO_RAW="https://raw.githubusercontent.com/dcryptoRS/netmeter-4848s040/main"
OS="$(uname -s)"
case "$OS" in
    Linux)  APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/netmeter" ;;
    Darwin) APP_DIR="$HOME/Library/Application Support/NetMeter/app" ;;
    *) echo "Unsupported system: $OS (use install-windows.ps1 on Windows)"; exit 1 ;;
esac
BIN_DIR="$HOME/.local/bin"

say()  { printf '\033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m! %s\033[0m\n' "$*"; }

# ---- Python 3.8+ ----
PY="$(command -v python3 || true)"
if [ -z "$PY" ] || ! "$PY" -c 'import sys; sys.exit(sys.version_info < (3, 8))'; then
    say "Python 3.8 or newer is required."
    if [ "$OS" = "Darwin" ]; then
        echo "Install it from https://www.python.org/downloads/ (or run: xcode-select --install) and re-run this installer."
    else
        echo "Install it with your package manager, e.g.: sudo apt install python3 python3-venv"
    fi
    exit 1
fi

# ---- Private environment ----
say "Installing NetMeter into: $APP_DIR"
mkdir -p "$APP_DIR" "$BIN_DIR"
if ! "$PY" -m venv "$APP_DIR/venv" 2>/dev/null; then
    say "Python's venv module is missing."
    echo "On Debian/Ubuntu: sudo apt install python3-venv   — then re-run this installer."
    exit 1
fi
"$APP_DIR/venv/bin/python" -m pip install --quiet --upgrade pip
"$APP_DIR/venv/bin/python" -m pip install --quiet --upgrade "pyserial>=3.5" "psutil>=5.9"

# ---- The app itself: from this checkout if present, else from GitHub ----
HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
if [ -n "$HERE" ] && [ -f "$HERE/netmeter.py" ]; then
    cp "$HERE/netmeter.py" "$APP_DIR/netmeter.py"
else
    curl -fsSL "$REPO_RAW/host/netmeter.py" -o "$APP_DIR/netmeter.py"
fi

cat > "$BIN_DIR/netmeter" <<EOF
#!/bin/sh
exec "$APP_DIR/venv/bin/python" "$APP_DIR/netmeter.py" "\$@"
EOF
chmod +x "$BIN_DIR/netmeter"

# ---- Serial-port access (Linux) ----
if [ "$OS" = "Linux" ]; then
    # The USB-serial device belongs to "dialout" (Debian, Ubuntu, Fedora) or
    # "uucp" (Arch). Without membership the app can't open the screen.
    GROUP=dialout
    getent group uucp >/dev/null && ! getent group dialout >/dev/null && GROUP=uucp
    if ! id -nG "$USER" | tr ' ' '\n' | grep -qx "$GROUP"; then
        warn "Your user is not in the '$GROUP' group, so it can't open the USB screen yet."
        echo "  Run:  sudo usermod -aG $GROUP \$USER"
        echo "  then log out and back in (or reboot)."
    fi
    # brltty (a braille-display driver shipped with Ubuntu) grabs CH340 USB
    # adapters like the one on this board, and the port vanishes right after
    # it appears. Nobody without a braille display needs it.
    if command -v brltty >/dev/null 2>&1 || systemctl list-unit-files 2>/dev/null | grep -q '^brltty'; then
        warn "brltty is installed. On Ubuntu it can steal this board's USB port."
        echo "  If the screen is not detected, run:  sudo apt remove brltty"
    fi
fi

# ---- Autostart + start now ----
# NETMETER_NO_AUTOSTART=1 skips this step (used by CI, which has no login session).
if [ "${NETMETER_NO_AUTOSTART:-0}" != "1" ]; then
    say "Setting up autostart..."
    "$BIN_DIR/netmeter" install
fi

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) warn "$BIN_DIR is not in your PATH; use the full path $BIN_DIR/netmeter, or add it to PATH." ;;
esac

say "Done."
cat <<'EOF'

Next:
  1. Plug the screen into this computer with a USB data cable.
  2. On the screen, tap the gear icon and enter your internet plan
     (or: netmeter config --plan-down 600 --plan-up 600).

Useful commands:
  netmeter status        what it is doing right now
  netmeter config        show / change the screen's settings
  netmeter uninstall     stop it starting at login
EOF
