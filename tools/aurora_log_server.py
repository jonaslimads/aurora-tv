#!/usr/bin/env python3
"""Receive Aurora's UDP log stream and show a controller-drop timeline.

A debug build of Aurora (cmake -DAURORA_LOG_SHIP_TARGET=<this machine>:5514) copies every
log line of INFO level and above to this port. Run it and reproduce the fault on the TV:

    python3 tools/aurora_log_server.py

Useful knobs:

    --port 5514          listen port (must match the build)
    --grep REGEX         print only matching lines
    --out FILE           append every raw line, for attaching to a report
    --no-marks           skip the drop/flap banners

The app sends one line per datagram, prefixed with a level letter and its own
uptime in milliseconds; the wall clock and the gap to the previous line are added here,
because the gap between two removals is the measurement that matters.
"""

import argparse
import collections
import re
import socket
import struct
import sys
import time

# A controller that flaps produces these back to back; counting them turns a wall of
# text into "drop #7 after 3.4s".
MARK_RE = re.compile(r"(removal:|Removal context|disconnected|Controller #\d+ (arrived|removed))", re.IGNORECASE)
DROP_RE = re.compile(r"removal:", re.IGNORECASE)

LEVEL_COLORS = {"F": "\033[31m", "E": "\033[31m", "W": "\033[33m", "I": "\033[0m", "D": "\033[2m", "V": "\033[2m"}
RESET = "\033[0m"


def local_addresses():
    """Every address worth pointing the TV at, from the default route's source."""
    addrs = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        addrs.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addr = info[4][0]
            if addr not in addrs and not addr.startswith("127."):
                addrs.append(addr)
    except OSError:
        pass
    return addrs or ["<this machine's LAN IP>"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=5514)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--grep", metavar="REGEX", help="print only lines matching this regex")
    parser.add_argument("--out", metavar="FILE", help="append raw lines to FILE")
    parser.add_argument("--no-marks", action="store_true", help="do not print drop/flap banners")
    parser.add_argument("--no-color", action="store_true")
    args = parser.parse_args()

    want = re.compile(args.grep, re.IGNORECASE) if args.grep else None

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))

    print(f"listening on udp://{args.host}:{args.port}")
    print("point Aurora at one of these: " + ", ".join(f"{a}:{args.port}" for a in local_addresses()))
    print("waiting for the TV ... (Ctrl-C to stop)\n")

    sink = open(args.out, "a", buffering=1) if args.out else None
    previous = None
    drops = collections.deque(maxlen=200)
    marked = 0

    try:
        while True:
            data, peer = sock.recvfrom(4096)
            now = time.time()
            gap = 0.0 if previous is None else now - previous
            previous = now
            for raw in data.decode("utf-8", "replace").splitlines():
                line = raw.rstrip()
                if not line:
                    continue
                if sink:
                    sink.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {peer[0]} {line}\n")
                if DROP_RE.search(line):
                    drops.append(now)
                if want and not want.search(line):
                    continue
                if not args.no_marks and MARK_RE.search(line):
                    marked += 1
                    recent = [g for g in _gaps(drops)]
                    every = sum(recent[-8:]) / len(recent[-8:]) if recent else 0.0
                    banner = f"--- drop #{marked} after {gap:5.2f}s"
                    if every:
                        banner += f", recent average {every:.1f}s apart"
                    banner += " ---"
                    print(f"\033[36m{banner}{RESET}\n" if not args.no_color else banner)
                level = line[0] if line[0] in LEVEL_COLORS else " "
                color = "" if args.no_color else LEVEL_COLORS.get(level, "")
                print(f"{color}{time.strftime('%H:%M:%S')} +{gap:5.2f}s {line}{RESET}")
    except KeyboardInterrupt:
        print(f"\nstopped after {marked} marked line(s)")
    finally:
        if sink:
            sink.close()
            print(f"raw log: {args.out}")
    return 0


def _gaps(times):
    ordered = list(times)
    return [b - a for a, b in zip(ordered, ordered[1:])]


if __name__ == "__main__":
    sys.exit(main())
