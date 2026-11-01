#!/usr/bin/env python3
"""Generate files/unbound_bulk.bin from a pinned PUSE checkout: Pokemon Unbound's species names,
growth rates and National Dex numbers, by the game's own species id.

Usage: python3 -I tools/unbound_tables.py <PUSE>

PUSE is github.com/Zannael/PUSE, "Pokemon Unbound Save Editor", MIT License, Copyright (c) its
authors. The path must be the top of a checkout of the pinned commit; only these files are read,
as data: backend/data/pokemon.txt, species_growth_rates.json and pokedex_species_map.json. Nothing
in the checkout is built or run. Standard library only. On any failed check the script names the
first failure, exits 1 and writes nothing. Running it twice writes the same bytes.

Layout (version 1), little-endian:

    0      4          magic "UBND"
    4      2          version 1
    6      2          table count
    8      4          CRC-32 (IEEE) of the bytes from 12 to the end
    12     12 * count table directory: tag (4 ASCII), offset (4), entries (2), entry size (2)

Tables in version 1, each indexed by Unbound's species id 0 .. 1267:

    NAME   11 bytes   the name, uppercase ASCII, NUL-padded (id 0 all NUL)
    GROW    1 byte    growth rate 0..5, 0xFF unknown
    NDEX    2 bytes   National Dex number, 0 unmapped

Later versions append tables; these keep their tags and layouts.
"""

import json
import os
import struct
import subprocess
import sys
import zlib

PUSE_COMMIT = "2c624440fb24940480a3430fa17214942d4ba04a"
NAME_SIZE = 11
NAME_MAX = 10
VERSION = 1


class GenError(Exception):
    pass


def fail(message):
    raise GenError(message)


def read_names(path):
    names = {}
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            key, sep, name = line.partition(":")
            if not sep or not key.isdigit():
                fail(f"{path}:{n}: not 'id:Name'")
            if not name.isascii() or len(name) > NAME_MAX:
                fail(f"{path}:{n}: name {name!r} isn't ASCII of at most {NAME_MAX}")
            names[int(key)] = name.upper()
    count = len(names)
    if sorted(names) != list(range(count)):
        fail(f"{path}: ids aren't 0..{count - 1}")
    return names, count


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def build(puse):
    head = subprocess.run(["git", "-C", puse, "rev-parse", "HEAD"], capture_output=True, text=True)
    if head.returncode != 0 or head.stdout.strip() != PUSE_COMMIT:
        fail(f"{puse} isn't at PUSE commit {PUSE_COMMIT}")
    data = os.path.join(puse, "backend", "data")
    names, count = read_names(os.path.join(data, "pokemon.txt"))
    growth = read_json(os.path.join(data, "species_growth_rates.json"))
    dex = read_json(os.path.join(data, "pokedex_species_map.json")).get("species_to_dex")
    if not isinstance(dex, dict):
        fail("pokedex_species_map.json has no species_to_dex")

    name_t = bytearray()
    grow_t = bytearray()
    ndex_t = bytearray()
    for sid in range(count):
        name = "" if sid == 0 else names[sid]
        name_t += name.encode("ascii").ljust(NAME_SIZE, b"\0")
        g = growth.get(str(sid), {}).get("growth_rate")
        if g is not None and not (isinstance(g, int) and 0 <= g <= 5):
            fail(f"species {sid}: growth rate {g!r}")
        grow_t.append(0xFF if g is None else g)
        d = dex.get(str(sid), 0)
        if not (isinstance(d, int) and 0 <= d <= 0xFFFF):
            fail(f"species {sid}: Dex number {d!r}")
        ndex_t += struct.pack("<H", d)

    tables = [(b"NAME", name_t, NAME_SIZE), (b"GROW", grow_t, 1), (b"NDEX", ndex_t, 2)]
    offset = 12 + 12 * len(tables)
    toc = bytearray()
    body = bytearray()
    for tag, t, size in tables:
        toc += tag + struct.pack("<IHH", offset + len(body), count, size)
        body += t
    rest = bytes(toc + body)
    return b"UBND" + struct.pack("<HHI", VERSION, len(tables), zlib.crc32(rest)) + rest


def main():
    if len(sys.argv) != 2:
        print("usage: python3 -I tools/unbound_tables.py <PUSE>", file=sys.stderr)
        return 2
    try:
        out = build(sys.argv[1])
    except (GenError, OSError, ValueError) as e:
        print(f"unbound_tables.py: {e}", file=sys.stderr)
        return 1
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "files", "unbound_bulk.bin")
    with open(path, "wb") as f:
        f.write(out)
    print(f"files/unbound_bulk.bin: {len(out)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
