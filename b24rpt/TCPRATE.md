# TCPRATE – TCP-portok forgalmának mérése a CLIM-eken

## 1. Cél

A TCPRATE megmutatja, hány üzenet ment át egy adott TCP-porton (pl. HSM: 7000, ATM: 4275) egy mérési időszak alatt, hány kapcsolat él, hogyan oszlik meg a forgalom a túloldali gépek között, és van-e jele hálózati vagy alkalmazás-oldali problémának.

Két módon használható:

| Mód | Mire | Hogyan |
|---|---|---|
| **Kézi mérés** | „Mennyi forgalom megy most a 7000-es porton?” | `TCPRATE 7000 5` TACL-ből |
| **Folyamatos naplózás** | „Mennyi volt tegnap óránként?” | a `DAILYLOOP` 5 percenként mér, a `DAILYRPT` reggel órás riportot készít |

## 2. Működési elv

### 2.1 Honnan jön az adat

NonStop X rendszeren a TCP/IP protokollkezelés a CLIM-eken (Linux) fut. Az alkalmazások (BASE24) kapcsolatai a CLIM-en, egy-egy **hálózati névtérben** (network namespace, pl. `NMLNS005`, `NMLNS006`) láthatók. A TCPRATE ezeket a Linux `ss -tin` paranccsal kérdezi le, a `climcmd` segítségével:

```
climcmd <clim> ip netns exec <névtér> ss -tin dport = :<port> or sport = :<port>
```

- `-t`: csak TCP; `-i`: kapcsolatonkénti részletes számlálók; `-n`: IP-címek és portok névfeloldás nélkül.
- A szűrő mindkét irányt nézi (`dport` és `sport`), így mindegy, hogy a NonStop a kliens (pl. HSM) vagy a szerver (pl. ATM-koncentrátor).
- A névtér nélküli `ss` csak a CLIM menedzsment-kapcsolatait mutatja (SSH a 22-es porton), az alkalmazás-forgalmat nem.

### 2.2 Mit számol

Az `ss -tin` kapcsolatonként a **kapcsolat felépülése óta összesített** számlálókat adja. A TCPRATE ezek közül ezeket használja:

| Számláló | Jelentés |
|---|---|
| `data_segs_in` | adatot tartalmazó beérkezett TCP-szegmensek (a NonStop felé) |
| `data_segs_out` | adatot tartalmazó elküldött TCP-szegmensek (a NonStopról) |
| `bytes_received` / `bytes_sent` | bájtok |
| `bytes_retrans` | újraküldött bájtok |
| `rtt`, `minrtt` | oda-vissza idő (ms) |
| `lastrcv` | ms az utolsó fogadás óta |
| `Send-Q` | elküldött, de még nem nyugtázott bájtok |

Mivel a számlálók összesítettek, **egy lekérdezésből nem lehet sebességet számolni**. A TCPRATE ezért két pillanatképet készít a mérési idő különbséggel, és a kettő különbségét számolja:

```
üzenetek a mérés alatt  =  data_segs_in(2. kép)  − data_segs_in(1. kép)
üzenet / mp            =  különbség / mérési idő (mp)
```

**Üzenet ≈ szegmens:** kis, kérés–válasz jellegű üzeneteknél (HSM-parancs, ATM-üzenet, néhány száz bájt) egy alkalmazás-üzenet általában egy TCP-szegmens. Nagy üzenet több szegmensre is szétoszolhat, illetve több kis üzenet kerülhet egy szegmensbe, ezért ez közelítő érték.

### 2.3 Korlátok

- **Visszamenőleg nem mér.** A CLIM nem tárol előzményt; a kézi mérés *most indul* és a megadott ideig tart. Előzményt a `DAILYLOOP` folyamatos naplózása ad (lásd 6. fejezet).
- **Megszűnt kapcsolat** forgalma a mérés alatt nem számolható (a második pillanatképben már nincs meg). Jelzés: `MSZ`.
- **Közben létrejött kapcsolat** teljes forgalma beleszámít (a mérés alatt keletkezett). Jelzés: `UJ`.
- **Visszaugró számláló** (pl. ugyanazzal a porttal újraépült kapcsolat) 0-nak számít, hogy ne jöjjön ki negatív vagy értelmetlen érték.

## 3. Felépítés

```
TCPRATE makró (TACL)                         TCPRATE program (C)
──────────────────────                       ───────────────────
1. CLIM-ek névtereinek felderítése
   (climcmd … ip netns list)
2. 1. pillanatkép: CLIM/névtér páronként
   climcmd … ss -tin  →  munkafájl CT1kk
3. várakozás (mérési idő)
4. 2. pillanatkép  →  munkafájl CT2kk
5. vezérlő adatok (@@T, @@D, @@S, @@F)  ──►  munkafájlok beolvasása,
   átadása:  RUN TCPRATE /INV …/                 kapcsolatok párosítása,
                                                 különbségek, riport
```

### 3.1 Fájlok

| Fájl | Tartalom |
|---|---|
| `tcprate.tacl` | TACL makrókönyvtár: `TCPRATE` (fő makró), `CLIMV`, `CLIMF` (segédmakrók) |
| `tcprate.c` | C program (`TCPRATE`), három üzemmód: egyszeri mérés, naplósor írása (L), napi riport (D) |
| `napi.tacl` | a napi automatizmus: `DAILYLOOP` 5 perces mérés, `DAILYRPT` órás riport |

### 3.2 Segédmakrók

| Makró | Feladat |
|---|---|
| `CLIMV változó clim parancs…` | rövid `climcmd` kimenet (pl. névtérlista) beolvasása TACL-változóba (munkafájl + `FUP COPY`) |
| `CLIMF fájl clim parancs…` | `climcmd` kimenet munkafájlba; ezt a TCPRATE program olvassa be közvetlenül |

**Miért nem közvetlenül változóba?**
- A `climcmd` nem tud TACL-változóba írni (`/OUTV/`): *„Error: Filename must be specified with OUT run-option.”* Csak fájlnévvel megadott OUT-ba ír.
- A `FUP COPY` a hosszú rekordok végét levágja, ezért az `ss -tin` hosszú soraihoz nem használható (a levágás miatt kulcsok és értékek vesztek el). A hosszú kimenetet ezért a C program olvassa be a fájlból, teljes rekordhosszal.

### 3.3 A munkafájl formátuma

A `climcmd` a hosszú sorokat fix hosszú rekordokra vágja, akár szó vagy szám közepén is (pl. `…da` | `ta_segs_out:2967122`), és a rekordvégi szóközök elvesznek. A TCPRATE program ezért:

- új kapcsolatnak veszi az állapot-szóval (`ESTAB`, `TIME-WAIT` stb.) kezdődő sort;
- a nem szóközzel kezdődő sort az előző rekord folytatásának tekinti, és elválasztó nélkül hozzáragasztja;
- a kulcsokat (`data_segs_in:` stb.) szövegkereséssel keresi a kapcsolat összefűzött szövegében.

## 4. Telepítés

1. **Program fordítása:** a `tcprate.c` feltöltése EDIT fájlként (pl. `TCPRATEC`), majd fordítás a szokásos módon (pl. `CCOMP /IN TCPRATEC/ TCPRATE; RUNNABLE, SYSTYPE GUARDIAN`).
2. **Makró feltöltése:** a `tcprate.tacl` EDIT fájlként (pl. `TCPRMAC`).
3. **Beállítások** a makró elején (BEÁLLÍTÁSOK rész):

| Változó | Példa | Jelentés |
|---|---|---|
| `clims` | `nclim000 nclim001` | a CLIM-ek neve, szóközzel elválasztva |
| `prog` | `$DSMSCM.GINO.TCPRATE` | a lefordított program |
| `climv^tmp` | `$DSMSCM.GINO.CLIMTMP` | munkafájl a rövid `climcmd` kimenetekhez |
| `climv^sv` | `$DSMSCM.GINO` | alkötet a `CTpkk` munkafájloknak |

4. **Ellenőrzés:** a riport utolsó sorában a programverzió látszik, pl. `(TCPRATE 2026-11-16 v6 (munkafajl))`. Ha hiányzik vagy régebbi, nem a legutóbb fordított program fut.

## 5. Kézi mérés

### 5.1 Használat

```
LOAD /KEEP 1/ $DSMSCM.GINO.TCPRMAC
TCPRATE port [perc] [névtér] [R]
```

| Paraméter | Alapérték | Jelentés |
|---|---|---|
| `port` | – (kötelező) | a figyelt port |
| `perc` | 5 | mérési idő percben; a makró ennyit vár |
| `névtér` | `*` | adott névtér (pl. `NMLNS006`), vagy `*` = a CLIM-ek összes névtere |
| `R` | – | részletes kimenet kapcsolatonként |

Példák:

```
TCPRATE 7000 1               == HSM, 1 perc
TCPRATE 4275                 == ATM, 5 perc
TCPRATE 4275 10 * R          == ATM, 10 perc, részletesen
TCPRATE 7000 5 NMLNS006      == HSM, 5 perc, adott névtér
```

### 5.2 Rövid kimenet (alapértelmezés)

```
===============================================================================
Port 7000 forgalma: 2026.11.16 15:53:47 - 15:54:55 (1,1 perc)
===============================================================================
Beerkezo uzenet (a NonStop fele):        1 082   (15,7 / mp)
Kimeno uzenet (a NonStoprol):            1 082   (15,7 / mp)
Kapcsolatok szama:                          16
Megoszlas: 172.16.37.17 51%, 172.16.37.18 49%
Allapot: rendben
(TCPRATE 2026-11-16 v6 (munkafajl))
```

| Sor | Jelentés |
|---|---|
| Beérkező / Kimenő üzenet | adatszegmensek száma a mérés alatt és másodpercenként |
| Kapcsolatok száma | élő kapcsolatok a porton (az összes CLIM-en és névtérben) |
| Megoszlás | a forgalom (be + ki) aránya túloldali IP-címenként |
| Állapot | `rendben`, vagy figyelmeztető sorok (lásd 5.4) |

### 5.3 Részletes kimenet (`R`)

Kapcsolatonként:

| Oszlop | Jelentés |
|---|---|
| Tuloldal | túloldali IP:port |
| Be/mp, Ki/mp | adatszegmens/mp a mérés alatt |
| rtt, minrtt | átlagos és legkisebb oda-vissza idő (ms) |
| SendQ | nyugtázatlan bájtok a mérés végén |
| Utolso | mp az utolsó fogadás óta |
| Retr | a mérés alatt újraküldött bájtok |
| Megj. | jelzések (lásd 5.4) |

Ezen felül túloldali IP-cím szerinti összesítés (kapcsolatok, üzenetek, KB).

### 5.4 Jelzések

| Kód | Rövid kimenetben | Jelentés | Mire utal |
|---|---|---|---|
| `SQ` | FIGYELEM | Send-Q > 0: nyugtázatlan adat áll sorban | a túloldal lassú vagy nem fogad |
| `CS` | FIGYELEM | 60 mp-nél régebben jött adat | néma kapcsolat, a túloldal nem válaszol vagy nem kap forgalmat |
| `RT` | Megjegyzés | újraküldés volt | csomagvesztés a hálózaton |
| `UJ` | Megjegyzés | a kapcsolat a mérés alatt jött létre | újracsatlakozás |
| `MSZ` | Megjegyzés | a kapcsolat a mérés alatt megszűnt | kapcsolatbontás, újraindítás |

## 6. Folyamatos naplózás és órás riport (`napi.tacl`)

### 6.1 5 percenkénti mérés (`DAILYLOOP`)

A háttérben futó `DAILYLOOP` (process `$NAPI`) 5 percenként:

1. CLIM/névtér páronként **egyetlen** `climcmd`-dal lekérdezi az összes figyelt port kapcsolatait;
2. az előző méréshez képest kiszámolja az 5 perc forgalmát (`TCPRATE L` üzemmód);
3. portonként egy sort ír a napi naplóba: `<napi^tclog>.TCyymmdd` (entry-sequenced).

Két munkafájl-készlet váltakozik (`CTAkk` és `CTBkk`), hogy az előző mérés fájljai a következő mérésig megmaradjanak. A névtereket induláskor és naponta egyszer felderíti.

Beállítások a `napi^cfg` részben:

| Változó | Példa | Jelentés |
|---|---|---|
| `napi^ports` | `4275:ATM 7000:HSM` | figyelt portok `port:név` alakban; üres = nincs mérés |
| `napi^clims` | `nclim000 nclim001` | CLIM-ek |
| `napi^tclog` | `$DATA.EMSRPT` | a `TCyymmdd` naplók és a `CTA/CTB` munkafájlok helye |
| `climv^tmp` | `$DATA.EMSRPT.CLIMTMP` | munkafájl a rövid `climcmd` kimenetekhez |

### 6.2 Naplósor formátuma

```
2026-11-15 17:05:00 4275 ATM 300 18600 13950 12 0 0 0 0 0
```

| Mező | Jelentés |
|---|---|
| dátum, idő | a mérés vége (helyi idő) |
| port, név | a figyelt port és neve |
| mp | a két mérés közti idő |
| be, ki | adatszegmensek a két mérés között |
| kapcs | kapcsolatok száma |
| sq, cs, rt | kapcsolatok száma Send-Q > 0 / 60 mp-nél régebbi fogadás / újraküldés esetén |
| megsz, uj | megszűnt / új kapcsolatok |

### 6.3 Órás riport (`DAILYRPT`, `TCPRATE D`)

A reggeli `DAILYRPT` a tegnapi naplóból riportot készít (`$S.#B24NAPI.TCP`), portonként:

```
Port 4275 (ATM)
Ora        Be (db)    Ki (db)   Be/mp  Csucs Be/mp Kapcs  Figy Meres
-------------------------------------------------------------------------------
16-17      222 582    166 926    61,8         64,0     2     -    12
17-18      224 276    168 198    62,3         64,0     2     -    12
...
Osszesen: 2 243 478 be, 1 682 450 ki, csucs 64,0 be/mp (17-18 ora)
```

| Oszlop | Jelentés |
|---|---|
| Be/Ki (db) | üzenetek az órában |
| Be/mp | óraátlag |
| Csúcs Be/mp | az óra legforgalmasabb 5 percének átlaga |
| Kapcs | legtöbb kapcsolat az órában |
| Figy | hány mérésnél volt SQ, CS vagy megszűnt kapcsolat |
| Mérés | 5 perces mérések száma az órában (teljes óra: 12) |

Ha egy órában kevesebb mint 12 mérés van, a `DAILYLOOP` nem futott; a kimaradás utáni első mérés a kimaradt időszak teljes forgalmát az aktuális órához írja.

## 7. Hibaelhárítás

| Tünet | Ok | Teendő |
|---|---|---|
| `Error: Filename must be specified with OUT run-option.` | a `climcmd` nem tud változóba írni | a `CLIMV` / `CLIMF` makrót kell használni (benne van a jelenlegi makróban) |
| `FIGYELEM: nincs kapcsolat ezen a porton!` | rossz port, vagy a kapcsolatok más CLIM-en / névtérben vannak | `climcmd <clim> ip netns list`, majd kézzel: `climcmd <clim> ip netns exec <ns> ss -tin dport = :<port> or sport = :<port>` |
| Kimenő = 0, irreális bejövő érték | régi programverzió (a hosszú rekordok rossz beolvasása) | a riport utolsó sorában a verzió legyen legalább `v6` |
| Hiányzó verziósor | régi program fut | `FI TCP*`: a forrás és a program módosítási ideje friss-e; újrafordítás |
| Egy túloldali cím 0%-on áll | a cím felé nincs forgalom (pl. kiesett HSM) | `TCPRATE <port> 5 * R`: élnek-e a kapcsolatai, van-e `CS` jelzés |

## 8. Példa: HSM-kiesés (2026. november 16.)

A 172.16.37.17 HSM vasárnap kiesett, hétfő reggel újraindították.

- Az újraindítás előtt a teljes HSM-forgalmat a .18 vitte; kapcsolatai vasárnap óta ~2,97 millió kérést és választ számoltak.
- Az újraindítás után a .17 kapcsolatai újraépültek (kis összesített számlálók).
- A mérés délután: `Megoszlas: 172.16.37.17 51%, 172.16.37.18 49%`, bejövő = kimenő = 1 082 / 1,1 perc. A BASE24 mindkét HSM-et egyformán használja, minden kérésre jött válasz.

Az 5 perces naplózás mellett az ilyen kiesés az órás riportban úgy látszik, hogy az egyik túloldali cím forgalma 0-ra esik, a másiké a duplájára nő.

## 9. Verziótörténet

| Verzió | Változás |
|---|---|
| v1 | kapcsolatonkénti részletes riport |
| v2 | rövid összefoglaló alapból, részletek `R`-rel; perc alapú mérési idő |
| v3 | `L` (naplósor) és `D` (napi riport) üzemmód a `DAILYLOOP`-hoz |
| v4 | a `climcmd` kimenete munkafájlon keresztül (`OUTV` nem támogatott) |
| v5 | tördelés-tűrő beolvasás (szó közepén vágott rekordok) |
| v6 | a program maga olvassa a munkafájlokat (`@@F`), mert a `FUP COPY` levágta a hosszú rekordokat |
