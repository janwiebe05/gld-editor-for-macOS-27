#!/usr/bin/env python3
# Macht Bibliotheks-Abhängigkeiten in einer Mach-O-Datei optional
# (LC_LOAD_DYLIB -> LC_LOAD_WEAK_DYLIB). Fehlt die Bibliothek zur Laufzeit,
# startet das Programm trotzdem; ihre Symbole sind dann NULL.
# Aufruf: weaklink.py <mach-o> <teilstring-des-pfads> [...]

import struct
import sys

LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
MH_MAGIC_64 = 0xFEEDFACF


def patch(path, needles):
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        magic, _, _, _, ncmds = struct.unpack_from("<IiiII", data, 0)
        if magic != MH_MAGIC_64:
            sys.exit(f"{path}: kein 64-bit Mach-O (thin)")
        off = 32
        changed = []
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", data, off)
            if cmd == LC_LOAD_DYLIB:
                name_off = struct.unpack_from("<I", data, off + 8)[0]
                name = data[off + name_off:off + size].split(b"\0")[0].decode()
                if any(n in name for n in needles):
                    struct.pack_into("<I", data, off, LC_LOAD_WEAK_DYLIB)
                    changed.append(name)
            off += size
        f.seek(0)
        f.write(data)
    for name in changed:
        print(f"  weak: {name}")


if __name__ == "__main__":
    patch(sys.argv[1], sys.argv[2:])
