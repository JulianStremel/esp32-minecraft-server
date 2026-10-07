#!/usr/bin/env python3
"""Streams a board's serial console to stdout and lines from stdin to the board
(server console commands; used by test/hardware_smoke.js).

serial_monitor.py PORT [--reset] [--baud N]

--reset pulses RTS first, which resets an ESP32-S3 on its USB Serial/JTAG port
(and on the usual auto-reset circuit of a USB-UART bridge). Needs pyserial,
which ESP-IDF's Python environment provides.
"""
import argparse
import sys
import threading
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()
    s = serial.Serial(args.port, args.baud, timeout=0.2)
    if args.reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    port = [s]

    def forward():
        for line in sys.stdin.buffer:
            try:
                port[0].write(line.rstrip(b"\r\n") + b"\n")
            except serial.SerialException:
                pass

    threading.Thread(target=forward, daemon=True).start()
    out = sys.stdout.buffer
    while True:
        try:
            data = s.read(4096)
        except serial.SerialException:
            # USB Serial/JTAG re-enumerates when the chip resets
            s.close()
            while True:
                time.sleep(0.5)
                try:
                    s = serial.Serial(args.port, args.baud, timeout=0.2)
                    port[0] = s
                    break
                except serial.SerialException:
                    pass
            continue
        if data:
            out.write(data)
            out.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
