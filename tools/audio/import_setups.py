#!/usr/bin/env python3
"""Fill in the Vorbis setup headers the host cannot rebuild by itself.

The host rebuilds each FSB5 setup header with libvorbis's encoder and checks
it against the CRC32 in the bank (src/runtime/ds_audio.c). That works for the
32/44.1/48 kHz waves; the low-rate ones (8-24 kHz, ~1,000 waves in Don't
Starve) came out of a different encoder build. HearthSim's python-fsb5 (MIT,
https://github.com/HearthSim/python-fsb5) ships a CRC32 -> setup header table
covering them. This script downloads that table (or reads a local copy),
parses it as data (ast.literal_eval, nothing is executed), and appends the
headers the game's banks use to the host's cache, $DS_HOME_DIR/vorbis_setups.bin.

    python3 tools/audio/import_setups.py [--table vorbis_headers.py]

Cache record: u32 crc, i32 channels, i32 ident length, i32 setup length,
ident packet, setup packet (the layout vs_load_cache reads).
"""
import argparse, ast, glob, os, struct, sys, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsb5  # noqa: E402

URL = "https://raw.githubusercontent.com/HearthSim/python-fsb5/master/fsb5/vorbis_headers.py"
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def ident(channels, rate):
    """Vorbis identification header, blocksizes 256/2048 as FMOD's encoder uses."""
    return (b"\x01vorbis" + struct.pack("<IBIiii", 0, channels, rate, 0, 0, 0)
            + bytes([8 | (11 << 4), 1]))


def cached(path):
    have = set()
    if os.path.exists(path):
        d = open(path, "rb").read(); p = 0
        while p + 16 <= len(d):
            crc, ch, ni, ns = struct.unpack_from("<IiII", d, p)
            have.add((crc, ch)); p += 16 + ni + ns
    return have


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--table", help="local copy of python-fsb5's vorbis_headers.py")
    ap.add_argument("--game", default=os.environ.get("DS_GAME_DIR", os.path.join(ROOT, "gamedata")))
    ap.add_argument("--home", default=os.environ.get("DS_HOME_DIR", os.path.join(ROOT, "build", "home")))
    a = ap.parse_args()
    src = open(a.table, encoding="utf-8").read() if a.table else \
        urllib.request.urlopen(URL, timeout=120).read().decode("utf-8")
    tree = ast.parse(src)
    table = next(ast.literal_eval(n.value) for n in tree.body
                 if isinstance(n, ast.Assign) and getattr(n.targets[0], "id", "") == "lookup")
    need = {}
    for f in glob.glob(os.path.join(a.game, "data", "sound", "*.fsb")):
        try:
            S = fsb5.read(f)
        except ValueError:
            continue
        for s in S if isinstance(S, list) else S[1]:
            if s.crc is not None: need[(s.crc, s.channels)] = s.rate
    out = os.path.join(a.home, "vorbis_setups.bin")
    os.makedirs(a.home, exist_ok=True)
    have = cached(out); added = missing = 0
    with open(out, "ab") as f:
        for (crc, ch), rate in sorted(need.items()):
            if (crc, ch) in have: continue
            st = table.get(crc)
            if st is None: missing += 1; continue
            idp = ident(ch, rate)
            f.write(struct.pack("<IiII", crc, ch, len(idp), len(st)) + idp + st); added += 1
    print(f"{len(need)} setups used by the banks; {len(have)} already cached, {added} added, {missing} not in the table -> {out}")


if __name__ == "__main__":
    sys.exit(main())
