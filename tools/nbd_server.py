#!/usr/bin/env python3
"""Minimal Network Block Device server for the ESP32 Minecraft server.

Serves one sparse file as an NBD export (fixed newstyle protocol). Supports
NBD_OPT_GO / NBD_OPT_INFO / NBD_OPT_EXPORT_NAME / NBD_OPT_LIST / NBD_OPT_ABORT
and the commands READ, WRITE, FLUSH, TRIM, WRITE_ZEROES, DISC.

    python3 tools/nbd_server.py --file world.img --size 1G --port 10809

Any standard NBD server works just as well, e.g.:
    nbdkit -f file world.img            (or: nbdkit memory 1G)
    qemu-nbd -t -f raw -p 10809 world.img
    nbd-server 10809 /path/world.img
"""
import argparse
import asyncio
import os
import socket
import struct
import sys

NBDMAGIC = 0x4E42444D41474943
IHAVEOPT = 0x49484156454F5054
REPLY_MAGIC = 0x0003E889045565A9
REQUEST_MAGIC = 0x25609513
SIMPLE_REPLY = 0x67446698

OPT_EXPORT_NAME, OPT_ABORT, OPT_LIST, OPT_INFO, OPT_GO = 1, 2, 3, 6, 7
REP_ACK, REP_SERVER, REP_INFO = 1, 2, 3
REP_ERR_UNSUP, REP_ERR_UNKNOWN, REP_ERR_INVALID = 0x80000001, 0x80000006, 0x80000003
CMD_READ, CMD_WRITE, CMD_DISC, CMD_FLUSH, CMD_TRIM, CMD_WRITE_ZEROES = 0, 1, 2, 3, 4, 6
FLAG_HAS_FLAGS, FLAG_READ_ONLY, FLAG_SEND_FLUSH, FLAG_SEND_FUA = 1, 2, 4, 8
FLAG_SEND_TRIM, FLAG_SEND_WRITE_ZEROES, FLAG_CAN_MULTI_CONN = 32, 64, 256
EIO, EINVAL, ENOSPC, EPERM = 5, 22, 28, 1


def parse_size(s):
    s = s.strip().upper()
    mult = 1
    for suffix, m in (("K", 1 << 10), ("M", 1 << 20), ("G", 1 << 30), ("T", 1 << 40)):
        if s.endswith(suffix):
            mult, s = m, s[:-1]
            break
    return int(float(s) * mult)


# Windows has no pread/pwrite. All I/O runs on the single asyncio thread, so a
# seek followed by read/write cannot interleave with another request.
def _pread(fd, length, offset):
    os.lseek(fd, offset, os.SEEK_SET)
    return os.read(fd, length)


def _pwrite(fd, data, offset):
    os.lseek(fd, offset, os.SEEK_SET)
    return os.write(fd, data)


pread = getattr(os, "pread", _pread)
pwrite = getattr(os, "pwrite", _pwrite)


def grow_sparse(fd, size):
    """Extends the file to size without allocating disk space."""
    if sys.platform != "win32":
        os.ftruncate(fd, size)
        return
    # Windows: ftruncate() writes zeros. Mark the file sparse (NTFS), then move its end.
    import ctypes
    import msvcrt
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                         wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                         wintypes.LPVOID]
    kernel32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD]
    kernel32.SetEndOfFile.argtypes = [wintypes.HANDLE]
    handle = msvcrt.get_osfhandle(fd)
    FSCTL_SET_SPARSE = 0x000900C4
    returned = wintypes.DWORD()
    if not kernel32.DeviceIoControl(handle, FSCTL_SET_SPARSE, None, 0, None, 0, ctypes.byref(returned), None):
        print(f"nbd: cannot mark the image sparse (error {ctypes.get_last_error()}), it will use its full size",
              flush=True)
    if not kernel32.SetFilePointerEx(handle, size, None, 0) or not kernel32.SetEndOfFile(handle):
        raise OSError(ctypes.get_last_error(), "cannot resize the image")


class Export:
    def __init__(self, path, size, readonly):
        self.path = path
        self.readonly = readonly
        exists = os.path.exists(path)
        flags = (os.O_RDONLY if readonly else os.O_RDWR) | os.O_CREAT | getattr(os, "O_BINARY", 0)
        self.fd = os.open(path, flags, 0o644)
        cur = os.fstat(self.fd).st_size
        if size is None:
            size = cur
        if not readonly and cur < size:
            grow_sparse(self.fd, size)  # only written blocks use disk space
        self.size = size
        print(f"nbd: export {path} ({size >> 20} MiB{', read-only' if readonly else ''}{', existing' if exists else ', new'})",
              flush=True)
        self.stats = {"read": 0, "write": 0, "flush": 0, "trim": 0}

    def flags(self):
        f = FLAG_HAS_FLAGS | FLAG_SEND_FLUSH | FLAG_SEND_FUA | FLAG_SEND_TRIM | FLAG_SEND_WRITE_ZEROES | FLAG_CAN_MULTI_CONN
        if self.readonly:
            f |= FLAG_READ_ONLY
        return f


class Session:
    def __init__(self, export, name, reader, writer, verbose):
        self.export, self.name, self.r, self.w, self.verbose = export, name, reader, writer, verbose
        self.no_zeroes = False
        self.delay_ms = 0

    async def opt_reply(self, opt, rtype, data=b""):
        self.w.write(struct.pack(">QIII", REPLY_MAGIC, opt, rtype, len(data)) + data)
        await self.w.drain()

    async def negotiate(self):
        self.w.write(struct.pack(">QQH", NBDMAGIC, IHAVEOPT, 1 | 2))  # FIXED_NEWSTYLE | NO_ZEROES
        await self.w.drain()
        (cflags,) = struct.unpack(">I", await self.r.readexactly(4))
        self.no_zeroes = bool(cflags & 2)
        while True:
            magic, opt, length = struct.unpack(">QII", await self.r.readexactly(16))
            if magic != IHAVEOPT:
                return False
            data = await self.r.readexactly(length) if length else b""
            if opt == OPT_EXPORT_NAME:
                if self.name and data.decode(errors="replace") not in ("", self.name):
                    return False  # no way to report an error here: just close
                reply = struct.pack(">QH", self.export.size, self.export.flags())
                if not self.no_zeroes:
                    reply += b"\0" * 124
                self.w.write(reply)
                await self.w.drain()
                return True
            if opt in (OPT_GO, OPT_INFO):
                if len(data) < 6:
                    await self.opt_reply(opt, REP_ERR_INVALID)
                    continue
                (nlen,) = struct.unpack(">I", data[:4])
                req = data[4:4 + nlen].decode(errors="replace")
                if self.name and req not in ("", self.name):
                    await self.opt_reply(opt, REP_ERR_UNKNOWN, b"unknown export")
                    continue
                await self.opt_reply(opt, REP_INFO, struct.pack(">HQH", 0, self.export.size, self.export.flags()))
                await self.opt_reply(opt, REP_ACK)
                if opt == OPT_GO:
                    return True
                continue
            if opt == OPT_LIST:
                n = (self.name or "").encode()
                await self.opt_reply(opt, REP_SERVER, struct.pack(">I", len(n)) + n)
                await self.opt_reply(opt, REP_ACK)
                continue
            if opt == OPT_ABORT:
                await self.opt_reply(opt, REP_ACK)
                return False
            await self.opt_reply(opt, REP_ERR_UNSUP)

    async def reply(self, handle, error=0, data=b""):
        # Serial service delay for repeatable storage-stall tests (not network RTT).
        if self.delay_ms:
            await asyncio.sleep(self.delay_ms / 1000)
        self.w.write(struct.pack(">IIQ", SIMPLE_REPLY, error, handle) + data)

    async def serve(self):
        ex = self.export
        while True:
            hdr = await self.r.readexactly(28)
            magic, flags, cmd, handle, offset, length = struct.unpack(">IHHQQI", hdr)
            if magic != REQUEST_MAGIC:
                return
            if cmd == CMD_DISC:
                return
            in_range = offset + length <= ex.size
            if cmd == CMD_READ:
                if not in_range:
                    await self.reply(handle, EINVAL)
                else:
                    data = pread(ex.fd, length, offset)
                    data += b"\0" * (length - len(data))
                    ex.stats["read"] += length
                    await self.reply(handle, 0, data)
            elif cmd == CMD_WRITE:
                data = await self.r.readexactly(length)
                if ex.readonly:
                    await self.reply(handle, EPERM)
                elif not in_range:
                    await self.reply(handle, ENOSPC)
                else:
                    pwrite(ex.fd, data, offset)
                    if flags & 1:  # FUA
                        os.fsync(ex.fd)
                    ex.stats["write"] += length
                    await self.reply(handle)
            elif cmd == CMD_FLUSH:
                os.fsync(ex.fd)
                ex.stats["flush"] += 1
                await self.reply(handle)
            elif cmd in (CMD_TRIM, CMD_WRITE_ZEROES):
                if ex.readonly:
                    await self.reply(handle, EPERM)
                elif not in_range:
                    await self.reply(handle, EINVAL)
                else:
                    if cmd == CMD_WRITE_ZEROES:
                        pos, end = offset, offset + length
                        while pos < end:
                            n = min(end - pos, 1 << 20)
                            pwrite(ex.fd, b"\0" * n, pos)
                            pos += n
                    # TRIM is advisory: the data may simply stay
                    ex.stats["trim"] += 1
                    await self.reply(handle)
            else:
                await self.reply(handle, EINVAL)
            await self.w.drain()


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", required=True, help="backing file (created sparse if missing)")
    ap.add_argument("--size", default=None, help="export size, e.g. 512M or 2G (default: file size, or 1G for new files)")
    ap.add_argument("--port", type=int, default=10809)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--name", default="", help="export name to require (default: accept any)")
    ap.add_argument("--readonly", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--delay-ms", type=float, default=0, help="test-only service delay per transmission request")
    args = ap.parse_args()
    size = parse_size(args.size) if args.size else (None if os.path.exists(args.file) else 1 << 30)
    export = Export(args.file, size, args.readonly)

    async def handle(reader, writer):
        peer = writer.get_extra_info("peername")
        print(f"nbd: client {peer} connected", flush=True)
        # Replies to pipelined requests are several small writes; with Nagle each one waits
        # for the client's delayed ACK (lwIP: up to 250 ms).
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s = Session(export, args.name, reader, writer, args.verbose)
        s.delay_ms = max(0, args.delay_ms)
        try:
            if await s.negotiate():
                await s.serve()
        except (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError):
            pass
        finally:
            os.fsync(export.fd)
            writer.close()
            print(f"nbd: client {peer} gone ({export.stats})", flush=True)

    server = await asyncio.start_server(handle, args.bind, args.port)
    print(f"nbd: listening on {args.bind}:{args.port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
