/*
 * TRNLIST - BASE24 tranzakciok listazasa a PTLF-bol (POS) es/vagy a TLF-bol (ATM)
 *
 * Hasznalat (Guardian):
 *   RUN TRNLIST [opciok] fajl [fajl2]
 *   RUN TRNLIST /OUT $S.#TRNLIST/ -i 10:00-10:15 -e $DATA.TLF.TL261114
 *   RUN TRNLIST /OUT $DATA.LISTA.CSV1/ -c -k 1234 $DATA.PTLF.PO261114 $DATA.TLF.TL261114
 *
 * Opciok (mind elhagyhato, egyutt ES kapcsolatban):
 *   -i hh:mm[:ss]-hh:mm[:ss]  idotartomany (helyi ido); 23:00-01:00 = ejfelen at
 *   -k kartya     utolso 4 jegy (1234), eleje (411111*), vagy mindketto (411111*1234)
 *   -m terminal   terminal azonosito eleje (pl. ATM0001)
 *   -r kodok      valaszkod(ok) vesszovel: 051,055
 *   -e            csak az elutasitott tranzakciok
 *   -x kod        tranzakciokod (POS: TRAN-CDE.TC, ATM: TRAN-CDE.T-CDE), pl. 10
 *   -l            hosszu lista (masodik sor: hely, orszag, valaszido, sztorno ok)
 *   -c            CSV kimenet (pontosvesszovel elvalasztva, Excelhez)
 *   -n db         legfeljebb ennyi talalat fajlonkent (alap 500, 0 = mind; CSV: mind)
 *   -t P|T        fajltipus (P = PTLF, T = TLF), ha a nevbol nem derul ki
 *   -b 4096       blokkmeret (FUP INFO: BLOCK), az idopontra ugrashoz; alap 4096
 *   -s            teljes osszesites: a fajlt vegigolvassa (lassabb), alapbol a
 *                 program leall, ha a talalatok elerik a -n szamot, vagy az
 *                 idoablak lezarult
 *
 * A fajltipus a nev utolso reszebol derul ki: PO... = PTLF, TL... = TLF.
 * Ket fajl eseten mindkettot listazza, egymas utan.
 *
 * A kartyaszam mindig maszkolva jelenik meg (elso 6 es utolso 4 jegy).
 * A kimenet ekezet nelkuli ASCII, kepernyon legfeljebb 79 oszlop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef __TANDEM
#include <cextdecs(FILE_OPEN_,READX,FILE_GETINFO_,FILE_CLOSE_,FILE_SETPOSITION_,CONVERTTIMESTAMP,INTERPRETTIMESTAMP)>
#endif

#define MAXREC   4096
#define RPT_W    79
#define JTS_UNIX_EPOCH  210866760000000000LL
#define JTS_MIN_PLAUS   211813000000000000LL
#define JTS_MAX_PLAUS   214969000000000000LL

/* ---------- mezopoziciok ---------- */
typedef struct {
  char kind;                      /* 'P' = PTLF, 'T' = TLF */
  const char *name;
  int min_len;
  int rec_typ, rec_frmt;          /* rec_frmt < 0: nincs ilyen mezo */
  int pan, term_id, typ, tcde, resp, amt1, crncy, responder, rte_stat;
  int exit_tim, reentry_tim, name_loc, city, cntry, rvsl;
} Layout;

static const Layout L_PTLF = { 'P', "PTLF (POS)", 786, 8, 145,
  18, 105, 172, 378, 405, 408, 593, 179, 176, 190, 198, 250, 297, 313, 582 };
static const Layout L_TLF  = { 'T', "TLF (ATM)", 474, 8, -1,
  46, 22, 78, 180, 255, 226, 351, 85, 82, 94, 102, 258, 305, 321, 384 };

/* ---------- opciok ---------- */
static long  f_from = -1, f_to = -1;      /* mp az ejfel ota */
static char  f_card_pre[20], f_card_suf[20];
static int   f_card = 0;
static char  f_term[17];
static int   f_resp[32], n_resp;
static int   f_decl = 0;
static int   f_tcde = -1;
static int   o_long = 0, o_csv = 0;
static long  o_max = 500;
static int   o_max_set = 0;
static char  o_type = 0;
static char *files[2]; static int n_files;

static int big_endian = 1, endian_known = 0;

/* ---------- segedek ---------- */
static long long get64(const unsigned char *p)
{
  unsigned long long v = 0; int i;
  if (big_endian) for (i = 0; i < 8; i++) v = (v << 8) | p[i];
  else            for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return (long long)v;
}
static int plausible_ts(long long t) { return t > JTS_MIN_PLAUS && t < JTS_MAX_PLAUS; }

static long num(const unsigned char *p, int len)
{
  long v = 0; int i;
  for (i = 0; i < len; i++) {
    if (p[i] < '0' || p[i] > '9') return -1;
    v = v * 10 + (p[i] - '0');
  }
  return v;
}

static void text(const unsigned char *p, int len, char *out)
{
  int i;
  for (i = 0; i < len; i++) out[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : ' ';
  out[len] = 0;
  for (i = len - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

static char *fmt_n(long long v, char *out)
{
  char tmp[40]; int i, j = 0, len, neg = v < 0;
  unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
  sprintf(tmp, "%llu", u);
  len = (int)strlen(tmp);
  if (neg) out[j++] = '-';
  for (i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0) out[j++] = ' ';
    out[j++] = tmp[i];
  }
  out[j] = 0;
  return out;
}

static int crncy_exp(int code)
{
  switch (code) {
  case 392: case 410: case 352: case 152: case 704: return 0;
  case 414: case 48: case 400: case 512: case 788: return 3;
  default: return 2;
  }
}

/* osszeg minor unitbol; sep = 1: ezres tagolas (kepernyo), 0: nincs (CSV) */
static char *fmt_amt(long long v, int code, int sep, char *out)
{
  int e = crncy_exp(code), k; long long div = 1, ip, fp; char ib[40];
  for (k = 0; k < e; k++) div *= 10;
  ip = v / div; fp = v % div; if (fp < 0) fp = -fp;
  if (sep) fmt_n(ip, ib); else sprintf(ib, "%lld", ip);
  if (e == 0) strcpy(out, ib); else sprintf(out, "%s,%0*lld", ib, e, fp);
  return out;
}

static const char *crncy_name(int c)
{
  switch (c) {
  case 348: return "HUF"; case 978: return "EUR"; case 840: return "USD";
  case 826: return "GBP"; case 756: return "CHF"; case 203: return "CZK";
  case 985: return "PLN"; case 946: return "RON"; case 941: return "RSD";
  default: return NULL;
  }
}

static const char *decision(const unsigned char *r, const Layout *L)
{
  int resp = r[L->responder];
  long rte = num(r + L->rte_stat, 2);
  if (resp == '5') return "Host";
  if (resp == '3') return rte > 0 ? "STIP" : "B24";
  if (resp == '4') return "HostIF";
  if (resp == '6' || resp == '7') return "ICHG";
  return "?";
}

static int approved(long resp, char kind)
{
  if (kind == 'T') { long rs = resp % 100; return rs < 50 || (rs >= 90 && rs <= 92); }
  return resp >= 0 && resp < 50;
}

/* kartyaszam maszkolva: elso 6 + * + utolso 4 */
static void mask_pan(const char *pan, char *out)
{
  int n = (int)strlen(pan), i;
  if (n <= 4) { strcpy(out, pan); return; }
  if (n < 11) {
    for (i = 0; i < n - 4; i++) out[i] = '*';
    strcpy(out + n - 4, pan + n - 4);
    return;
  }
  memcpy(out, pan, 6);
  for (i = 6; i < n - 4; i++) out[i] = '*';
  strcpy(out + n - 4, pan + n - 4);
}

/* ---------- ido ---------- */
typedef struct { int y, mo, d, h, mi, s; } TmX;

static int ts_local(long long jts, TmX *t)
{
#ifdef __TANDEM
  short dt[8];
  long long l = CONVERTTIMESTAMP(jts, 0);   /* 0 = GMT -> helyi civil ido */
  INTERPRETTIMESTAMP(l, dt);
  t->y = dt[0]; t->mo = dt[1]; t->d = dt[2];
  t->h = dt[3]; t->mi = dt[4]; t->s = dt[5];
  return 0;
#else
  long long us = jts - JTS_UNIX_EPOCH;
  time_t s = (time_t)(us / 1000000LL);
  struct tm *g = localtime(&s);
  if (!g) return -1;
  t->y = g->tm_year + 1900; t->mo = g->tm_mon + 1; t->d = g->tm_mday;
  t->h = g->tm_hour; t->mi = g->tm_min; t->s = g->tm_sec;
  return 0;
#endif
}

/* GMT -> helyi ido eltolas (mikroszekundum), oranként egyszer szamolva,
   hogy ne kelljen minden rekordnal rendszerhivas */
static long long off_us, off_from = 0, off_to = -1;
static long long local_off(long long jts)
{
  if (jts < off_from || jts > off_to) {
#ifdef __TANDEM
    off_us = CONVERTTIMESTAMP(jts, 0) - jts;
#else
    time_t s = (time_t)((jts - JTS_UNIX_EPOCH) / 1000000LL);
    struct tm lt = *localtime(&s), gt = *gmtime(&s);
    long d = (lt.tm_yday - gt.tm_yday);
    if (d > 1) d = -1; else if (d < -1) d = 1;          /* evvaltas */
    off_us = ((d * 24L + lt.tm_hour - gt.tm_hour) * 3600L
              + (lt.tm_min - gt.tm_min) * 60L) * 1000000LL;
#endif
    off_from = jts - jts % 3600000000LL;
    off_to = off_from + 3600000000LL - 1;
  }
  return off_us;
}

/* mp a helyi ejfel ota */
static long sec_of_day(long long jts)
{
  long long s = (jts - JTS_UNIX_EPOCH + local_off(jts)) / 1000000LL;
  return (long)(s % 86400);
}

/* idoablak: ha a -ig korabbi, mint a -tol, ejfelen atnyulo szakasz */
static int in_window(long sod)
{
  if (f_from <= f_to) return sod >= f_from && sod <= f_to;
  return sod >= f_from || sod <= f_to;
}

/* "hh:mm[:ss]" -> mp az ejfel ota, -1 ha hibas */
static long parse_hms(const char *s)
{
  int h = 0, m = 0, sec = 0, n = sscanf(s, "%d:%d:%d", &h, &m, &sec);
  if (n < 2 || h < 0 || h > 24 || m < 0 || m > 59 || sec < 0 || sec > 59) return -1;
  return h * 3600L + m * 60L + sec;
}

/* ---------- fajl olvasas ---------- */
static int o_full = 0;          /* -s: teljes osszesites (vegigolvas) */
#ifdef __TANDEM
static int sbb_on = 0;          /* 1 = blokkos olvasas (SBB) aktiv */
static short fnum = -1;
static int seq_open(const char *name)
{
  /* sequential block buffering: egy fizikai olvasas sok rekordot hoz be
     (28 KB puffer); ha nem engedelyezett, sima megnyitas */
  short err = FILE_OPEN_(name, (short)strlen(name), &fnum, 1, 0, 0, 0, 0, 0, 28672);
  sbb_on = (err == 0);
  if (err) err = FILE_OPEN_(name, (short)strlen(name), &fnum, 1, 0);
  if (err) { fprintf(stderr, "FILE_OPEN_ hiba %d: %s\n", err, name); return -1; }
  return 0;
}
static int seq_next(unsigned char *buf, int max)
{
  unsigned short cnt = 0; short err = 0;
  READX(fnum, (char *)buf, (unsigned short)max, &cnt);
  FILE_GETINFO_(fnum, &err);
  if (err == 1) return 0;
  if (err) { fprintf(stderr, "READX hiba %d\n", err); return -1; }
  return cnt;
}
static void seq_close(void) { if (fnum >= 0) FILE_CLOSE_(fnum); fnum = -1; }
#else
/* teszteleshez: minden rekord elott 2 bajtos (big-endian) hossz */
static FILE *rf;
static int seq_open(const char *name)
{
  rf = fopen(name, "rb");
  if (!rf) { perror(name); return -1; }
  return 0;
}
static int seq_next(unsigned char *buf, int max)
{
  unsigned char lb[2]; int len;
  if (fread(lb, 1, 2, rf) != 2) return 0;
  len = (lb[0] << 8) | lb[1];
  if (len > max) return -1;
  if ((int)fread(buf, 1, len, rf) != len) return -1;
  return len;
}
static void seq_close(void) { if (rf) fclose(rf); rf = NULL; }
#endif

/* ---------- ugras idopontra: felezeses kereses blokkszinten ----------
   Az entry-sequenced fajlt "unstructured" modon is megnyitjuk: igy barmely
   bajtpoziciora allhatunk (FILE_SETPOSITION_), es 4096 bajtos blokkokat
   olvashatunk. A blokkban a rekord elejet az idobelyeg (DAT-TIM) es a
   rekordtipus (REC-TYP) alapjan ismerjuk fel. A rekordok idorendben vannak,
   igy felezessel ~20 olvasassal megtalalhato az idoablak eleje.
   Ha barmi nem sikerul, a program a sima, vegigolvaso modot hasznalja. */
#define OPEN_UNSTRUCT 0x2000          /* FILE_OPEN_ options.<2>: unstructured */
#define CHUNK_BLKS    8               /* egyszerre beolvasott blokkok */
static int  blk_sz = 4096;            /* -b: blokkmeret */
static int  o_rawtest = 0;            /* -B: (teszt) nyers blokkfajl */
static const Layout *cur_L;
static int  blk_mode = 0;             /* 1 = blokkokbol olvasunk */
static long blk_start = -1;           /* ide ugrottunk (blokk) */
static long long file_t0 = 0;         /* a fajl elso rekordjanak ideje */
static int  blk_probes = 0;           /* felezeses olvasasok szama */
static char seek_fail[80] = "";       /* miert nem sikerult az ugras */
static int  o_diag = 0;               /* -v: blokk-diagnosztika */
static unsigned char chunk[CHUNK_BLKS * 8192];   /* blokkmeret max. 8192 */
static long chunk_n = 0, chunk_pos = 0;
static long next_blk = 0;            /* a kovetkezo beolvasando blokk */
static int  chunk_blks = CHUNK_BLKS; /* egyszerre olvasott blokkok (hiba: 1) */
static long blk_recs = 0;            /* blokkokbol kiadott rekordok */

#ifdef __TANDEM
static short bfnum = -1;
static int blk_open(const char *name)
{
  short err = FILE_OPEN_(name, (short)strlen(name), &bfnum, 1, 0, 0, 0, OPEN_UNSTRUCT);
  if (err) { bfnum = -1; sprintf(seek_fail, "blokkos megnyitas hiba %d", err); return -1; }
  return 0;
}
/* n bajt olvasasa a bajtpoziciotol (pos < 0: folytatas) */
static long blk_read(long long pos, unsigned char *buf, long n)
{
  unsigned short cnt = 0; short err = 0;
  if (pos >= 0) {
    err = FILE_SETPOSITION_(bfnum, pos);
    if (err) return -1;
  }
  if (n <= 0) return 0;                       /* csak pozicionalas */
  READX(bfnum, (char *)buf, (unsigned short)n, &cnt);
  FILE_GETINFO_(bfnum, &err);
  if (err == 1) return 0;
  if (err) return -1;
  return cnt;
}
static void blk_close(void) { if (bfnum >= 0) FILE_CLOSE_(bfnum); bfnum = -1; }
#else
static FILE *bf;
static int blk_open(const char *name)
{
  bf = fopen(name, "rb");
  return bf ? 0 : -1;
}
static long blk_read(long long pos, unsigned char *buf, long n)
{
  if (pos >= 0 && fseek(bf, (long)pos, SEEK_SET) != 0) return -1;
  if (n <= 0) return 0;
  /* teszt: a TRN_MAXREAD kornyezeti valtozo a NonStop olvasasi korlatjat utanozza */
  if (getenv("TRN_MAXREAD") && n > atol(getenv("TRN_MAXREAD"))) return -1;
  return (long)fread(buf, 1, (size_t)n, bf);
}
static void blk_close(void) { if (bf) fclose(bf); bf = NULL; }
#endif

/* rekord eleje a blokkban: hihető idobelyeg + ervenyes rekordtipus */
static int is_rec_start(const unsigned char *p, long avail)
{
  long rt;
  if (avail < 10 || !plausible_ts(get64(p))) return 0;
  rt = num(p + 8, 2);
  if (rt == 1) {
    if (avail < cur_L->min_len) return 0;
    if (num(p + cur_L->typ, 4) < 0) return 0;
    if (cur_L->rec_frmt >= 0 && p[cur_L->rec_frmt] != '5') return 0;
    return 1;
  }
  return rt == 0 || rt == 4 || (rt >= 20 && rt <= 23);
}

/* a blokk elso TRANZAKCIOS rekordjanak (REC-TYP 01) ideje; 0: nincs ilyen.
   A fajl elejen allo "Initial record" (REC-TYP 00) a fajl letrehozasakor
   keszult (akar napokkal korabban), ezert az idohoz nem hasznalhato. */
static long long blk_first_ts(long blk)
{
  static unsigned char b[65536];
  long n = blk_read((long long)blk * blk_sz, b, blk_sz), i;
  blk_probes++;
  for (i = 0; i + 10 <= n; i++)
    if (is_rec_start(b + i, n - i) && num(b + i + 8, 2) == 1) return get64(b + i);
  return 0;
}

/* elso felismerheto rekord ideje a blk-tol elore (legfeljebb 16 blokk) */
static long long ts_near(long blk, long last)
{
  long k;
  for (k = 0; k < 64 && blk + k <= last; k++) {
    long long t = blk_first_ts(blk + k);
    if (t) return t;
  }
  return 0;
}

/* -v: nehany blokkban kiirja az idobelyegnek latszo helyeket es a
   szerkezeti mezoket (kartyaadat nelkul) */
static void blk_diag(void)
{
  static unsigned char b[65536];
  static const long probe[] = { 0, 1, 2, 3, 100, 1000, 10000, 100000, -1 };
  int k, e;
  for (k = 0; probe[k] >= 0; k++) {
    long n = blk_read((long long)probe[k] * blk_sz, b, blk_sz), i, shown = 0;
    printf("DIAG blokk %ld: %ld bajt\n", probe[k], n);
    if (n <= 0) break;
    for (e = 1; e >= 0; e--) {
      int save = big_endian;
      big_endian = e;
      for (i = 0; i + 8 <= n && shown < 8; i++) {
        long long t = get64(b + i);
        if (plausible_ts(t)) {
          TmX tm; char rt[3] = "  ", fr = ' ', ty[5] = "    ";
          ts_local(t, &tm);
          if (i + 10 <= n) { rt[0] = (char)b[i + 8]; rt[1] = (char)b[i + 9]; }
          if (i + 146 <= n) fr = (char)b[i + 145];
          if (i + 176 <= n) memcpy(ty, b + i + 172, 4);
          {
            int q;
            for (q = 0; q < 2; q++) if (rt[q] < 32 || rt[q] > 126) rt[q] = '.';
            if (fr < 32 || fr > 126) fr = '.';
            for (q = 0; q < 4; q++) if (ty[q] < 32 || ty[q] > 126) ty[q] = '.';
          }
          printf("  off %5ld %s %04d.%02d.%02d %02d:%02d:%02d REC-TYP='%s' +145='%c' +172='%s'\n",
                 i, e ? "BE" : "LE", tm.y, tm.mo, tm.d, tm.h, tm.mi, tm.s, rt, fr, ty);
          shown++;
        }
      }
      big_endian = save;
    }
  }
}

/* felezeses kereses: az idoablak elotti blokkra all. 1 = sikerult */
static int blk_seek_time(const char *fn)
{
  long long t0, target;
  long hi, lo, last, mid;
  if (blk_open(fn)) return 0;
  blk_probes = 0;
  if (o_diag) blk_diag();
  /* bajtsorrend az elso blokkbol */
  big_endian = 1;
  t0 = ts_near(0, 63);
  if (!t0) { big_endian = 0; t0 = ts_near(0, 63); }
  if (!t0) {
    blk_close(); big_endian = 1;
    strcpy(seek_fail, "a fajl elejen nem ismert fel tranzakcios rekordot");
    return 0;
  }
  endian_known = 1;
  file_t0 = t0;

  /* utolso blokk: exponencialis, majd felezeses kereses */
  hi = 1;
  { static unsigned char b[65536];
    while (hi < (1L << 30) && blk_read((long long)hi * blk_sz, b, blk_sz) > 0) { hi *= 2; blk_probes++; }
    lo = hi / 2;
    while (hi - lo > 1) {
      mid = lo + (hi - lo) / 2;
      blk_probes++;
      if (blk_read((long long)mid * blk_sz, b, blk_sz) > 0) lo = mid; else hi = mid;
    }
    last = lo;
  }

  /* cel: az idoablak elso elofordulasa a fajl eleje utan, 2 perc rahagyassal */
  {
    long sod0 = sec_of_day(t0);
    if (in_window(sod0)) target = 0;
    else target = t0 + (long long)((f_from - sod0 + 86400L) % 86400L) * 1000000LL - 120000000LL;
  }

  /* elso blokk, amelynek ideje >= cel */
  lo = 0; hi = last + 1;
  if (target > 0) {
    while (lo < hi) {
      long long t;
      mid = lo + (hi - lo) / 2;
      t = ts_near(mid, last);
      if (!t) {                                    /* nem ismerheto fel: vegigolvasas */
        blk_close();
        sprintf(seek_fail, "a %ld. blokkban nem ismert fel rekordot", mid);
        return 0;
      }
      if (t < target) lo = mid + 1; else hi = mid;
    }
  }
  blk_start = lo > 0 ? lo - 1 : 0;                /* egy blokkal korabban kezdunk */
  next_blk = blk_start;
  chunk_n = chunk_pos = 0; chunk_blks = CHUNK_BLKS; blk_recs = 0;
  blk_mode = 1;
  return 1;
}

/* kovetkezo rekord a blokkokbol */
static int blk_next(unsigned char *buf, int max)
{
  for (;;) {
    long i, j, len;
    if (chunk_pos >= chunk_n) {
      chunk_n = blk_read((long long)next_blk * blk_sz, chunk, (long)chunk_blks * blk_sz);
      if (chunk_n < 0 && chunk_blks > 1) {          /* nagy olvasas nem engedett */
        chunk_blks = 1;
        chunk_n = blk_read((long long)next_blk * blk_sz, chunk, (long)blk_sz);
      }
      chunk_pos = 0;
      if (chunk_n <= 0) return chunk_n < 0 ? -1 : 0;
      next_blk += (chunk_n + blk_sz - 1) / blk_sz;
    }
    /* rekord keresese a blokkhatarig */
    {
      long bend = (chunk_pos / blk_sz + 1) * blk_sz;
      if (bend > chunk_n) bend = chunk_n;
      for (i = chunk_pos; i + 10 <= bend; i++)
        if (is_rec_start(chunk + i, bend - i)) break;
      if (i + 10 > bend) { chunk_pos = bend; continue; }      /* kovetkezo blokk */
      /* a rekord vege: a kovetkezo rekord eleje vagy a blokk vege */
      for (j = i + (num(chunk + i + 8, 2) == 1 ? cur_L->min_len : 60); j + 10 <= bend; j++)
        if (is_rec_start(chunk + j, bend - j)) break;
      if (j + 10 > bend) j = bend;
      len = j - i;
      if (len > max) len = max;
      memcpy(buf, chunk + i, (size_t)len);
      chunk_pos = j;
      blk_recs++;
      return (int)len;
    }
  }
}

/* ---------- olvasas: blokkos ugras vagy sima sorrendi ---------- */
static const char *rd_name;
static int blk_fallback = 0;         /* 1 = ugras utan vegigolvasasra valtottunk */
static int rd_open(const char *name)
{
  rd_name = name;
  blk_fallback = 0;
  seek_fail[0] = 0;
  blk_mode = 0; blk_start = -1; file_t0 = 0;
  if (o_rawtest) {                       /* teszt: nyers blokkfajl elejetol */
    if (blk_open(name)) { perror(name); return -1; }
    next_blk = 0; chunk_n = chunk_pos = 0; chunk_blks = CHUNK_BLKS; blk_recs = 0; blk_mode = 1;
    if (f_from >= 0 && !o_full) { blk_close(); blk_mode = 0; if (blk_seek_time(name)) return 0; return -1; }
    return 0;
  }
#ifdef __TANDEM
  /* idoszuresnel ugras az idoablak elejere (felezessel) */
  if (f_from >= 0 && !o_full && blk_seek_time(name)) return 0;
#endif
  return seq_open(name);
}
static int rd_next(unsigned char *buf, int max)
{
  if (blk_mode) {
    int r = blk_next(buf, max);
    if (r > 0 || blk_recs > 0 || o_rawtest) return r;
    /* az ugras utan egy rekord sem jott: sima vegigolvasas */
    blk_close(); blk_mode = 0; blk_start = -1; blk_fallback = 1;
    big_endian = 1; endian_known = 0;
    if (seq_open(rd_name)) return -1;
  }
  return seq_next(buf, max);
}
static void rd_close(void)
{
  if (blk_mode) blk_close(); else seq_close();
  blk_mode = 0;
}

/* fajltipus a nevbol: az utolso '.' utani resz PO... vagy TL... */
static char kind_of(const char *fn)
{
  const char *p = strrchr(fn, '.'), *q = strrchr(fn, '/');
  if (q && (!p || q > p)) p = q;
  p = p ? p + 1 : fn;
  if (toupper((unsigned char)p[0]) == 'P' && toupper((unsigned char)p[1]) == 'O') return 'P';
  if (toupper((unsigned char)p[0]) == 'T' && toupper((unsigned char)p[1]) == 'L') return 'T';
  return 0;
}

static void line(int n, char c) { while (n-- > 0) putchar(c); putchar('\n'); }

static void print_filters(void)
{
  char b[200] = "";
  if (f_from >= 0) {
    char t[80];
    sprintf(t, "ido %02ld:%02ld:%02ld-%02ld:%02ld:%02ld, ", f_from / 3600, f_from / 60 % 60,
            f_from % 60, f_to / 3600, f_to / 60 % 60, f_to % 60);
    strcat(b, t);
  }
  if (f_card) strcat(b, "kartya, ");
  if (f_term[0]) { strcat(b, "terminal "); strcat(b, f_term); strcat(b, "*, "); }
  if (n_resp) strcat(b, "valaszkod, ");
  if (f_decl) strcat(b, "csak elutasitott, ");
  if (f_tcde >= 0) { char t[20]; sprintf(t, "tr.kod %02d, ", f_tcde); strcat(b, t); }
  if (b[0]) b[strlen(b) - 2] = 0; else strcpy(b, "nincs (minden tranzakcio)");
  printf("Szures: %s\n", b);
}

/* ---------- egy fajl listazasa ---------- */
static void list_file(const char *fn, char kind)
{
  static unsigned char r[MAXREC];
  const Layout *L = kind == 'P' ? &L_PTLF : &L_TLF;
  long n_tran = 0, n_hit = 0, n_hit_decl = 0, n_shown = 0;
  int len, lastday = -1, was_in = 0, stop_time = 0, stop_limit = 0;
  long long last_in = 0, first_ts = 0, last_ts = 0, fstart = 0;
  char a[48], b[48];

  endian_known = 0; big_endian = 1;
  cur_L = L;
  if (rd_open(fn)) return;

  if (!o_csv) {
    printf("\n");
    line(RPT_W, '=');
    printf("TRNLIST - %s  %s\n", L->name, fn);
    print_filters();
    line(RPT_W, '=');
    printf("%-8s %-4s %-2s %-19s %13s %-3s %-3s %-12s %s\n", "Ido", "Tip", "Tr",
           "Kartya", "Osszeg", "Dev", "Vk", "Terminal", "Dontes");
    line(RPT_W, '-');
  }

  while ((len = rd_next(r, MAXREC)) > 0) {
    long long ts, et, rt;
    long typ, tcde, resp;
    int crn, appr;
    TmX t;
    char pan[20], pm[20], term[17], cn[16], dt[48], tm[48];

    if (len < L->min_len) continue;
    if (num(r + L->rec_typ, 2) != 1) continue;
    if (L->rec_frmt >= 0 && r[L->rec_frmt] != '5') continue;

    ts = get64(r);
    if (!endian_known) {
      if (!plausible_ts(ts)) {
        big_endian = !big_endian;
        if (!plausible_ts(get64(r))) big_endian = !big_endian;
      }
      endian_known = 1;
      ts = get64(r);
    }
    typ = num(r + L->typ, 4);
    if (kind == 'P' && (typ == 500 || typ == 412)) continue;
    if (kind == 'T' && (typ == 9980 || typ == 9991)) continue;
    n_tran++;

    /* --- szurok --- */
    if (!first_ts) first_ts = ts;
    if (!fstart) fstart = (blk_start >= 0 && file_t0) ? file_t0 : ts;   /* a fajl valodi eleje */
    last_ts = ts;
    if (f_from >= 0) {
      long sod = sec_of_day(ts);
      if (in_window(sod)) { was_in = 1; last_in = ts; }
      else {
        /* A fajl idorendben van es kb. 24 orat fog at. Ha az idoablak lezarult
           (10 perc turessel a kesve irt rekordok miatt), es a kovetkezo
           elofordulasa mar a fajl kezdete utani 25 oran tul lenne, nem
           olvasunk tovabb. (Az ablak a fajl elejen es vegen is lehet,
           pl. 20:30-as napvaltasnal 20:00-21:00.) */
        if (was_in && ts - last_in > 600000000LL) {
          long wait = (f_from - sod + 86400L) % 86400L;
          long long next_start = ts + (long long)wait * 1000000LL;
          if (!o_full && next_start - fstart > 25LL * 3600LL * 1000000LL) { stop_time = 1; break; }
        }
        continue;
      }
    }

    tcde = num(r + L->tcde, 2);
    if (f_tcde >= 0 && tcde != f_tcde) continue;

    resp = num(r + L->resp, 3);
    appr = approved(resp, kind);
    if (f_decl && appr) continue;
    if (n_resp) {
      int k, ok = 0;
      for (k = 0; k < n_resp; k++) if (f_resp[k] == resp) ok = 1;
      if (!ok) continue;
    }

    text(r + L->term_id, 16, term);
    if (f_term[0] && strncmp(term, f_term, strlen(f_term)) != 0) continue;

    text(r + L->pan, 19, pan);
    if (f_card) {
      size_t pl = strlen(f_card_pre), sl = strlen(f_card_suf), n = strlen(pan);
      if (pl && strncmp(pan, f_card_pre, pl) != 0) continue;
      if (sl && (n < sl || strcmp(pan + n - sl, f_card_suf) != 0)) continue;
    }

    if (o_max > 0 && n_shown >= o_max) {           /* van meg talalat */
      stop_limit = 1;
      /* idoszuresnel az idoablak vegeig tovabb szamol (pontos darabszam),
         -s eseten a fajl vegeig; egyebkent itt megall */
      if (!o_full && f_from < 0) break;
      n_hit++;                                     /* szamol, de nem ir */
      if (!appr) n_hit_decl++;
      continue;
    }
    n_hit++;
    if (!appr) n_hit_decl++;
    n_shown++;
    if (ts_local(ts, &t) != 0) t.y = t.mo = t.d = t.h = t.mi = t.s = 0;

    /* --- kiiras --- */
    mask_pan(pan, pm);
    crn = (int)num(r + L->crncy, 3);
    if (crn < 0) crn = 0;
    if (crncy_name(crn)) strcpy(cn, crncy_name(crn)); else sprintf(cn, "%03d", crn);
    sprintf(dt, "%04d.%02d.%02d", t.y, t.mo, t.d);
    sprintf(tm, "%02d:%02d:%02d", t.h, t.mi, t.s);
    et = get64(r + L->exit_tim); rt = get64(r + L->reentry_tim);

    if (o_csv) {
      char loc[26], city[14], cc[3];
      text(r + L->name_loc, 25, loc);
      text(r + L->city, 13, city);
      text(r + L->cntry, 2, cc);
      printf("%s;%s;%s;%04ld;%02ld;%s;%s;%s;%03ld;%s;%s;%s;%s;%s;%s;",
             kind == 'P' ? "POS" : "ATM", dt, tm, typ, tcde, pm,
             fmt_amt(get64(r + L->amt1), crn, 0, a), cn, resp, appr ? "I" : "N",
             term, loc, city, cc, decision(r, L));
      if (et > 0 && rt >= et) printf("%lld", (rt - et) / 1000);
      printf(";%02ld\n", num(r + L->rvsl, 2) < 0 ? 0 : num(r + L->rvsl, 2));
      continue;
    }

    if (t.d != lastday) {                 /* datumvaltas jelzese */
      if (lastday >= 0) printf("--- %s ---\n", dt);
      lastday = t.d;
    }
    printf("%-8s %04ld %02ld %-19s %13s %-3s %03ld%c%-12.12s %s\n", tm, typ,
           tcde < 0 ? 0 : tcde, pm, fmt_amt(get64(r + L->amt1), crn, 1, a), cn, resp,
           appr ? ' ' : '*', term, decision(r, L));
    if (o_long) {
      char loc[26], cc[3], rs[8];
      long rv = num(r + L->rvsl, 2);
      text(r + L->name_loc, 25, loc);
      text(r + L->cntry, 2, cc);
      if (et > 0 && rt >= et) sprintf(b, "%lld ms", (rt - et) / 1000); else strcpy(b, "-");
      if (rv > 0) sprintf(rs, "%02ld", rv); else strcpy(rs, "-");
      printf("         %-25.25s %-2s  valaszido %-9s sztorno ok %s\n", loc, cc, b, rs);
    }
  }
  rd_close();

  if (!o_csv) {
    line(RPT_W, '-');
    {
      TmX t1, t2; char h1[16] = "-", h2[16] = "-";
      if (first_ts && !ts_local(first_ts, &t1)) sprintf(h1, "%02d:%02d:%02d", t1.h, t1.mi, t1.s);
      if (last_ts && !ts_local(last_ts, &t2)) sprintf(h2, "%02d:%02d:%02d", t2.h, t2.mi, t2.s);
      if (stop_limit && !o_full && f_from < 0)
        printf("Talalat: %s+ tranzakcio (az elso %s; ebbol elutasitott %s)\n",
               fmt_n(n_hit, a), fmt_n(n_shown, b), fmt_n(n_hit_decl, a + 24));
      else if (stop_limit)
        printf("Talalat: %s tranzakcio (ebbol elutasitott %s), megjelenitve az elso %s\n",
               fmt_n(n_hit, a), fmt_n(n_hit_decl, b), fmt_n(n_shown, a + 24));
      else
        printf("Talalat: %s tranzakcio (ebbol elutasitott %s)\n", fmt_n(n_hit, a), fmt_n(n_hit_decl, b));
      printf("Atvizsgalva: %s rekord, %s - %s%s%s\n", fmt_n(n_tran, a), h1, h2,
             (stop_limit && !o_full && f_from < 0) || stop_time ? " (tovabb nem olvastam)"
                                                  : " (a teljes fajl)", "");
      if (stop_limit && !o_full && f_from < 0)
        printf("Mindet listazni: -n 0 (spoolerbe / CSV-be); teljes osszesites: -s\n");
      else if (stop_limit)
        printf("Mindet listazni: -n 0 (spoolerbe / CSV-be)\n");
      if (blk_start >= 0)
        printf("Olvasas: ugras a %ld. blokkra (%d probaolvasas)\n", blk_start, blk_probes);
      else if (blk_fallback)
        printf("Olvasas: az ugras nem sikerult, vegigolvasas\n");
#ifdef __TANDEM
      else
        printf("Olvasas: %s\n", sbb_on ? "blokkos (SBB)" : "rekordonkent");
#endif
      if (seek_fail[0] && f_from >= 0)
        printf("Ugras nem sikerult: %.60s\n", seek_fail);
    }
    printf("* = elutasitott valaszkod. Dontes: Host / B24 / STIP / HostIF / ICHG\n");
  }
}

static void usage(void)
{
  fprintf(stderr,
    "Hasznalat: TRNLIST [opciok] fajl [fajl2]\n"
    "  -i hh:mm-hh:mm  idotartomany      -k 1234 | 411111* | 411111*1234  kartya\n"
    "  -m ATM0001      terminal eleje    -r 051,055   valaszkod(ok)\n"
    "  -e              csak elutasitott  -x 10        tranzakciokod\n"
    "  -l              hosszu lista      -c           CSV kimenet\n"
    "  -n 500          max. talalat (0 = mind)   -t P|T  fajltipus (PTLF/TLF)\n"
    "  -s              teljes osszesites (vegigolvassa a fajlt)\n");
  exit(2);
}

int main(int argc, char *argv[])
{
  int i, k;
  for (i = 1; i < argc; i++) {
    char *a = argv[i];
    if (a[0] == '-' && a[1] && !a[2]) {
      char o = (char)tolower((unsigned char)a[1]);
      if (o == 'e') { f_decl = 1; continue; }
      if (o == 'l') { o_long = 1; continue; }
      if (o == 'c') { o_csv = 1; continue; }
      if (o == 's') { o_full = 1; continue; }
      if (a[1] == 'B') { o_rawtest = 1; continue; }     /* teszt: nyers blokkfajl */
      if (o == 'v') { o_diag = 1; continue; }
      if (i + 1 >= argc) usage();
      a = argv[++i];
      switch (o) {
      case 'i': {
        char *d = strchr(a, '-');
        if (!d) usage();
        *d = 0;
        f_from = parse_hms(a); f_to = parse_hms(d + 1);
        if (f_from < 0 || f_to < 0) usage();
        if (!strchr(d + 1 + 3, ':')) f_to += 59;   /* hh:mm -> a perc vegeig */
        if (f_to >= 86400) f_to = 86399;
        /* ha a -ig korabbi, mint a -tol: ejfelen atnyulo szakasz (pl. 23:00-01:00) */
        break;
      }
      case 'k': {
        char *s = strchr(a, '*');
        f_card = 1;
        if (s) {
          *s = 0;
          sprintf(f_card_pre, "%.19s", a); sprintf(f_card_suf, "%.19s", s + 1);
        } else if (strlen(a) <= 4) sprintf(f_card_suf, "%.4s", a);
        else { sprintf(f_card_pre, "%.19s", a); sprintf(f_card_suf, "%.19s", a); }
        break;
      }
      case 'm': sprintf(f_term, "%.16s", a); for (k = 0; f_term[k]; k++) f_term[k] = (char)toupper((unsigned char)f_term[k]); break;
      case 'r': {
        char *p = a;
        while (*p && n_resp < 32) { f_resp[n_resp++] = atoi(p); p = strchr(p, ','); if (!p) break; p++; }
        break;
      }
      case 'x': f_tcde = atoi(a); break;
      case 'b': blk_sz = atoi(a); if (blk_sz < 512 || blk_sz > 8192) usage(); break;
      case 'n': o_max = atol(a); o_max_set = 1; break;
      case 't': o_type = (char)toupper((unsigned char)a[0]); if (o_type != 'P' && o_type != 'T') usage(); break;
      default: usage();
      }
    } else if (n_files < 2) files[n_files++] = a;
    else usage();
  }
  if (!n_files) usage();
  if (o_csv && !o_max_set) o_max = 0;

  if (o_csv)
    printf("tipus;datum;ido;uzenet;tr_kod;kartya;osszeg;deviza;valaszkod;engedelyezett;"
           "terminal;hely;varos;orszag;dontes;valaszido_ms;sztorno_ok\n");

  for (i = 0; i < n_files; i++) {
    char kind = o_type ? o_type : kind_of(files[i]);
    if (!kind) {
      fprintf(stderr, "Nem derul ki a fajltipus: %s (add meg: -t P vagy -t T)\n", files[i]);
      continue;
    }
    list_file(files[i], kind);
  }
  return 0;
}
