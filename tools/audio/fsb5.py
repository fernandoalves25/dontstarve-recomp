#!/usr/bin/env python3
"""FSB5 (FMOD Ex 4.44 sound bank) reader: sample table, names, Vorbis CRCs.

    python3 tools/audio/fsb5.py <bank.fsb> [...]     # list the samples

Layout (version 1): 'FSB5', version, numSamples, sampleHeadersSize,
nameTableSize, dataSize, mode (15 = Vorbis), 8 zero bytes, 16-byte hash,
8 bytes -> 60-byte header. Each sample header is a u64
  bit 0 more chunks | 1-4 rate index | 5 stereo | 6-33 data offset/16 |
  34-63 sample count
followed by chunks (u32: bit 0 more | 1-24 size | 25-31 type); type 11 =
Vorbis data: the CRC32 of the stripped setup header, then a seek table.
Vorbis sample data is a run of [u16 size][packet].
"""
import struct, sys

RATES = {1: 8000, 2: 11000, 3: 11025, 4: 16000, 5: 22050, 6: 24000, 7: 32000, 8: 44100, 9: 48000}


class Sample:
    __slots__ = ("name", "rate", "channels", "offset", "samples", "crc", "size", "loop")


def read(path):
    d = open(path, "rb").read()
    if d[:4] != b"FSB5":
        raise ValueError(f"{path}: not FSB5")
    ver, n, shsize, ntsize, dsize, mode = struct.unpack_from("<6I", d, 4)
    hdr = 60 if ver == 1 else 64
    pos = hdr
    out = []
    for _ in range(n):
        raw, = struct.unpack_from("<Q", d, pos); pos += 8
        s = Sample()
        more = raw & 1
        s.rate = RATES.get((raw >> 1) & 0xF, 44100)
        s.channels = ((raw >> 5) & 1) + 1
        s.offset = ((raw >> 6) & 0x0FFFFFFF) * 16
        s.samples = (raw >> 34) & 0x3FFFFFFF
        s.crc = None; s.loop = None
        while more:
            c, = struct.unpack_from("<I", d, pos); pos += 4
            more = c & 1; size = (c >> 1) & 0xFFFFFF; typ = (c >> 25) & 0x7F
            body = d[pos:pos + size]; pos += size
            if typ == 1: s.channels = body[0]
            elif typ == 2: s.rate, = struct.unpack_from("<I", body)
            elif typ == 3: s.loop = struct.unpack_from("<II", body)
            elif typ == 11: s.crc, = struct.unpack_from("<I", body)
        out.append(s)
    names = hdr + shsize
    if ntsize:
        offs = struct.unpack_from(f"<{n}I", d, names)
        for s, o in zip(out, offs):
            e = d.index(b"\0", names + o)
            s.name = d[names + o:e].decode("latin1")
    else:
        for i, s in enumerate(out): s.name = f"sample{i}"
    data = names + ntsize
    ends = [s.offset for s in out[1:]] + [dsize]
    for s, e in zip(out, ends):
        s.size = e - s.offset
        s.offset += data
    return mode, out, d


if __name__ == "__main__":
    for p in sys.argv[1:]:
        mode, ss, _ = read(p)
        print(f"{p}: mode {mode}, {len(ss)} samples")
        for s in ss[:8]:
            print(f"  {s.name:40s} {s.rate:6d} Hz {s.channels}ch {s.samples:8d} smp crc {s.crc:08X} loop {s.loop}")
