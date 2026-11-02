#!/usr/bin/env python3
"""Generate files/unbound_bulk.bin from a pinned PUSE checkout: Pokemon Unbound's species, move, item
and ability names, growth rates, Dex numbers, base stats, abilities, gender thresholds and level-up
learnsets, all by the game's own ids.

Usage: python3 -I tools/unbound_tables.py <PUSE>

PUSE is github.com/Zannael/PUSE, "Pokemon Unbound Save Editor", MIT License, Copyright (c) its
authors. The path must be the top of a checkout of the pinned commit; only these files of
backend/data are read, as data: pokemon.txt, moves.txt, items.txt, abilities.txt,
species_growth_rates.json, pokedex_species_map.json, move_table_from_rom.json,
item_pocket_map.json, species_abilities_meta.json, species_base_stats.json,
species_identity_meta.json and species_learnsets.json. Nothing in the checkout is built or run.
Standard library only. On any failed check the script names the first failure, exits 1 and writes
nothing. Running it twice writes the same bytes.

Layout (version 3), little-endian:

    0      4          magic "UBND"
    4      2          version 3
    6      2          table count
    8      4          CRC-32 (IEEE) of the bytes from 12 to the end
    12     12 * count table directory: tag (4 ASCII), offset (4), entries (2), entry size (2)

Tables, in this order. Names are uppercase ASCII, NUL-padded, with at least one NUL. Species ids
run 0 .. 1267 (1268 entries); id 0 is empty or zero in every table.

    NAME   11 bytes   species name (id 0 all NUL)
    GROW    1 byte    growth rate 0..5, 0xFF unknown
    NDEX    2 bytes   National Dex number, 0 unmapped
    MOVE   13 bytes   move name, ids 0 .. 922 (923 entries)
    MVPP    1 byte    move base PP, ids 0 .. 922
    ITEM   18 bytes   item name, ids 0 .. 728 (729 entries); "e" with an acute accent becomes E
    ITPK    1 byte    item pocket: 1 main, 2 ball, 3 berry, 4 tm, 5 hm, 6 key, 0 none (no name)
    ABIL   16 bytes   ability name, ids 0 .. 254 (255 entries)
    SABL    3 bytes   ability ids: slot 1, slot 2, hidden; 0 none or the species is missing
    BASE    6 bytes   base stats in the order HP, Atk, Def, Spe, SpA, SpD; 0s if missing
    GEND    1 byte    gender threshold, 255 if missing
    LVUP    2 bytes   1269 start offsets (in entries, counted from the first move entry, so offset
                      k to offset k+1 is species k's list; a species with no learnset has an empty
                      one), then the level-up move ids of species 0 .. 1267 in the file's order
    SRTS    2 bytes   species ids 1 .. 1267 sorted by the stored name
    SRTM    2 bytes   move ids 1 .. 922 sorted by the stored name
    SRTI    2 bytes   ids of the named items in pockets 1 to 3, sorted by the stored name
    SHIP    2 bytes   species ids 0 .. 1267: the National Dex number 1 .. 386 whose shipped picture
                      (files/all_sprites.fxbm, index number - 1) the species uses, else 0 (fetch it
                      from the ROM). A species gets its NDEX number if that is 1 .. 386 and it is
                      the lowest species id with that number; id 0 is 0.

Sorting compares the stored uppercase ASCII bytes, as strcmp does, and breaks ties by id.

Version 1 had NAME, GROW and NDEX alone, each of 1268 entries; version 2 added the tables from
MOVE to SRTI. Version 3 adds SHIP at the end; all the earlier tables are unchanged.
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
VERSION = 3
MOVE_SIZE = 13
ITEM_SIZE = 18
ABIL_SIZE = 16
POCKETS = {"main": 1, "ball": 2, "berry": 3, "tm": 4, "hm": 5, "key": 6}


class GenError(Exception):
    pass


def fail(message):
    raise GenError(message)


def read_names(path, name_max, first):
    names = {}
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            key, sep, name = line.partition(":")
            if not sep or not key.isdigit():
                fail(f"{path}:{n}: not 'id:Name'")
            name = name.replace("\u00e9", "E")
            if not name.isascii() or len(name) > name_max:
                fail(f"{path}:{n}: name {name!r} isn't ASCII of at most {name_max}")
            names[int(key)] = name.upper()
    count = len(names) + first
    if sorted(names) != list(range(first, count)):
        fail(f"{path}: ids aren't {first}..{count - 1}")
    return names, count


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def build(puse):
    head = subprocess.run(["git", "-C", puse, "rev-parse", "HEAD"], capture_output=True, text=True)
    if head.returncode != 0 or head.stdout.strip() != PUSE_COMMIT:
        fail(f"{puse} isn't at PUSE commit {PUSE_COMMIT}")
    data = os.path.join(puse, "backend", "data")
    names, count = read_names(os.path.join(data, "pokemon.txt"), NAME_MAX, 0)
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

    tables = [(b"NAME", name_t, NAME_SIZE, count), (b"GROW", grow_t, 1, count), (b"NDEX", ndex_t, 2, count)]
    tables += more_tables(data, names, count)
    tables.append((b"SHIP", ship_table(ndex_t, count), 2, count))
    offset = 12 + 12 * len(tables)
    toc = bytearray()
    body = bytearray()
    for tag, t, size, entries in tables:
        if len(t) != size * entries or entries > 0xFFFF:
            fail(f"table {tag!r}: {len(t)} bytes isn't {entries} of {size}")
        toc += tag + struct.pack("<IHH", offset + len(body), entries, size)
        body += t
    rest = bytes(toc + body)
    return b"UBND" + struct.pack("<HHI", VERSION, len(tables), zlib.crc32(rest)) + rest


def ship_table(ndex_t, count):
    """SHIP: the National number 1..386 for the lowest species id with it, else 0."""
    seen = set()
    ship = bytearray()
    for sid in range(count):
        n = struct.unpack_from("<H", ndex_t, 2 * sid)[0]
        use = sid != 0 and 1 <= n <= 386 and n not in seen
        if use:
            seen.add(n)
        ship += struct.pack("<H", n if use else 0)
    return ship


def padded(name, size):
    return name.encode("ascii").ljust(size, b"\0")


def byte_value(v, what):
    if not (isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 255):
        fail(f"{what}: {v!r} isn't a byte")
    return v


def sorted_ids(ids, name_of):
    return sorted(ids, key=lambda i: (name_of(i).encode("ascii"), i))


def more_tables(data, species_names, species_count):
    """The version 2 tables, as (tag, bytes, entry size, entries)."""
    move_names, move_count = read_names(os.path.join(data, "moves.txt"), MOVE_SIZE - 1, 1)
    item_names, item_count = read_names(os.path.join(data, "items.txt"), ITEM_SIZE - 1, 1)
    abil_names, abil_count = read_names(os.path.join(data, "abilities.txt"), ABIL_SIZE - 1, 1)
    rom_moves = read_json(os.path.join(data, "move_table_from_rom.json")).get("moves")
    pocket_map = read_json(os.path.join(data, "item_pocket_map.json")).get("pockets")
    sabl = read_json(os.path.join(data, "species_abilities_meta.json"))
    base = read_json(os.path.join(data, "species_base_stats.json"))
    ident = read_json(os.path.join(data, "species_identity_meta.json"))
    learn = read_json(os.path.join(data, "species_learnsets.json")).get("learnsets")
    if not isinstance(rom_moves, list) or not isinstance(pocket_map, dict) or not isinstance(learn, dict):
        fail("a data file isn't in the expected shape")

    move_t = bytearray(MOVE_SIZE)
    mvpp_t = bytearray(1)
    pp = {}
    for m in rom_moves:
        mid = m.get("move_id")
        if mid in pp:
            fail(f"move {mid} is in move_table_from_rom.json twice")
        pp[mid] = byte_value(m.get("base_pp"), f"move {mid} base_pp")
    for mid in range(1, move_count):
        if mid not in pp:
            fail(f"move {mid} has no base_pp")
        move_t += padded(move_names[mid], MOVE_SIZE)
        mvpp_t.append(pp[mid])

    pocket = {}
    for pname, code in POCKETS.items():
        if pname == "main":
            continue
        ids = pocket_map.get(pname, {}).get("ids")
        if not isinstance(ids, list):
            fail(f"item_pocket_map.json: pocket {pname} has no ids")
        for i in ids:
            if i in pocket:
                fail(f"item {i} is in two pockets")
            pocket[i] = code
    item_t = bytearray(ITEM_SIZE)
    itpk_t = bytearray(1)
    for i in range(1, item_count):
        item_t += padded(item_names[i], ITEM_SIZE)
        itpk_t.append(pocket.get(i, POCKETS["main"]))
    for i in pocket:
        if not (isinstance(i, int) and 1 <= i < item_count):
            fail(f"pocket item id {i!r} is out of range")

    abil_t = bytearray(ABIL_SIZE)
    for a in range(1, abil_count):
        abil_t += padded(abil_names[a], ABIL_SIZE)

    sabl_t = bytearray()
    base_t = bytearray()
    gend_t = bytearray()
    lvup_off = bytearray()
    lvup_moves = bytearray()
    n_moves = 0
    for sid in range(species_count):
        k = str(sid)
        ab = sabl.get(k)
        if ab is None:
            sabl_t += bytes(3)
        else:
            for f in ("ability_1_id", "ability_2_id", "hidden_ability_id"):
                v = byte_value(ab.get(f, 0), f"species {sid} {f}")
                if v >= abil_count:
                    fail(f"species {sid} {f}: ability {v} has no name")
                sabl_t.append(v)
        bs = base.get(k)
        if bs is None:
            base_t += bytes(6)
        else:
            for f in ("hp", "atk", "def", "spe", "spa", "spd"):
                base_t.append(byte_value(bs.get(f), f"species {sid} {f}"))
        g = ident.get(k)
        gend_t.append(255 if g is None else byte_value(g.get("gender_threshold"), f"species {sid} gender"))
        lvup_off += struct.pack("<H", n_moves)
        for mv in learn.get(k, {}).get("level_up", []):
            if not (isinstance(mv, int) and 1 <= mv < move_count):
                fail(f"species {sid}: level-up move {mv!r} is out of range")
            lvup_moves += struct.pack("<H", mv)
            n_moves += 1
    # The file also lists ids 1268 .. 1293, which have no name or stats in PUSE; they are left out.
    for k in learn:
        if not k.isdigit():
            fail(f"species_learnsets.json: species {k!r} isn't an id")
    lvup_off += struct.pack("<H", n_moves)
    if n_moves > 0xFFFF:
        fail("too many level-up moves")

    def pack_ids(ids):
        return b"".join(struct.pack("<H", i) for i in ids)

    srts = sorted_ids(range(1, species_count), lambda i: species_names[i])
    srtm = sorted_ids(range(1, move_count), lambda i: move_names[i])
    srti = sorted_ids([i for i in range(1, item_count) if itpk_t[i] in (1, 2, 3)], lambda i: item_names[i])
    return [
        (b"MOVE", move_t, MOVE_SIZE, move_count),
        (b"MVPP", mvpp_t, 1, move_count),
        (b"ITEM", item_t, ITEM_SIZE, item_count),
        (b"ITPK", itpk_t, 1, item_count),
        (b"ABIL", abil_t, ABIL_SIZE, abil_count),
        (b"SABL", sabl_t, 3, species_count),
        (b"BASE", base_t, 6, species_count),
        (b"GEND", gend_t, 1, species_count),
        (b"LVUP", lvup_off + lvup_moves, 2, species_count + 1 + n_moves),
        (b"SRTS", pack_ids(srts), 2, len(srts)),
        (b"SRTM", pack_ids(srtm), 2, len(srtm)),
        (b"SRTI", pack_ids(srti), 2, len(srti)),
    ]


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
