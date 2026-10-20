#!/usr/bin/env python3
"""Don't Starve asset tool for the R36S port (stdlib only, runs on PC or device).

  extract  <gog_installer.sh>... --out <gamedata>   unpack GOG Linux installers
  optimize <gamedata> [--dry-run] [--jobs N]        halve world textures losslessly

KTEX layout (verified against the shipped files):
  "KTEX" | u32 header | num_mips x (u16 w, u16 h, u16 pitch, u32 size) | mip data...
  header bits: 0-3 platform, 4-8 pixel format, 9-12 texture type, 13-17 mip count.

`optimize` drops mip 0 of DXT textures that carry a full mip chain. The remaining
mips are copied byte for byte, so nothing is re-encoded. Only textures whose
on-screen size comes from geometry are touched (animation atlases and ground);
UI atlases are left alone because the engine sizes UI widgets from texture
dimensions. The engine's own use_small_textures mode skips mip 0 at load time
but keeps the original size, which is why the launcher turns it off once these
files are optimized: both together would quarter the world textures.
"""
import argparse
import os
import struct
import sys
import zipfile
from concurrent.futures import ProcessPoolExecutor, as_completed

DXT_FORMATS = {0, 1, 2}          # DXT1, DXT3, DXT5
MIN_DIM = 64                     # keep tiny textures intact
MANIFEST = ".r36s_optimized"

# (relative dir under data/, kind). "zip" = .tex entries inside animation zips.
TARGETS = [
    ("anim", "zip"),
    ("levels/textures", "tex"),
    ("levels/tiles", "tex"),
]


def is_pot(n):
    return n > 0 and n & (n - 1) == 0


def drop_top_mip(buf):
    """Return the KTEX without its largest mip level, or None if not eligible."""
    if len(buf) < 18 or buf[:4] != b"KTEX":
        return None
    header = struct.unpack_from("<I", buf, 4)[0]
    pixel_format = (header >> 4) & 0x1F
    num_mips = (header >> 13) & 0x1F
    if pixel_format not in DXT_FORMATS or num_mips < 2:
        return None
    mips = [struct.unpack_from("<HHHI", buf, 8 + 10 * i) for i in range(num_mips)]
    data_start = 8 + 10 * num_mips
    if data_start + sum(m[3] for m in mips) != len(buf):
        return None  # unexpected layout: leave the file alone
    w, h = mips[0][0], mips[0][1]
    if not (is_pot(w) and is_pot(h)) or min(w, h) < MIN_DIM:
        return None
    header = (header & ~(0x1F << 13)) | ((num_mips - 1) << 13)
    out = bytearray(b"KTEX" + struct.pack("<I", header))
    for m in mips[1:]:
        out += struct.pack("<HHHI", *m)
    out += buf[data_start + mips[0][3]:]
    return bytes(out)


def optimize_tex(path, dry_run):
    with open(path, "rb") as f:
        buf = f.read()
    new = drop_top_mip(buf)
    if new is None:
        return path, len(buf), len(buf)
    if not dry_run:
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(new)
        os.replace(tmp, path)
    return path, len(buf), len(new)


def optimize_zip(path, dry_run):
    with zipfile.ZipFile(path) as z:
        entries = [(info, z.read(info)) for info in z.infolist()]
    changed = False
    raw_before = raw_after = 0
    out = []
    for info, data in entries:
        if info.filename.lower().endswith(".tex"):
            raw_before += len(data)
            new = drop_top_mip(data)
            if new is not None:
                data, changed = new, True
            raw_after += len(data)
        out.append((info, data))
    if dry_run:
        return path, raw_before, raw_after
    before = os.path.getsize(path)
    if not changed:
        return path, before, before
    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for info, data in out:
            zi = zipfile.ZipInfo(info.filename, info.date_time)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = info.external_attr
            z.writestr(zi, data)
    os.replace(tmp, path)
    return path, before, os.path.getsize(path)


def _work(job):
    kind, path, dry_run = job
    try:
        return (optimize_zip if kind == "zip" else optimize_tex)(path, dry_run)
    except Exception as e:  # a broken file must not stop the whole run
        print(f"  skip {path}: {e}", file=sys.stderr)
        return path, 0, 0


def find_data_dirs(root):
    """Every data/ folder that holds game scripts (GOG ships one per build)."""
    found = []
    for dirpath, dirnames, _ in os.walk(root):
        if os.path.basename(dirpath) == "data" and "scripts" in dirnames:
            found.append(dirpath)
            dirnames[:] = []
        dirnames[:] = [d for d in dirnames if d not in ("mods", "bin", "bin64")]
    return found


def cmd_optimize(args):
    data_dirs = find_data_dirs(args.gamedata)
    if not data_dirs:
        sys.exit(f"no Don't Starve data/ folder under {args.gamedata}")
    seen = set()
    for data in data_dirs:
        manifest = os.path.join(data, MANIFEST)
        done = set()
        if os.path.exists(manifest):
            with open(manifest) as f:
                done = {line.strip() for line in f if line.strip()}
        jobs = []
        # DLCs (Reign of Giants, Shipwrecked, Hamlet) keep their assets in data/DLC000N/
        roots = [""] + sorted(d + "/" for d in os.listdir(data)
                              if d.upper().startswith("DLC") and os.path.isdir(os.path.join(data, d)))
        for root in roots:
            for sub, kind in TARGETS:
                base = os.path.join(data, root, sub)
                ext = ".zip" if kind == "zip" else ".tex"
                # Top level only: Hamlet keeps its interior minimap (UI) and wall
                # textures in subfolders of levels/textures.
                names = sorted(os.listdir(base)) if os.path.isdir(base) else []
                for name in names:
                    full = os.path.join(base, name)
                    rel = f"{root}{sub}/{name}"
                    real = os.path.realpath(full)
                    if name.lower().endswith(ext) and os.path.isfile(full) and rel not in done and real not in seen:
                        seen.add(real)
                        jobs.append((kind, full, args.dry_run))
        print(f"{data}: {len(jobs)} files to check")
        before = after = touched = 0
        log = None if args.dry_run else open(manifest, "a")
        with ProcessPoolExecutor(max_workers=args.jobs) as pool:
            # Log each file as soon as it is rewritten so an interrupted run
            # never halves the same texture twice.
            for fut in as_completed([pool.submit(_work, j) for j in jobs]):
                path, b, a = fut.result()
                if a != b:
                    touched += 1
                before += b
                after += a
                if log:
                    log.write(os.path.relpath(path, data).replace(os.sep, "/") + "\n")
                    log.flush()
        if log:
            log.close()
        if args.dry_run:
            print(f"  would shrink {touched} files: {before / 1e6:.1f} MB -> {after / 1e6:.1f} MB of texture data")
        else:
            print(f"  shrank {touched} files: {before / 1e6:.1f} MB -> {after / 1e6:.1f} MB on disk")


def cmd_extract(args):
    os.makedirs(args.out, exist_ok=True)
    prefix = "data/noarch/game/"
    for installer in args.installers:
        print(f"extracting {installer}")
        # GOG .sh installers are a shell stub + MojoSetup + a zip; zipfile
        # copes with the leading bytes on its own.
        with zipfile.ZipFile(installer) as z:
            for info in z.infolist():
                if not info.filename.startswith(prefix) or info.is_dir():
                    continue
                rel = info.filename[len(prefix):]
                dest = os.path.join(args.out, *rel.split("/"))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(info) as src, open(dest, "wb") as dst:
                    while True:
                        chunk = src.read(1 << 20)
                        if not chunk:
                            break
                        dst.write(chunk)
                mode = (info.external_attr >> 16) & 0o777
                if mode:
                    os.chmod(dest, mode)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract", help="unpack GOG Linux installers")
    e.add_argument("installers", nargs="+")
    e.add_argument("--out", required=True)
    o = sub.add_parser("optimize", help="halve animation/ground textures in place")
    o.add_argument("gamedata")
    o.add_argument("--dry-run", action="store_true")
    o.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = p.parse_args()
    (cmd_extract if args.cmd == "extract" else cmd_optimize)(args)


if __name__ == "__main__":
    main()
