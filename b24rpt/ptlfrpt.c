/*
 * PTLFRPT - BASE24-pos (classic) PTLF napi riport
 *
 * Egy napi PTLF fajl (POyymmdd) egyszeri vegigolvasasaval keszit:
 *   1. Napi osszesitot (tipus, engedelyezes, dontes helye, osszegek devizankent)
 *   2. Valaszkod-elemzest (dontes helye szerinti bontassal)
 *   3. Oranketi forgalmat (STIP, idotullepeses sztorno, host valaszido)
 *   4. Mellekletkent tranzakcios orszaglistat
 *
 * Hasznalat (Guardian):
 *   RUN PTLFRPT /OUT $S.#PTLFRPT/ [-a nnn] [-c HU] [-r respfile] [-z perc]
 *                                [-d n] $DATA.PTLF.PO261114
 *
 *   -a nnn     elso ELUTASITO valaszkod (alap: 050; ennel kisebb = engedelyezett)
 *   -c XX      hazai orszagkod (alap: HU) a belfold/kulfold bontashoz
 *   -r fajl    valaszkod-megnevezesek ("nnn szoveg" soronkent, # = megjegyzes)
 *   -z perc    idoeltolas GMT-hez kepest percben (alap Guardianon: a rendszer
 *              helyi ideje; mashol: 0)
 *   -d n       diagnosztika: az elso n tranzakcios rekord mezoinek kiirasa
 *              (kartyaszam, track2, lejarat es szamlaszam NELKUL), majd kilep
 *
 * A mezopoziciok a BASE24-pos Release 6.0 DDLFPTLF / DDLGDEFS alapjan:
 * HEAD = 172 bajt, AUTH = 614 bajt, minimalis rekordhossz 786 bajt.
 *
 * A kimenet szandekosan ekezet nelkuli (ASCII), hogy spooleren is olvashato
 * legyen. Kartyaadatot (PAN, TRACK2, EXP-DAT, ACCT) a program nem ir ki.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __TANDEM
/* Guardian alatt a fajl neve $SYSTEM.SYSTEM.CEXTDECS (nincs "h" a vegen),
   ezert ".h" nelkul, a szukseges szekciok felsorolasaval kell behuzni. */
#include <cextdecs(FILE_OPEN_,READX,FILE_GETINFO_,FILE_CLOSE_,CONVERTTIMESTAMP,INTERPRETTIMESTAMP)>
#include <tal.h>
#endif

/* ---------- PTLF mezopoziciok (0-tol szamolt bajt offset) ---------- */
#define OFF_DAT_TIM       0    /* HEAD.DAT-TIM         BINARY 64          */
#define OFF_REC_TYP       8    /* HEAD.REC-TYP         X(2)               */
#define OFF_REC_FRMT      145  /* HEAD.TKEY.RKEY.REC-FRMT X               */
#define OFF_TYP           172  /* AUTH.TYP             9(4)               */
#define OFF_RTE_STAT      176  /* AUTH.RTE-STAT        99                 */
#define OFF_ORIGINATOR    178  /* AUTH.ORIGINATOR      X                  */
#define OFF_RESPONDER     179  /* AUTH.RESPONDER       X                  */
#define OFF_ISS_CDE       180  /* AUTH.ISS-CDE         XX                 */
#define OFF_ENTRY_TIM     182  /* AUTH.ENTRY-TIM       BINARY 64          */
#define OFF_EXIT_TIM      190  /* AUTH.EXIT-TIM        BINARY 64          */
#define OFF_REENTRY_TIM   198  /* AUTH.RE-ENTRY-TIM    BINARY 64          */
#define OFF_TRAN_DAT      206  /* AUTH.TRAN-DAT        YYMMDD             */
#define OFF_TRAN_TIM      212  /* AUTH.TRAN-TIM        HHMMSSTT           */
#define OFF_TERM_CITY     297  /* AUTH.TERM-CITY       X(13)              */
#define OFF_TERM_CNTRY    313  /* AUTH.TERM-CNTRY-CDE  X(2)               */
#define OFF_ORIG          370  /* AUTH.ORIG            X(4)               */
#define OFF_DEST          374  /* AUTH.DEST            X(4)               */
#define OFF_TC            378  /* AUTH.TRAN-CDE.TC     XX                 */
#define OFF_T             380  /* AUTH.TRAN-CDE.T      X                  */
#define OFF_RESP_CDE      405  /* AUTH.RESP-CDE        999                */
#define OFF_AMT_1         408  /* AUTH.AMT-1           BINARY 64          */
#define OFF_AMT_2         416  /* AUTH.AMT-2           BINARY 64          */
#define OFF_AUTHORIZER    516  /* AUTH.AUTHORIZER      X(16)              */
#define OFF_AUTH_IND      532  /* AUTH.AUTH-IND        X                  */
#define OFF_APPRV_CDE     539  /* AUTH.APPRV-CDE       X(8)               */
#define OFF_RVRL_CDE      582  /* AUTH.RVRL-CDE        99                 */
#define OFF_ENTRY_MDE     589  /* AUTH.PT-SRV-ENTRY-MDE 999               */
#define OFF_ORIG_CRNCY    593  /* AUTH.ORIG-CRNCY-CDE  9(3)               */
#define OFF_EXCP_RSN      702  /* AUTH.EXCP-RSN-CDE    X(3)               */
#define MIN_REC_LEN       786

#define MAXREC            4096
#define RT_MAX_MS         5000   /* valaszido hisztogram felso hatara (ms) */
#define MAX_CRNCY         64
#define STIP_GAP          5      /* STIP-idoszakok osszevonasa (perc) */
#define MAX_CNTRY         400
#define MAX_AUTHZ         64

/* JULIANTIMESTAMP ertek 1970-01-01 00:00:00 GMT-kor (mikroszekundum) */
#define JTS_UNIX_EPOCH    210866760000000000LL
#define JTS_MIN_PLAUS     211813000000000000LL  /* kb. 2000 */
#define JTS_MAX_PLAUS     214969000000000000LL  /* kb. 2100 */

/* ---------- Tranzakciotipusok ---------- */
enum { T_PUR, T_PRE, T_COMP, T_MOTO, T_REF, T_CASH, T_CBK, T_VER, T_BAL,
       T_ADJ, T_REV, T_OTH, NTYPE };
static const char *type_name[NTYPE] = {
  "Vasarlas", "Eloengedelyezes", "Eloeng. lezaras", "Levelezeses/tel.",
  "Visszaterites", "Keszpenzfelvetel", "Vasarlas+cashback", "Kartyaellenorzes",
  "Egyenleglekerdezes", "Korrekcio", "Sztorno (0420)", "Egyeb" };

/* ---------- Dontes helye ---------- */
enum { D_HOST, D_B24, D_STIP, D_HIF, D_NET, D_OTH, NDEC };
static const char *dec_name[NDEC] = {
  "Host", "BASE24", "STIP", "HostIF", "ICHG", "Egyeb" };
static const char *dec_long[NDEC] = {
  "Host (online)", "BASE24 Authorization", "STIP (host nem elerheto)",
  "Host Interface", "Interchange (RESP. 6/7)", "Egyeb" };

/* ---------- Opciok ---------- */
static int  opt_appr_limit = 50;
static char opt_home[3] = "HU";
static const char *opt_respfile = NULL;
static int  opt_tz_set = 0;
static long opt_tz_min = 0;
static int  opt_dump = 0;
static const char *opt_file = NULL;

/* ---------- Statisztikak ---------- */
typedef struct { long cnt, appr; } CntAppr;

static long n_rec, n_tran, n_short, n_setl, n_other, n_excp[4], n_skip_typ;
static CntAppr by_type[NTYPE];
static CntAppr by_dec[NDEC];
static CntAppr dom, frn;
static long resp_cnt[1000][NDEC];
static char *resp_desc[1000];

typedef struct {
  long cnt, decl, stip, tmo_rev;
  long long rt_sum_us; long rt_n;
  unsigned long rt_hist[RT_MAX_MS + 1];
} Hour;
static Hour hour[24];
static long minute_cnt[1440], minute_stip[1440];
static long second_cnt[86400];
static long long first_ts, last_ts;

typedef struct {
  int code; long cnt;
  long long pur, ref, rev, pre;   /* engedelyezett osszegek, minor unit */
} Crncy;
static Crncy crncy[MAX_CRNCY]; static int n_crncy;

typedef struct { char cc[3]; long cnt, decl; } Cntry;
static Cntry cntry[MAX_CNTRY]; static int n_cntry;

/* engedelyezo (AUTH.AUTHORIZER) + RESPONDER szerinti bontas */
typedef struct { char name[17]; char resp; long cnt, appr; } Authz;
static Authz authz[MAX_AUTHZ]; static int n_authz;

static int big_endian = 1, endian_known = 0;

/* ---------- Segedfuggvenyek ---------- */
static long long get64(const unsigned char *p)
{
  unsigned long long v = 0; int i;
  if (big_endian) for (i = 0; i < 8; i++) v = (v << 8) | p[i];
  else            for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return (long long)v;
}

static int plausible_ts(long long t) { return t > JTS_MIN_PLAUS && t < JTS_MAX_PLAUS; }

static long num(const unsigned char *p, int len)    /* -1, ha nem szam */
{
  long v = 0; int i;
  for (i = 0; i < len; i++) {
    if (p[i] < '0' || p[i] > '9') return -1;
    v = v * 10 + (p[i] - '0');
  }
  return v;
}

/* szam ezres tagolassal (szokoz) */
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

/* osszeg minor unitbol, tizedesvesszovel */
static char *fmt_amt(long long v, int code, char *out)
{
  int e = crncy_exp(code); long long div = 1, ip, fp; char ib[40]; int k;
  for (k = 0; k < e; k++) div *= 10;
  ip = v / div; fp = v % div; if (fp < 0) fp = -fp;
  fmt_n(ip, ib);
  if (v < 0 && ip == 0) { memmove(ib + 1, ib, strlen(ib) + 1); ib[0] = '-'; }
  if (e == 0) strcpy(out, ib);
  else sprintf(out, "%s,%0*lld", ib, e, fp);
  return out;
}

/* lebegopontos szam tizedesvesszovel */
static char *fmt_d(double v, int dec, char *out)
{
  char *c;
  sprintf(out, "%.*f", dec, v);
  if ((c = strchr(out, '.')) != NULL) *c = ',';
  return out;
}

static char *pct(long a, long b, char *out)
{
  if (b == 0) strcpy(out, "-");
  else { sprintf(out, "%.2f%%", 100.0 * a / b); { char *c = strchr(out, '.'); if (c) *c = ','; } }
  return out;
}

static const char *crncy_name(int c)
{
  switch (c) {
  case 348: return "HUF"; case 978: return "EUR"; case 840: return "USD";
  case 826: return "GBP"; case 756: return "CHF"; case 203: return "CZK";
  case 985: return "PLN"; case 946: return "RON"; case 941: return "RSD";
  case 975: return "BGN"; case 208: return "DKK"; case 752: return "SEK";
  case 578: return "NOK"; case 392: return "JPY"; case 980: return "UAH";
  case 949: return "TRY"; case 124: return "CAD"; case 36:  return "AUD";
  default: return NULL;
  }
}

static const char *cntry_name(const char *cc)
{
  static const char *t[][2] = {
    {"HU","Magyarorszag"},{"AT","Ausztria"},{"DE","Nemetorszag"},
    {"SK","Szlovakia"},{"RO","Romania"},{"HR","Horvatorszag"},
    {"RS","Szerbia"},{"SI","Szlovenia"},{"UA","Ukrajna"},{"CZ","Csehorszag"},
    {"PL","Lengyelorszag"},{"IT","Olaszorszag"},{"ES","Spanyolorszag"},
    {"FR","Franciaorszag"},{"GB","Egyesult Kiralysag"},{"IE","Irorszag"},
    {"NL","Hollandia"},{"BE","Belgium"},{"LU","Luxemburg"},{"CH","Svajc"},
    {"GR","Gorogorszag"},{"PT","Portugalia"},{"SE","Svedorszag"},
    {"DK","Dania"},{"NO","Norvegia"},{"FI","Finnorszag"},{"BG","Bulgaria"},
    {"TR","Torokorszag"},{"US","Egyesult Allamok"},{"CA","Kanada"},
    {"MT","Malta"},{"CY","Ciprus"},{"EG","Egyiptom"},{"AE","Egyesult Arab Em."},
    {"TH","Thaifold"},{"CN","Kina"},{"JP","Japan"},{"ME","Montenegro"},
    {"BA","Bosznia-Hercegovina"},{"MK","Eszak-Macedonia"},{"AL","Albania"},
    {"EE","Esztorszag"},{"LV","Lettorszag"},{"LT","Litvania"},{"IS","Izland"},
    {"SG","Szingapur"},{"AU","Ausztralia"},{"MX","Mexiko"},{"IL","Izrael"},
    {"??","(ures/ismeretlen)"},
    {NULL,NULL}};
  int i;
  for (i = 0; t[i][0]; i++) if (strcmp(t[i][0], cc) == 0) return t[i][1];
  return "";
}

/* ---------- Ido ---------- */
typedef struct { int y, mo, d, h, mi, s; } TmX;

static int ts_local(long long jts, TmX *t)
{
#ifdef __TANDEM
  if (!opt_tz_set) {
    short dt[8];
    long long l = CONVERTTIMESTAMP(jts, 0);  /* 0 = GMT -> helyi civil ido */
    INTERPRETTIMESTAMP(l, dt);
    t->y = dt[0]; t->mo = dt[1]; t->d = dt[2];
    t->h = dt[3]; t->mi = dt[4]; t->s = dt[5];
    return 0;
  }
#endif
  {
    long long us = jts - JTS_UNIX_EPOCH;
    time_t s = (time_t)(us / 1000000LL) + (time_t)opt_tz_min * 60;
    struct tm *g = gmtime(&s);
    if (!g) return -1;
    t->y = g->tm_year + 1900; t->mo = g->tm_mon + 1; t->d = g->tm_mday;
    t->h = g->tm_hour; t->mi = g->tm_min; t->s = g->tm_sec;
    return 0;
  }
}

static char *fmt_ts(long long jts, char *out)
{
  TmX t;
  if (!jts || ts_local(jts, &t)) strcpy(out, "-");
  else sprintf(out, "%04d.%02d.%02d %02d:%02d:%02d", t.y, t.mo, t.d, t.h, t.mi, t.s);
  return out;
}

/* ---------- Fajl olvasas ---------- */
#ifdef __TANDEM
static short fnum = -1;
static int rd_open(const char *name)
{
  short err = FILE_OPEN_(name, (short)strlen(name), &fnum, 1 /*read*/, 0 /*shared*/);
  if (err) { fprintf(stderr, "FILE_OPEN_ hiba %d: %s\n", err, name); return -1; }
  return 0;
}
static int rd_next(unsigned char *buf, int max)
{
  unsigned short cnt = 0; short err = 0;
  READX(fnum, (char *)buf, (unsigned short)max, &cnt);
  FILE_GETINFO_(fnum, &err);               /* 0 = rendben, 1 = EOF */
  if (err != 0) {
    if (err == 1) return 0;                 /* EOF */
    fprintf(stderr, "READX hiba %d\n", err);
    return -1;
  }
  return cnt;
}
static void rd_close(void) { if (fnum >= 0) FILE_CLOSE_(fnum); }
#else
/* Teszteleshez: minden rekord elott 2 bajtos (big-endian) hossz */
static FILE *rf;
static int rd_open(const char *name)
{
  rf = fopen(name, "rb");
  if (!rf) { perror(name); return -1; }
  return 0;
}
static int rd_next(unsigned char *buf, int max)
{
  unsigned char lb[2]; int len;
  if (fread(lb, 1, 2, rf) != 2) return 0;
  len = (lb[0] << 8) | lb[1];
  if (len > max) { fprintf(stderr, "tul hosszu rekord: %d\n", len); return -1; }
  if ((int)fread(buf, 1, len, rf) != len) return -1;
  return len;
}
static void rd_close(void) { if (rf) fclose(rf); }
#endif

/* ---------- Valaszkod-megnevezesek ---------- */
static void load_resp(const char *fn)
{
  FILE *f = fopen(fn, "r"); char line[256];
  if (!f) { fprintf(stderr, "Nem nyithato meg: %s (megnevezesek nelkul folytatom)\n", fn); return; }
  while (fgets(line, sizeof line, f)) {
    char *p = line, *e; long c;
    while (*p == ' ') p++;
    if (*p == '#' || *p == '\n' || *p == 0) continue;
    c = strtol(p, &e, 10);
    if (e == p || c < 0 || c > 999) continue;
    while (*e == ' ' || *e == '\t') e++;
    e[strcspn(e, "\r\n")] = 0;
    free(resp_desc[c]);
    resp_desc[c] = (char *)malloc(strlen(e) + 1);
    if (resp_desc[c]) strcpy(resp_desc[c], e);
  }
  fclose(f);
}

/* ---------- Osztalyozas ---------- */
static int classify(long typ, long tc)
{
  if (typ == 420) return T_REV;
  switch (tc) {
  case 10: return T_PUR;  case 11: return T_PRE;  case 12: return T_COMP;
  case 13: return T_MOTO; case 14: return T_REF;  case 15: return T_CASH;
  case 16: return T_VER;  case 17: return T_BAL;  case 18: return T_CBK;
  case 21: case 22: case 23: case 24: return T_ADJ;
  default: return T_OTH;
  }
}

static int decision(const unsigned char *r)
{
  int resp = r[OFF_RESPONDER];
  long rte = num(r + OFF_RTE_STAT, 2);
  if (resp == '5') return D_HOST;
  if (resp == '3') return (rte > 0) ? D_STIP : D_B24;
  if (resp == '4') return D_HIF;
  if (resp == '6' || resp == '7') return D_NET;
  return D_OTH;
}

static Crncy *get_crncy(int code)
{
  int i;
  for (i = 0; i < n_crncy; i++) if (crncy[i].code == code) return &crncy[i];
  if (n_crncy >= MAX_CRNCY) return NULL;
  memset(&crncy[n_crncy], 0, sizeof(Crncy));
  crncy[n_crncy].code = code;
  return &crncy[n_crncy++];
}

static Cntry *get_cntry(const char *cc)
{
  int i;
  for (i = 0; i < n_cntry; i++) if (strcmp(cntry[i].cc, cc) == 0) return &cntry[i];
  if (n_cntry >= MAX_CNTRY) return NULL;
  memset(&cntry[n_cntry], 0, sizeof(Cntry));
  strcpy(cntry[n_cntry].cc, cc);
  return &cntry[n_cntry++];
}

static void printable(const unsigned char *p, int len, char *out)
{
  int i;
  for (i = 0; i < len; i++) out[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
  out[len] = 0;
}

static void dump_rec(const unsigned char *r, int len, long n)
{
  char a[64], b[64], c[64], d[64], e[64];
  long long et = get64(r + OFF_EXIT_TIM), rt = get64(r + OFF_REENTRY_TIM);
  printable(r + OFF_REC_TYP, 2, a);
  printable(r + OFF_TYP, 4, b);
  printable(r + OFF_TC, 2, c);
  printable(r + OFF_RESP_CDE, 3, d);
  printf("#%ld hossz=%d REC-TYP=%s FRMT=%c TYP=%s TC=%s T=%c RESP=%s\n",
         n, len, a, r[OFF_REC_FRMT], b, c, r[OFF_T], d);
  printable(r + OFF_RTE_STAT, 2, a);
  printable(r + OFF_ISS_CDE, 2, b);
  printf("   ORIGINATOR=%c RESPONDER=%c RTE-STAT=%s ISS-CDE=%s AUTH-IND=%c\n",
         r[OFF_ORIGINATOR], r[OFF_RESPONDER], a, b, r[OFF_AUTH_IND]);
  printable(r + OFF_ORIG, 4, a);
  printable(r + OFF_DEST, 4, b);
  printable(r + OFF_AUTHORIZER, 16, c);
  printf("   ORIG='%s' DEST='%s' AUTHORIZER='%s'\n", a, b, c);
  printable(r + OFF_ORIG_CRNCY, 3, a);
  printable(r + OFF_TERM_CNTRY, 2, b);
  printable(r + OFF_APPRV_CDE + 6, 2, d);
  printable(r + OFF_RVRL_CDE, 2, e);
  printf("   AMT-1=%lld AMT-2=%lld CRNCY=%s CNTRY=%s RVRL=%s APPRV(7-8)='%s'\n",
         get64(r + OFF_AMT_1), get64(r + OFF_AMT_2), a, b, e, d);
  printable(r + OFF_TERM_CITY, 13, c);
  printable(r + OFF_ENTRY_MDE, 3, d);
  printf("   CITY='%s' ENTRY-MDE=%s\n", c, d);
  fmt_ts(get64(r + OFF_DAT_TIM), a);
  printable(r + OFF_TRAN_DAT, 6, b);
  printable(r + OFF_TRAN_TIM, 8, c);
  printf("   DAT-TIM=%s TRAN-DAT=%s TRAN-TIM=%s\n", a, b, c);
  if (et && rt && rt >= et) printf("   host valaszido=%lld ms\n", (rt - et) / 1000);
  else printf("   host valaszido=-\n");
}

/* ---------- Feldolgozas ---------- */
static void process(const unsigned char *r, int len)
{
  long rt_typ, typ, tc, resp, rvrl;
  int ty, dc, appr, h, mi, sec, code;
  long long ts, et, rt;
  TmX t;
  char cc[3];
  Crncy *cr; Cntry *cn;

  n_rec++;
  if (len < 10) { n_short++; return; }
  rt_typ = num(r + OFF_REC_TYP, 2);
  if (rt_typ >= 20 && rt_typ <= 23) { n_excp[rt_typ - 20]++; return; }
  if (rt_typ == 4) { n_setl++; return; }
  if (rt_typ != 1) { n_other++; return; }
  if (len < MIN_REC_LEN) { n_short++; return; }
  if (r[OFF_REC_FRMT] != '5') { n_other++; return; }

  ts = get64(r + OFF_DAT_TIM);
  if (!endian_known) {
    if (!plausible_ts(ts)) {
      big_endian = !big_endian;
      if (!plausible_ts(get64(r + OFF_DAT_TIM))) big_endian = !big_endian;
    }
    endian_known = 1;
    ts = get64(r + OFF_DAT_TIM);
  }

  typ = num(r + OFF_TYP, 4);
  if (typ == 500 || typ == 412) { n_skip_typ++; return; }

  n_tran++;
  if (opt_dump) {
    if (n_tran <= opt_dump) dump_rec(r, len, n_tran);
    return;
  }

  tc   = num(r + OFF_TC, 2);
  resp = num(r + OFF_RESP_CDE, 3);
  if (resp < 0) resp = 999;
  appr = resp < opt_appr_limit;
  ty = classify(typ, tc);
  dc = decision(r);

  by_type[ty].cnt++;  if (appr) by_type[ty].appr++;
  by_dec[dc].cnt++;   if (appr) by_dec[dc].appr++;
  resp_cnt[resp][dc]++;

  /* engedelyezo */
  {
    char nm[17]; int k; Authz *az = NULL;
    printable(r + OFF_AUTHORIZER, 16, nm);
    for (k = 15; k >= 0 && nm[k] == ' '; k--) nm[k] = 0;
    if (!nm[0]) strcpy(nm, "(ures)");
    for (k = 0; k < n_authz; k++)
      if (authz[k].resp == (char)r[OFF_RESPONDER] && strcmp(authz[k].name, nm) == 0) { az = &authz[k]; break; }
    if (!az && n_authz < MAX_AUTHZ) {
      az = &authz[n_authz++];
      memset(az, 0, sizeof(Authz));
      strcpy(az->name, nm); az->resp = (char)r[OFF_RESPONDER];
    }
    if (az) { az->cnt++; if (appr) az->appr++; }
  }

  /* orszag */
  cc[0] = (char)r[OFF_TERM_CNTRY]; cc[1] = (char)r[OFF_TERM_CNTRY + 1]; cc[2] = 0;
  if (cc[0] == ' ' || cc[0] == 0) strcpy(cc, "??");
  if (strcmp(cc, opt_home) == 0) { dom.cnt++; if (appr) dom.appr++; }
  else                           { frn.cnt++; if (appr) frn.appr++; }
  if ((cn = get_cntry(cc)) != NULL) { cn->cnt++; if (!appr) cn->decl++; }

  /* osszegek */
  code = (int)num(r + OFF_ORIG_CRNCY, 3);
  if (code < 0) code = 0;
  if (appr && (cr = get_crncy(code)) != NULL) {
    long long a1 = get64(r + OFF_AMT_1);
    cr->cnt++;
    switch (ty) {
    case T_PUR: case T_COMP: case T_MOTO: case T_CASH: case T_CBK: cr->pur += a1; break;
    case T_REF: cr->ref += a1; break;
    case T_REV: cr->rev += a1; break;
    case T_PRE: cr->pre += a1; break;
    default: break;
    }
  }

  /* ido */
  if (ts && !ts_local(ts, &t)) {
    if (!first_ts || ts < first_ts) first_ts = ts;
    if (ts > last_ts) last_ts = ts;
    h = t.h; mi = t.h * 60 + t.mi; sec = mi * 60 + t.s;
    hour[h].cnt++;
    if (!appr) hour[h].decl++;
    if (dc == D_STIP) { hour[h].stip++; minute_stip[mi]++; }
    rvrl = num(r + OFF_RVRL_CDE, 2);
    if (ty == T_REV && rvrl == 1) hour[h].tmo_rev++;
    minute_cnt[mi]++;
    second_cnt[sec]++;
    et = get64(r + OFF_EXIT_TIM); rt = get64(r + OFF_REENTRY_TIM);
    if (et > 0 && rt >= et && plausible_ts(et)) {
      long long us = rt - et; long ms = (long)(us / 1000);
      if (ms > RT_MAX_MS) ms = RT_MAX_MS;
      hour[h].rt_sum_us += us; hour[h].rt_n++;
      hour[h].rt_hist[ms]++;
    }
  }
}

/* ---------- Riportok ---------- */
static void line(int n, char c) { while (n-- > 0) putchar(c); putchar('\n'); }

/* Minden riportsor legfeljebb RPT_W karakter, hogy 80 oszlopos terminalon
   se torjon meg. */
#define RPT_W 79

static void print_header(const char *title)
{
  char a[32], b[32];
  printf("\n");
  line(RPT_W, '=');
  printf("BASE24 POS (KIBOCSATOI) - %s\n", title);
  printf("Forras:  %s\n", opt_file);
  printf("Idoszak: %s - %s\n", fmt_ts(first_ts, a), fmt_ts(last_ts, b));
  line(RPT_W, '=');
}

static void row_ca(const char *name, CntAppr *x)
{
  char a[32], b[32], c[32], d[32];
  printf("%-28.28s%11s%14s%13s%10s\n", name, fmt_n(x->cnt, a), fmt_n(x->appr, b),
         fmt_n(x->cnt - x->appr, c), pct(x->appr, x->cnt, d));
}

static void head_ca(const char *first)
{
  printf("%-28s%11s%14s%13s%10s\n", first, "Osszes", "Engedelyez.", "Elutasitott", "Jovah.%");
  line(76, '-');
}

static void report_summary(void)
{
  int i; CntAppr tot = {0, 0};
  char a[40], b[40], c[40], d[40], e[40];

  print_header("NAPI OSSZESITO");
  printf("Rekordok:  osszes %s, tranzakcio %s, terminal-osszesito %s,\n",
         fmt_n(n_rec, a), fmt_n(n_tran, b), fmt_n(n_setl, c));
  printf("           egyeb %s, rovid %s\n", fmt_n(n_other + n_skip_typ, d), fmt_n(n_short, e));
  printf("Kivetelek: 20 konyvelt %ld, 21 nem konyvelt %ld, 22 reszben %ld,\n",
         n_excp[0], n_excp[1], n_excp[2]);
  printf("           23 hibas adat %ld\n", n_excp[3]);
  printf("Engedelyezett: valaszkod < %03d\n\n", opt_appr_limit);

  head_ca("Tipus");
  for (i = 0; i < NTYPE; i++) {
    if (!by_type[i].cnt) continue;
    row_ca(type_name[i], &by_type[i]);
    tot.cnt += by_type[i].cnt; tot.appr += by_type[i].appr;
  }
  line(76, '-');
  row_ca("OSSZESEN", &tot);

  printf("\n");
  head_ca("Bontas");
  row_ca("Belfold", &dom);
  row_ca("Kulfold", &frn);

  printf("\n");
  head_ca("Dontes helye");
  for (i = 0; i < NDEC; i++) if (by_dec[i].cnt) row_ca(dec_long[i], &by_dec[i]);

  printf("\n");
  head_ca("Engedelyezo (RESPONDER)");
  for (i = 0; i < n_authz; i++) {
    char nm[40]; CntAppr x;
    sprintf(nm, "%.16s (%c)", authz[i].name, authz[i].resp);
    x.cnt = authz[i].cnt; x.appr = authz[i].appr;
    row_ca(nm, &x);
  }

  /* devizankent soronkent egy blokk, hogy a nagy osszegek is elferjenek */
  printf("\nEngedelyezett osszegek devizankent (a tranzakcio devizajaban)\n");
  line(RPT_W, '-');
  for (i = 0; i < n_crncy; i++) {
    Crncy *c0 = &crncy[i];
    const char *nm = crncy_name(c0->code);
    char cn[8];
    if (nm) strcpy(cn, nm); else sprintf(cn, "%03d", c0->code);
    printf("%-4s %s db\n", cn, fmt_n(c0->cnt, a));
    printf("     Vasarlas+lezaras  %18s   Visszaterites   %17s\n",
           fmt_amt(c0->pur, c0->code, b), fmt_amt(c0->ref, c0->code, c));
    printf("     Sztorno           %18s   Eloeng.(zarolt) %17s\n",
           fmt_amt(c0->rev, c0->code, d), fmt_amt(c0->pre, c0->code, e));
    printf("     NETTO             %18s\n", fmt_amt(c0->pur - c0->ref - c0->rev, c0->code, b));
  }
  line(RPT_W, '-');
  printf("Netto = vasarlas + lezaras + MOTO + keszpenz + cashback\n");
  printf("        - visszaterites - sztorno\n");
}

static long g_sort_tot[1000];
static int cmp_code(const void *a, const void *b)
{
  long x = g_sort_tot[*(const int *)a], y = g_sort_tot[*(const int *)b];
  return (y > x) - (y < x);
}

static void resp_table(int approved)
{
  int idx[1000], n = 0, i, k, used[NDEC]; long tot = 0;
  char a[32], b[32], c[16];
  for (k = 0; k < NDEC; k++) used[k] = 0;
  for (i = 0; i < 1000; i++) {
    long s = 0;
    if ((i < opt_appr_limit) != approved) continue;
    for (k = 0; k < NDEC; k++) { s += resp_cnt[i][k]; if (resp_cnt[i][k]) used[k] = 1; }
    if (!s) continue;
    g_sort_tot[i] = s; tot += s; idx[n++] = i;
  }
  if (!n) { printf("  nincs\n"); return; }
  qsort(idx, n, sizeof(int), cmp_code);
  /* csak azok a dontes-oszlopok jelennek meg, amelyekben van adat */
  printf("%-4s%-20s%8s%8s", "Kod", "Megnevezes", "Darab", "Arany");
  for (k = 0; k < NDEC; k++) if (used[k]) printf("%7s", dec_name[k]);
  printf("\n");
  line(RPT_W, '-');
  for (i = 0; i < n; i++) {
    int code = idx[i]; char desc[21];
    strncpy(desc, resp_desc[code] ? resp_desc[code] : "", 19); desc[19] = 0;
    printf("%03d %-20s%8s%8s", code, desc, fmt_n(g_sort_tot[code], a), pct(g_sort_tot[code], tot, b));
    for (k = 0; k < NDEC; k++) {
      if (!used[k]) continue;
      if (resp_cnt[code][k]) fmt_n(resp_cnt[code][k], c); else strcpy(c, "-");
      printf("%7s", c);
    }
    printf("\n");
  }
  line(RPT_W, '-');
  printf("    %-20s%8s\n", "OSSZESEN", fmt_n(tot, a));
}

static void report_resp(void)
{
  long decl = 0; int i; char a[32], b[32];
  for (i = 0; i < NTYPE; i++) decl += by_type[i].cnt - by_type[i].appr;
  print_header("VALASZKOD-ELEMZES");
  printf("Elutasitott: %s db (a forgalom %s-a)\n\n", fmt_n(decl, a), pct(decl, n_tran, b));
  printf("A) Elutasito valaszkodok\n");
  resp_table(0);
  printf("\nB) Engedelyezo valaszkodok\n");
  resp_table(1);
  printf("\nDontes helye: Host   = RESPONDER 5\n");
  printf("              BASE24 = RESPONDER 3 es RTE-STAT 00\n");
  printf("              STIP   = RESPONDER 3 es RTE-STAT <> 00\n");
  printf("              HostIF = RESPONDER 4,  ICHG = RESPONDER 6/7\n");
}

static long pctile(const unsigned long *hist, long n, double p)
{
  long need = (long)(n * p + 0.999999), acc = 0; int i;
  if (n == 0) return -1;
  for (i = 0; i <= RT_MAX_MS; i++) { acc += (long)hist[i]; if (acc >= need) return i; }
  return RT_MAX_MS;
}

#define BAR_W 22

static void report_hourly(void)
{
  int h, i; long mx = 0, tot = 0, decl = 0, peak_h = 0;
  long pm = 0, pmi = 0, ps = 0, psi = 0;
  char a[32], b[32], c[32], e[32], f[32];
  static unsigned long all_hist[RT_MAX_MS + 1]; long all_n = 0; long long all_sum = 0;

  print_header("ORANKENTI FORGALOM");
  for (h = 0; h < 24; h++) {
    if (hour[h].cnt > mx) { mx = hour[h].cnt; peak_h = h; }
    tot += hour[h].cnt; decl += hour[h].decl;
  }
  printf("%-6s%8s%7s%6s%6s%6s%6s\n", "Ora", "Darab", "Elut%", "STIP", "TOszt", "Atlms", "p95ms");
  line(RPT_W, '-');
  for (h = 0; h < 24; h++) {
    Hour *x = &hour[h]; int bar = mx ? (int)((x->cnt * BAR_W + mx / 2) / mx) : 0;
    double dr = x->cnt ? 100.0 * x->decl / x->cnt : 0;
    char drs[16];
    fmt_d(dr, 1, drs);
    if (x->rt_n) {
      sprintf(e, "%lld", x->rt_sum_us / x->rt_n / 1000);
      sprintf(f, "%ld", pctile(x->rt_hist, x->rt_n, 0.95));
    } else { strcpy(e, "-"); strcpy(f, "-"); }
    printf("%02d-%02d %8s%7s%6s%6s%6s%6s ", h, h + 1, fmt_n(x->cnt, a),
           x->cnt ? drs : "-", x->stip ? fmt_n(x->stip, b) : "-",
           x->tmo_rev ? fmt_n(x->tmo_rev, c) : "-", e, f);
    for (i = 0; i < bar; i++) putchar('#');
    if (x->cnt && x->cnt >= 50 && decl && dr > 2.0 * 100.0 * decl / tot) printf(" <ELUT");
    if (h == peak_h && mx) printf(" <CSUCS");
    printf("\n");
    for (i = 0; i <= RT_MAX_MS; i++) all_hist[i] += x->rt_hist[i];
    all_n += x->rt_n; all_sum += x->rt_sum_us;
  }
  line(RPT_W, '-');
  printf("Elut%% = elutasitasi arany, TOszt = idotullepes miatti sztorno\n");
  printf("(0420, RVRL-CDE 01), Atlms/p95ms = host valaszido (ms)\n\n");
  for (i = 0; i < 1440; i++) if (minute_cnt[i] > pm) { pm = minute_cnt[i]; pmi = i; }
  for (i = 0; i < 86400; i++) if (second_cnt[i] > ps) { ps = second_cnt[i]; psi = i; }
  printf("Napi atlag:      %s tranzakcio/mp\n", fmt_d(tot / 86400.0, 2, a));
  if (mx) {
    fmt_d(mx / 3600.0, 2, a);
    printf("Csucsora:        %02ld:00-%02ld:00  %s db, atlag %s tr/mp\n",
           peak_h, peak_h + 1, fmt_n(mx, b), a);
    fmt_d(pm / 60.0, 1, a);
    printf("Csucsperc:       %02ld:%02ld        %s db, atlag %s tr/mp\n",
           pmi / 60, pmi % 60, fmt_n(pm, b), a);
    printf("Csucsmasodperc:  %02ld:%02ld:%02ld     %ld tr/mp\n",
           psi / 3600, (psi / 60) % 60, psi % 60, ps);
  }
  if (all_n) {
    printf("Host valaszido:  atlag %lld ms, median %ld ms, p95 %ld ms, p99 %ld ms\n",
           all_sum / all_n / 1000, pctile(all_hist, all_n, 0.5), pctile(all_hist, all_n, 0.95),
           pctile(all_hist, all_n, 0.99));
    printf("                 (%s meres)\n", fmt_n(all_n, a));
  }

  /* STIP idoszakok: STIP-es percek, legfeljebb STIP_GAP perc szunettel osszevonva */
  printf("\nSTIP-idoszakok (host nem elerheto):\n");
  {
    int found = 0, s = -1, last = -1; long cnt = 0;
    for (i = 0; i <= 1440; i++) {
      int has = (i < 1440) && minute_stip[i] > 0;
      if (has) {
        if (s < 0) { s = i; cnt = 0; }
        cnt += minute_stip[i]; last = i;
      } else if (s >= 0 && (i - last > STIP_GAP || i == 1440)) {
        printf("  %02d:%02d-%02d:%02d  %s tranzakcio\n", s / 60, s % 60,
               (last + 1) / 60 % 24, (last + 1) % 60, fmt_n(cnt, a));
        found = 1; s = -1;
      }
    }
    if (!found) printf("  nem volt\n");
  }
}

static int cmp_cntry(const void *a, const void *b)
{
  long x = ((const Cntry *)a)->cnt, y = ((const Cntry *)b)->cnt;
  return (y > x) - (y < x);
}

static void report_country(void)
{
  int i, top = 25; long rc = 0, rd = 0; char a[32], b[32], c[32];
  print_header("MELLEKLET: TRANZAKCIOS ORSZAGOK");
  qsort(cntry, n_cntry, sizeof(Cntry), cmp_cntry);
  printf("%-4s%-22s%10s%9s%9s\n", "Kod", "Orszag", "Darab", "Arany", "Elut.%");
  line(60, '-');
  for (i = 0; i < n_cntry; i++) {
    if (i < top) printf("%-4s%-22s%10s%9s%9s\n", cntry[i].cc, cntry_name(cntry[i].cc),
                        fmt_n(cntry[i].cnt, a), pct(cntry[i].cnt, n_tran, b),
                        pct(cntry[i].decl, cntry[i].cnt, c));
    else { rc += cntry[i].cnt; rd += cntry[i].decl; }
  }
  if (rc) {
    char nm[32]; sprintf(nm, "Egyeb (%d orszag)", n_cntry - top);
    printf("%-4s%-22s%10s%9s%9s\n", "..", nm, fmt_n(rc, a), pct(rc, n_tran, b), pct(rd, rc, c));
  }
  line(60, '-');
  printf("Forras: AUTH.TERM-CNTRY-CDE; ?? = ures mezo\n");
}

/* ---------- main ---------- */
static void usage(void)
{
  fprintf(stderr, "Hasznalat: PTLFRPT [-a nnn] [-c XX] [-r respfile] [-z perc] [-d n]\n"
                  "                  ptlf-fajl\n");
  exit(2);
}

int main(int argc, char *argv[])
{
  static unsigned char buf[MAXREC];
  int i, len;

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] && argv[i][2] == 0) {
      if (i + 1 >= argc) usage();
      switch (argv[i][1]) {
      case 'a': case 'A': opt_appr_limit = atoi(argv[++i]); break;
      case 'c': case 'C':
        strncpy(opt_home, argv[++i], 2); opt_home[2] = 0;
        if (opt_home[0] >= 'a') opt_home[0] -= 32;
        if (opt_home[1] >= 'a') opt_home[1] -= 32;
        break;
      case 'r': case 'R': opt_respfile = argv[++i]; break;
      case 'z': case 'Z': opt_tz_min = atol(argv[++i]); opt_tz_set = 1; break;
      case 'd': case 'D': opt_dump = atoi(argv[++i]); break;
      default: usage();
      }
    } else if (!opt_file) opt_file = argv[i];
    else usage();
  }
  if (!opt_file) usage();
  if (opt_respfile) load_resp(opt_respfile);

  if (rd_open(opt_file)) return 1;
  while ((len = rd_next(buf, MAXREC)) > 0) process(buf, len);
  rd_close();
  if (len < 0) { fprintf(stderr, "Olvasasi hiba, a riport hianyos lehet.\n"); }

  if (opt_dump) {
    printf("Bajtsorrend: %s. Tranzakcios rekordok: %ld\n",
           big_endian ? "big-endian" : "little-endian", n_tran);
    return 0;
  }
  if (n_tran == 0) {
    printf("Nincs tranzakcios rekord (REC-TYP 01, REC-FRMT 5) a fajlban: %s\n", opt_file);
    return 0;
  }
  report_summary();
  report_resp();
  report_hourly();
  report_country();
  return 0;
}
