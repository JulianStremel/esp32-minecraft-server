#!/usr/bin/env python3
"""Relay stdin/stdout to a TCP server: QEMU's guestfwd=...-cmd: runs this once per guest
connection, which lets the firmware reach a host NBD server on any port (and reconnect).

    python3 relay.py HOST PORT
"""
import os
import selectors
import socket
import sys


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    try:
        sock = socket.create_connection((host, port), timeout=5)
    except OSError as e:
        sys.stderr.write(f"relay: cannot connect to {host}:{port}: {e}\n")
        return 1
    sock.settimeout(None)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sel = selectors.DefaultSelector()
    sel.register(0, selectors.EVENT_READ, "guest")
    sel.register(sock, selectors.EVENT_READ, "host")
    while True:
        for key, _ in sel.select():
            if key.data == "guest":
                data = os.read(0, 65536)
                if not data:
                    return 0
                sock.sendall(data)
            else:
                data = sock.recv(65536)
                if not data:
                    return 0
                view = memoryview(data)
                while view:
                    n = os.write(1, view)
                    view = view[n:]


if __name__ == "__main__":
    sys.exit(main())
