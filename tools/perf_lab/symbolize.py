#!/usr/bin/env python3
"""Name the guest functions of addresses from host.txt's crash report, from
the image's ld.lld map (halo_guest.elf.map), without llvm-symbolizer.

    tools/perf_lab/symbolize.py build/macos/Halo/halo_guest.elf.map 882aa934 882a7f94 ...

With no addresses it reads host.txt's "(guest <address>)" and "lr" values
from standard input.
"""

import bisect
import re
import sys


def load(path):
    symbols = []
    for line in open(path, encoding="utf-8", errors="replace"):
        # VMA LMA Size Align Out In Symbol: a symbol line has no size column
        # worth reading, only its address and name
        match = re.match(r"\s*([0-9a-f]{8,16})\s+[0-9a-f]{8,16}\s+0\s+1\s+(\S+)\s*$", line)
        if match:
            symbols.append((int(match.group(1), 16), match.group(2)))
    symbols.sort()
    return symbols


def name(symbols, address):
    addresses = [s[0] for s in symbols]
    index = bisect.bisect_right(addresses, address) - 1
    if index < 0:
        return "?"
    base, symbol = symbols[index]
    return "%s+0x%x" % (symbol, address - base)


def main():
    symbols = load(sys.argv[1])
    values = sys.argv[2:]
    if not values:
        text = sys.stdin.read()
        values = re.findall(r"\(guest ([0-9a-f]+)\)", text) + re.findall(r"lr 0000000[0-9a-f]([0-9a-f]{8})", text)
    for value in values:
        address = int(value, 16) & 0xFFFFFFFF
        print("%08x %s" % (address, name(symbols, address)))


if __name__ == "__main__":
    main()
