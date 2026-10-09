#!/usr/bin/env python3
"""Improv Serial on the board, as the web flasher (ESP Web Tools) uses it.

  python test/improv_serial.py COM5 [--fresh]

Needs pyserial (ESP-IDF's Python has it). The WiFi credentials come from
include/config.h (WIFI_SSID, WIFI_PASSWORD) and are never printed. --fresh: the board
was just flashed with the release firmware (no WiFi yet), so the full setup is tested:
the state before, a scan, a wrong password, the right one, the dashboard link, and that
the setting survives a reset. Without it only the queries and the reset are checked.
"""
import re
import socket
import sys
import time
import urllib.request

import serial

HEADER = b"IMPROV"
CMD_WIFI, CMD_STATE, CMD_INFO, CMD_SCAN = 1, 2, 3, 4
TYPE_STATE, TYPE_ERROR, TYPE_RPC, TYPE_RESULT = 1, 2, 3, 4
READY, PROVISIONING, PROVISIONED = 2, 3, 4


def frame(ftype, payload):
    body = HEADER + bytes([1, ftype, len(payload)]) + payload
    return body + bytes([sum(body) & 0xFF])


def rpc(cmd, *strings):
    data = b"".join(bytes([len(s)]) + s for s in strings)
    return frame(TYPE_RPC, bytes([cmd, len(data)]) + data)


class Board:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.05
        self.s.dtr = False   # opening must not reset the board
        self.s.rts = False
        self.s.open()
        self.buf = b""
        self.log = b""

    def reset(self):
        self.s.rts = True    # EN low
        time.sleep(0.1)
        self.s.rts = False
        self.buf = b""

    def frames(self, seconds):
        """Improv frames arriving within the time, as (type, payload)."""
        end = time.time() + seconds
        while time.time() < end:
            self.buf += self.s.read(4096)
            while True:
                i = self.buf.find(HEADER)
                if i < 0:
                    self.log += self.buf[:-5]
                    self.buf = self.buf[-5:]
                    break
                self.log += self.buf[:i]
                if len(self.buf) < i + 9 or len(self.buf) < i + 10 + self.buf[i + 8]:
                    self.buf = self.buf[i:]
                    break
                n = self.buf[i + 8]
                raw = self.buf[i:i + 10 + n]
                self.buf = self.buf[i + 10 + n:]
                if sum(raw[:-1]) & 0xFF != raw[-1]:
                    raise AssertionError("bad checksum from the board")
                yield raw[7], raw[9:9 + n]

    def request(self, data, want, seconds=5):
        """Sends a frame and collects frames until want(frames) is true."""
        self.s.write(data)
        got = []
        for f in self.frames(seconds):
            got.append(f)
            if want(got):
                return got
        raise AssertionError(f"no answer in {seconds} s (got {got})")


def strings(payload):
    out, i, end = [], 2, 2 + payload[1]
    while i < end:
        out.append(payload[i + 1:i + 1 + payload[i]].decode())
        i += 1 + payload[i]
    return out


def result_for(cmd):
    return lambda got: any(t == TYPE_RESULT and p[0] == cmd for t, p in got)


def creds():
    cfg = open("include/config.h", encoding="utf-8").read()
    get = lambda k: re.search(r'#define\s+' + k + r'\s+"([^"]*)"', cfg).group(1)
    return get("WIFI_SSID"), get("WIFI_PASSWORD")


def main():
    port, fresh = sys.argv[1], "--fresh" in sys.argv
    b = Board(port)
    ssid, password = creds()
    # 1. after a reset (as ESP Web Tools does after flashing): how soon Improv answers
    b.reset()
    t0 = time.time()
    while True:
        try:
            got = b.request(rpc(CMD_INFO), result_for(CMD_INFO), 0.5)
            break
        except AssertionError:
            if time.time() - t0 > 15:
                raise AssertionError("Improv did not answer within 15 s of a reset")
    info = strings([p for t, p in got if t == TYPE_RESULT][0])
    print(f"device info after {time.time() - t0:.1f} s: {info}")
    assert len(info) == 4 and info[0] == "ESP32 Minecraft server" and info[2] == "ESP32-S3", info
    got = b.request(rpc(CMD_STATE), lambda g: any(t == TYPE_STATE for t, _ in g))
    state = [p[0] for t, p in got if t == TYPE_STATE][0]
    print("state:", {READY: "ready (no WiFi)", PROVISIONED: "provisioned"}.get(state, state))
    if fresh:
        assert state == READY, "a freshly flashed board must wait for WiFi settings"
        # 2. the networks ESP Web Tools offers
        got = b.request(rpc(CMD_SCAN), lambda g: any(t == TYPE_RESULT and p[0] == CMD_SCAN and p[1] == 0 for t, p in g), 15)
        nets = [strings(p) for t, p in got if t == TYPE_RESULT and p[1]]
        print(f"scan: {len(nets)} networks, ours among them: {any(n[0] == ssid for n in nets)}, "
              f"fields like {['<ssid>', nets[0][1], nets[0][2]] if nets else None}")
        assert nets and all(len(n) == 3 and n[2] in ("YES", "NO") for n in nets)
        # 3. a wrong password: an error, and the board stays unconfigured
        t = time.time()
        got = b.request(rpc(CMD_WIFI, ssid.encode(), b"wrong-password-123"),
                        lambda g: any(t_ == TYPE_ERROR and p[0] == 3 for t_, p in g), 40)
        states = [p[0] for t_, p in got if t_ == TYPE_STATE]
        print(f"wrong password: unable to connect after {time.time() - t:.1f} s, states {states}")
        assert PROVISIONING in states
        got = b.request(rpc(CMD_STATE), lambda g: any(t_ == TYPE_STATE for t_, _ in g))
        assert [p[0] for t_, p in got if t_ == TYPE_STATE][0] == READY
        # 4. the right one
        t = time.time()
        got = b.request(rpc(CMD_WIFI, ssid.encode(), password.encode()), result_for(CMD_WIFI), 40)
        urls = strings([p for t_, p in got if t_ == TYPE_RESULT][0])
        print(f"provisioned after {time.time() - t:.1f} s, redirect {urls}")
        assert urls and urls[0].startswith("http://")
    # 5. the setting survives a reset; the status page and the game port answer
    b.reset()
    t0 = time.time()
    url = None
    while time.time() - t0 < 30 and not url:
        try:
            got = b.request(rpc(CMD_STATE), lambda g: any(t_ == TYPE_STATE for t_, _ in g), 0.5)
        except AssertionError:
            continue
        if [p[0] for t_, p in got if t_ == TYPE_STATE][-1] == PROVISIONED:
            more = got if any(t_ == TYPE_RESULT for t_, _ in got) else got + list(b.frames(0.5))
            res = [strings(p) for t_, p in more if t_ == TYPE_RESULT and p[0] == CMD_STATE]
            url = res[0][0] if res and res[0] else ""
        else:
            time.sleep(0.5)
    print(f"after a reset: online in {time.time() - t0:.1f} s, status page {url}")
    assert url, "the board did not come back online with the stored WiFi settings"
    page = urllib.request.urlopen(url, timeout=10).read()
    host = url.split("//")[1].split("/")[0].split(":")[0]
    deadline = time.time() + 120   # the world is opened after the network is up
    while True:
        try:
            socket.create_connection((host, 25565), timeout=3).close()
            break
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(2)
    print(f"status page {len(page)} bytes, Minecraft port open")
    # 6. the console still takes commands (Improv bytes are not mistaken for input)
    b.s.write(b"list\n")
    list(b.frames(1.5))
    assert b"players online" in b.log + b.buf, "the console stopped answering"
    print("IMPROV SERIAL OK")


if __name__ == "__main__":
    main()
