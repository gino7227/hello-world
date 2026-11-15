#!/usr/bin/env python3
"""Szintetikus PTLF tesztfajl a PTLFRPT program kiprobalasahoz (nem NonStop gepen).

Formatum: minden rekord elott 2 bajtos big-endian hossz (a program nem-Tandem
olvasoja ezt varja). Valodi kartyaszam nincs benne: a PAN mezo csupa 'X'.

Hasznalat: gen_ptlf.py kimenet [--little-endian] [--seed N] [--count N]
"""
import argparse
import random
import struct
from datetime import datetime, timedelta, timezone

JTS_UNIX_EPOCH = 210866760000000000
REC_LEN = 786


def jts(dt):
    return JTS_UNIX_EPOCH + int(dt.timestamp() * 1_000_000)


def put(buf, off, s):
    b = s.encode("ascii") if isinstance(s, str) else s
    buf[off:off + len(b)] = b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--little-endian", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=20000)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    q = "<q" if a.little_endian else ">q"
    day = datetime(2026, 11, 14, tzinfo=timezone.utc)

    # oranketi sulyok (UTC ido; a riport -z 60-nal CET-ben mutatja)
    w = [3, 2, 1, 1, 1, 2, 8, 17, 24, 27, 28, 30, 33, 30, 28, 29, 32, 35, 33, 26, 19, 12, 9, 5]
    countries = ["HU"] * 80 + ["AT"] * 3 + ["DE"] * 3 + ["IE"] * 2 + ["HR", "SK", "NL", "IT", "GB", "RO", "US", "  "]
    tcs = ["10"] * 92 + ["11", "11", "12", "12", "14", "18", "17", "16"]
    decl_codes = [51] * 40 + [55] * 17 + [61] * 12 + [57] * 9 + [50] * 7 + [54] * 6 + [62] * 4 + [59] * 3 + [75] * 2

    recs = []

    def base(dt):
        r = bytearray(b" " * REC_LEN)
        put(r, 0, struct.pack(q, jts(dt)))
        put(r, 18, "X" * 19)                 # PAN (maszkolt)
        put(r, 145, "5")
        put(r, 428, "X" * 40)                # TRACK2 (maszkolt)
        return r

    for _ in range(a.count):
        h = rnd.choices(range(24), weights=w)[0]
        dt = day + timedelta(hours=h, seconds=rnd.randrange(3600), microseconds=rnd.randrange(10**6))
        r = base(dt)
        put(r, 8, "01")
        stip = (dt.hour == 2 and dt.minute < 50) or (dt.hour == 18 and 41 <= dt.minute <= 43)
        rev = rnd.random() < 0.006
        put(r, 172, "0420" if rev else "0210")
        put(r, 176, "11" if stip else "00")
        put(r, 178, "7")
        put(r, 179, "3" if stip else rnd.choices("53", weights=[90, 10])[0])
        put(r, 180, "35")
        exit_t = dt - timedelta(milliseconds=rnd.randint(150, 400))
        put(r, 182, struct.pack(q, jts(exit_t - timedelta(milliseconds=5))))
        if not stip:
            put(r, 190, struct.pack(q, jts(exit_t)))
            ms = int(rnd.lognormvariate(4.7, 0.4)) if dt.hour != 12 else int(rnd.lognormvariate(5.6, 0.5))
            put(r, 198, struct.pack(q, jts(exit_t + timedelta(milliseconds=ms))))
        else:
            put(r, 190, struct.pack(q, 0))
            put(r, 198, struct.pack(q, 0))
        put(r, 206, dt.strftime("%y%m%d"))
        put(r, 212, dt.strftime("%H%M%S") + "00")
        put(r, 297, "BUDAPEST     ")
        put(r, 313, rnd.choice(countries))
        put(r, 378, rnd.choice(tcs))
        put(r, 380, rnd.choice("12"))
        declined = rnd.random() < (0.07 if stip else 0.05)
        code = rnd.choice(decl_codes) if declined else 0
        put(r, 405, "%03d" % code)
        put(r, 408, struct.pack(q, rnd.randint(500, 60000) * 100))
        put(r, 416, struct.pack(q, 0))
        put(r, 532, "P")
        put(r, 539, "      " + (" B" if stip else "  "))
        put(r, 582, "01" if rev and rnd.random() < 0.6 else ("08" if rev else "00"))
        put(r, 589, rnd.choice(["051", "071", "012", "021"]))
        put(r, 593, "348" if rnd.random() < 0.85 else rnd.choice(["978", "840", "826"]))
        recs.append((dt, r))

    # kivetelek es osszesito rekordok
    for typ in ("20", "21", "23"):
        dt = day + timedelta(hours=10)
        r = base(dt)
        put(r, 8, typ)
        recs.append((dt, r))
    for i in range(5):
        dt = day + timedelta(hours=23, minutes=i)
        r = base(dt)[:400]
        put(r, 8, "04")
        put(r, 145, "2")
        recs.append((dt, r))

    recs.sort(key=lambda x: x[0])
    with open(a.out, "wb") as f:
        for _, r in recs:
            f.write(struct.pack(">H", len(r)))
            f.write(bytes(r))


if __name__ == "__main__":
    main()
