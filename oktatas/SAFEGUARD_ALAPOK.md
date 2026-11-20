# Safeguard – kezdőknek

*Kik vagyunk a rendszer szemében, ki mit érhet el, és mit tegyél, ha „Security violation” hibát kapsz.*

---

## 0. Biztonsági szabályok (ezzel kezdünk)

1. **A felhasználói azonosítód személyes.** A jelszavadat ne add tovább, és ne dolgozz más nevében.
2. **Kezdőként a Safeguardban csak nézelődsz.** Az `INFO` parancsok ártalmatlanok. Az `ADD`, `ALTER`, `DELETE`, `FREEZE`, `THAW` parancsokat nem használod: ezek a biztonsági adminisztrátor dolgai.
3. **Ha egy fájlhoz nincs jogod, ne kerülgesd.** Ne másold át, ne próbálkozz más azonosítóval. Szólj a mentorodnak; ha valóban kell a jog, a biztonsági adminisztrátor adja meg.
4. **A prod rendszeren (\EH08) minden hozzáférés naplózódhat.** A sikertelen próbálkozások is.

---

## 1. Mi a Safeguard?

A NonStop gépeknek két biztonsági rétegük van:

| Réteg | Mit tud | Hol látod |
|---|---|---|
| **Guardian-biztonság** (alap) | fájlonként 4 betű: ki olvashat, írhat, futtathat, törölhet (RWEP) | `FILEINFO`, `FUP INFO` |
| **Safeguard** (kiegészítő termék) | felhasználók és jelszavak kezelése, részletes hozzáférési listák (ACL), naplózás | `SAFECOM` |

A Safeguard **a Guardian fölé épül**. Ha egy fájlra, alkötetre vagy kötetre Safeguard-védelem van beállítva, a hozzáférésről a Safeguard dönt, a Guardian-féle RWEP-betűk helyett.

A Safeguard ezen felül:
- kezeli a **bejelentkezést** (felhasználónév, jelszó, jelszólejárat, zárolás hibás próbálkozások után);
- **naplózza** a hozzáféréseket (auditálás), ha be van állítva;
- védheti a processzeket és az eszközöket (terminálok, nyomtatók) is.

---

## 2. Felhasználók: csoport és felhasználó

Minden felhasználónak két alakja van:

| Alak | Példa | Jelentés |
|---|---|---|
| **név** | `GRP.USER` | csoportnév, pont, felhasználónév |
| **szám** | `8,3` | csoportszám, vessző, felhasználószám |

- A **csoport** (group) a csapat, például az üzemeltetők. A csoport tagjai jellemzően hozzáférnek egymás fájljaihoz.
- A **felhasználó** egy személy vagy egy technikai azonosító (például a BASE24 alkalmazás futtatója).
- **`n,255`**: a csoport **menedzsere**, aki a csoportja felhasználóit kezelheti.
- **`255,255`** a **SUPER.SUPER**: a rendszer legerősebb azonosítója. Kezdőként nem használod.

### Alias

Egy felhasználónak lehet **aliasa**: egy másik, kényelmesebb bejelentkezési név (például a saját neved), amely mögött ugyanaz a `csoport,felhasználó` szám áll. Ha bejelentkezéskor egy rövid nevet írsz be, és nem `CSOPORT.NÉV`-et, az valószínűleg alias.

### Ki vagyok?

```
WHO                 == a bejelentkezett felhasználó, a default alkötet, a gép
```

A `WHO` megmutatja a felhasználóneved és a számát is. Érdemes megjegyezni, mert a jogosultsági listákban gyakran számként szerepel (`8,3`).

---

## 3. Bejelentkezés, jelszó

```
LOGON GRP.USER        == bejelentkezés (vagy az aliasoddal)
LOGOFF                == kijelentkezés
PASSWORD              == jelszócsere (a rendszer kéri a régit és az újat)
```

- A jelszónak **lejárata** lehet. Ha lejár, bejelentkezéskor új jelszót kér.
- Több hibás próbálkozás után a felhasználó **zárolódhat** (FROZEN). Ilyenkor csak a biztonsági adminisztrátor tudja feloldani (THAW). Ha ez történik, ne próbálkozz tovább: szólj.
- A terminált ne hagyd bejelentkezve, amikor felállsz.

---

## 4. Guardian-biztonság röviden (RWEP)

Ezt az 1. cikkben már láttad, itt csak emlékeztetőül. Minden fájlnak négy betűje van, ebben a sorrendben:

| Pozíció | Jog |
|---|---|
| R | olvasás (Read) |
| W | írás (Write) |
| E | futtatás (Execute) |
| P | törlés (Purge) |

A betűk értéke azt mondja meg, **ki** kapja az adott jogot:

| Betű | Ki |
|---|---|
| `O` | csak a tulajdonos (Owner) |
| `G` | a tulajdonos csoportja (Group) |
| `A` | bárki ezen a gépen (All) |
| `C` | a tulajdonos csoportja a hálózatban (más gépről is) |
| `N` | bárki a hálózatban (Network) |
| `U` | a tulajdonos a hálózatban (User) |
| `-` | csak a SUPER.SUPER |

Példa: `NUNU` – mindenki olvashat és futtathat, de írni és törölni csak a tulajdonos.

**Ha Safeguard védi a fájlt**, a `FUP INFO` a betűk helyett ezt mutatja: `*SG*`. Ilyenkor a RWEP-betűk nem számítanak; a jogokat a `SAFECOM` mutatja meg.

---

## 5. Safeguard-védelem: mit lehet védeni?

A Safeguard **védelmi rekordokkal** (protection record) dolgozik. Egy védelmi rekord egy objektumhoz tartozik:

| Objektum | Példa | Jelentés |
|---|---|---|
| **VOLUME** | `$HD1` | az egész kötet |
| **SUBVOLUME** | `$HD1.GYAK` | egy alkötet minden fájlja |
| **DISKFILE** | `$HD1.GYAK.NAPLO` | egy fájl (minta is lehet: `$HD1.GYAK.*`) |
| **PROCESS** | `$GY01` | egy processznév |
| **DEVICE** | `$S`, egy terminál | egy eszköz |

A rendszer beállításától függ, **melyik szintet nézi** a Safeguard (kötet, alkötet, fájl), és melyik az erősebb. Kezdőként elég annyit tudni: ha egy fájlnak nincs saját rekordja, a jogok jöhetnek az alkötet vagy a kötet rekordjából is.

### A hozzáférési lista (ACL)

A védelmi rekord fő része egy lista: **kinek milyen joga van**.

| Jog | Jelentés |
|---|---|
| `R` | olvasás (READ) |
| `W` | írás (WRITE) |
| `E` | futtatás (EXECUTE) |
| `P` | törlés (PURGE) |
| `C` | létrehozás (CREATE) – kötetnél, alkötetnél: hozhatsz-e létre benne fájlt |
| `O` | tulajdonosi jog (OWNER) – módosíthatja a védelmi rekordot |

Egy bejegyzés lehet egy felhasználó (`8,3`), egy egész csoport (`8,*`) vagy mindenki (`*,*`). Létezik **tiltó** bejegyzés is (DENY): az a megengedő bejegyzésnél erősebb.

---

## 6. Nézelődés: a SAFECOM

A `SAFECOM` a Safeguard parancsértelmezője. Futtatható interaktívan vagy egyetlen paranccsal:

```
SAFECOM
=INFO USER GRP.USER                 == egy felhasználó adatai
=INFO DISKFILE $HD1.GYAK.NAPLO      == egy fájl védelmi rekordja
=INFO DISKFILE $HD1.GYAK.*          == egy alkötet összes védett fájlja
=INFO SUBVOLUME $HD1.GYAK           == az alkötet védelme
=INFO VOLUME $HD1                   == a kötet védelme
=EXIT
```

Egyetlen parancsként is működik:

```
SAFECOM INFO USER GRP.USER
```

A `=` a SAFECOM promptja. A `HELP` parancs a SAFECOM-on belül is működik (`=HELP INFO`).

### Mit látsz egy fájlnál? (minta, a pontos forma eltérhet)

```
$HD1.GYAK
NAPLO        LAST-MODIFIED     OWNER   STATUS   WARNING-MODE
             20NOV26, 10:15    8,3     THAWED   OFF

             8,3       R,W,E,P,O
             8,*       R
```

Olvasd így:
- **OWNER 8,3**: a rekord tulajdonosa (ő módosíthatja).
- **STATUS THAWED**: a védelem aktív, a lista szerint lehet hozzáférni. FROZEN állapotban senki nem fér hozzá.
- **8,3 R,W,E,P,O**: a 8,3-as felhasználó mindent tehet.
- **8,\* R**: a 8-as csoport többi tagja csak olvashat.

### Mit látsz egy felhasználónál?

Jellemző mezők: a felhasználó neve és száma, a **STATUS** (THAWED = aktív, FROZEN = zárolt), a jelszó lejárata (PASSWORD-EXPIRES), a default kötet/alkötet (DEFAULT-VOLUME), és hogy mikor jelentkezett be utoljára.

---

## 7. Hibák, amelyekkel találkozni fogsz

| Hiba | Jelentés | Mit tegyél |
|---|---|---|
| **ERROR 48** – security violation | nincs jogod a művelethez (olvasás, írás, futtatás, törlés) | nézd meg a fájl védelmét (`FUP INFO`, ha `*SG*`: `SAFECOM INFO DISKFILE`); ha kell a jog, szólj |
| **ERROR 11** – file not found | nincs ilyen fájl, **vagy** el van rejtve előled | ellenőrizd a nevet és a default alkötetet |
| bejelentkezési hiba | rossz jelszó, lejárt jelszó vagy zárolt felhasználó | ne próbálkozz sokszor; szólj |

**Tipp:** az ERROR 48 nem hiba a rendszerben, hanem a védelem működése. A prod rendszeren gyakran **szándékos**, hogy egy üzemeltető ne írhasson egy alkalmazásfájlba.

---

## 8. Gyakorlás (\EH06)

Csak nézelődős feladatok, semmit nem módosítanak.

1. Futtasd a `WHO`-t. Mi a felhasználóneved, és mi a száma (`csoport,felhasználó`)?
2. Nézd meg a saját felhasználód adatait: `SAFECOM INFO USER <neved>`. Mikor jár le a jelszavad?
3. A homokozóban (`$HD1.GYAK`) futtasd: `FUP INFO *`. Van-e olyan fájl, amelynél `*SG*` áll?
4. Futtasd: `SAFECOM INFO SUBVOLUME $HD1.GYAK` és `SAFECOM INFO VOLUME $HD1`. Van-e védelmi rekord? Ha nincs, a SAFECOM ezt jelzi – ez is válasz.
5. Keress egy fájlt, amelyhez nincs jogod olvasni (kérj példát a mentorodtól), és próbáld meg: `FUP COPY <fajl>`. Milyen hibaszámot kaptál?

<details>
<summary>Megoldások</summary>

1. `WHO` – a kimenetben a „User” vagy „Logged on as” sor mutatja a nevet és a számot.
2. A `PASSWORD-EXPIRES` mező mutatja; ha nincs kitöltve, nincs lejárat.
3. Ha minden fájlnál RWEP-betűk állnak (pl. `NUNU`), egyiket sem védi saját Safeguard-rekord.
4. A homokozónak általában nincs saját rekordja; a kötetnek lehet.
5. ERROR 48 (security violation). FUP COPY-t csak a mentor által mutatott, nem éles fájlon próbálj!
</details>

---

## 9. Összefoglaló

| Kérdés | Parancs |
|---|---|
| Ki vagyok? | `WHO` |
| Jelszócsere | `PASSWORD` |
| Milyen a fájl alapvédelme? | `FUP INFO fajl` vagy `FILEINFO fajl` |
| Safeguard védi? | `*SG*` a `FUP INFO`-ban |
| Kinek milyen joga van? | `SAFECOM INFO DISKFILE fajl` |
| Egy felhasználó adatai | `SAFECOM INFO USER név` |
| „Security violation” | ERROR 48 – nincs jogod; szólj, ne kerülgesd |

*Megjegyzés: a Safeguard beállításai (mely szinteket nézi, van-e auditálás, a jelszószabályok) rendszerenként eltérnek. A pontos kimeneti formát és a nálunk érvényes szabályokat a mentorod vagy a biztonsági adminisztrátor tudja megmutatni.*
