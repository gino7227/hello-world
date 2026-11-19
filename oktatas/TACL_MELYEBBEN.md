# TACL mélyebben – változók, MACRO, ROUTINE, kiértékelés, hibakezelés

*Azoknak, akik már írnak TACL makrókat, de szeretnék érteni, mi történik a háttérben.*

---

## 1. Az alapgondolat: a TACL-ben minden változó

A TACL-ben nincs külön „program” és „adat”: **minden változó**, és a változóknak **típusa** van. Amikor beírsz egy szót a promptba, a TACL megnézi, van-e ilyen nevű változó – ha MACRO vagy ROUTINE, végrehajtja; ha nincs, programként próbálja futtatni (`RUN`).

| Típus | Mi van benne | Mire való |
|---|---|---|
| **TEXT** | szöveg | adat, beállítás (`#SET x 5`) |
| **MACRO** | TACL-szöveg, amelyben `%1%`-szerű helyek vannak | egyszerű parancssorozat, „rövidítés” |
| **ROUTINE** | TACL-szöveg, saját argumentum-feldolgozással | igazi „függvény”: ellenőrzött paraméterek, visszatérési érték, hibakezelés |
| **ALIAS** | egy másik parancs neve | átnevezés, rövidítés (`#DEF ll ALIAS |BODY| FILEINFO`) |
| **DIRECTORY** | további változók | névtér (mint egy mappa) |
| **STRUCT** | bináris rekordszerkezet | rendszerhívások paramétereihez |
| **DELTA** | #DELTA szerkesztő-parancsok | szövegfeldolgozás (ritka) |

Egy változó típusát így nézheted meg: `#VARIABLEINFO /TYPE/ nev` (ha a rendszeretek TACL-verziója támogatja), vagy egyszerűen: `#DEF` listázza a saját definícióidat a `VARIABLES` paranccsal.

---

## 2. Hogyan jön létre egy változó?

### 2.1 Közvetlenül

```
#PUSH x                      == új (üres) TEXT változó
#SET x hello                 == érték
#DEF koszon MACRO |BODY| #OUTPUT Szia, %1%!
```

### 2.2 Könyvtárfájlból (`?SECTION` + `LOAD`) – ez a megszokott út

Egy EDIT fájlban több definíció lehet, mindegyik egy `?SECTION`-nel kezdődik:

```
?TACL MACRO
?SECTION koszon MACRO
#OUTPUT Szia, %1%!

?SECTION masol ROUTINE
... (lásd 6. fejezet)
```
Betöltés: `LOAD /KEEP 1/ $HD1.SAJAT.LIB`

- A `LOAD` minden szekcióból egy változót csinál a szekció nevével és típusával.
- A **`/KEEP 1/`** azt jelenti: ha a változó már létezik, csak 1 szintet tartson meg belőle – vagyis az új definíció **lecseréli** a régit, nem halmozódik. `KEEP` nélkül minden `LOAD` újabb szintet tenne a régire.
- A `?TACL MACRO` sor a fájl elején csak annyit jelent, hogy a fájl önállóan is futtatható (a fájl nevét beírva) – könyvtárként betöltve nincs szerepe.

---

## 3. Változószintek: `#PUSH`, `#POP`, `#FRAME`, `#UNFRAME`

Ez a TACL egyik legfontosabb – és legkevésbé ismert – tulajdonsága.

### 3.1 Szintek
Egy változónak **több szintje** lehet, mint egy veremnek:

```
#PUSH x          == x új szintje (üres)
#SET x 1
#PUSH x          == újabb szint
#SET x 2
#OUTPUT [x]      == 2   (mindig a legfelső szint látszik)
#POP x           == a felső szint eldobva
#OUTPUT [x]      == 1
```
A `#SET` mindig a **legfelső** szintet módosítja; a `#PUSH` újat tesz rá; a `#POP` leveszi.

### 3.2 Keret (`#FRAME` / `#UNFRAME`)
```
?SECTION pelda MACRO
#FRAME
#PUSH a b c
... munka ...
#UNFRAME
```
- A `#FRAME` megjegyzi a pillanatnyi állapotot.
- Az `#UNFRAME` **minden** `#PUSH`-t visszavon, ami a `#FRAME` óta történt – így a makró „helyi változói” eltűnnek, és nem írnak felül semmit, ami a hívónál ugyanilyen néven létezik.
- **Szabály:** minden saját makró/rutin `#FRAME` – `#PUSH` a használt változókra – munka – `#UNFRAME`.

### 3.3 Láthatóság: a hívott makró látja a hívó változóit
A TACL-ben **dinamikus** a láthatóság: ha az `A` makró `#PUSH`-ol egy `x`-et, és meghívja a `B` makrót, akkor `B` látja és módosíthatja `A` `x`-ét.

- **Előny:** egy segédmakró (pl. egy „beállítások” szekció) közvetlenül beállíthatja a hívó változóit – a `#SET`-ek a hívó szintjeire mennek.
- **Veszély:** ha `B` ugyanolyan nevű változót használ `#PUSH` nélkül, akkor `A` változóját írja felül. Ezért: **amit egy makró csak magának használ, azt `#PUSH`-olja**, és adj egyedi, előtagos neveket (`napi^ido`, `gyak^sv`) – a `^` megengedett a nevekben.

### 3.4 Könyvtárak (DIRECTORY)
A változók névterekben (könyvtárakban) élnek, pl. `:UTILS:TACL` alatt vannak a rendszer makrói. A saját változóid az alapértelmezett „otthoni” könyvtáradba kerülnek. A legtöbb makróíráshoz ezzel nem kell foglalkozni – csak ha két könyvtárban ugyanaz a név szerepel.

---

## 4. Kiértékelés: a szögletes zárójel

### 4.1 `[ … ]` = „számold ki, és tedd a helyére”
```
#SET x 5
#OUTPUT x            == kiírja: x
#OUTPUT [x]          == kiírja: 5
#OUTPUT [#COMPUTE x * 2]    == 10
```
- `[valtozo]` – a változó (legfelső szintjének) tartalma.
- `[#fuggveny …]` – a beépített függvény eredménye.
- `[rutin …]` – egy ROUTINE eredménye (lásd 6.3).
- A zárójelek egymásba ágyazhatók: `[#FILEINFO /EOF/ [f]]`.

### 4.2 Mikor nem kell zárójel?
A **kifejezésekben** (`#IF`, `#COMPUTE`, `#LOOP |WHILE|`) a változónevet zárójel nélkül is a tartalmával helyettesíti:
```
[#IF x > 3 |THEN| ...]                  == x itt a tartalma
#SET y [#COMPUTE x + 1]
```
Függvényhívás viszont kifejezésben is zárójel: `[#IF [#EMPTYV x] |THEN| ...]`.

### 4.3 Eredmény „lenyelése”: `SINK`
Ha egy függvény eredményt ad, de nincs rá szükséged, a `SINK` eldobja – különben a TACL parancsként próbálná végrehajtani az eredményt:
```
SINK [#SETMANY a b , [szoveg]]          == #SETMANY a maradék szöveget adja vissza
```

---

## 5. MACRO – szöveghelyettesítés

### 5.1 Hogyan működik
A MACRO hívásakor a TACL a makró szövegében **kicseréli** a `%…%` jelöléseket az argumentumokra, és az így kapott szöveget hajtja végre – mintha te gépelted volna be.

| Jelölés | Jelentés |
|---|---|
| `%1%`, `%2%` … | az 1., 2. … argumentum |
| `%2 TO 4%` | a 2–4. argumentum |
| `%2 TO *%` | a 2. és az összes további |
| `%*%` | az összes argumentum |

```
?SECTION fi MACRO
FILEINFO %1%
FUP INFO %1%, DETAIL
```
`fi NAPLO` → `FILEINFO NAPLO` és `FUP INFO NAPLO, DETAIL`.

### 5.2 Előnyök és korlátok
- **Előny:** egyszerű, gyors megírni; ideális „parancs-rövidítéshez”.
- **Korlát:**
  - nincs argumentum-ellenőrzés: ha hiányzik egy argumentum, a `%1%` helyére üres szöveg kerül, és a parancs furcsa hibát ad;
  - a helyettesítés **szövegszintű**: a `%1%` a megjegyzésben és az idézőjelek között is kicserélődik;
  - az argumentumokat a vessző és a szóköz választja el – vesszőt tartalmazó szöveget nem lehet egy argumentumként átadni;
  - nincs igazi visszatérési érték és hibakezelés.

### 5.3 Hiányzó argumentum kezelése
```
#SET p %1%
[#IF [#EMPTYV p] |THEN| #OUTPUT Hasznalat: fi fajlnev |ELSE| FILEINFO [p]]
```

---

## 6. ROUTINE – „igazi” eljárás

### 6.1 Argumentumok: `#ARGUMENT`
A ROUTINE nem `%1%`-et használ, hanem **feldolgozza** az argumentumokat, típus szerint ellenőrizve:

```
?SECTION masol ROUTINE
#FRAME
#PUSH forras cel
SINK [#ARGUMENT /VALUE forras/ FILENAME]
SINK [#ARGUMENT /VALUE cel/ FILENAME]
SINK [#ARGUMENT END]
FUP DUP [forras], [cel]
#UNFRAME
```
- `#ARGUMENT /VALUE v/ <típus>` – a következő argumentumot ellenőrzi, és a `v` változóba teszi.
- Típusok például: `FILENAME`, `NUMBER`, `KEYWORD /WORDLIST …/`, `TEXT`, `STRING`, `END` (nincs több argumentum).
- Több lehetőség is megadható; az eredmény a megfelelő lehetőség sorszáma:
  ```
  [#CASE [#ARGUMENT /VALUE v/ NUMBER KEYWORD /WORDLIST ALL/ END]
   |1| == szamot kaptunk
   |2| == ALL kulcsszo
   |3| == nincs argumentum
  ]
  ```
- Ha az argumentum nem felel meg, a TACL **hibát jelez** („Expecting …”), és a rutin nem fut tovább – nem kell kézzel ellenőrizni.
- `[#MORE]` – van-e még argumentum; `[#REST]` – a maradék argumentumok szövegként.

### 6.2 Visszatérés: `#RETURN`
```
[#IF [#EMPTYV forras] |THEN| #OUTPUT Nincs forras; #RETURN]
```
A `#RETURN` azonnal kilép a rutinból.

### 6.3 Visszatérési érték: `#RESULT`
A rutin **függvényként** is hívható zárójelben, és amit `#RESULT`-tal ad, az lesz az értéke:
```
?SECTION fajlmeret ROUTINE
#FRAME
#PUSH f
SINK [#ARGUMENT /VALUE f/ FILENAME]
SINK [#ARGUMENT END]
#RESULT [#FILEINFO /EOF/ [f]]
#UNFRAME
```
Használat: `#OUTPUT A NAPLO merete: [fajlmeret NAPLO] bajt`

### 6.4 MACRO vagy ROUTINE?

| Szempont | MACRO | ROUTINE |
|---|---|---|
| Argumentumok | `%1%` szöveghelyettesítés | `#ARGUMENT`, típus-ellenőrzéssel |
| Hiányzó / rossz argumentum | csendben rossz parancs lesz belőle | azonnali, érthető hibaüzenet |
| Visszatérési érték | nincs igazán | `#RESULT`, függvényként hívható |
| Korai kilépés | körülményes (`#IF … |ELSE| …` blokkok) | `#RETURN` |
| Hibakezelés | korlátozott | `#FILTER` / `#EXCEPTION` (7. fejezet) |
| Megírás | gyors, egyszerű | kicsit több „szertartás” |
| **Mikor** | rövidítés, egyszerű parancssor, saját használat | mások által is használt eszköz, paraméterezett, hibatűrő logika |

Ökölszabály: ha a makróban már `#IF [#EMPTYV …]` ellenőrzésekkel vadászod a hiányzó argumentumokat, vagy egy értéket akarsz visszaadni, **írd át ROUTINE-ra**.

---

## 7. Vezérlés

```
[#IF feltetel |THEN|
  ... 
|ELSE|
  ...
]

[#CASE [valtozo]
 |A|      ...
 |B C|    ...                 == B vagy C
 |OTHERWISE| ...
]

[#LOOP |WHILE| i < 10 |DO|
  ...
  #SET i [#COMPUTE i + 1]
]

[#LOOP |DO| ... |UNTIL| feltetel]
```
- A `|THEN|`, `|ELSE|`, `|DO|` stb. **címkék** – a blokk a következő címkéig, illetve a záró `]`-ig tart.
- Az `#IF` feltétele számként értelmeződik: **0 = hamis, minden más igaz**. Sok függvény -1-et ad igaznak (pl. `#FILEINFO /EXISTENCE/`, `#EMPTYV`).

---

## 8. Kifejezések és összehasonlítás

| Fajta | Operátorok | Példa |
|---|---|---|
| szám | `=  <>  <  >  <=  >=` | `[#IF n > 10 |THEN| …]` |
| szöveg | `'='  '<>'  '<'  '>'` (aposztróffal!) | `[#IF valasz '=' "IGEN" |THEN| …]` |
| logikai | `AND  OR  NOT` | `[#IF NOT [#EMPTYV x] AND n > 0 |THEN| …]` |
| aritmetika | `+  -  *  /` | `[#COMPUTE (a + b) * 2]` |

**Csapda:** szöveget `=`-vel összehasonlítva a TACL számként próbálja értelmezni – szövegre mindig `'='` kell.

---

## 9. Hibakezelés: `#FILTER`, `#EXCEPTION`, `#RAISE`

Egy ROUTINE „elkaphatja” a benne keletkező hibát:

```
?SECTION biztos ROUTINE
#FRAME
#FILTER _ERROR
[#CASE [#EXCEPTION]
 |_ERROR|
   #OUTPUT Hiba tortent: [#ERRORNUMBERS]
   #UNFRAME
   #RETURN
 |OTHERWISE|
]
== --- normal futas ---
FUP DUP NINCSILYEN, VALAMI
#OUTPUT Kesz.
#UNFRAME
```
- `#FILTER _ERROR` – a rutin jelzi, hogy a `_ERROR` kivételt maga kezeli.
- Kivétel esetén a TACL a rutint **az elejétől újraindítja**, és ekkor a `[#EXCEPTION]` a kivétel nevét adja (normál indításkor üres). Ezért van a kezelő a rutin elején.
- `[#ERRORNUMBERS]` – a hiba részletei (hibaszámok).
- `#RAISE _ERROR` – saját kivétel kiváltása.
- További kivételek: `_BREAK` (a felhasználó BREAK-et nyomott), `_CALLERS_ERROR` (a hívó oldalán jelzett hiba).

> A pontos viselkedés TACL-verziónként kicsit eltérhet – egy új hibakezelő rutint mindig próbálj ki egy szándékosan hibás paranccsal.

---

## 10. Programok futtatása makróból

```
RUN prog /NAME $X, NOWAIT, OUT $S.#LISTA, CPU 2/ param1 param2
prog /OUTV valt/ param1          == a kimenet egy TACL-változóba
prog /INV valt/ param1           == a bemenet (IN) egy TACL-változóból
```
- **`/OUTV v/`**: a program kimenete a `v` változóba kerül (soronként) – utána `#LINECOUNT`, `#LINEGET`, `#CHARFIND`-dal feldolgozható.
- **`/INV v/`**: a program a `v` változó sorait olvassa bemenetként.
- **Nem minden program támogatja**: amelyik az OUT-ját fájlként kezeli, az hibát ad (pl. a `climcmd`: „Filename must be specified with OUT run-option”). Ilyenkor: kimenet egy munkafájlba (`/OUT fajl/`), majd `FUP /OUTV v/ COPY fajl`.
- **A `FUP COPY` a hosszú rekordokat levághatja**, ha változóba ír – nagyon hosszú sorokat inkább egy programmal érdemes közvetlenül a fájlból olvasni.
- A TACL **beépített parancsai** (pl. `FILEINFO`, `STATUS`) nem programok, ezért `/OUT …/` opciót nem kapnak – helyettük a megfelelő program (pl. `FUP INFO`) vagy függvény (`#FILEINFO`) használható.

---

## 11. Hasznos beépített függvények

| Függvény | Mit ad | Példa |
|---|---|---|
| `#OUTPUT` / `#OUTPUTV` | kiír szöveget / egy változó tartalmát | `#OUTPUTV lista` |
| `#EMPTYV v` | üres-e a változó (-1 / 0) | `[#IF [#EMPTYV v] …]` |
| `#COMPUTE` | aritmetika | `[#COMPUTE x * 60]` |
| `#CHARFIND v poz szöveg` | szöveg helye a változóban (0 = nincs) | `[#CHARFIND v 1 ERROR]` |
| `#CHARGET v a TO b` | részszöveg | `[#CHARGET d 6 TO 7]` |
| `#CHARCOUNT v` | hossz | |
| `#SHIFTSTRING /UP/` | nagybetűsítés | `[#SHIFTSTRING /UP/ [x]]` |
| `#SETMANY a b , szöveg` | szavak szétosztása változókba; a maradékot adja vissza | `SINK [#SETMANY h m , [ido]]` |
| `#APPEND v szöveg` / `#APPENDV v w` | sor hozzáfűzése / változó hozzáfűzése | |
| `#LINECOUNT v` / `#LINEGET v n` | sorok száma / n. sor | |
| `#FILEINFO /opció/ fájl` | `EXISTENCE`, `CODE`, `EOF`, `SECURITY` … | `[#FILEINFO /EXISTENCE/ [f]]` |
| `#FILENAMES minta` | a mintára illeszkedő fájlnevek | `[#FILENAMES $HD1.GYAK.*]` |
| `#DELAY n` | várakozás **századmásodpercben** | `#DELAY 30000` = 5 perc |
| `#TIMESTAMP` + `#CONTIME` | helyi idő: `év hó nap óra perc mp szmp` | `[#CONTIME [#TIMESTAMP]]` |
| `#JULIANTIMESTAMP` + `#INTERPRETTIMESTAMP` | 64 bites időbélyeg és felbontása | |
| `#DEFAULTS` | alapértelmezett kötet/alkötet | |

---

## 12. Szintaxis-apróságok

| Jel | Jelentés |
|---|---|
| `==` | megjegyzés a sor végéig |
| `{ … }` | megjegyzés (több sor is lehet) |
| `&` a sor végén | a parancs a következő sorban folytatódik |
| `| … |` | címke (`|THEN|`, `|DO|`) |
| `^` | megengedett karakter változónévben (`napi^ido`) – jó előtagokhoz |
| `!` (pl. `FUP PURGE x!`) | megerősítés nélkül – óvatosan! |

---

## 13. Gyakori csapdák

1. **`#PUSH` nélküli változó** – felülírja a hívó ugyanilyen nevű változóját, vagy hibát ad, ha nem létezik. Mindig `#FRAME` / `#PUSH` / `#UNFRAME`.
2. **Függvényeredmény nincs elnyelve** – `#SETMANY`, `#ARGUMENT` és társai eredményét `SINK`-kel dobd el, különben a TACL parancsként futtatja.
3. **Szöveg-összehasonlítás `=`-vel** – szövegre `'='`.
4. **`#DELAY` másodperc helyett századmásodperc** – `#DELAY 60` csak 0,6 mp.
5. **`LOAD` `/KEEP 1/` nélkül** – a definíciók halmozódnak, és nem mindig a legújabb fut.
6. **Makró-argumentumban vessző** – a vessző elválasztó; ilyenkor ROUTINE és `#REST`, vagy a vesszőt tartalmazó parancsot írd be magába a makróba.
7. **`/OUTV/` nem működik egy programmal** – munkafájl + `FUP COPY`.
8. **A `%1%` a megjegyzésben is kicserélődik** – ne írj `%…%`-ot a makró megjegyzéseibe, ha nem helyettesítésnek szánod.

---

## 14. Ugyanaz MACRO-ként és ROUTINE-ként

Feladat: egy fájl méretének kiírása; ha nincs ilyen fájl, érthető üzenet.

**MACRO:**
```
?SECTION meret MACRO
#FRAME
#PUSH f
#SET f %1%
[#IF [#EMPTYV f] |THEN|
  #OUTPUT Hasznalat: meret fajlnev
|ELSE|
  [#IF [#FILEINFO /EXISTENCE/ [f]] |THEN|
    #OUTPUT [f]: [#FILEINFO /EOF/ [f]] bajt
  |ELSE|
    #OUTPUT Nincs ilyen fajl: [f]
  ]
]
#UNFRAME
```

**ROUTINE:**
```
?SECTION meret ROUTINE
#FRAME
#PUSH f
SINK [#ARGUMENT /VALUE f/ FILENAME]      == hianyzo/hibas nev: automatikus hiba
SINK [#ARGUMENT END]
[#IF NOT [#FILEINFO /EXISTENCE/ [f]] |THEN|
  #OUTPUT Nincs ilyen fajl: [f]
  #UNFRAME
  #RETURN
]
#OUTPUT [f]: [#FILEINFO /EOF/ [f]] bajt
#UNFRAME
```
A ROUTINE-ban nincs szükség a „Hasznalat” ágra – a `#ARGUMENT` maga jelez, ha hiányzik vagy rossz a fájlnév –, és a `#RETURN` miatt nem kell egymásba ágyazni az `#IF`-eket.

---

*Megjegyzés: a TACL-verziók között apró eltérések lehetnek. Ha egy függvény vagy opció nálatok másképp viselkedik, a „TACL Reference Manual” az irányadó; új szerkezetet először mindig a homokozóban próbálj ki.*
