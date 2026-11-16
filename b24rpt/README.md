# BASE24 napi riportok (HP NonStop, C)

Két önálló C program Guardian alá:

| Program | Bemenet | Riportok |
|---|---|---|
| `ptlfrpt.c` | BASE24-pos PTLF (`POyymmdd`) | napi összesítő, válaszkód-elemzés, óránkénti forgalom, országlista |
| `tlfrpt.c` | BASE24-atm TLF (`TLyymmdd`) | napi összesítő, válaszkód-elemzés, óránkénti forgalom, saját ATM-ek forgalma (`ATM...`), sztornóokok |

A kimenet ékezet nélküli ASCII, legfeljebb 79 oszlop: terminálon és spoolerben is olvasható.
Kártyaszámot, Track 2-t, lejáratot, számlaszámot és egyenleget egyik program sem ír ki.

## Fordítás
Töltsd fel a forrást EDIT fájlként, és fordítsd le ugyanúgy, mint eddig, pl.:
```
CCOMP /IN PTLFRPTC/ PTLFRPT; RUNNABLE, SYSTYPE GUARDIAN
CCOMP /IN TLFRPTC/  TLFRPT;  RUNNABLE, SYSTYPE GUARDIAN
```

## Futtatás
```
RUN PTLFRPT /OUT $S.#PTLFRPT/ $vol.subvol.POyymmdd
RUN TLFRPT  /OUT $S.#TLFRPT/  $vol.subvol.TLyymmdd
```
Egyetlen opció a `-d n` (diagnosztika): az első *n* tranzakció mezőit írja ki, majd kilép.
Új fájlnál vagy BASE24-verzióváltás után érdemes vele ellenőrizni, hogy a mezők jó helyről jönnek-e:
```
RUN PTLFRPT -d 5 $vol.subvol.POyymmdd
```

## Beépített beállítások
- **Idő:** a rendszer helyi ideje (`HEAD.DAT-TIM`, GMT → helyi idő a `CONVERTTIMESTAMP`-pal).
- **Belföld:** `HU` (`AUTH.TERM-CNTRY-CDE`), csak a PTLF riportban.
- **Engedélyezett válaszkód:** POS `000–049`; ATM ok-kód `00–49` és `90–92` (a válaszkód első jegye
  `1` = elnyelt kártya).
- **Válaszkód-megnevezések:** a BASE24-pos és BASE24-atm kódok a programokba vannak építve.
  A `respdesc.txt` és az `atmresp.txt` csak a forrásuk.
- **Döntés helye:** `RESPONDER` 5 = host, 3 = BASE24 (ha `RTE-STAT` ≠ 00: STIP), 4 = Host Interface,
  6/7 = interchange.

## Feltételezések
- PTLF nettó: vásárlás + lezárás + MOTO + készpénz + cashback − visszatérítés − sztornó
  (`AMT-1`, a tranzakció devizájában). Az előengedélyezés külön.
- TLF nettó kifizetés: engedélyezett felvét − sztornózott (0420: `AMT-1 − AMT-2`) + korrekció
  (5400: `AMT-2 − AMT-1`). Saját kártya: `CRD.FIID` = `TERM.FIID`.
- Mezőpozíciók: BASE24 Release 6.0 DDL. PTLF: HEAD 172 + AUTH 614 bájt; TLF: HEAD 76 + AUTH 398 bájt.

## Teszt (nem NonStop gépen)
```
python3 test/gen_ptlf.py /tmp/po.dat; python3 test/gen_tlf.py /tmp/tl.dat
gcc -std=c89 -Wall -o /tmp/ptlfrpt ptlfrpt.c; gcc -std=c89 -Wall -o /tmp/tlfrpt tlfrpt.c
TZ=Europe/Budapest /tmp/ptlfrpt /tmp/po.dat
TZ=Europe/Budapest /tmp/tlfrpt /tmp/tl.dat
```
A tesztfájlok szintetikusak, valódi kártyaadat nincs bennük.

---

# EMSERR / EMSSCAN – hibakeresés az EMS logban

Az EMS logfájlok (`ZZEVnnnn`, 843-as kód) binárisak, ezért két lépésben dolgozunk:

1. **`emserr.tacl`** (TACL makró): az `EMSDIST` printing distributorral a `$0` collector
   logjaiból egy nap eseményeit szövegfájlba íratja (`TIME`/`STOP`, collector: `$PCOLL`).
2. **`emsscan.c`** (C program): a szövegben hibára utaló eseményeket keres, és riportot készít:
   keresőszavanként és forrásonként (processz, subsystem) összesít, majd eseménytípusonként
   (processz + subsystem + eseményszám) kiírja a darabszámot, az első/utolsó időpontot és egy mintát.

```
LOAD /KEEP 1/ $DATA.EMSRPT.EMSERR
EMSERR                 == tegnapi nap
EMSERR 2026-11-15      == adott nap
RUN EMSSCAN $DATA.EMSRPT.EMSTXT ABEND DOWN    == közvetlenül, saját keresőszavakkal
```

Beépített keresőszavak: ERROR, FAIL, ABEND, ABORT, TRAP, HALT, DOWN, TIMEOUT, TIME-OUT,
TIMED OUT, UNABLE, CANNOT, NOT AVAILABLE, NOT RESPONDING, STOPPED, DEADLOCK, VIOLATION,
CRITICAL, LOST, DISCONNECT, OVERFLOW, FULL, UNAVAILABLE. A kritikus események (`*` a subsystem előtt)
mindig találatok.

---

# NAPI – reggeli automatikus futtatás (`napi.tacl`)

- **`DAILYRPT [yyyy-mm-dd]`** – egy nap összes riportja (alap: tegnap): `PTLFRPT` → `$S.#B24NAPI.POS`,
  `TLFRPT` → `$S.#B24NAPI.ATM`, `EMSERR` → `$S.#EMSERR`. Hiányzó PTLF/TLF fájlt jelez, és továbblép.
- **`DAILYLOOP`** – háttérben fut, minden nap a beállított időpont (alap 06:00) után egyszer lefuttatja
  a `DAILYRPT`-t; 5 percenként ellenőriz. Leállítás: a stop-fájl létrehozása vagy `STOP $NAPI`.

A fájlneveket a `napi^cfg` szekcióban kell beállítani.

Indítás háttérben (a `NAPIIN` EDIT fájlban: `LOAD /KEEP 1/ $DATA.EMSRPT.NAPI` és `DAILYLOOP`):
```
TACL /NAME $NAPI, NOWAIT, IN $DATA.EMSRPT.NAPIIN, OUT $S.#B24NAPI.LOG/
```
NetBatch esetén a job egyszerűen: `LOAD /KEEP 1/ $DATA.EMSRPT.NAPI` majd `DAILYRPT`.

---

# TCPRATE – egy port TCP-forgalma a CLIM-eken (`tcprate.tacl` + `tcprate.c`)

```
LOAD /KEEP 1/ $DATA.EMSRPT.TCPRMAC
TCPRATE 4275                 == ATM, minden névtér, 60 mp
TCPRATE 7000 NMLNS006        == HSM, adott névtér
TCPRATE 4275 * 30            == minden névtér, 30 mp
```
- Mindkét irányt nézi (`dport` és `sport`), így mindegy, hogy a BASE24 a szerver vagy a kliens.
- Névtér nélkül (vagy `*`) a beállított CLIM-ek összes névterét végignézi (`ip netns list`).
- Két pillanatkép (`climcmd … ip netns exec <ns> ss -tin`) a megadott idő különbséggel; a `TCPRATE`
  program kapcsolatonként kiszámolja az üzenet/mp értéket (`data_segs_in/out` különbség), és jelzi:
  `SQ` (Send-Q > 0), `CS` (60 mp-nél régebben jött adat), `RT` (újraküldés), `UJ`/`MSZ`.
- Összesítés túloldali IP-cím szerint (terheléselosztás) és mindösszesen.

A CLIM-ek nevét és a program helyét a makró BEÁLLÍTÁSOK részében kell megadni.
