# HP NonStop alapok – fájlok, processzek, spooler

*Bevezető új kollégáknak. Előismeret nem kell. A második rész (BASE24: PTLF, TLF, napváltás, saját eszközök) külön cikkben lesz.*

---

## 0. Mielőtt bármit begépelsz – biztonsági szabályok

A NonStop éles rendszeren a banki tranzakciók valós időben futnak. Néhány parancs visszavonhatatlan.

| Szabály | Miért |
|---|---|
| **Éles BASE24-fájlokat (PTLF, TLF, CAF, PBF stb.) soha ne írj, ne törölj, ne nevezz át.** | Leállhat a kártyaforgalom. Olvasni (pl. `FILEINFO`, `FUP INFO`) szabad. |
| **Gyakorolni csak a saját gyakorló alkötetedben** (`$HD1.GYAK`) szabad. | Ott nem ronthatsz el semmit. |
| **`STOP` csak olyan processzre, amit te indítottál.** | Egy rendszer- vagy BASE24-processz leállítása üzemzavart okoz. |
| **A `!` a `FUP PURGE` végén megerősítés nélkül töröl.** | Kezdőként mindig `!` nélkül használd, és olvasd el, mit kérdez. |
| **Ha bizonytalan vagy, kérdezz, mielőtt Entert nyomsz.** | |

---

## 1. Alapfogalmak

### 1.1 A rendszer felépítése
- **Node (rendszer):** egy NonStop gép, neve `\` jellel kezdődik, pl. `\EH06`.
- **CPU:** a rendszerben több processzor van (0, 1, 2, …). Ha az egyik kiesik, a többi átveszi a munkát – ezért „NonStop”.
- **Volume (kötet):** egy logikai lemez, neve `$` jellel kezdődik, pl. `$HD1`, `$SYSTEM`.
- **Subvolume (alkötet):** a kötet „mappája”, pl. `GYAK`. Csak egy szint van – alkötetben nincs további alkötet.
- **Fájl:** az alkötetben, pl. `MINTA`.

### 1.2 Fájlnév
```
\EH06.$HD1.GYAK.MINTA
  │      │    │    └── fájl      (max. 8 karakter, betűvel kezdődik)
  │      │    └─────── alkötet   (max. 8 karakter, betűvel kezdődik)
  │      └──────────── kötet     ($ jellel kezdődik)
  └─────────────────── node      (\ jellel; a saját node-on elhagyható)
```
- Kis- és nagybetű mindegy (`minta` = `MINTA`).
- Ha beállítottad az alapértelmezett kötetet és alkötetet (lásd 2.2), elég a rövid név: `MINTA`.

### 1.3 Fájlkódok (CODE)
A fájlkód megmondja, mi van a fájlban:

| Kód | Jelentés |
|---|---|
| 101 | EDIT fájl (szöveg: forrásprogram, makró, jegyzet) |
| 500 / 800 / 700 | futtatható program (x86 / Itanium / MIPS rendszeren) |
| 0 | adatfájl (általános) |

### 1.4 Fájltípusok
| Típus | FUP-ban | Jellemző |
|---|---|---|
| Unstructured | `TYPE U` | bájtfolyam, nincs rekordszerkezet (EDIT fájl is ilyen alapon) |
| Entry-sequenced | `TYPE E` | rekordok érkezési sorrendben, a végére íródik (pl. PTLF, TLF naplók) |
| Key-sequenced | `TYPE K` | rekordok kulcs szerint rendezve, kulccsal kereshető (pl. CAF, PBF) |
| Relative | `TYPE R` | rekordok sorszám szerint |

### 1.5 Processz
- A **processz** egy futó program. Lehet neve (`$` jellel, pl. `$NAPI`) vagy név nélküli.
- **Processzpár:** sok rendszer- és BASE24-processz két példányban fut két külön CPU-n: **primary** (dolgozik) és **backup** (készenlétben). Ha a primary CPU kiesik, a backup azonnal átveszi.
- Minden processzt a CPU száma és egy sorszám (PIN) azonosít, pl. `3,210`.

---

## 2. A TACL – a parancssor

A **TACL** a NonStop parancsértelmezője (mint Windows-on a cmd, Linuxon a bash).

### 2.1 Belépés, kilépés, ki vagyok
```
LOGON csoport.felhasznalo      == belépés (jelszót kér)
WHO                            == ki vagyok, mi az alapértelmezett kötet/alkötet
LOGOFF                         == kilépés
```
A TACL promptja megmutatja az aktuális kötetet/alkötetet és a parancs sorszámát, pl. `$HD1 GYAK 12>`.

### 2.2 Alapértelmezett hely
```
VOLUME $HD1.GYAK               == ide „állsz be”; utána elég a rövid fájlnév
```

### 2.3 Hasznos TACL-trükkök
| Parancs | Jelentés |
|---|---|
| `HISTORY` | az utolsó parancsok listája |
| `FC` | az utolsó parancs javítása és újrafuttatása |
| `FC 12` / `FC fup` | a 12. parancs / a legutóbbi „fup…” kezdetű parancs |
| `== szöveg` | megjegyzés (a sor végéig) |

---

## 3. Fájlműveletek

### 3.1 Listázás, információ (csak olvas – mindig biztonságos)
```
FILES                          == az aktuális alkötet fájljai
FILES $HD1.GYAK               == egy adott alkötet fájljai
FILEINFO                       == fájlok kóddal, mérettel (EOF), dátummal, tulajdonossal
FILEINFO MINTA                 == egy fájlé
FILEINFO PT*                   == "PT"-vel kezdődő fájlok (* = bármi)
FUP INFO MINTA, DETAIL         == minden részlet: típus, rekordhossz, blokkméret, kulcsok…
```
A `FILEINFO` oszlopai: **CODE** (fájlkód), **EOF** (méret bájtban), **LAST MODIFIED** (utolsó módosítás), **OWNER** (tulajdonos), **RWEP** (jogosultság, lásd 3.4).

### 3.2 FUP – File Utility Program
A `FUP` a fájlkezelés fő eszköze. Egy parancs: `FUP <parancs>`; vagy belépsz (`FUP`), több parancsot adsz, majd `EXIT`.

| Parancs | Mit csinál | Példa |
|---|---|---|
| `INFO` | információ | `FUP INFO MINTA, DETAIL` |
| `CREATE` | új fájl létrehozása | `FUP CREATE NAPLO, TYPE E, REC 200` |
| `DUP` | fájl másolása (új fájl jön létre) | `FUP DUP MINTA, MINTA2` |
| `COPY` | rekordok másolása; cél nélkül a képernyőre listáz | `FUP COPY MINTA` |
| `RENAME` | átnevezés | `FUP RENAME MINTA2, MINTA3` |
| `PURGE` | **törlés** (megerősítést kér) | `FUP PURGE MINTA3` |
| `SECURE` | jogosultság beállítása | `FUP SECURE NAPLO, "NUNU"` |

### 3.3 Szöveges (EDIT) fájl szerkesztése
```
TEDIT JEGYZET                  == teljes képernyős szerkesztő (ha nincs ilyen fájl, létrehozza)
EDIT JEGYZET                   == soronkénti szerkesztő (régebbi)
```

### 3.4 Jogosultság (RWEP)
A `FILEINFO` RWEP oszlopa négy betű: **R**ead (olvasás), **W**rite (írás), **E**xecute (futtatás), **P**urge (törlés). Mindegyik helyen egy betű mondja meg, *ki* teheti meg:

| Betű | Ki |
|---|---|
| `N` | bárki a hálózatban |
| `A` | bárki ezen a node-on |
| `G` / `C` | a tulajdonos csoportja (helyi / hálózati) |
| `O` / `U` | csak a tulajdonos (helyi / hálózati) |
| `-` | csak a super ID |

Pl. `NUNU`: bárki olvashatja és futtathatja, de írni és törölni csak a tulajdonos tudja. Ezen felül a **Safeguard** további szabályokat adhat – ha valamire „security violation” hibát kapsz, az nem a te hibád, szólj.

---

## 4. Processzek

### 4.1 Processzek megtekintése (csak olvas – biztonságos)
```
STATUS *, TERM                 == a terminálodról indított processzek
STATUS *, USER                 == a felhasználód összes processze
STATUS $NAPI                   == egy név szerinti processz
STATUS $NAPI, DETAIL           == részletesen (program, CPU, prioritás…)
PPD $NAPI                      == processzpár: primary és backup CPU,PIN
```

### 4.2 Program futtatása
```
RUN PROG paraméterek                           == futtatás, megvárja a végét
PROG paraméterek                               == ugyanaz (a RUN elhagyható)
RUN PROG /OUT $S.#LISTA/ paraméterek           == kimenet a spoolerbe
RUN PROG /NAME $GY01, NOWAIT/ paraméterek      == háttérben, $GY01 néven
```
A `/…/` közötti rész a **futtatási opciók**: `NAME` (processznév), `NOWAIT` (ne várja meg), `OUT` (kimenet helye), `IN` (bemenet), `CPU n` (melyik CPU-n).

### 4.3 Processz leállítása
```
STOP $GY01                     == CSAK a saját processzedet!
```
Előtte mindig nézd meg, kié és mi az: `STATUS $GY01, DETAIL`.

---

## 5. Spooler és PERUSE

A **spooler** a nyomtatási sor: a programok ide írhatják a kimenetüket, ahonnan később megnézhető vagy kinyomtatható. A `/OUT …/` opciót programok kapják (pl. `FUP`, saját programok); a TACL beépített parancsai (pl. `FILEINFO`) nem.

```
RUN PROG /OUT $S.#GYAK/        == a kimenet a spoolerbe, a #GYAK csoportba
PERUSE                         == a spooler-jobok böngészője
```
PERUSE-ban:

| Parancs | Mit csinál |
|---|---|
| `JOB` | a jobjaid listája |
| `JOB 1234` | egy job kiválasztása |
| `LIST` | a kiválasztott job tartalma (lapozás: Enter) |
| `DEL` | a kiválasztott job törlése (csak a sajátodat!) |
| `EXIT` | kilépés |

---

## 6. Hibaüzenetek – mit jelentenek

A NonStop a hibákat számmal jelzi (`FILE SYSTEM ERROR 11`). A leggyakoribbak:

| Hiba | Jelentés | Teendő |
|---|---|---|
| 11 | nincs ilyen fájl / rekord | elírtad a nevet, vagy rossz alkötetben vagy (`WHO`) |
| 10 | a fájl már létezik | más nevet adj, vagy előbb nézd meg, mi az |
| 12 | a fájlt más használja | várj, vagy kérdezd meg, ki használja |
| 14 | nincs ilyen eszköz / processz | elírtad a `$` nevet |
| 48 | security violation | nincs jogosultságod – szólj |

Bármilyen hibaszám jelentése: `ERROR 48` (TACL-ban).

---

## 7. Gyakorló feladatok

### 7.0 A homokozó
A feladatokat a **homokozóban** (gyakorló alkötetben) végzed: `\EH06.$HD1.GYAK`. A homokozót és az ellenőrzést a `GYAK` makrók kezelik:

```
LOAD /KEEP 1/ $HD1.OKTATAS.GYAK      == a makrók betöltése (minden belépés után)
GYAKINIT                             == homokozó létrehozása: $HD1.GYAK
GYAKCHK                              == hol tartok? (feladatonként OK / MEG NEM)
GYAKTOROL                            == a homokozó törlése (újrakezdéshez)
```
A homokozóban induláskor három fájl van: **MINTA** (szöveges, EDIT), **ADAT** (entry-sequenced), **KULCS** (key-sequenced).

A `GYAKCHK` kimenete például:
```
 1. OK        Beallas a homokozoba
 2. ONELLEN.  Listazas - a 101-es kodu (szoveges) fajl: MINTA
 4. MEG NEM   NAPLO letrehozasa (nincs NAPLO fajl)
 ...
Eredmeny: 3 / 6 automatikusan ellenorzott feladat kesz
```
Az „ONELLEN.” (önellenőrzés) feladatok nem hagynak nyomot a rendszerben; ezeknél a sor a helyes választ mutatja.

### 7.1 Feladatok

1. **Hol vagyok?** Nézd meg, ki vagy és mi az alapértelmezett helyed, majd állj be a homokozóba.
2. **Mi van itt?** Listázd ki a homokozó fájljait kóddal és mérettel. Melyik a szöveges (EDIT) fájl?
3. **Részletek.** Kérd le az `ADAT` és a `KULCS` fájl összes adatát. Milyen típusúak, mekkora a rekordhossz?
4. **Új fájl.** Hozz létre egy entry-sequenced fájlt `NAPLO` néven, 200 bájtos rekordokkal. Ellenőrizd.
5. **Szöveg.** Hozz létre egy `JEGYZET` nevű szöveges fájlt, írj bele két sort, mentsd el, és listázd ki a tartalmát.
6. **Másolás, átnevezés, törlés.** Másold le a `JEGYZET`-et `JEGYZET2` néven, a `JEGYZET2`-t nevezd át `JEGYZ3`-ra, végül töröld az `ADAT` fájlt (megerősítéssel!).
7. **Jogosultság.** Állítsd be a `NAPLO`-t úgy, hogy ezen a node-on bárki olvashassa (`A`), de írni, futtatni és törölni csak te tudd (`O`). Ellenőrizd.
8. **Processzek.** Nézd meg, milyen processzeid futnak a terminálodról.
9. **Spooler.** Küldd a homokozó fájllistáját a spoolerbe, keresd meg a PERUSE-ban, nézd meg, majd töröld a jobot.
10. **Hibaüzenet.** Kérd le egy nem létező fájl adatait. Milyen hibaszámot kaptál, és mit jelent?

Időnként futtasd a `GYAKCHK`-t, és nézd meg, hol tartasz.

### Megoldások

*(Előbb próbáld meg magad!)*

```
1.  WHO
    VOLUME $HD1.GYAK
2.  FILEINFO                                   == az EDIT fájl kódja 101
3.  FUP INFO ADAT, DETAIL
    FUP INFO KULCS, DETAIL
4.  FUP CREATE NAPLO, TYPE E, REC 200
    FUP INFO NAPLO, DETAIL
5.  TEDIT JEGYZET                              == írj, mentés és kilépés
    FUP COPY JEGYZET                           == a tartalom a képernyőre
6.  FUP DUP JEGYZET, JEGYZET2
    FUP RENAME JEGYZET2, JEGYZ3
    FUP PURGE ADAT                             == megerősítés: Y
7.  FUP SECURE NAPLO, "AOOO"
    FILEINFO NAPLO                             == RWEP oszlop: AOOO
8.  STATUS *, TERM
9.  FUP /OUT $S.#GYAK/ INFO *               == a FUP egy program, ezért kaphat /OUT/-ot
    PERUSE
      JOB                                      == keresd meg a #GYAK jobot
      JOB <szám>
      LIST
      DEL
      EXIT
10. FILEINFO NINCSILYEN                         == ERROR 11
    ERROR 11
```

---

## 8. Következő lépések

- **2. rész:** BASE24 a NonStopon – PTLF és TLF naplók, napváltás (20:30), hol vannak a fájlok, és a saját eszközeink (`PTLFRPT`, `TLFRPT`, `TRNLIST`, `TCPRATE`, `EMSERR`).
- Kérdés esetén: a mentorod vagy a csapat.
