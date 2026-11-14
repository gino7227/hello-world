# TACL példa: CWATCH

`cwatch.tacl` egy HP NonStop TACL makrókönyvtár. Figyeli egy subvolume-ot,
és ha új, `C` betűvel kezdődő fájl jelenik meg benne, lefuttat egy parancsot.

## Telepítés
1. Másold fel a fájlt a NonStop rendszerre szövegként (FTP ASCII módban), majd
   alakítsd EDIT fájllá (code 101), pl.: `CTOEDIT cwatch, $DATA.TACLLIB.CWATCH`
2. Töltsd be: `LOAD /KEEP 1/ $DATA.TACLLIB.CWATCH`

## Használat
```
CWATCH $DATA.INBOX 10 CHANDLR
```
- `$DATA.INBOX`: a figyelt subvolume
- `10`: lekérdezési időköz másodpercben
- `CHANDLR`: a futtatandó parancs. Utolsó argumentumként megkapja az új fájl nevét.

Leállítás: BREAK, vagy létrehozol egy `$DATA.INBOX.STOPCW` fájlt.

## Megjegyzések
- A TACL nem kap értesítést a fájlrendszertől, ezért a makró időközönként
  lekérdezi a fájlokat (polling).
- Az induláskor már meglévő `C*` fájlokra nem fut le a parancs.
- Háttérben futtatáshoz indíts egy külön TACL folyamatot, pl.:
  `TACL /NAME $CWAT, NOWAIT, IN $DATA.TACLLIB.CWRUN, OUT $S.#CWATCH/`
  ahol a `CWRUN` fájl tartalmazza a `LOAD` és a `CWATCH ...` sorokat.
