#!/usr/bin/env python3
"""Teszt: a hossz-elotagos tesztfajlbol Enscribe-szeru nyers blokkfajl
(fejlec + rekordok + kitoltes) a TRNLIST -B (ugras) modjanak kiprobalasahoz.
Hasznalat: to_blocks.py be ki [blokkmeret] [max_rekord_blokkonkent]"""
import struct, sys, random
src, dst = sys.argv[1], sys.argv[2]
bs = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
per = int(sys.argv[4]) if len(sys.argv) > 4 else 99
d = open(src, 'rb').read(); i = 0; recs = []
while i < len(d):
    n = struct.unpack('>H', d[i:i+2])[0]; recs.append(d[i+2:i+2+n]); i += 2 + n
rnd = random.Random(7); out = bytearray(); blk = bytearray(rnd.randbytes(40)); k = 0
for r in recs:
    if len(blk) + len(r) + 64 > bs or k >= per:
        out += blk + bytes(bs - len(blk)); blk = bytearray(rnd.randbytes(40)); k = 0
    blk += r; k += 1
out += blk + bytes(bs - len(blk))
open(dst, 'wb').write(out)
print(len(recs), "rekord,", len(out) // bs, "blokk")
