#!/usr/bin/env python3
"""
pack-bundle.py - append the SFX bundle data to pgbundle.exe.

Reads a stub MZ executable (compiled from pgbundle.c) and a list of
input files, writes a final EXE with the file directory and contents
appended after the MZ image, and a 12-byte trailer at the very end so
the stub can find the data at runtime.

Format (matches pgbundle.c):

    [ MZ stub                                    ]
    [ entry 1: name[16] + size_u32 + size bytes  ]
    [ entry 2: name[16] + size_u32 + size bytes  ]
    ...
    [ trailer:  magic[4]="PGZ1"
                dir_offset_u32
                file_count_u32                   ]

Names are uppercased and NUL-padded to 16 bytes. Sizes / offsets are
little-endian, matching how Watcom on x86 reads them back.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--stub",   required=True, type=Path,
                    help="compiled pgbundle.exe stub (MZ executable)")
    ap.add_argument("--out",    required=True, type=Path,
                    help="destination path for the final SFX EXE")
    ap.add_argument("--name",   action="append", default=[],
                    help="rename a file inside the bundle: --name FROM=TO "
                         "(applied before truncating to 16 chars)")
    ap.add_argument("files", nargs="+", type=Path,
                    help="input files to embed")
    args = ap.parse_args()

    if not args.stub.is_file():
        print(f"!! stub not found: {args.stub}", file=sys.stderr)
        return 1

    rename_map: dict[str, str] = {}
    for spec in args.name:
        if "=" not in spec:
            print(f"!! bad --name spec: {spec!r} (want FROM=TO)", file=sys.stderr)
            return 1
        src, dst = spec.split("=", 1)
        rename_map[src] = dst

    args.out.parent.mkdir(parents=True, exist_ok=True)

    with args.out.open("wb") as out:
        # Copy the stub verbatim.
        with args.stub.open("rb") as stub:
            out.write(stub.read())
        dir_offset = out.tell()

        # Sanity guard against a clearly-busted stub.
        if dir_offset < 256:
            print(f"!! stub is suspiciously small ({dir_offset} bytes)",
                  file=sys.stderr)
            return 1

        # Append each file.
        for path in args.files:
            data = path.read_bytes()
            name = path.name
            if name in rename_map:
                name = rename_map[name]
            name = name.upper().encode("ascii", "strict")
            if len(name) > 16:
                print(f"!! name too long (>16): {name!r}", file=sys.stderr)
                return 1
            name = name.ljust(16, b"\0")

            out.write(name)
            out.write(struct.pack("<I", len(data)))
            out.write(data)

        # 12-byte trailer at end-of-file.
        out.write(b"PGZ1")
        out.write(struct.pack("<II", dir_offset, len(args.files)))

    total = args.out.stat().st_size
    print(f">> wrote {args.out} ({total:,} bytes, {len(args.files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
