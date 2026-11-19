### Parancs javítása: FC

Az `FC` (Fix Command) parancs újra elővesz egy korábbi parancsot, hogy javítás után újra lefuttathasd, és ne kelljen az egészet újragépelni.

```
FC              == a legutóbbi parancs
FC 12           == a 12. parancs (a sorszámot a prompt és a HISTORY mutatja)
FC fup          == a legutóbbi "fup"-pal kezdődő parancs
```

Az `FC` kiírja a parancsot, alá pedig egy **szerkesztősort** ad, amely `..`-tal kezdődik (pl. `13..`). Ebbe a sorba **pontosan a módosítandó karakter alá** írod a szerkesztő utasítást:

| Utasítás | Mit csinál |
|---|---|
| szöveg | **csere**: a karakterek alá írt szöveg felülírja a felettük lévőket |
| `R` + szöveg | **csere** ugyanígy – akkor kell, ha a szöveg D, I vagy R betűvel kezdődik (különben utasításnak venné) |
| `I` + szöveg | **beszúrás**: az `I` feletti karakter **elé** szúrja be a szöveget |
| `D` | **törlés**: minden `D` a felette lévő karaktert törli (`DDD` = hármat) |
| üres sor (Enter) | a javított parancs **végrehajtása** |
| `//` | **megszakítás**, nem hajtja végre |

Minden javítás után az `FC` kiírja a módosított parancsot, és újabb `..` sort ad – annyiszor javíthatsz, ahányszor kell; a végén üres Enter.

**1. Törlés – egy betűvel több:**
```
12> FUP INFO NAPLOO, DETAIL
...
13> FC
13> FUP INFO NAPLOO, DETAIL
13..              D
13> FUP INFO NAPLO, DETAIL
13..                                   (üres Enter: végrehajtás)
```

**2. Beszúrás – kimaradt egy betű:**
```
14> FC
14> FUP INFO NAPL, DETAIL
14..             IO
14> FUP INFO NAPLO, DETAIL
14..
```
Az `I` a vessző alatt áll, ezért az „O” a vessző elé kerül.

**3. Csere – azonos hosszú szövegre:**
```
15> FC
15> STATUS $GY01
15..           2
15> STATUS $GY02
15..
```

**Vigyázat a cserénél:** a csere annyi karaktert ír felül, amilyen hosszú az új szöveg. Ha az új szöveg hosszabb, a mögötte lévő karaktereket (pl. vesszőt, szóközt) is felülírja. Ilyenkor az azonos hosszú részt cseréld, a többletet pedig `I`-vel szúrd be – vagy két lépésben javíts.

Tipp: a `HISTORY` paranccsal megnézheted az előző parancsokat a sorszámukkal.
