# PTLFRPT – BASE24-pos PTLF napi riport

C program HP NonStop (Guardian) rendszerre. A napi PTLF fájlt (`POyymmdd`) egyszer
olvassa végig, és négy riportot készít belőle:

1. **Napi összesítő** – tranzakciótípusonként (engedélyezett, elutasított), belföld/külföld,
   döntés helye (host, BASE24, STIP, Host Interface, hálózat), összegek devizánként, kivétel-rekordok.
2. **Válaszkód-elemzés** – elutasító és engedélyező kódok, döntés helye szerinti bontásban.
3. **Óránkénti forgalom** – darab, elutasítási arány, STIP, időtúllépéses sztornó,
   host válaszidő (átlag, p95), csúcsóra/-perc/-másodperc, STIP-időszakok.
4. **Melléklet: országok** – `TERM-CNTRY-CDE` szerint.

A kimenet ékezet nélküli ASCII, hogy spooleren is olvasható legyen.
Kártyaadatot (PAN, TRACK2, lejárat, számlaszám) a program **nem** ír ki.

## Fordítás NonStopon
1. Töltsd fel a `ptlfrpt.c` fájlt EDIT fájlként (pl. `$DATA.B24RPT.PTLFRPTC`),
   ugyanúgy, ahogy az előző C programot.
2. Fordítsd le ugyanazzal a fordítóval és opciókkal, mint az előzőt. Például:
   `CCOMP /IN PTLFRPTC/ PTLFRPT; RUNNABLE, SYSTYPE GUARDIAN`
   Itaniumon (TNS/E) a fordító neve `CCOMP`, x86-on (TNS/X) szintén `CCOMP`; ha nálatok
   más a megszokott parancs (pl. külön `ELD`/`XLD` linkelés), azt használd.

A program Guardian alatt a `FILE_OPEN_` / `READX` eljárásokkal olvassa az entry-sequenced
PTLF-et, és a `CONVERTTIMESTAMP` segítségével a rendszer helyi idejére vált.

## Futtatás
```
RUN PTLFRPT /OUT $S.#PTLFRPT/ -h BICI -r $DATA.B24RPT.RESPDESC $DATA.PTLF.PO261114
```

| Opció | Jelentés |
|---|---|
| `-a nnn` | Az első **elutasító** válaszkód (alap: `050`); ennél kisebb = engedélyezett |
| `-c XX` | Hazai országkód a belföld/külföld bontáshoz (alap: `HU`) |
| `-r fájl` | Válaszkód-megnevezések (`respdesc.txt`: a BASE24-pos kódok listája) |
| `-h dest` | A saját host `DEST` neve (pl. `BICI`), ha a host interchange interfészen át kapcsolódik: `RESPONDER` 6/7 és ez a `DEST` → Host |
| `-z perc` | Időeltolás GMT-hez képest percben (alap: a rendszer helyi ideje) |
| `-d n` | Diagnosztika: az első *n* tranzakció mezőinek kiírása (kártyaadat nélkül) |

### Első futtatás: ellenőrzés
Először futtasd `-d 5` opcióval, és nézd meg, hogy a mezők értelmesek-e:
`REC-TYP=01`, `TYP=0210`, `TC=10`, `RESP=000`, `DAT-TIM` mai dátum, `CRNCY=348` stb.
Ha valami elcsúszott, a DDL eltérhet a Release 6.0-tól; ilyenkor küldd el a kimenetet.

## Feltételezések (ellenőrizendő)
- **Engedélyezés:** `RESP-CDE < 050` → engedélyezett. A `-a` opcióval módosítható.
- **Döntés helye:** `RESPONDER` 5 = host, 3 = BASE24 (ha `RTE-STAT` ≠ 00, akkor STIP),
  4 = Host Interface, 6/7 = hálózat.
- **Nettó:** vásárlás + lezárás + MOTO + készpénz + cashback − visszatérítés − sztornó
  (`AMT-1`, a tranzakció devizájában). Az előengedélyezés külön oszlopban.
- **Időpont:** `HEAD.DAT-TIM` (rendszeridő), nem a terminál helyi ideje.
- **Válaszidő:** `RE-ENTRY-TIM − EXIT-TIM` (a host válaszideje a Host Interface szerint).
- A mezőpozíciók a BASE24-pos Release 6.0 DDL-jéből számolva: HEAD 172, AUTH 614 bájt.

## Válaszkód-fájlok
- `respdesc.txt` – BASE24-pos (PTLF) válaszkódok, ékezet nélkül. EDIT fájlként töltsd fel, és add meg `-r`-rel.
- `atmresp.txt` – BASE24-atm (TLF) válaszkódok a későbbi ATM riporthoz.

## Teszt (nem NonStop gépen)
```
python3 test/gen_ptlf.py /tmp/po.dat          # szintetikus PTLF, kártyaszám nélkül
gcc -std=c89 -Wall -o /tmp/ptlfrpt ptlfrpt.c
/tmp/ptlfrpt -z 60 -r respdesc.txt /tmp/po.dat
```
