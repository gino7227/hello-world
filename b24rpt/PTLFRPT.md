# PTLFRPT – BASE24-pos napi tranzakciós riport (PTLF)

## 1. Cél

A PTLFRPT a BASE24-pos (classic) napi tranzakciós naplójából, a **PTLF**-ből (`POyymmdd`) készít üzemeltetési riportot. A banknál kibocsátói nézetből: a saját kártyabirtokosok más bankok POS-terminálján végzett tranzakciói, amelyek a kártyahálózaton (Visa, Mastercard) érkeznek engedélyezésre.

A program a fájlt egyszer olvassa végig, és négy riportot ír:

| # | Riport | Kérdés, amire válaszol |
|---|---|---|
| 1 | Napi összesítő | Mennyi és milyen tranzakció volt, mennyit engedélyeztünk, ki döntött, mekkora összegben? |
| 2 | Válaszkód-elemzés | Miért utasítottunk el, és ki hozta a döntést (host, BASE24, STIP)? |
| 3 | Óránkénti forgalom | Mikor volt csúcs, mikor nem volt elérhető a host (STIP), milyen gyorsan válaszolt? |
| 4 | Melléklet: országok | Mely országokban használták a kártyákat? (tájékoztató) |

A kimenet ékezet nélküli ASCII, legfeljebb 79 oszlop széles, így terminálon és spoolerben is olvasható. **Kártyaadatot (kártyaszám, Track 2, lejárat, számlaszám) a program nem ír ki.**

## 2. Használat

```
RUN PTLFRPT /OUT $S.#PTLFRPT/ $vol.subvol.POyymmdd      == riport a spoolerbe
RUN PTLFRPT $vol.subvol.POyymmdd                        == riport a terminálra
RUN PTLFRPT -d 5 $vol.subvol.POyymmdd                   == diagnosztika
```

Az egyetlen opció a `-d n`: az első *n* tranzakciós rekord fontos mezőit írja ki (kártyaadat nélkül), majd kilép. Új fájlnál vagy BASE24-verzióváltás után ezzel lehet ellenőrizni, hogy a mezők jó helyről olvasódnak-e.

Diagnosztika példa:

```
#1 hossz=1846 REC-TYP=01 FRMT=5 TYP=0210 TC=10 T=2 RESP=001
   ORIGINATOR=7 RESPONDER=7 RTE-STAT=00 ISS-CDE=00 AUTH-IND=P
   ORIG='VISA' DEST='BICI' AUTHORIZER='AUTHH'
   AMT-1=100000 AMT-2=0 CRNCY=348 CNTRY=US RVRL=00 APPRV(7-8)='  '
   CITY='New York     ' ENTRY-MDE=012
   DAT-TIM=2026.11.02 12:52:12 TRAN-DAT=261102 TRAN-TIM=14521000
   host valaszido=2164 ms
Bajtsorrend: big-endian. Tranzakcios rekordok: 78
```

A napi automatikus futtatást a `napi.tacl` `DAILYRPT` makrója végzi (a tegnapi PTLF-re, kimenet: `$S.#B24NAPI.POS`).

## 3. Telepítés

1. A `ptlfrpt.c` feltöltése EDIT fájlként (pl. `PTLFRPTC`).
2. Fordítás a szokásos módon, pl.: `CCOMP /IN PTLFRPTC/ PTLFRPT; RUNNABLE, SYSTYPE GUARDIAN`.
3. Első futtatás diagnosztikával (`-d 5`).

Guardian-specifikus részek a programban:

- fájlolvasás: `FILE_OPEN_`, `READX`, `FILE_GETINFO_` (a `cextdecs` behúzása `.h` nélkül: `#include <cextdecs(…)>`);
- időátváltás: `CONVERTTIMESTAMP` (GMT → helyi idő) és `INTERPRETTIMESTAMP`.

## 4. Adatforrás

### 4.1 A PTLF

- Fájlnév: `POyymmdd`, entry-sequenced, egy fájl egy üzleti nap.
- Rekord: `HEAD` (172 bájt) + `AUTH` (614 bájt) = minimum 786 bájt (a ténylegesen beolvasott rekordok hosszabbak lehetnek a token-adatok miatt).
- A mezőpozíciók a BASE24-pos Release 6.0 DDL-jéből (`DDLFPTLF`, `DDLGDEFS`) származnak.

### 4.2 Mely rekordokat dolgozza fel

| Rekord | Feltétel | Kezelés |
|---|---|---|
| Tranzakció | `HEAD.REC-TYP = 01` és `REC-FRMT = 5` | feldolgozza |
| Kivétel | `REC-TYP` 20–23 | csak megszámolja (posted / not posted / partially posted / invalid data) |
| Terminál-összesítő | `REC-TYP = 04` | megszámolja |
| Egyeztetés, issuer reversal response | `AUTH.TYP` 0500, 0412 | kihagyja („egyéb”) |

### 4.3 Felhasznált mezők

| Mire | Mező |
|---|---|
| Időpont | `HEAD.DAT-TIM` (rendszeridő, helyi időre váltva) |
| Üzenettípus | `AUTH.TYP` (0210, 0220, 0420) |
| Tranzakciótípus | `AUTH.TRAN-CDE.TC` |
| Válaszkód | `AUTH.RESP-CDE` (3 jegy) |
| Összeg, deviza | `AUTH.AMT-1`, `AUTH.ORIG-CRNCY-CDE` |
| Döntés helye | `AUTH.RESPONDER`, `AUTH.RTE-STAT` |
| Engedélyező | `AUTH.AUTHORIZER` |
| Host válaszidő | `AUTH.RE-ENTRY-TIM − AUTH.EXIT-TIM` |
| Sztornó oka | `AUTH.RVRL-CDE` |
| Ország | `AUTH.TERM-CNTRY-CDE` |

A 64 bites bináris mezők bájtsorrendjét a program az első rekord időbélyegéből magától felismeri.

## 5. Beépített szabályok

| Szabály | Érték |
|---|---|
| Engedélyezett válaszkód | `000–049` (a többi elutasított) |
| Belföld | `TERM-CNTRY-CDE = HU`, minden más külföld (üres: `??`) |
| Döntés helye | `RESPONDER` 5 = Host; 3 és `RTE-STAT = 00` = BASE24; 3 és `RTE-STAT ≠ 00` = STIP (host nem elérhető); 4 = Host Interface; 6/7 = Interchange |
| Válaszkód-megnevezések | a BASE24-pos kódok (122 db) a programba építve |
| STIP-időszakok | STIP-es percek, legfeljebb 5 perc szünettel összevonva |

**Tranzakciótípusok** (`TRAN-CDE.TC`): 10 vásárlás, 11 előengedélyezés, 12 előengedélyezés lezárása, 13 levelezéses/telefonos, 14 visszatérítés, 15 készpénzfelvétel, 16 kártyaellenőrzés, 17 egyenleglekérdezés, 18 vásárlás + cashback, 21–24 korrekció. A 0420-as üzenet típustól függetlenül sztornó.

**Nettó forgalom** (devizánként, a tranzakció devizájában, csak engedélyezett tranzakciók):

```
Nettó = vásárlás + lezárás + MOTO + készpénz + cashback − visszatérítés − sztornó
```

Az előengedélyezés (zárolt összeg) külön oszlopban jelenik meg, a nettóba nem számít bele.

Megjegyzés a döntés helyéhez: ha a saját host egy interchange interfészen át kapcsolódik (pl. `DEST = BICI`), a normál host-engedélyezések „Interchange” sorban jelennek meg. Ezt az „Engedélyező (RESPONDER)” tábla mutatja meg, ahol az `AUTHORIZER` neve is látszik.

## 6. A riportok

### 6.1 Napi összesítő

```
===============================================================================
BASE24 POS (KIBOCSATOI) - NAPI OSSZESITO
Forras:  $DATA.PTLF.PO261114
Idoszak: 2026.11.14 00:00:16 - 2026.11.14 23:59:11
===============================================================================
Rekordok:  osszes 20 008, tranzakcio 20 000, terminal-osszesito 5,
           egyeb 0, rovid 0
Kivetelek: 20 konyvelt 1, 21 nem konyvelt 1, 22 reszben 0,
           23 hibas adat 1
Engedelyezett: valaszkod < 050

Tipus                            Osszes   Engedelyez.  Elutasitott   Jovah.%
----------------------------------------------------------------------------
Vasarlas                         18 340        17 463          877    95,22%
Eloengedelyezes                     382           361           21    94,50%
...
OSSZESEN                         20 000        19 037          963    95,19%
```

Táblák sorrendben:

| Tábla | Tartalom |
|---|---|
| Típus | tranzakciótípusonként összes, engedélyezett, elutasított, jóváhagyási arány |
| Bontás | belföld / külföld |
| Döntés helye | Host, BASE24, STIP, Host Interface, Interchange |
| Engedélyező (RESPONDER) | `AUTHORIZER` neve + `RESPONDER` kód szerint |
| Összegek devizánként | vásárlás + lezárás, visszatérítés, sztornó, előengedélyezés (zárolt), **nettó** |

A rekordszámok és a kivételek sora jelzi, ha a fájlban szokatlanul sok kivétel-rekord van (feldolgozási hiba a BASE24-ben).

### 6.2 Válaszkód-elemzés

```
A) Elutasito valaszkodok
Kod Megnevezes                              Darab    Arany   Host BASE24   STIP
-------------------------------------------------------------------------------
051 Lejart kartya                             371   38,53%    333     38      -
055 Nem engedelyezett tranzakciotipus         167   17,34%    148     19      -
...
```

- **A)** elutasító, **B)** engedélyező kódok, gyakoriság szerint.
- A „döntés helye” oszlopok közül csak azok jelennek meg, amelyekben van adat.
- Ha egy elutasító kód a **STIP** vagy **BASE24** oszlopban sok, az nem a kártyabirtokosra, hanem a host elérhetőségére vagy BASE24-konfigurációra utal.

### 6.3 Óránkénti forgalom

```
Ora      Darab  Elut%  STIP TOszt Atlms p95ms
-------------------------------------------------------------------------------
...
17-18    1 432    4,8     -     5   118   210 ####################
18-19    1 577    4,4     -     3   117   218 ###################### <CSUCS
...
Napi atlag:      0,23 tranzakcio/mp
Csucsora:        18:00-19:00  1 577 db, atlag 0,44 tr/mp
Csucsperc:       13:53        39 db, atlag 0,7 tr/mp
Csucsmasodperc:  15:32:03     5 tr/mp
Host valaszido:  atlag 132 ms, median 114 ms, p95 266 ms, p99 474 ms

STIP-idoszakok (host nem elerheto):
  03:02-03:50  39 tranzakcio
  19:41-19:44  64 tranzakcio
```

| Oszlop / sor | Jelentés |
|---|---|
| Elut% | elutasítási arány az órában |
| STIP | STIP-ben engedélyezett/elutasított tranzakciók |
| TOszt | időtúllépés miatti sztornó (0420, `RVRL-CDE = 01`) |
| Atlms / p95ms | host válaszidő átlaga és 95. percentilise (ms) |
| `<CSUCS` | a legforgalmasabb óra |
| `<ELUT` | az elutasítási arány a napi átlag 2-szerese felett (min. 50 tranzakció) |
| STIP-időszakok | mikor nem volt elérhető a host, és hány tranzakciót érintett |

### 6.4 Melléklet: országok

A legforgalmasabb 25 ország darabszámmal, aránnyal és elutasítási aránnyal; a többi „Egyéb” sorban. Forrás: `TERM-CNTRY-CDE`; `??` = üres mező.

## 7. Hibaelhárítás

| Tünet | Ok | Teendő |
|---|---|---|
| `cannot open source file "cextdech"` | a `cextdecs` `.h`-val lett behúzva | `#include <cextdecs(FILE_OPEN_,…)>` – `.h` nélkül |
| `unresolved reference to _status_eq` | régi forrásváltozat | a jelenlegi forrás `FILE_GETINFO_`-val ellenőrzi a `READX`-et |
| `Nincs tranzakcios rekord …` | rossz fájl, vagy eltérő rekordszerkezet | `-d 5` kimenete: `REC-TYP=01`, `FRMT=5`, `TYP=0210` legyen |
| Értelmetlen mezőértékek a diagnosztikában | eltérő BASE24 DDL (más release) | a mezőpozíciókat a saját DDL alapján kell igazítani |
| Minden „Interchange” döntés | a host interchange interfészen kapcsolódik | normális; az „Engedélyező” tábla mutatja az `AUTHORIZER` nevet |
| Időpontok eltolva | – | a program a rendszer helyi idejét használja (`CONVERTTIMESTAMP`) |

## 8. Kapcsolódó programok

| Program | Leírás |
|---|---|
| `TLFRPT` | ugyanez a BASE24-atm TLF-re (`TLyymmdd`), saját ATM-ek listájával |
| `EMSERR` / `EMSSCAN` | hibakeresés az EMS logban |
| `TCPRATE` | TCP-portok forgalma a CLIM-eken |
| `napi.tacl` | `DAILYRPT` (napi riportok) és `DAILYLOOP` (háttérfutás) |
