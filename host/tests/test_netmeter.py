"""Unit tests for the NetMeter PC app. Run: python -m pytest host/tests"""
import struct
import sys
import types
import urllib.error
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import netmeter  # noqa: E402


def test_fmt_rate_units():
    assert netmeter.fmt_rate(512e6) == "512 Mbps"
    assert netmeter.fmt_rate(1.5e9) == "1.5 Gbps"
    assert netmeter.fmt_rate(64e3) == "64 Kbps"


def test_parse_pct():
    assert netmeter.parse_pct("off") == 0
    assert netmeter.parse_pct("80") == 80
    with pytest.raises(Exception):
        netmeter.parse_pct("5")


def test_write_png_roundtrip(tmp_path):
    # 2x1: pure red, pure blue in little-endian RGB565
    raw = struct.pack("<HH", 0xF800, 0x001F)
    out = tmp_path / "a.png"
    netmeter.write_png(str(out), 2, 1, raw)
    data = out.read_bytes()
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    idat = data[data.index(b"IDAT") + 4:]
    length = struct.unpack(">I", data[data.index(b"IDAT") - 4:data.index(b"IDAT")])[0]
    pixels = zlib.decompress(idat[:length])
    assert pixels == bytes([0, 255, 0, 0, 0, 0, 255])


class FakePsutil:
    def __init__(self):
        self.rx, self.tx = 1_000_000, 500_000

    def net_if_addrs(self):
        return {}

    def net_io_counters(self, pernic):
        return {"eth0": types.SimpleNamespace(bytes_recv=self.rx, bytes_sent=self.tx)}


def make_pc_source(monkeypatch, fake):
    monkeypatch.setitem(sys.modules, "psutil", fake)
    src = netmeter.PcSource("eth0")
    return src


def test_pc_source_rates(monkeypatch):
    fake = FakePsutil()
    src = make_pc_source(monkeypatch, fake)
    clock = [100.0]
    monkeypatch.setattr(netmeter.time, "monotonic", lambda: clock[0])
    assert src.read() is None            # first call only primes the counters
    clock[0] += 2.0
    fake.rx += 2_000_000                  # 1 MB/s down
    fake.tx += 200_000                    # 100 KB/s up
    s = src.read()
    assert s["d"] == 1_000_000 and s["u"] == 100_000
    assert s["src"] == "eth0"
    assert s["rx"] == pytest.approx(3.0) and s["tx"] == pytest.approx(0.7)


def test_pc_source_counter_reset_skips_sample(monkeypatch):
    fake = FakePsutil()
    src = make_pc_source(monkeypatch, fake)
    clock = [0.0]
    monkeypatch.setattr(netmeter.time, "monotonic", lambda: clock[0])
    src.read()
    clock[0] += 1
    fake.rx = 10                          # interface re-created: counters restart
    assert src.read() is None
    clock[0] += 1
    fake.rx = 1010
    assert src.read()["d"] == 1000


def test_unifi_health(monkeypatch):
    src = netmeter.UnifiSource("192.168.1.1", "k")
    health = {"data": [
        {"subsystem": "wan", "rx_bytes-r": 1234.0, "tx_bytes-r": 56.0, "gw_name": "UCG Ultra"},
        {"subsystem": "www", "latency": 9},
    ]}
    monkeypatch.setattr(src, "_get", lambda path: health)
    s = src.read()
    assert s == {"d": 1234, "u": 56, "src": "UCG Ultra", "p": 9}
    assert src.mode == "health"


def test_unifi_falls_back_to_integration_api(monkeypatch):
    src = netmeter.UnifiSource("192.168.1.1", "k")

    def fake_get(path):
        if path.endswith("/stat/health"):
            raise urllib.error.HTTPError(path, 401, "Unauthorized", {}, None)
        if path.endswith("/integration/v1/sites"):
            return {"data": [{"id": "s1", "internalReference": "default"}]}
        if "/devices?" in path:
            return {"data": [{"id": "ap", "model": "U6-Lite", "features": ["accessPoint"]},
                             {"id": "gw", "model": "UCG-Ultra", "name": "Gateway",
                              "features": ["switching", "gateway"]}]}
        if path.endswith("/devices/gw/statistics/latest"):
            return {"uplink": {"rxRateBps": 8_000_000, "txRateBps": 800_000}}
        raise AssertionError(path)

    monkeypatch.setattr(src, "_get", fake_get)
    s = src.read()
    assert src.mode == "integration"
    assert s == {"d": 1_000_000, "u": 100_000, "src": "Gateway"}   # bits -> bytes


def test_unifi_backs_off_after_failure(monkeypatch):
    src = netmeter.UnifiSource("192.168.1.1", "k")
    calls = []

    def boom(path):
        calls.append(path)
        raise OSError("unreachable")

    monkeypatch.setattr(src, "_get", boom)
    clock = [0.0]
    monkeypatch.setattr(netmeter.time, "monotonic", lambda: clock[0])
    assert src.read() is None
    n = len(calls)
    clock[0] += 1.0
    assert src.read() is None and len(calls) == n    # still backing off
    clock[0] += 5.0
    src.read()
    assert len(calls) > n


def test_config_merges_defaults(tmp_path, monkeypatch):
    monkeypatch.setattr(netmeter, "config_dir", lambda: tmp_path)
    (tmp_path / "config.json").write_text('{"source": "unifi", "unifi": {"host": "10.0.0.1"}}')
    (tmp_path / "unifi_key").write_text("secret\n")
    cfg = netmeter.load_config()
    assert cfg["source"] == "unifi"
    assert cfg["unifi"] == {"host": "10.0.0.1", "api_key": "secret", "site": "default"}
    assert cfg["notifications"] is True
