#!/usr/bin/env python3
"""Szintetikus BASE24-atm TLF tesztfajl a TLFRPT program kiprobalasahoz.

Formatum: minden rekord elott 2 bajtos big-endian hossz. Kartyaszam es
szamlaszam nincs benne (csupa 'X' / '0').

Hasznalat: gen_tlf.py kimenet [--little-endian] [--seed N] [--count N]
"""
import argparse
import random
import struct
from datetime import datetime, timedelta, timezone

JTS_UNIX_EPOCH = 210866760000000000
REC_LEN = 474


def jts(dt):
    return JTS_UNIX_EPOCH + int(dt.timestamp() * 1_000_000)


def put(buf, off, s):
    b = s.encode("ascii") if isinstance(s, str) else s
    buf[off:off + len(b)] = b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--little-endian", action="store_true")
    ap.add_argument("--seed", type=int, default=2)
    ap.add_argument("--count", type=int, default=15000)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    q = "<q" if a.little_endian else ">q"
    day = datetime(2026, 11, 14, tzinfo=timezone.utc)
    w = [2, 1, 1, 1, 1, 2, 6, 14, 20, 24, 26, 28, 32, 28, 26, 27, 30, 34, 30, 24, 17, 10, 7, 4]
    atms = ["A%04d" % i for i in range(1, 41)]
    locs = {t: ("BUDAPEST %s FIOK" % t[-2:]) for t in atms}
    broken = "A0007"     # ez az ATM 10:00 utan elhallgat
    faulty = "A0013"     # ennél sok a hardverhiba
    recs = []

    def head(dt, term, rectyp):
        r = bytearray(b" " * REC_LEN)
        put(r, 0, struct.pack(q, jts(dt)))
        put(r, 8, rectyp)
        put(r, 14, "BANK")
        put(r, 18, "0001")
        put(r, 22, term.ljust(16))
        put(r, 46, "X" * 19)
        put(r, 65, "000")
        return r

    for _ in range(a.count):
        h = rnd.choices(range(24), weights=w)[0]
        dt = day + timedelta(hours=h, seconds=rnd.randrange(3600), microseconds=rnd.randrange(10**6))
        term = rnd.choice(atms)
        if term == broken and dt.hour >= 9:
            term = rnd.choice(atms[:6])
        r = head(dt, term, "01")
        own = rnd.random() < 0.7
        put(r, 38, "BANK" if own else rnd.choice(["VISA", "MC  "]))
        put(r, 42, "0001" if own else "9999")
        stip = own and dt.hour == 3 and dt.minute < 40
        rev = rnd.random() < (0.05 if term == faulty else 0.008)
        put(r, 76, "31")
        put(r, 78, "0420" if rev else "0210")
        put(r, 82, "11" if stip else "00")
        put(r, 84, "1")
        put(r, 85, "3" if stip else ("5" if own else "7"))
        exit_t = dt - timedelta(milliseconds=rnd.randint(100, 300))
        put(r, 86, struct.pack(q, jts(exit_t - timedelta(milliseconds=3))))
        if stip:
            put(r, 94, struct.pack(q, 0))
            put(r, 102, struct.pack(q, 0))
        else:
            put(r, 94, struct.pack(q, jts(exit_t)))
            put(r, 102, struct.pack(q, jts(exit_t + timedelta(milliseconds=int(rnd.lognormvariate(4.9, 0.35))))))
        put(r, 110, dt.strftime("%y%m%d"))
        put(r, 116, dt.strftime("%H%M%S") + "00")
        put(r, 154, "01")
        tc = rnd.choices(["10", "30", "40", "81", "20"], weights=[70, 20, 4, 2, 4])[0]
        put(r, 180, tc + "1100")
        put(r, 186, "0" * 19)
        put(r, 206, "0" * 19)
        amt = rnd.choice([5000, 10000, 20000, 30000, 50000, 100000]) * 100 if tc in ("10", "20") else 0
        put(r, 226, struct.pack(q, amt))
        if rev:
            put(r, 234, struct.pack(q, rnd.choice([0, 0, amt // 2])))
        else:
            put(r, 234, struct.pack(q, 123456789))   # egyenleg (nem jelenhet meg)
        put(r, 242, struct.pack(q, 0))
        declined = (not rev) and rnd.random() < 0.06
        code = "000"
        if declined:
            code = rnd.choices(["061", "058", "053", "051", "062", "150", "072"], weights=[30, 25, 20, 8, 5, 3, 9])[0]
        put(r, 255, code)
        put(r, 258, locs[term].ljust(25))
        put(r, 305, "BUDAPEST     ")
        put(r, 321, "HU")
        put(r, 351, "348")
        if rev:
            rs = rnd.choices(["01", "10", "08", "02", "03"], weights=[30, 40 if term == faulty else 5, 50, 5, 10])[0]
        else:
            rs = "00"
        put(r, 384, rs)
        put(r, 403, "P")
        recs.append((dt, r))

    # adminisztrativ (kazetta) rekordok
    for t in atms:
        for k in range(rnd.randint(0, 3)):
            dt = day + timedelta(hours=rnd.randint(6, 20), minutes=rnd.randint(0, 59))
            r = head(dt, t, "04")[:120]
            put(r, 76, dt.strftime("%y%m%d"))
            put(r, 82, dt.strftime("%H%M%S") + "00")
            put(r, 90, rnd.choice(["03", "04", "01"]))
            recs.append((dt, r))
    # korrekcio (5400) egy hibas penzkiadasra
    dt = day + timedelta(hours=11)
    r = recs[0][1][:]
    put(r, 0, struct.pack(q, jts(dt)))
    put(r, 22, faulty.ljust(16))
    put(r, 78, "5400")
    put(r, 180, "101100")
    put(r, 226, struct.pack(q, 2000000))
    put(r, 234, struct.pack(q, 1000000))
    put(r, 255, "000")
    put(r, 384, "15")
    recs.append((dt, r))

    recs.sort(key=lambda x: x[0])
    with open(a.out, "wb") as f:
        for _, r in recs:
            f.write(struct.pack(">H", len(r)))
            f.write(bytes(r))


if __name__ == "__main__":
    main()
