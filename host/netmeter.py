#!/usr/bin/env python3
"""NetMeter PC app: measures internet traffic and streams it to the NetMeter
screen (Guition ESP32-4848S040) over USB.

    netmeter                      run in the foreground (what autostart runs)
    netmeter install              start automatically at login, and start now
    netmeter uninstall            remove the autostart entry
    netmeter status               is it running, which port, what is measured
    netmeter config [options]     show or change the screen's settings
    netmeter unifi --host IP --api-key KEY
                                  measure a UniFi gateway's WAN instead of this PC
    netmeter screenshot out.png   capture the screen (for bug reports / docs)

Needs: Python 3.8+, pyserial, psutil.
"""
from __future__ import annotations

import argparse
import json
import logging
import logging.handlers
import os
import platform
import queue
import socket
import ssl
import statistics
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

VERSION = "1.0.0"
APP = "NetMeter"
BAUD = 115200
SAMPLE_INTERVAL = 0.5          # seconds between samples sent to the screen
CONTROL_PORT = 47819           # localhost-only control socket (also the single-instance lock)
PROBE_TIMEOUT = 3.0            # seconds to wait for the screen to answer "#?"
# USB-serial bridges this board ships with (WCH CH340 / CH343 family).
USB_IDS = {(0x1A86, 0x7523), (0x1A86, 0x7522), (0x1A86, 0x55D3), (0x1A86, 0x5523)}

SYSTEM = platform.system()
FROZEN = getattr(sys, "frozen", False)   # PyInstaller build (NetMeter.exe)

log = logging.getLogger("netmeter")


# =============================================================================
# Paths and config
# =============================================================================
def config_dir() -> Path:
    if SYSTEM == "Windows":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
        return base / APP
    if SYSTEM == "Darwin":
        return Path.home() / "Library" / "Application Support" / APP
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "netmeter"


DEFAULT_CONFIG = {
    "source": "pc",            # "pc" = this computer, "unifi" = a UniFi gateway's WAN
    "interface": None,         # force a network interface; None = the one that reaches the internet
    "port": None,              # force a serial port; None = auto-detect
    "notifications": True,     # desktop notification when the screen raises an alert
    "unifi": {"host": "", "api_key": "", "site": "default"},
}


def load_config() -> dict:
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))
    path = config_dir() / "config.json"
    try:
        user = json.loads(path.read_text(encoding="utf-8"))
        for k, v in user.items():
            if isinstance(v, dict) and isinstance(cfg.get(k), dict):
                cfg[k].update(v)
            else:
                cfg[k] = v
    except FileNotFoundError:
        pass
    except (OSError, ValueError) as e:
        log.warning("ignoring unreadable %s: %s", path, e)
    # Let the key live in its own file so it never has to be pasted anywhere.
    key_file = config_dir() / "unifi_key"
    if not cfg["unifi"].get("api_key") and key_file.exists():
        cfg["unifi"]["api_key"] = key_file.read_text(encoding="utf-8").strip()
    return cfg


def save_config(cfg: dict) -> None:
    d = config_dir()
    d.mkdir(parents=True, exist_ok=True)
    path = d / "config.json"
    path.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    if SYSTEM != "Windows":
        os.chmod(path, 0o600)   # may hold an API key


def setup_logging(verbose: bool) -> None:
    log.setLevel(logging.DEBUG if verbose else logging.INFO)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    if sys.stderr is not None:   # None under pythonw / --noconsole
        h = logging.StreamHandler()
        h.setFormatter(fmt)
        log.addHandler(h)
    try:
        config_dir().mkdir(parents=True, exist_ok=True)
        fh = logging.handlers.RotatingFileHandler(
            config_dir() / "netmeter.log", maxBytes=512 * 1024, backupCount=1, encoding="utf-8")
        fh.setFormatter(fmt)
        log.addHandler(fh)
    except OSError:
        pass


# =============================================================================
# Traffic sources
# =============================================================================
def local_ip_for(host: str = "1.1.1.1") -> str | None:
    """The local address the OS would use to reach `host` (no packet is sent)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((host, 53))
            return s.getsockname()[0]
    except OSError:
        return None


class PcSource:
    """Traffic through this computer's internet-facing interface."""

    REPICK_EVERY = 30.0   # re-check the interface (Wi-Fi <-> cable, VPN up/down)

    def __init__(self, forced_iface: str | None = None):
        import psutil  # imported here so `netmeter --help` works without it
        self.psutil = psutil
        self.forced = forced_iface
        self.iface: str | None = None
        self.picked_at = 0.0
        self.prev: tuple[float, int, int] | None = None

    def pick_interface(self) -> str | None:
        if self.forced:
            return self.forced
        ip = local_ip_for()
        if ip:
            for name, addrs in self.psutil.net_if_addrs().items():
                if any(a.family == socket.AF_INET and a.address == ip for a in addrs):
                    return name
        # No route (offline): keep the last interface so the screen shows 0, not garbage.
        return self.iface

    def read(self) -> dict | None:
        now = time.monotonic()
        if self.iface is None or now - self.picked_at > self.REPICK_EVERY:
            new = self.pick_interface()
            if new != self.iface:
                log.info("measuring interface: %s", new)
                self.iface, self.prev = new, None
            self.picked_at = now
        if not self.iface:
            return None
        counters = self.psutil.net_io_counters(pernic=True).get(self.iface)
        if counters is None:
            self.iface = None
            return None
        rx, tx = counters.bytes_recv, counters.bytes_sent
        sample = None
        if self.prev:
            t0, rx0, tx0 = self.prev
            dt = now - t0
            if dt > 0 and rx >= rx0 and tx >= tx0:   # counters reset on reconnect: skip one sample
                sample = {"d": round((rx - rx0) / dt), "u": round((tx - tx0) / dt),
                          "rx": round(rx / 1e6, 1), "tx": round(tx / 1e6, 1),
                          "src": self.iface}
        self.prev = (now, rx, tx)
        return sample


class UnifiSource:
    """WAN traffic of a UniFi gateway (UCG Ultra, UDM, UDR, UXG...) via its
    local API, authenticated with an API key (UniFi Network 9+).

    Uses the classic stat/health endpoint when the key is accepted there (it
    also gives latency and the ISP name) and falls back to the official
    Integration API's device statistics otherwise.
    """

    def __init__(self, host: str, api_key: str, site: str = "default"):
        if not host or not api_key:
            raise ValueError("UniFi source needs a host and an API key")
        self.base = host if host.startswith("http") else f"https://{host}"
        self.key = api_key
        self.site = site or "default"
        # Consoles ship a self-signed certificate for their LAN address.
        self.tls = ssl.create_default_context()
        self.tls.check_hostname = False
        self.tls.verify_mode = ssl.CERT_NONE
        self.mode: str | None = None          # "health" or "integration"
        self.gateway: tuple[str, str] | None = None   # (site_id, device_id)
        self.label = "UniFi"
        self.last: dict | None = None
        self.last_at = 0.0
        self.retry_at = 0.0

    def _get(self, path: str):
        req = urllib.request.Request(self.base + path, headers={
            "X-API-KEY": self.key, "Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=4, context=self.tls) as r:
            return json.loads(r.read().decode("utf-8"))

    def _health(self) -> dict | None:
        data = self._get(f"/proxy/network/api/s/{self.site}/stat/health").get("data", [])
        wan = next((s for s in data if s.get("subsystem") == "wan"), None)
        www = next((s for s in data if s.get("subsystem") == "www"), {})
        if not wan or "rx_bytes-r" not in wan:
            return None
        self.label = wan.get("gw_name") or wan.get("isp_name") or "UniFi"
        out = {"d": round(wan.get("rx_bytes-r", 0)), "u": round(wan.get("tx_bytes-r", 0)),
               "src": self.label}
        lat = www.get("latency")
        if isinstance(lat, (int, float)) and lat >= 0:
            out["p"] = int(lat)
        return out

    def _find_gateway(self) -> tuple[str, str]:
        sites = self._get("/proxy/network/integration/v1/sites").get("data", [])
        site = next((s for s in sites if s.get("internalReference") == self.site), None)
        site = site or (sites[0] if sites else None)
        if not site:
            raise RuntimeError("no sites visible to this API key")
        devices = self._get(f"/proxy/network/integration/v1/sites/{site['id']}/devices?limit=200")
        devices = devices.get("data", [])

        def is_gateway(d: dict) -> bool:
            feats = [str(f).lower() for f in d.get("features", [])]
            model = str(d.get("model", "")).upper()
            return "gateway" in feats or model.startswith(("UCG", "UDM", "UDR", "UXG", "UX", "USG", "EFG"))
        gw = next((d for d in devices if is_gateway(d)), None)
        if not gw:
            raise RuntimeError("no gateway found among the site's devices")
        self.label = gw.get("name") or gw.get("model") or "UniFi"
        return site["id"], gw["id"]

    def _integration(self) -> dict:
        if not self.gateway:
            self.gateway = self._find_gateway()
        site_id, dev_id = self.gateway
        st = self._get(f"/proxy/network/integration/v1/sites/{site_id}/devices/{dev_id}/statistics/latest")
        up = st.get("uplink") or {}
        # Integration API rates are bits per second; the screen expects bytes.
        return {"d": round(up.get("rxRateBps", 0) / 8), "u": round(up.get("txRateBps", 0) / 8),
                "src": self.label}

    def read(self) -> dict | None:
        now = time.monotonic()
        if now < self.retry_at:
            return None
        # The console refreshes these figures every few seconds; polling it
        # twice a second would only add load, so reuse the last reading.
        if self.last and now - self.last_at < 2.0:
            return dict(self.last)
        try:
            out = None
            if self.mode in (None, "health"):
                try:
                    out = self._health()
                except urllib.error.HTTPError as e:
                    # 401/403/404 on first contact: this key or firmware only
                    # opens the Integration API. Anything else is a real error.
                    if self.mode == "health" or e.code not in (401, 403, 404):
                        raise
                if out is not None:
                    if self.mode is None:
                        log.info("UniFi: reading WAN rates from stat/health")
                    self.mode = "health"
                elif self.mode == "health":
                    raise RuntimeError("stat/health stopped reporting WAN rates")
                else:
                    self.mode = "integration"
                    log.info("UniFi: reading WAN rates from the Integration API")
            if self.mode == "integration":
                out = self._integration()
        except (OSError, ValueError, RuntimeError, KeyError) as e:
            log.warning("UniFi read failed: %s (retrying in 5 s)", e)
            self.gateway, self.last = None, None
            self.retry_at = now + 5.0
            return None
        self.last, self.last_at = out, now
        return dict(out)


class Pinger(threading.Thread):
    """Latency as TCP connect time to a public anycast resolver, every 2 s.
    Unprivileged on every OS (ICMP needs root on some), and close to RTT."""

    TARGETS = [("1.1.1.1", 443), ("8.8.8.8", 443)]

    def __init__(self):
        super().__init__(daemon=True)
        self.recent: list[float] = []
        self.value = -1

    def run(self):
        while True:
            ms = None
            for host, port in self.TARGETS:
                t0 = time.perf_counter()
                try:
                    with socket.create_connection((host, port), timeout=1.5):
                        ms = (time.perf_counter() - t0) * 1000
                    break
                except OSError:
                    continue
            if ms is None:
                self.recent.clear()
                self.value = -1
            else:
                self.recent = (self.recent + [ms])[-3:]
                self.value = int(round(statistics.median(self.recent)))
            time.sleep(2.0)


# =============================================================================
# Serial device
# =============================================================================
def candidate_ports(forced: str | None) -> list[str]:
    if forced:
        return [forced]
    from serial.tools import list_ports
    return [p.device for p in sorted(list_ports.comports(), key=lambda p: p.device)
            if (p.vid, p.pid) in USB_IDS]


def open_port(name: str):
    import serial
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = BAUD
    ser.timeout = 0.1
    ser.write_timeout = 2
    # DTR/RTS drive the board's auto-reset circuit, which pulls EN low only
    # while RTS and DTR *differ*. Leave both at pyserial's default (asserted):
    # the OS raises them together on open, so they never differ. Setting both
    # False before open() looks safer but is what resets the board: pyserial
    # applies DTR then RTS in two steps, and the moment in between is a reset
    # pulse (measured: 3/3 opens rebooted the screen that way, 0/3 this way).
    ser.open()
    return ser


def probe(name: str):
    """Open `name` and return it if a NetMeter screen answers, else None."""
    try:
        ser = open_port(name)
    except Exception as e:  # noqa: BLE001 — permission, busy, vanished...
        log.debug("cannot open %s: %s", name, e)
        if "ermission" in str(e) and SYSTEM == "Linux":
            log.warning("no permission for %s: add yourself to the 'dialout' group "
                        "(sudo usermod -aG dialout $USER) and log in again", name)
        return None
    try:
        ser.reset_input_buffer()
        ser.write(b"\n#?\n")
        deadline = time.monotonic() + PROBE_TIMEOUT
        while time.monotonic() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if line.startswith("#NETMETER"):
                return ser
    except Exception as e:  # noqa: BLE001
        log.debug("probe %s failed: %s", name, e)
    ser.close()
    return None


def write_png(path: str, w: int, h: int, rgb565: bytes) -> None:
    """Minimal PNG writer (RGB8) so screenshots need no imaging library."""
    rows = bytearray()
    for y in range(h):
        rows.append(0)   # filter: none
        line = rgb565[y * w * 2:(y + 1) * w * 2]
        for i in range(0, len(line), 2):
            v = line[i] | (line[i + 1] << 8)
            rows += bytes((((v >> 11) & 0x1F) * 255 // 31,
                           ((v >> 5) & 0x3F) * 255 // 63,
                           (v & 0x1F) * 255 // 31))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


# =============================================================================
# Notifications
# =============================================================================
TEXT = {
    "es": {"down": "Consumo alto de bajada", "up": "Consumo alto de subida",
           "body": "{rate} · {pct} % de tu plan"},
    "en": {"down": "High download usage", "up": "High upload usage",
           "body": "{rate} · {pct} % of your plan"},
}


def fmt_rate(bits: float) -> str:
    for unit, div in (("Gbps", 1e9), ("Mbps", 1e6)):
        if bits >= div:
            return f"{bits / div:.3g} {unit}"
    return f"{bits / 1e3:.3g} Kbps"


def notify(title: str, body: str) -> None:
    try:
        if SYSTEM == "Linux":
            subprocess.Popen(["notify-send", "-a", APP, "-u", "critical", title, body],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif SYSTEM == "Darwin":
            script = f'display notification {json.dumps(body)} with title {json.dumps(title)}'
            subprocess.Popen(["osascript", "-e", script],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif SYSTEM == "Windows":
            def esc(s: str) -> str:
                return s.replace("'", "''")
            ps = (
                "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, "
                "ContentType = WindowsRuntime] | Out-Null;"
                "$x = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent("
                "[Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
                f"$x.GetElementsByTagName('text')[0].AppendChild($x.CreateTextNode('{esc(title)}')) | Out-Null;"
                f"$x.GetElementsByTagName('text')[1].AppendChild($x.CreateTextNode('{esc(body)}')) | Out-Null;"
                "$id = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe';"
                "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($id).Show("
                "[Windows.UI.Notifications.ToastNotification]::new($x))")
            subprocess.Popen(["powershell", "-NoProfile", "-WindowStyle", "Hidden", "-Command", ps],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             creationflags=0x08000000)   # CREATE_NO_WINDOW
    except OSError as e:
        log.debug("notification failed: %s", e)


# =============================================================================
# Agent
# =============================================================================
class Agent:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.ser = None
        self.port_name: str | None = None
        self.device_cfg: dict = {}
        self.source = None
        self.requests: queue.Queue = queue.Queue()
        self.pinger = Pinger()
        self.last_sample: dict | None = None

    # ---- source ----
    def make_source(self):
        if self.cfg.get("source") == "unifi":
            u = self.cfg.get("unifi", {})
            try:
                return UnifiSource(u.get("host", ""), u.get("api_key", ""), u.get("site", "default"))
            except ValueError as e:
                log.error("%s; measuring this PC instead", e)
        return PcSource(self.cfg.get("interface"))

    # ---- serial ----
    def connect(self) -> bool:
        for name in candidate_ports(self.cfg.get("port")):
            ser = probe(name)
            if ser:
                self.ser, self.port_name = ser, name
                log.info("screen found on %s", name)
                return True
        return False

    def disconnect(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:  # noqa: BLE001
                pass
        self.ser, self.port_name = None, None

    def send(self, line: str):
        self.ser.write((line + "\n").encode("utf-8"))

    def handle_line(self, line: str):
        if line.startswith("#CFG "):
            try:
                self.device_cfg = json.loads(line[5:])
            except ValueError:
                pass
        elif line.startswith("#ALERT ") and self.cfg.get("notifications", True):
            parts = line.split()
            log.info("screen alert: %s", line[7:])
            if len(parts) >= 4:
                lang = self.device_cfg.get("lang", "es")
                t = TEXT.get(lang, TEXT["es"])
                notify(t["down" if parts[1] == "down" else "up"],
                       t["body"].format(rate=fmt_rate(float(parts[3])), pct=parts[2]))
        elif line.startswith("#NETMETER"):
            log.info("screen (re)started: %s", line)
            self.send("#C?")

    def pump(self, until=None, timeout=0.0):
        """Read device lines; stop early when `until(line)` is true. Returns that line."""
        deadline = time.monotonic() + timeout
        while True:
            raw = self.ser.readline()
            if raw:
                line = raw.decode("utf-8", "replace").strip()
                if line:
                    self.handle_line(line)
                    if until and until(line):
                        return line
            elif time.monotonic() >= deadline:
                return None

    # ---- control requests (from `netmeter config/screenshot/status`) ----
    def serve_request(self, req: dict) -> dict:
        cmd = req.get("cmd")
        if cmd == "status":
            return {"ok": True, "version": VERSION, "port": self.port_name,
                    "source": self.cfg.get("source"), "sample": self.last_sample,
                    "device": self.device_cfg}
        if cmd == "quit":
            log.info("quit requested")
            os._exit(0)
        if not self.ser:
            return {"ok": False, "error": "screen not connected"}
        if cmd == "config":
            changes = req.get("set") or {}
            self.send("#C " + json.dumps(changes, separators=(",", ":")) if changes else "#C?")
            line = self.pump(lambda l: l.startswith(("#CFG", "#ERR")), timeout=3)
            if line and line.startswith("#CFG"):
                return {"ok": True, "device": self.device_cfg}
            return {"ok": False, "error": line or "no answer from the screen"}
        if cmd == "screenshot":
            return self.screenshot(req["path"])
        if cmd == "raw":   # developer aid: a console line for the firmware
            self.send(req["line"])
            return {"ok": True}
        return {"ok": False, "error": f"unknown command {cmd!r}"}

    def screenshot(self, path: str) -> dict:
        self.ser.reset_input_buffer()
        self.send("screenshot")
        line = self.pump(lambda l: l.startswith(("SCREENSHOT_START", "SCREENSHOT_ERR")), timeout=5)
        if not line or not line.startswith("SCREENSHOT_START"):
            return {"ok": False, "error": "screen did not start a capture"}
        _, w, h, size = line.split()
        w, h, size = int(w), int(h), int(size)
        data = bytearray()
        deadline = time.monotonic() + 90
        while len(data) < size and time.monotonic() < deadline:
            data += self.ser.read(min(65536, size - len(data)))
        if len(data) < size:
            return {"ok": False, "error": f"capture truncated ({len(data)}/{size} bytes)"}
        write_png(path, w, h, bytes(data))
        return {"ok": True, "path": path}

    # ---- main loop ----
    def run(self):
        self.pinger.start()
        self.source = self.make_source()
        waiting_logged = False
        while True:
            if not self.ser:
                if not self.connect():
                    if not waiting_logged:
                        log.info("waiting for the screen (USB)...")
                        waiting_logged = True
                    self.drain_requests()
                    time.sleep(2.0)
                    continue
                waiting_logged = False
                try:
                    self.send("#C?")
                except Exception:  # noqa: BLE001
                    self.disconnect()
                    continue
            try:
                t0 = time.monotonic()
                self.tick()
                self.drain_requests()
                self.pump(timeout=max(0.0, SAMPLE_INTERVAL - (time.monotonic() - t0)))
            except Exception as e:  # noqa: BLE001 — unplugged, port vanished, ...
                log.warning("connection lost: %s", e)
                self.disconnect()
                time.sleep(1.0)

    def tick(self):
        sample = self.source.read()
        if sample is None:
            return
        if "p" not in sample:
            sample["p"] = self.pinger.value
        # Local wall-clock time with the UTC offset already applied.
        sample["t"] = int(time.time() + time.localtime().tm_gmtoff)
        self.last_sample = sample
        self.send("#N " + json.dumps(sample, separators=(",", ":")))

    def drain_requests(self):
        while True:
            try:
                req, reply = self.requests.get_nowait()
            except queue.Empty:
                return
            try:
                reply.put(self.serve_request(req))
            except Exception as e:  # noqa: BLE001
                reply.put({"ok": False, "error": str(e)})


def control_server(agent: Agent, sock: socket.socket):
    """Line-delimited JSON on 127.0.0.1, so CLI commands can reach the running agent."""
    while True:
        conn, _ = sock.accept()
        with conn:
            try:
                conn.settimeout(120)
                data = b""
                while not data.endswith(b"\n"):
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                reply: queue.Queue = queue.Queue()
                agent.requests.put((json.loads(data.decode("utf-8")), reply))
                conn.sendall((json.dumps(reply.get(timeout=110)) + "\n").encode("utf-8"))
            except Exception as e:  # noqa: BLE001
                try:
                    conn.sendall((json.dumps({"ok": False, "error": str(e)}) + "\n").encode())
                except OSError:
                    pass


def ask_agent(req: dict, timeout: float = 120) -> dict | None:
    """Send a request to the running agent; None if none is running."""
    try:
        with socket.create_connection(("127.0.0.1", CONTROL_PORT), timeout=2) as s:
            s.settimeout(timeout)
            s.sendall((json.dumps(req) + "\n").encode("utf-8"))
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
            return json.loads(data.decode("utf-8"))
    except (OSError, ValueError):
        return None


def one_shot(req: dict, cfg: dict) -> dict:
    """Run a request through the agent if it is running, else talk to the screen directly."""
    r = ask_agent(req)
    if r is not None:
        return r
    agent = Agent(cfg)
    if not agent.connect():
        return {"ok": False, "error": "screen not found (is it plugged in?)"}
    try:
        agent.send("#C?")
        agent.pump(lambda l: l.startswith("#CFG"), timeout=2)
        return agent.serve_request(req)
    finally:
        agent.disconnect()


# =============================================================================
# Autostart
# =============================================================================
def launch_command() -> list[str]:
    if FROZEN:
        return [sys.executable, "run"]
    exe = sys.executable
    if SYSTEM == "Windows":
        w = Path(exe).with_name("pythonw.exe")   # no console window
        if w.exists():
            exe = str(w)
    return [exe, str(Path(__file__).resolve()), "run"]


def install() -> str:
    cmd = launch_command()
    if SYSTEM == "Linux":
        unit_dir = Path.home() / ".config" / "systemd" / "user"
        unit_dir.mkdir(parents=True, exist_ok=True)
        exec_line = " ".join(f'"{c}"' if " " in c else c for c in cmd)
        (unit_dir / "netmeter.service").write_text(
            "[Unit]\nDescription=NetMeter - internet traffic on the USB screen\n"
            "After=network-online.target\n\n"
            f"[Service]\nExecStart={exec_line}\nRestart=always\nRestartSec=3\n\n"
            "[Install]\nWantedBy=default.target\n", encoding="utf-8")
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
        subprocess.run(["systemctl", "--user", "enable", "--now", "netmeter.service"], check=True)
        subprocess.run(["systemctl", "--user", "restart", "netmeter.service"], check=True)
        return "systemd user service 'netmeter' enabled and started"
    if SYSTEM == "Darwin":
        plist = Path.home() / "Library" / "LaunchAgents" / "com.netmeter.agent.plist"
        plist.parent.mkdir(parents=True, exist_ok=True)
        args = "".join(f"<string>{c}</string>" for c in cmd)
        plist.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
            '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
            '<plist version="1.0"><dict>'
            '<key>Label</key><string>com.netmeter.agent</string>'
            f'<key>ProgramArguments</key><array>{args}</array>'
            '<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>'
            '</dict></plist>\n', encoding="utf-8")
        subprocess.run(["launchctl", "unload", str(plist)], capture_output=True)
        subprocess.run(["launchctl", "load", "-w", str(plist)], check=True)
        return f"LaunchAgent installed: {plist}"
    if SYSTEM == "Windows":
        import winreg
        value = subprocess.list2cmdline(cmd)
        # CreateKeyEx, not OpenKey: a fresh profile may not have a Run key yet.
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER,
                                r"Software\Microsoft\Windows\CurrentVersion\Run", 0,
                                winreg.KEY_SET_VALUE) as k:
            winreg.SetValueEx(k, APP, 0, winreg.REG_SZ, value)
        # Replace a running copy (e.g. after an update) with the new one.
        ask_agent({"cmd": "quit"}, timeout=3)
        time.sleep(1.0)
        subprocess.Popen(cmd, creationflags=0x00000008 | 0x08000000,   # DETACHED, NO_WINDOW
                         close_fds=True)
        return "starts with Windows (HKCU Run key) and is running now"
    raise SystemExit(f"autostart not supported on {SYSTEM}")


def uninstall() -> str:
    if SYSTEM == "Linux":
        subprocess.run(["systemctl", "--user", "disable", "--now", "netmeter.service"],
                       capture_output=True)
        (Path.home() / ".config" / "systemd" / "user" / "netmeter.service").unlink(missing_ok=True)
        subprocess.run(["systemctl", "--user", "daemon-reload"], capture_output=True)
        return "systemd user service removed"
    if SYSTEM == "Darwin":
        plist = Path.home() / "Library" / "LaunchAgents" / "com.netmeter.agent.plist"
        subprocess.run(["launchctl", "unload", "-w", str(plist)], capture_output=True)
        plist.unlink(missing_ok=True)
        return "LaunchAgent removed"
    if SYSTEM == "Windows":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Microsoft\Windows\CurrentVersion\Run", 0,
                                winreg.KEY_SET_VALUE) as k:
                winreg.DeleteValue(k, APP)
        except FileNotFoundError:
            pass
        ask_agent({"cmd": "quit"}, timeout=3)
        return "autostart entry removed and NetMeter stopped"
    raise SystemExit(f"autostart not supported on {SYSTEM}")


def message_box(text: str) -> None:
    """Feedback for the double-clicked Windows .exe, which has no console."""
    if SYSTEM == "Windows":
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, text, APP, 0x40)
    else:
        print(text)


# =============================================================================
# CLI
# =============================================================================
def cmd_run(cfg: dict, args) -> int:
    if args.port:
        cfg["port"] = args.port
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # The bound port doubles as the single-instance lock. On Windows,
    # SO_REUSEADDR would let a second instance share it, so ask for exclusive
    # use there; elsewhere SO_REUSEADDR only stops a restart from failing on
    # the previous run's TIME_WAIT connections.
    if SYSTEM == "Windows":
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    else:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("127.0.0.1", CONTROL_PORT))
    except OSError:
        log.error("NetMeter is already running (control port %d in use)", CONTROL_PORT)
        return 1
    sock.listen(4)
    agent = Agent(cfg)
    threading.Thread(target=control_server, args=(agent, sock), daemon=True).start()
    log.info("NetMeter %s started (source: %s)", VERSION, cfg.get("source"))
    try:
        agent.run()
    except KeyboardInterrupt:
        pass
    return 0


def parse_pct(v: str) -> int:
    if v.lower() in ("off", "no", "0"):
        return 0
    n = int(v)
    if not 10 <= n <= 100:
        raise argparse.ArgumentTypeError("alert % must be 10-100 or 'off'")
    return n


def cmd_config(cfg: dict, args) -> int:
    changes = {}
    if args.plan_down is not None:
        changes["pd"] = args.plan_down
    if args.plan_up is not None:
        changes["pu"] = args.plan_up
    if args.alert_down is not None:
        changes["ad"] = args.alert_down
    if args.alert_up is not None:
        changes["au"] = args.alert_up
    if args.brightness is not None:
        changes["br"] = args.brightness
    if args.lang:
        changes["lang"] = args.lang
    for k in ("pd", "pu"):
        if k in changes and not 1 <= changes[k] <= 100000:
            print("plan must be 1-100000 Mbps", file=sys.stderr)
            return 2
    r = one_shot({"cmd": "config", "set": changes}, cfg)
    if not r.get("ok"):
        print(f"error: {r.get('error')}", file=sys.stderr)
        return 1
    d = r["device"]
    off = lambda p: "off" if p == 0 else f"{p} %"   # noqa: E731
    print(f"plan:       {d['pd']} Mbps down / {d['pu']} Mbps up"
          f"{'' if d.get('set') else '  (not set yet)'}")
    print(f"alerts:     download at {off(d['ad'])}, upload at {off(d['au'])}")
    print(f"brightness: {d['br']} %")
    print(f"language:   {d['lang']}")
    return 0


def cmd_status(cfg: dict, args) -> int:
    r = ask_agent({"cmd": "status"}, timeout=5)
    if r is None:
        print("NetMeter is not running. Start it with: netmeter install")
        return 1
    print(f"running, version {r['version']}")
    print(f"screen:  {r['port'] or 'not connected'}")
    print(f"source:  {r['source']}")
    s = r.get("sample")
    if s:
        print(f"now:     down {fmt_rate(s['d'] * 8)}, up {fmt_rate(s['u'] * 8)}, "
              f"ping {s.get('p', -1)} ms ({s.get('src')})")
    return 0


def cmd_unifi(cfg: dict, args) -> int:
    if args.off:
        cfg["source"] = "pc"
        save_config(cfg)
        print("Measuring this PC again. Restart NetMeter to apply (or: netmeter install).")
        return 0
    key = args.api_key or cfg["unifi"].get("api_key")
    host = args.host or cfg["unifi"].get("host")
    src = UnifiSource(host, key, args.site or cfg["unifi"].get("site", "default"))
    sample = src.read()
    if sample is None:
        print("Could not read traffic from the gateway; see the log above.", file=sys.stderr)
        return 1
    print(f"OK: {src.label} via {src.mode}: down {fmt_rate(sample['d'] * 8)}, "
          f"up {fmt_rate(sample['u'] * 8)}")
    cfg["source"] = "unifi"
    cfg["unifi"].update({"host": host, "site": args.site or cfg["unifi"].get("site", "default")})
    if args.api_key:
        cfg["unifi"]["api_key"] = args.api_key
    save_config(cfg)
    print(f"Saved to {config_dir() / 'config.json'}. Restart NetMeter to apply (or: netmeter install).")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="netmeter", description=__doc__.split("\n\n")[0])
    p.add_argument("--version", action="version", version=f"NetMeter {VERSION}")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd")
    r = sub.add_parser("run", help="run in the foreground (default)")
    r.add_argument("--port", help="serial port, e.g. COM5 or /dev/ttyUSB0 (default: auto)")
    sub.add_parser("install", help="start at login, and start now")
    sub.add_parser("uninstall", help="remove the autostart entry")
    sub.add_parser("status", help="show what the running app is doing")
    c = sub.add_parser("config", help="show or change the screen's settings")
    c.add_argument("--plan-down", type=int, metavar="MBPS", help="contracted download speed")
    c.add_argument("--plan-up", type=int, metavar="MBPS", help="contracted upload speed")
    c.add_argument("--alert-down", type=parse_pct, metavar="PCT|off", help="alert threshold, %% of plan")
    c.add_argument("--alert-up", type=parse_pct, metavar="PCT|off")
    c.add_argument("--brightness", type=int, choices=range(5, 101), metavar="5-100")
    c.add_argument("--lang", choices=["es", "en"])
    u = sub.add_parser("unifi", help="measure a UniFi gateway's WAN instead of this PC")
    u.add_argument("--host", help="gateway address, e.g. 192.168.1.1")
    u.add_argument("--api-key", help="UniFi Network > Settings > Control Plane > Integrations")
    u.add_argument("--site", help="site name (default: default)")
    u.add_argument("--off", action="store_true", help="go back to measuring this PC")
    s = sub.add_parser("screenshot", help="save what the screen shows as a PNG")
    s.add_argument("path")
    x = sub.add_parser("send")   # developer aid, e.g. `netmeter send settings`
    x.add_argument("line")

    # Double-clicking NetMeter.exe passes no arguments: install and say so.
    if FROZEN and SYSTEM == "Windows" and not (argv if argv is not None else sys.argv[1:]):
        setup_logging(False)
        try:
            what = install()
            message_box("NetMeter is installed and running.\n\n"
                        "NetMeter está instalado y funcionando.\n\n" + what)
            return 0
        except Exception as e:  # noqa: BLE001
            message_box(f"NetMeter could not be installed:\n{e}")
            return 1

    args = p.parse_args(argv)
    setup_logging(args.verbose)
    cfg = load_config()
    if args.cmd in (None, "run"):
        if args.cmd is None:
            args.port = None
        return cmd_run(cfg, args)
    if args.cmd == "install":
        print(install())
        return 0
    if args.cmd == "uninstall":
        print(uninstall())
        return 0
    if args.cmd == "status":
        return cmd_status(cfg, args)
    if args.cmd == "config":
        return cmd_config(cfg, args)
    if args.cmd == "unifi":
        return cmd_unifi(cfg, args)
    if args.cmd == "send":
        r = one_shot({"cmd": "raw", "line": args.line}, cfg)
        return 0 if r.get("ok") else 1
    if args.cmd == "screenshot":
        r = one_shot({"cmd": "screenshot", "path": str(Path(args.path).resolve())}, cfg)
        print(r.get("path") if r.get("ok") else f"error: {r.get('error')}")
        return 0 if r.get("ok") else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
