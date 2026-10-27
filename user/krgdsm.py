#!/usr/bin/env python3
"""krgdsm - a named shared segment, page by page.

A segment is a name plus page slots, and nothing is allocated until a page is
actually touched. The home node writes pages; any other node allocates the
same name (optionally with a cache limit) and pulls the pages it reads.

  krgdsm alloc NAME PAGES [--cache N]
                                   reserve slots; with --cache N this node
                                   keeps at most N pages and evicts the rest
  krgdsm fill  NAME PAGE TEXT      put TEXT at the start of one page
  krgdsm fillrange NAME FIRST COUNT
                                   stamp COUNT pages with a 64-byte record
  krgdsm write NAME PAGE FILE      put a file (<= 4096 B) at the start of a page
  krgdsm seal  NAME                freeze: no more local writes
  krgdsm info  NAME                pages / sealed / cache / resident / fetched
  krgdsm read  NAME PAGE [--off N] [--len N] [--hex]
  krgdsm fetch NAME PAGE HOST:PORT pull one page from a peer
  krgdsm fetchrange NAME FIRST LAST HOST:PORT
                                   pull a run of pages, BULK per request
  krgdsm stream NAME FIRST LAST HOST:PORT [--verify]
                                   pull and check a run, report peak memory
  krgdsm free  NAME

Still an explicit API, not a transparent mapping: the program asks for bytes.
"""
import ctypes
import fcntl
import os
import socket
import struct
import sys
import time

DEV = "/dev/krgdsm"
FMT = "<32sIIIIQIIIIII"
NAME = 32
PAGE = 4096
FLAG_REMOTE = 1


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("D") << 8) | nr


IOWR, IOW, IOR = 3, 1, 2
IOC_ALLOC = _ioc(IOWR, 1, struct.calcsize(FMT))
IOC_WRITE = _ioc(IOW, 2, struct.calcsize(FMT))
IOC_READ = _ioc(IOWR, 3, struct.calcsize(FMT))
IOC_SEAL = _ioc(IOW, 4, struct.calcsize(FMT))
IOC_FREE = _ioc(IOW, 5, struct.calcsize(FMT))
IOC_INFO = _ioc(IOWR, 6, struct.calcsize(FMT))

BULK = 64          # pages per DSMGET request; krgtool.c caps at 64


def _open():
    return os.open(DEV, os.O_RDWR)


def _pack(name, pages=0, page=0, off=0, ln=0, buf=0, flags=0, cache=0):
    return struct.pack(FMT, name.encode()[:NAME - 1], pages, page, off, ln,
                       buf, 0, 0, 0, flags, cache, 0)


def _unpack(raw):
    (name, pages, page, off, ln, buf, sealed, present, fetched, flags,
     cache, resident) = struct.unpack(FMT, raw)
    return dict(name=name.split(b"\0")[0].decode(), pages=pages, page=page,
                off=off, len=ln, sealed=sealed, present=present,
                fetched=fetched, flags=flags, cache=cache, resident=resident)


def _die(name, err):
    sys.exit("%s: %s" % (name, err.strerror))


def cmd_alloc(name, pages, cache=0):
    fd = _open()
    try:
        fcntl.ioctl(fd, IOC_ALLOC, _pack(name, pages=int(pages),
                                         cache=int(cache)))
    except OSError as e:
        _die(name, e)
    finally:
        os.close(fd)
    if cache:
        print("%s: %s page slots reserved, cache %s pages"
              % (name, pages, cache))
    else:
        print("%s: %s page slots reserved" % (name, pages))


def _write_page(fd, name, page, data, remote=False):
    buf = ctypes.create_string_buffer(data, PAGE)
    try:
        fcntl.ioctl(fd, IOC_WRITE,
                    _pack(name, page=int(page), ln=len(data),
                          buf=ctypes.addressof(buf),
                          flags=FLAG_REMOTE if remote else 0))
    except OSError as e:
        _die("%s page %s" % (name, page), e)


def _record(page):
    """The 64-byte stamp fillrange writes and stream verifies."""
    return ("krgdsm page %d" % page).encode().ljust(64, b"\0")


def cmd_fill(name, page, text):
    data = text.encode()
    fd = _open()
    try:
        _write_page(fd, name, page, (data * (PAGE // len(data) + 1))[:PAGE])
    finally:
        os.close(fd)
    print("%s: page %s filled with %r" % (name, page, text[:40]))


def cmd_fillrange(name, first, count):
    first, count = int(first), int(count)
    fd = _open()
    t0 = time.time()
    try:
        for p in range(first, first + count):
            _write_page(fd, name, p, _record(p))
    finally:
        os.close(fd)
    el = time.time() - t0
    print("%d pages stamped in %.3f s: %.0f us/page"
          % (count, el, el * 1e6 / max(count, 1)))


def cmd_write(name, page, path):
    with open(path, "rb") as fp:
        data = fp.read(PAGE)
    fd = _open()
    try:
        _write_page(fd, name, page, data)
    finally:
        os.close(fd)
    print("%s: page %s <- %s (%d bytes)" % (name, page, path, len(data)))


def _simple(name, ioc):
    fd = _open()
    try:
        fcntl.ioctl(fd, ioc, _pack(name))
    except OSError as e:
        _die(name, e)
    finally:
        os.close(fd)


def cmd_seal(name):
    _simple(name, IOC_SEAL)
    print("%s: sealed" % name)


def cmd_free(name):
    _simple(name, IOC_FREE)
    print("%s: freed" % name)


def _info(name):
    fd = _open()
    try:
        raw = fcntl.ioctl(fd, IOC_INFO, _pack(name), True)
    except OSError as e:
        _die(name, e)
    finally:
        os.close(fd)
    return _unpack(raw)


def cmd_info(name):
    i = _info(name)
    print("%s: %d pages, %s, cache %s, resident %d, fetched %d"
          % (i["name"], i["pages"], "sealed" if i["sealed"] else "open",
             i["cache"] or "-", i["resident"], i["fetched"]))


def cmd_read(name, page, off, ln, as_hex):
    buf = ctypes.create_string_buffer(PAGE)
    fd = _open()
    try:
        fcntl.ioctl(fd, IOC_READ,
                    _pack(name, page=int(page), off=off, ln=ln,
                          buf=ctypes.addressof(buf)), True)
    except OSError as e:
        _die("%s page %s" % (name, page), e)
    finally:
        os.close(fd)
    data = buf.raw[:ln]
    if as_hex:
        print(data.hex())
    else:
        sys.stdout.write(data.rstrip(b"\0").decode(errors="replace"))
        sys.stdout.write("\n")


def _read_page(fd, name, page, ln=PAGE):
    buf = ctypes.create_string_buffer(PAGE)
    fcntl.ioctl(fd, IOC_READ,
                _pack(name, page=int(page), ln=ln,
                      buf=ctypes.addressof(buf)), True)
    return buf.raw[:ln]


def _fetch_pages(fd, name, first, count, peer):
    """Pull `count` pages from `peer` into the local segment. Returns pages."""
    first = int(first)
    count = min(int(count), BULK)
    if count <= 0:
        return 0
    host, port = peer.rsplit(":", 1)
    s = socket.create_connection((host, int(port)), timeout=10)
    try:
        s.sendall(("DSMGET %s %d %d\n" % (name, first, count)).encode())
        head = b""
        while not head.endswith(b"\n"):
            c = s.recv(1)
            if not c:
                sys.exit("%s: peer closed" % peer)
            head += c
        if not head.startswith(b"OK "):
            sys.exit("%s: %s" % (peer, head.decode().strip()))
        n = int(head.split()[1])
        data = b""
        while len(data) < n:
            chunk = s.recv(n - len(data))
            if not chunk:
                sys.exit("%s: short read" % peer)
            data += chunk
    finally:
        s.close()
    got = n // PAGE
    for i in range(got):
        _write_page(fd, name, first + i, data[i * PAGE:(i + 1) * PAGE],
                    remote=True)
    return got


def cmd_fetch(name, page, peer):
    fd = _open()
    try:
        n = _fetch_pages(fd, name, page, 1, peer)
    finally:
        os.close(fd)
    print("%s: page %s <- %s (%d pages)" % (name, page, peer, n))


def cmd_fetchrange(name, first, last, peer):
    first, last = int(first), int(last)
    fd = _open()
    t0 = time.time()
    n = 0
    try:
        p = first
        while p <= last:
            cnt = min(BULK, last - p + 1)
            n += _fetch_pages(fd, name, p, cnt, peer)
            p += cnt
    finally:
        os.close(fd)
    el = time.time() - t0
    print("%d pages in %.3f s: %.0f us/page, %.2f MiB/s"
          % (n, el, el * 1e6 / n, n * PAGE / el / 1048576))


def cmd_stream(name, first, last, peer, verify):
    first, last = int(first), int(last)
    cache = _info(name)["cache"]
    chunk = min(BULK, cache) if cache else BULK
    fd = _open()
    t0 = time.time()
    pages = bad = peak = 0
    try:
        p = first
        while p <= last:
            cnt = min(chunk, last - p + 1)
            pages += _fetch_pages(fd, name, p, cnt, peer)
            if verify:
                for q in range(p, p + cnt):
                    got = _read_page(fd, name, q, 64)
                    if got != _record(q):
                        bad += 1
            r = _info(name)["resident"]
            peak = max(peak, r)
            p += cnt
    finally:
        os.close(fd)
    el = time.time() - t0
    print("%d pages in %.3f s: %.0f us/page, %.2f MiB/s, peak resident %d "
          "pages (%.1f MiB), mismatches %d"
          % (pages, el, el * 1e6 / pages, pages * PAGE / el / 1048576,
             peak, peak * PAGE / 1048576.0, bad))


def usage():
    print(__doc__.strip())


def main(argv):
    if len(argv) < 2:
        usage()
        return 1
    cmd, args = argv[1], argv[2:]

    def without(flag):
        return [a for a in args if a != flag]

    if cmd == "alloc" and len(args) >= 2:
        cache = 0
        if "--cache" in args:
            i = args.index("--cache")
            cache = int(args[i + 1])
            args = args[:i] + args[i + 2:]
        cmd_alloc(args[0], args[1], cache)
    elif cmd == "fill" and len(args) >= 3:
        cmd_fill(args[0], args[1], " ".join(args[2:]))
    elif cmd == "fillrange" and len(args) == 3:
        cmd_fillrange(args[0], args[1], args[2])
    elif cmd == "write" and len(args) == 3:
        cmd_write(args[0], args[1], args[2])
    elif cmd == "seal" and len(args) == 1:
        cmd_seal(args[0])
    elif cmd == "free" and len(args) == 1:
        cmd_free(args[0])
    elif cmd == "info" and len(args) == 1:
        cmd_info(args[0])
    elif cmd == "read" and len(args) >= 2:
        off, ln, as_hex = 0, PAGE, False
        if "--off" in args:
            off = int(args[args.index("--off") + 1])
        if "--len" in args:
            ln = int(args[args.index("--len") + 1])
        if "--hex" in args:
            as_hex = True
        cmd_read(args[0], args[1], off, ln, as_hex)
    elif cmd == "fetch" and len(args) == 3:
        cmd_fetch(args[0], args[1], args[2])
    elif cmd == "fetchrange" and len(args) == 4:
        cmd_fetchrange(args[0], args[1], args[2], args[3])
    elif cmd == "stream" and len(args) >= 4:
        verify = "--verify" in args
        a = without("--verify")
        cmd_stream(a[0], a[1], a[2], a[3], verify)
    else:
        usage()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
