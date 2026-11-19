# BASE24 a NonStopon – 2. rész

*Folytatás a „NonStop alapok” cikkhez. A fájl-, processz- és spooler-alapokat ismertnek tekintjük.*

---

## 0. Biztonság – ami itt még fontosabb

| Szabály | Miért |
|---|---|
| **A BASE24 éles fájljait (PTLF, TLF, CAF, PBF, konfigurációs fájlok) soha nem írjuk, nem töröljük, nem nevezzük át.** | Megáll a kártyaforgalom vagy elvesznek tranzakciók. |
| **BASE24-processzt nem állítunk le, nem indítunk el** – csak a felelős csapat, egyeztetve. | Egy rossz `STOP` az összes ATM-et vagy POS-t leállíthatja. |
| **A tranzakciós naplókban kártyaadat van** (kártyaszám, Track 2, lejárat). | PCI DSS: kártyaszámot nem másolunk ki, nem küldünk e-mailben, nem teszünk képernyőképre. |
| **`FUP COPY` PTLF-re vagy TLF-re: tilos.** | Kilistázná a kártyaadatokat a képernyőre / spoolerbe. |
| Olvasni szabad: `FILEINFO`, `FUP INFO … DETAIL`, `STATUS`. | Ezek nem változtatnak semmit. |

---

## 1. Mi a BASE24?

A **BASE24** az ACI Worldwide kártyás tranzakciókezelő rendszere; nálunk a **BASE24 classic** fut a NonStopon. Feladata: a kártyás tranzakciókat fogadni, eldönteni (vagy továbbítani), hogy engedélyezhetők-e, válaszolni, és mindent naplózni.

Két modulja érint minket:

| Modul | Mit kezel | Nálunk |
|---|---|---|
| **BASE24-atm** | ATM-tranzakciók | a **saját ATM-jeink** forgalma (saját és idegen kártyával) |
| **BASE24-pos** | POS (kártyás vásárlás) tranzakciók | a **saját kártyáink** más bankok POS-terminálján – kibocsátói (issuer) oldal |

Két szerep, amit érdemes megkülönböztetni:

- **Elfogadó (acquirer):** a mi eszközünkön (ATM) használt kártya – lehet saját vagy idegen.
- **Kibocsátó (issuer):** a mi kártyánk, bárhol a világon – a tranzakció a kártyahálózaton (Visa, Mastercard) érkezik hozzánk engedélyezésre.

---

## 2. Hogyan megy végig egy tranzakció?

```
  ATM / POS                                          Engedélyező
  ─────────                                          ───────────
  ATM ──► ATM-koncentrátor ──► CLIM ──► Device Handler ─┐
                                                        ├─► Authorization ──► Host / Interchange
  POS (más bank) ──► Visa / Mastercard ──► Interchange ─┘        │
                                       Interface                  ▼
                                                            HSM (PIN, kriptográfia)
```

1. **Fogadás:** az ATM-eink néhány **ATM-koncentrátoron** keresztül, TCP-kapcsolattal érik el a NonStopot (a hálózati forgalom a **CLIM**-eken megy át). A POS-tranzakciók a kártyahálózattól az **Interchange Interface** processzen jönnek.
2. **Authorization:** a BASE24 eldönti, ki engedélyez:
   - a bank **hostja** (a számlavezető rendszer), vagy
   - a BASE24 maga (pl. lejárt kártya, rossz PIN – ezt helyben is tudja), vagy
   - **STIP** (stand-in processing): ha a host nem érhető el, a BASE24 előre beállított szabályok szerint helyettesítőként dönt.
3. **HSM:** a PIN-ellenőrzés és a kriptográfiai műveletek egy hardveres biztonsági modulban (HSM, pl. Atalla) történnek. A BASE24 több párhuzamos kapcsolaton, két HSM között elosztva küldi a kéréseket.
4. **Válasz** vissza a terminálnak / hálózatnak, és **naplózás** a tranzakciós naplóba (TLF / PTLF).

---

## 3. Üzenettípusok és válaszkódok

### 3.1 Üzenettípusok (ISO 8583 alapján)

| Típus | Jelentés |
|---|---|
| 0200 | engedélyezési kérés (pénzügyi tranzakció) |
| 0210 | válasz a 0200-ra – **ez kerül a naplóba** a kérés eredményével |
| 0220 | advice: utólagos értesítés egy már megtörtént döntésről |
| 0420 | **sztornó** (reversal): a tranzakció visszavonása, pl. időtúllépés vagy hiba miatt |

### 3.2 Válaszkódok

A BASE24 háromjegyű belső válaszkódokat használ:

| Kód | Jelentés |
|---|---|
| **000, 001** | engedélyezve (egyenleginfóval / anélkül) |
| 003, 004 | engedélyezve (POS: további azonosítás / aláírt bizonylat) |
| 050 felett | **elutasítva** – pl. 051 lejárt kártya, 055 nem engedélyezett tranzakció, 076 nincs fedezet (POS) |
| ATM: 1xx | az első jegy 1 = **az ATM elnyelte a kártyát** (pl. 150) |

A teljes kódlista a BASE24-pos és BASE24-atm Transaction Processing Manualban, illetve a csapat Confluence-oldalán van.

---

## 4. A tranzakciós naplók: TLF és PTLF

Minden tranzakció (engedélyezett és elutasított is) bekerül egy **napi naplófájlba**:

| Napló | Modul | Fájlnév | Példa |
|---|---|---|---|
| **TLF** (Transaction Log File) | ATM | `TLyymmdd` | `TL261102` = 2026.11.02. üzleti nap |
| **PTLF** (POS Transaction Log File) | POS | `POyymmdd` | `PO261102` |

### 4.1 Jellemzők

- **Entry-sequenced** fájl: a rekordok érkezési (idő)rendben, a végére íródnak.
- **Nagy:** egy napi PTLF nálunk ~3 GB, közel 1 millió rekord. Egy rekord egy tranzakció (vagy egy adminisztratív / kivétel-bejegyzés).
- **Alternatív kulcsok:** a fájlban kártyaszám (`CR`), kereskedő (`RT`) és terminál + idő (`TT`, `TK`) szerint is lehet keresni. A kulcsok külön fájlokban vannak (lásd `FUP INFO … DETAIL`: `ALTKEY`, `ALTFILE`) – akár más köteteken.
- A rekordok szerkezetét a BASE24 **DDL**-je írja le (`DDLFPTLF`, `DDLTLF`).

### 4.2 Rekordtípusok

A rekord elején a `REC-TYP` mező mondja meg, mi van benne:

| REC-TYP | Jelentés |
|---|---|
| 00 | kezdő rekord (a fájl létrehozásakor íródik) |
| 01 | **ügyféltranzakció** – a lényeg |
| 04 | adminisztratív rekord (pl. ATM-kazettaműveletek, terminál-összesítők) |
| 20–23 | kivétel: a tranzakciót nem sikerült rendesen feldolgozni |

### 4.3 Néhány fontos mező (a teljesség igénye nélkül)

| Mező | Jelentés |
|---|---|
| `DAT-TIM` | a naplózás időpontja (rendszeridő) |
| `TRAN-DAT`, `TRAN-TIM` | a tranzakció ideje az üzenet szerint (külföldi POS-nál a helyi idő!) |
| `TYP` | üzenettípus (0210, 0420 …) |
| `TRAN-CDE` | tranzakciókód (pl. POS 10 = vásárlás, ATM 10 = készpénzfelvétel) |
| `RESP-CDE` | válaszkód |
| `AMT-1` | összeg (a deviza legkisebb egységében: fillér, cent) |
| `RESPONDER` | ki adta a választ (host, BASE24, interchange …) |
| `TERM-ID` | terminál azonosító (saját ATM-jeink: `ATM…`) |

---

## 5. A napváltás

A BASE24 **üzleti napja nem éjfélkor** kezdődik, hanem nálunk **20:30-kor**.

- A `PO261102` fájl tehát **2026.11.01. 20:30-tól 2026.11.02. 20:30-ig** tartalmazza a tranzakciókat.
- Egy időpontban **két napló is nyitva van**: az aktuális és a következő napi. A terminálok átállása (cutover) nem egyszerre történik; amelyik terminál már átállt, annak a tranzakciói már a következő napi fájlba íródnak.
- A következő napi fájl **előre létrejön** (ezért lehet a `FUP INFO` „CREATION DATE” mezője napokkal korábbi, mint a fájlban lévő adatok).
- A **hálózati elszámolás (settlement)** után az előző napi naplót a rendszer lezárja – ekkortól használható riportokra és adatkivonatokra.

**Gyakorlati következmények:**

- „Mi történt tegnap este 21-kor?” → az a **mai** dátumú fájlban van (21:00 > 20:30).
- „Mi történt ma 10-kor?” → szintén a **mai** dátumú fájlban (tegnap 20:30 és ma 20:30 között).
- Időszakos lekérdezéseknél figyelni kell, hogy az időablak átnyúlhat a napváltáson (két fájl).

---

## 6. Hol vannak a fájlok?

A PTLF egy alkötetben van, a napi fájlok egymás mellett; az alternatív kulcs-fájlok más köteteken (pl. `$SD1`–`$SD4`), azonos alkötetnévvel. Példa:

```
FILEINFO $SD5.PRO1PTLF.PO*            == a napi PTLF-ek listája
FUP INFO $SD5.PRO1PTLF.PO261102, DETAIL
```
A `FUP INFO … DETAIL` kimenetében látszik a típus (`TYPE E`), a formátum (`FORMAT 2`), a rekord- és blokkméret, az alternatív kulcsok és kulcsfájlok, a méret (`EOF`) és az időpontok.

A TLF hasonló felépítésű alkötetben van; a pontos helyeket a mentorod mutatja meg.

---

## 7. EMS – az eseménynapló

A NonStop és a BASE24 eseményeit (hibák, figyelmeztetések, állapotváltozások) az **EMS** (Event Management Service) gyűjti.

- Az eseményeket egy **collector** processz (nálunk `$PCOLL`) gyűjti, és **log fájlokba** írja (`ZZEVnnnn`, 843-as fájlkód). Ezek binárisak – közvetlenül nem olvashatók.
- Szöveggé az **EMSDIST** alakítja. Egy időszak eseményei a terminálra:

```
EMSDIST TYPE PRINTING, COLLECTOR $PCOLL, TEXTOUT [#MYTERM], &
  TIME 2026-11-15 09:00:00, STOP 2026-11-15 09:10:00
```
A `STOP` időpontnál az EMSDIST magától leáll.

Egy esemény így néz ki:
```
26-11-15 09:00:16 \EH08.$P1XML       *ACI.1572.1000        000075 Mgr PPD:
                                       TSEC: Hardware Security Module error,
                                       device = SECDEV1, response message = HSM
                                       Device Hardware Error
```
| Rész | Jelentés |
|---|---|
| dátum, idő | mikor |
| `\EH08.$P1XML` | melyik processz küldte |
| `*` | **kritikus** (kiemelt) esemény |
| `ACI.1572.1000` | alrendszer (itt BASE24) |
| `000075` | eseményszám |
| szöveg | a hiba leírása |

---

## 8. Hálózat: CLIM és kapcsolatok

A NonStop X rendszeren a TCP/IP-forgalmat a **CLIM**-ek (Linux alapú hálózati egységek) kezelik. A BASE24 kapcsolatai a CLIM-eken, **hálózati névterekben** (namespace) láthatók:

```
climcmd <clim> ip netns list                          == a névterek listája
climcmd <clim> ip netns exec <névtér> ss -tin dport = :7000 or sport = :7000
```
A második parancs egy port összes kapcsolatát mutatja: túloldali cím, sorban álló adat (`Send-Q`), válaszidő (`rtt`), forgalmi számlálók.

Tipikus portok nálunk: **4275** – ATM-koncentrátorok, **7000** – HSM.

> A `climcmd` használata jogosultsághoz kötött; kezdetben a mentoroddal együtt használd.

---

## 9. Gyakorló kérdések

*Csak olvasó parancsokkal! A válaszokat a mentoroddal beszéld meg.*

1. Mi a tegnapi üzleti nap PTLF fájljának neve? Mettől meddig tartalmaz tranzakciókat?
2. Kérd le a `FUP INFO … DETAIL` adatait. Mekkora a fájl (`EOF`), milyen típusú és formátumú, mekkora a blokkméret? Mikor jött létre, és mikor módosult utoljára? Miért tér el a kettő?
3. Hány alternatív kulcsa van? Mire valók, és melyik köteteken vannak a kulcsfájlok?
4. Egy ügyfél azt mondja, tegnap este 21:15-kor nem sikerült fizetnie. Melyik napi PTLF-ben keresnéd a tranzakciót?
5. Mi a különbség a 0210 és a 0420 üzenet között?
6. Mit jelent, ha egy ATM-tranzakció válaszkódja 151?
7. Mi a STIP, és mikor lép működésbe?
8. Kérdezd le az EMS-ből egy 10 perces időszak eseményeit. Melyik kritikus? Melyik processz küldte?
9. Melyik porton kapcsolódnak az ATM-ek, és melyiken a HSM-ek? Mit mutat a `Send-Q`, ha nem nulla?
10. Miért tilos a PTLF-re `FUP COPY`-t futtatni?

### Megoldások (röviden)

1. A tegnapi dátumú `POyymmdd`: tegnapelőtt 20:30-tól tegnap 20:30-ig.
2. Pl. ~3 GB, `TYPE E`, `FORMAT 2`, `BLOCK 4096`. A fájl a napváltás előtt napokkal jön létre (a következő napi fájl előre elkészül), adat csak az üzleti napon kerül bele.
3. Öt: `CR` (kártya), `RT` (kereskedő), `TT`, `TK` (terminál + idő), `RK`; az `ALTFILE` sorok mutatják, melyik köteten vannak.
4. A **mai** dátumú fájlban – a 21:15 már a következő üzleti naphoz tartozik.
5. 0210: válasz egy engedélyezési kérésre; 0420: sztornó, egy korábbi tranzakció visszavonása.
6. Az ATM elnyelte a kártyát (első jegy 1), oka: lejárt kártya (51).
7. Helyettesítő engedélyezés: ha a host nem érhető el, a BASE24 előre beállított szabályok szerint dönt.
8. A `*` jelű esemény kritikus; a sor elején a processz neve áll.
9. ATM: 4275, HSM: 7000. A nem nulla `Send-Q` azt jelenti, hogy a túloldal nem nyugtázta az elküldött adatot – lassú vagy nem válaszol.
10. Kiírná a kártyaszámokat és a Track 2 adatokat – PCI-szabálysértés.

---

## 10. Szószedet

| Fogalom | Jelentés |
|---|---|
| Acquirer / elfogadó | akinek az eszközén (ATM, POS) a kártyát használják |
| Issuer / kibocsátó | aki a kártyát kiadta |
| Interchange | kártyahálózat (Visa, Mastercard) és a hozzá kapcsolódó BASE24-interfész |
| Host | a bank számlavezető rendszere |
| STIP | stand-in processing: helyettesítő engedélyezés host-kiesés esetén |
| HSM | hardveres biztonsági modul (PIN, kulcsok, kriptográfia) |
| TLF / PTLF | ATM / POS tranzakciós napló |
| Cutover | a terminál átállása a következő üzleti napra |
| Settlement | elszámolás; utána a napi napló lezárul |
| Reversal / sztornó | tranzakció visszavonása (0420) |
| CLIM | NonStop X hálózati egység (Linux), rajta futnak a TCP-kapcsolatok |
| EMS | a NonStop eseménynaplózó rendszere |
| DDL | adatdefiníciós nyelv; a BASE24-fájlok rekordszerkezetét írja le |
| PCI DSS | a kártyaadatok védelmére vonatkozó szabvány |
