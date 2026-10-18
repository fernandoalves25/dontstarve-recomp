#!/usr/bin/env python3
"""FEV (FMOD Designer 4.4x event project) reader -- work in progress.

A RIFF 'FEV ' file: FMT, then LIST PROJ with OBCT, PROP, LGCY (the classic
FEV1 binary, the event data), EPRP, STRR (string table), LANG. LGCY so far:
  u32, u32, str project name, u32 nbanks, u32 ?,
  nbanks x { u32 loadmode, u32 maxstreams, u64 hash (= the FSB5 header
             hash), u32 0, str name }
  category tree: { str name, f32 volume, f32 pitch, u32, u32, u32 nchildren,
                   children... }
  event groups / events (not decoded yet; a simple event record is 0xEE
  bytes + its category path string, e.g. "set_sfx/creature")
  u32 nconfigs, sound definition configs ...
  u32 nsounddefs, nsounddefs x { u32 name (STRR index), u32 config,
      u32 nentries, nentries x { u32 type, u32 weight, type 0 (wave):
      str "folder/file.wav", u32 bank index, u32 wave index in that bank's
      FSB, u32 length in ms; types 2/3: nothing more } }
  reverb presets ...
Strings are u32 length (with NUL) + bytes; STRR is u32 n, n x u32 offset,
then NUL-terminated strings. Simple events use sound definitions named
"/__simpleevent_sounddef__/<event>_<n>".
"""
import re, struct, sys


class R:
    def __init__(self, b, p=0): self.b, self.p = b, p
    def u32(self): v = struct.unpack_from("<I", self.b, self.p)[0]; self.p += 4; return v
    def i32(self): v = struct.unpack_from("<i", self.b, self.p)[0]; self.p += 4; return v
    def f32(self): v = struct.unpack_from("<f", self.b, self.p)[0]; self.p += 4; return v
    def u64(self): v = struct.unpack_from("<Q", self.b, self.p)[0]; self.p += 8; return v
    def str(self):
        n = self.u32(); s = self.b[self.p:self.p + n - 1].decode("latin1") if n else ""; self.p += n; return s


def chunks(d):
    out = {}
    def walk(off, end):
        while off + 8 <= end:
            cid = d[off:off + 4]; size = struct.unpack_from("<I", d, off + 4)[0]; body = off + 8
            if cid in (b"RIFF", b"LIST"): walk(body + 4, body + size)
            else: out[cid.decode("latin1")] = d[body:body + size]
            off = body + size + (size & 1)
    walk(0, len(d))
    return out


def strings(strr):
    n = struct.unpack_from("<I", strr, 0)[0]
    offs = struct.unpack_from(f"<{n}I", strr, 4); base = 4 + 4 * n
    return [strr[base + o:strr.index(b"\0", base + o)].decode("latin1") for o in offs]


def read_category(r, depth, out):
    name = r.str(); vol = r.f32(); pitch = r.f32(); a = r.u32(); b = r.u32(); n = r.u32()
    out.append(("  " * depth) + f"{name} vol={vol:g} pitch={pitch:g} {a} {b}")
    for _ in range(n): read_category(r, depth + 1, out)


def parse(path, verbose=False):
    d = open(path, "rb").read()
    c = chunks(d)
    S = strings(c["STRR"])
    r = R(c["LGCY"])
    h0, h1 = r.u32(), r.u32()
    proj = r.str()
    nbanks = r.u32(); _x = r.u32()
    banks = []
    for _ in range(nbanks):
        lm, ms, hsh, z = r.u32(), r.u32(), r.u64(), r.u32()
        banks.append(r.str())
    cats = []
    read_category(r, 0, cats)
    return dict(proj=proj, banks=banks, cats=cats, S=S, r=r, L=c["LGCY"])


def find_sounddefs(L, S):
    """Locate the sound definition table: a u32 count, then records whose
    names are '/...' sound definition paths. Its first name is not always the
    first '/' string, so try every spot and keep the largest table that reads
    through cleanly."""
    best = None
    for p in range(4, len(L) - 8):
        v = struct.unpack_from("<I", L, p)[0]
        if v >= len(S) or not S[v].startswith("/"): continue
        n = struct.unpack_from("<I", L, p - 4)[0]
        if not 0 < n < 100000 or (best and n <= len(best[1])): continue
        try:
            defs = read_sounddefs(L, S, p - 4)
        except Exception:
            continue
        if all(d["name"].startswith("/") for d in defs): best = (p - 4, defs)
    if not best: raise ValueError("no sound definition table")
    return best


def read_sounddefs(L, S, at):
    r = R(L, at); n = r.u32(); defs = []
    for _ in range(n):
        name = S[r.u32()]
        if not name.startswith("/"): raise ValueError("not a sound definition")
        cfg = r.u32(); ne = r.u32(); ents = []
        for _ in range(ne):
            t = r.u32(); w = r.u32()
            if t == 0:
                fn = r.str(); bank = r.u32(); idx = r.u32(); ms = r.u32()
                ents.append(dict(file=fn, bank=bank, index=idx, ms=ms, weight=w))
            elif t in (2, 3):
                ents.append(dict(file=None, type=t, weight=w))
            else:
                raise ValueError(f"sound def entry type {t} at {r.p:#x}")
        defs.append(dict(name=name, config=cfg, entries=ents))
    return defs


LAYER = re.compile(rb"\x02\x00\xff\xff..([\x01-\x08])\x00([\x00-\x20])\x00(..)", re.S)


def walk(path, simple_off=172):
    """Event tree -> (parsed, sound defs, [{path, type, raw: [sound def indices]}]).
    Groups: u32 name, u32 nprops (0 here), u32 nsubgroups, u32 nevents,
    subgroups, events. An event starts u32 type (0x10 simple, 8 complex),
    u32 name, 16-byte GUID, and ends with its category path string. A simple
    event's sound definition is the u16 at +172; a complex event has layers,
    each u16 flags (2), i16 priority (-1), i16 parameter, u16 nsounds,
    u16 nenvelopes, then sound instances starting with a u16 sound definition
    (layer headers found by scan)."""
    P = parse(path); L = P["L"]; S = P["S"]
    at, defs = find_sounddefs(L, S)
    catpaths = set(); stack = []
    for l in P["cats"]:
        d = (len(l) - len(l.lstrip(" "))) // 2; nm = l.strip().split(" ")[0]
        stack = stack[:d] + [nm]; catpaths.add("/".join(stack[1:]) if d > 0 else nm)
    pats = [struct.pack("<I", len(c) + 1) + c.encode() + b"\0" for c in catpaths]
    r = R(L, P["r"].p); events = []

    def find_cat(p):
        best = None
        for b in pats:
            j = L.find(b, p, at)
            if j >= 0 and (best is None or j < best[0]): best = (j, len(b))
        return best

    def parse_event(gp):
        st = r.p; typ = r.u32(); name = S[r.u32()]
        j, n = find_cat(st + 8); body = L[st:j]
        if typ == 0x10:
            raws = [struct.unpack_from("<H", L, st + simple_off)[0]]
        else:
            raws = [struct.unpack("<H", m.group(3))[0] for m in LAYER.finditer(body)]
        events.append(dict(path=gp + "/" + name, type=typ, raw=raws, st=st, end=j + n)); r.p = j + n

    def parse_group(gp):
        name = S[r.u32()]; nprops = r.u32()
        assert nprops == 0, (gp, name, hex(r.p))
        nsub = r.u32(); nev = r.u32()
        for _ in range(nsub): parse_group(gp + "/" + name)
        for _ in range(nev): parse_event(gp + "/" + name)

    for _ in range(r.u32()): parse_group(P["proj"])
    return P, defs, events


def dump(L, S, a, n):
    q = a
    while q < a + n:
        v = struct.unpack_from("<I", L, q)[0]
        if 1 < v < 200 and q + 4 + v <= len(L) and L[q + 4 + v - 1] == 0 and all(32 <= ch < 127 for ch in L[q + 4:q + 4 + v - 1]):
            print(f"{q:6x}: str \"{L[q+4:q+4+v-1].decode()}\""); q += 4 + v; continue
        f = struct.unpack_from("<f", L, q)[0]
        tag = f"  S[{v}]={S[v]!r}" if 0 < v < len(S) else ""
        print(f"{q:6x}: {v:08x} {v:10d} f={f:.4g}{tag}"); q += 4


if __name__ == "__main__":
    P = parse(sys.argv[1])
    print(P["proj"], len(P["banks"]), "banks;", len(P["cats"]), "categories")
    print("\n".join(P["cats"][:40]))
    at, defs = find_sounddefs(P["L"], P["S"])
    print(f"{len(defs)} sound definitions at {at:#x}, {sum(len(d['entries']) for d in defs)} entries")
    for d in defs[:3]: print(" ", d["name"], d["entries"][:2])
    P2, defs2, ev = walk(sys.argv[1])
    print(len(ev), "events")
    for e in ev[:5]:
        print(" ", e["path"], [[x.get("file") for x in defs2[r]["entries"]][:2] for r in e["raw"] if r < len(defs2)])
    if len(sys.argv) > 2: dump(P["L"], P["S"], P["r"].p, int(sys.argv[2], 0))
