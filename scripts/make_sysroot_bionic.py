#!/usr/bin/env python3
"""Build an aarch64 sysroot with an OLD glibc for the R36S build.

ArkOS (the R36S image) has glibc 2.30; a binary linked on a current distro
needs 2.34+ and will not load there. This fetches Ubuntu 18.04 "bionic" arm64
packages (glibc 2.27) -- libc, SDL2, EGL, GLESv2, libvorbis/libogg/zlib and their headers -- from
ports.ubuntu.com and unpacks them into build/sysroot-arm64. Then:

    SYSROOT=$PWD/build/sysroot-arm64 bash scripts/build_arm64.sh link

Nothing is installed system-wide; the .debs are only unpacked (dpkg-deb -x).
"""
import gzip, io, os, subprocess, sys, urllib.request

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITES = ["bionic-updates", "bionic"]                 # first match wins
COMPONENTS = ["main", "universe"]
PACKAGES = [
    "libc6", "libc6-dev", "linux-libc-dev",
    "libsdl2-2.0-0", "libsdl2-dev",
    "libglvnd0", "libegl1", "libgles2", "libglvnd-dev",
    "libegl1-mesa-dev", "libgles2-mesa-dev", "mesa-common-dev", "libglx0", "libgl1", "libopengl0", "libglx-dev", "libgl-dev",
    "libogg0", "libogg-dev", "libvorbis0a", "libvorbisenc2", "libvorbis-dev", "zlib1g", "zlib1g-dev",
]
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "build", "sysroot-arm64")
DL = os.path.join(ROOT, "build", "sysroot-dl")


def index():
    found = {}
    for suite in reversed(SUITES):                       # later (updates) overrides
        for comp in COMPONENTS:
            url = f"{MIRROR}/dists/{suite}/{comp}/binary-arm64/Packages.gz"
            data = gzip.decompress(urllib.request.urlopen(url, timeout=60).read()).decode("utf-8", "replace")
            for stanza in data.split("\n\n"):
                fields = dict(l.split(": ", 1) for l in stanza.splitlines() if ": " in l and not l.startswith(" "))
                if fields.get("Package") in PACKAGES:
                    found[fields["Package"]] = fields["Filename"]
    return found


def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs(DL, exist_ok=True)
    found = index()
    missing = [p for p in PACKAGES if p not in found]
    if missing:
        print("not in bionic:", ", ".join(missing))
    for p, fn in sorted(found.items()):
        deb = os.path.join(DL, os.path.basename(fn))
        if not os.path.exists(deb):
            print("fetch", os.path.basename(fn))
            with urllib.request.urlopen(f"{MIRROR}/{fn}", timeout=120) as r, open(deb, "wb") as f:
                f.write(r.read())
        subprocess.run(["dpkg-deb", "-x", deb, OUT], check=True)
    # absolute symlinks inside the sysroot (libfoo.so -> /lib/...) must point into it
    for d, _, files in os.walk(OUT):
        for f in files:
            p = os.path.join(d, f)
            if os.path.islink(p) and os.readlink(p).startswith("/"):
                tgt = OUT + os.readlink(p)
                os.remove(p); os.symlink(os.path.relpath(tgt, d), p)
    print("sysroot:", OUT)


if __name__ == "__main__":
    sys.exit(main())
