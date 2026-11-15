/*
 * TLFRPT - BASE24-atm (classic) TLF napi riport
 *
 * Egy napi TLF fajl (TLyymmdd) egyszeri vegigolvasasaval keszit:
 *   1. Napi osszesitot (tipus, sajat/idegen kartya, dontes helye,
 *      kifizetett osszeg devizankent, elnyelt kartyak)
 *   2. Valaszkod-elemzest (dontes helye szerinti bontassal)
 *   3. Oranketi forgalmat (STIP, idotullepes, hardverhiba, host valaszido)
 *   4. ATM-enkenti forgalmat es a figyelendo ATM-ek listajat
 *   5. Mellekletkent a sztorno/korrekcio okokat
 *
 * Hasznalat (Guardian):
 *   RUN TLFRPT /OUT $S.#TLFRPT/ [-a nn] [-r respfile] [-z perc] [-d n]
 *                              $DATA.TLF.TL261114
 *
 *   -a nn      elso ELUTASITO ok-kod (a valaszkod 2-3. jegye, alap: 50);
 *              ennel kisebb, illetve a 90-92 = engedelyezett
 *   -r fajl    valaszkod-megnevezesek felulirasa/kiegeszitese ("nnn szoveg"
 *              soronkent, # = megjegyzes). A BASE24-atm kodok be vannak
 *              epitve, ez az opcio csak eltereshez kell.
 *   -z perc    idoeltolas GMT-hez kepest percben (alap Guardianon: a rendszer
 *              helyi ideje; mashol: 0)
 *   -d n       diagnosztika: az elso n tranzakcios rekord mezoinek kiirasa
 *              (kartyaszam, szamlaszam es egyenleg NELKUL), majd kilep
 *
 * A mezopoziciok a BASE24-atm Release 6.0 DDLTLF / DDLGDEFS alapjan:
 * HEAD = 76 bajt, AUTH = 398 bajt, minimalis rekordhossz 474 bajt.
 *
 * A kimenet ekezet nelkuli (ASCII) es legfeljebb 79 oszlop szeles.
 * Kartyaadatot (PAN, szamlaszam) es egyenleget a program nem ir ki.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __TANDEM
/* Guardian alatt a fajl neve $SYSTEM.SYSTEM.CEXTDECS (nincs "h" a vegen),
   ezert ".h" nelkul, a szukseges szekciok felsorolasaval kell behuzni. */
#include <cextdecs(FILE_OPEN_,READX,FILE_GETINFO_,FILE_CLOSE_,CONVERTTIMESTAMP,INTERPRETTIMESTAMP)>
#endif

/* ---------- TLF mezopoziciok (0-tol szamolt bajt offset) ---------- */
#define OFF_DAT_TIM       0    /* HEAD.DAT-TIM          BINARY 64         */
#define OFF_REC_TYP       8    /* HEAD.REC-TYP          X(2)              */
#define OFF_TERM_LN       14   /* HEAD.TERM.LN          X(4)              */
#define OFF_TERM_FIID     18   /* HEAD.TERM.FIID        X(4)              */
#define OFF_TERM_ID       22   /* HEAD.TERM.TERM-ID     X(16)             */
#define OFF_CRD_LN        38   /* HEAD.CRD.LN           X(4)              */
#define OFF_CRD_FIID      42   /* HEAD.CRD.FIID         X(4)              */
#define OFF_TYP           78   /* AUTH.TYP              9(4)              */
#define OFF_RTE_STAT      82   /* AUTH.RTE-STAT         99                */
#define OFF_ORIGINATOR    84   /* AUTH.ORIGINATOR       X                 */
#define OFF_RESPONDER     85   /* AUTH.RESPONDER        X                 */
#define OFF_ENTRY_TIM     86   /* AUTH.ENTRY-TIM        BINARY 64         */
#define OFF_EXIT_TIM      94   /* AUTH.EXIT-TIM         BINARY 64         */
#define OFF_REENTRY_TIM   102  /* AUTH.RE-ENTRY-TIM     BINARY 64         */
#define OFF_TRAN_DAT      110  /* AUTH.TRAN-DAT         YYMMDD            */
#define OFF_TRAN_TIM      116  /* AUTH.TRAN-TIM         HHMMSSTT          */
#define OFF_TERM_TYP      154  /* AUTH.TERM-TYP         99                */
#define OFF_T_CDE         180  /* AUTH.TRAN-CDE.T-CDE   XX                */
#define OFF_T_FROM        182  /* AUTH.TRAN-CDE.T-FROM  XX                */
#define OFF_T_TO          184  /* AUTH.TRAN-CDE.T-TO    XX                */
#define OFF_AMT_1         226  /* AUTH.AMT-1            BINARY 64         */
#define OFF_AMT_2         234  /* AUTH.AMT-2            BINARY 64         */
#define OFF_AMT_3         242  /* AUTH.AMT-3            BINARY 64         */
#define OFF_RESP_CDE      255  /* AUTH.RESP-CDE         X(3)              */
#define OFF_TERM_NAME_LOC 258  /* AUTH.TERM-NAME-LOC    X(25)             */
#define OFF_TERM_CITY     305  /* AUTH.TERM-CITY        X(13)             */
#define OFF_TERM_CNTRY    321  /* AUTH.TERM-CNTRY-X     X(2)              */
#define OFF_ORIG_CRNCY    351  /* AUTH.ORIG-CRNCY-CDE   9(3)              */
#define OFF_RVSL_RSN      384  /* AUTH.RVSL-RSN         99                */
#define OFF_DEST_ORDER    403  /* AUTH.DEST-ORDER       X                 */
#define MIN_REC_LEN       474

/* Adminisztrativ rekord (REC-TYP 04): HEAD + ADMIN-DAT + ADMIN-TIM + ADMIN-CDE */
#define OFF_ADMIN_CDE     90   /* TERM-SETL/TERM-CASH/SETL-TTL.ADMIN-CDE 9(2) */

#define MAXREC            4096
#define RT_MAX_MS         5000
#define MAX_CRNCY         64
#define MAX_LN            64
#define TERM_SLOTS        8192   /* ATM hash-tabla merete (max. ~6000 ATM) */
#define STIP_GAP          5
#define RPT_W             79

#define JTS_UNIX_EPOCH    210866760000000000LL
#define JTS_MIN_PLAUS     211813000000000000LL
#define JTS_MAX_PLAUS     214969000000000000LL

/* ---------- Tranzakciotipusok (T-CDE) ---------- */
enum { T_WD, T_BAL, T_XFR, T_DEP, T_DEPCB, T_PAY, T_TOPUP, T_STMT, T_PIN,
       T_INQ, T_OTH, NTYPE };
static const char *type_name[NTYPE] = {
  "Keszpenzfelvetel", "Egyenleglekerdezes", "Atutalas", "Befizetes",
  "Befizetes+cashback", "Fizetes", "Mobil feltoltes", "Kivonat",
  "PIN-csere/feloldas", "Egyeb lekerdezes", "Egyeb" };

/* uzenettipus szerinti sorok */
enum { M_RESP, M_ADV, M_REV, M_ADJ, NMSG };
static const char *msg_name[NMSG] = {
  "Valasz (0210)", "Advice (0220)", "Sztorno (0420)", "Korrekcio (5400)" };

/* ---------- Dontes helye ---------- */
enum { D_HOST, D_B24, D_STIP, D_HIF, D_NET, D_OTH, NDEC };
static const char *dec_name[NDEC] = {
  "Host", "BASE24", "STIP", "HostIF", "ICHG", "Egyeb" };
static const char *dec_long[NDEC] = {
  "Host (online)", "BASE24 Authorization", "STIP (host nem elerheto)",
  "Host Interface", "Interchange (RESP. 6/7)", "Egyeb" };

/* ---------- Beepitett BASE24-atm valaszkodok (a -r fajl felulirhatja) ---------- */
static const struct { int code; const char *desc; } resp_builtin[] = {
  {  0, "Approved with balances"},
  {  1, "Approved - no balances"},
  { 50, "Unauthorized usage"},
  { 51, "Expired card"},
  { 52, "Invalid card"},
  { 53, "Invalid PIN"},
  { 54, "Database problem"},
  { 55, "Ineligible transaction"},
  { 56, "Ineligible account"},
  { 57, "Transaction not supported"},
  { 58, "Insufficient funds (no amount 3)"},
  { 59, "Insufficient funds (amount 3)"},
  { 60, "Uses limit exceeded"},
  { 61, "Withdrawal limit would be exceeded"},
  { 62, "PIN tries exceeded"},
  { 63, "Withdrawal limit already reached"},
  { 64, "Invalid credit card cash advance"},
  { 65, "No statement information"},
  { 66, "Statement info not available"},
  { 67, "Invalid cash back amount"},
  { 68, "External decline"},
  { 69, "No sharing arrangement"},
  { 70, "System error"},
  { 71, "Contact card issuer"},
  { 72, "Destination not available"},
  { 73, "Routing look up problem"},
  { 74, "Message edit error"},
  { 81, "Security module parameter error"},
  { 82, "Security module failure"},
  { 83, "KEYI record not found"},
  { 84, "ATC check failure"},
  { 85, "CVR decline"},
  { 86, "TVR decline"},
  { 87, "Request cryptogram failure"},
  { 88, "Fallback decline"},
  { 90, "Approved - inside window"},
  { 91, "Approved - outside window"},
  { 92, "Approved (balance anytime)"},
  {150, "Unauthorized usage (capture card)"},
  {151, "Expired card (capture card)"},
  {162, "PIN tries exceeded (capture card)"},
  { -1, NULL}
};

/* sztorno (0420) es korrekcio (5400) okok */
static const char *rvsl_name(int typ, int c)
{
  if (typ == 420) switch (c) {
    case 0:  return "(nincs megadva)";
    case 1:  return "Idotullepes (time-out)";
    case 2:  return "Command reject";
    case 3:  return "Cel nem elerheto";
    case 8:  return "Tranzakcio megszakitva";
    case 10: return "Hardverhiba";
    case 20: return "Gyanus tranzakcio";
    case 30: return "Stored value: funds oldal hiba";
    case 31: return "Stored value: SVC oldal hiba";
    case 40: return "Split routing: masodlagos elut.";
  }
  if (typ == 5400) switch (c) {
    case 12: return "Eredeti osszeg hibas";
    case 13: return "ATM meghibasodas";
    case 14: return "Gyanus sztorno feluliras";
    case 15: return "Hibas penzkiadas feluliras";
    case 16: return "Duplikalt tranzakcio";
    case 17: return "Egyeztetesi hiba";
    case 18: return "PLUS keszpenz hozzaadas";
    case 21: return "MAC hiba";
    case 22: return "MAC kulcs szinkron hiba";
    case 23: return "Uzenet ismetles hiba";
    case 24: return "Ervenytelen MAC";
  }
  return "";
}

/* "hibas" esemeny: ATM- vagy kapcsolati hiba miatti sztorno/korrekcio */
static int is_fault(int typ, int c)
{
  if (typ == 420) return c == 1 || c == 2 || c == 3 || c == 10;
  if (typ == 5400) return c == 12 || c == 13 || c == 15;
  return 0;
}

/* ---------- Opciok ---------- */
static int  opt_appr_limit = 50;
static const char *opt_respfile = NULL;
static int  opt_tz_set = 0;
static long opt_tz_min = 0;
static int  opt_dump = 0;
static const char *opt_file = NULL;

/* ---------- Statisztikak ---------- */
typedef struct { long cnt, appr; } CntAppr;

static long n_rec, n_tran, n_short, n_admin, n_other, n_excp[3], n_skip_typ;
static long n_retained;
static CntAppr by_type[NTYPE], by_msg[NMSG], by_dec[NDEC], own_crd, frn_crd;
static long resp_cnt[1000][NDEC];
static char *resp_desc[1000];
static long rvsl_cnt[2][100];        /* [0]=0420, [1]=5400 */

typedef struct { char ln[5]; long cnt, appr; } LnStat;
static LnStat lnst[MAX_LN]; static int n_ln;

typedef struct {
  long cnt, decl, stip, tmo, fault;
  long long rt_sum_us; long rt_n;
  unsigned long rt_hist[RT_MAX_MS + 1];
} Hour;
static Hour hour[24];
static long minute_cnt[1440], minute_stip[1440];
static long second_cnt[86400];
static long long first_ts, last_ts;

typedef struct {
  int code; long wd_cnt;
  long long wd, rev, adj, dep;      /* minor unit */
} Crncy;
static Crncy crncy[MAX_CRNCY]; static int n_crncy;

typedef struct {
  int used;
  char fiid[5], id[17], loc[26];
  long cnt, decl, wd_cnt, fault, retained, cashops;
  long long wd_amt, last_ts;
  int crncy, mixed;
} Term;
static Term terms[TERM_SLOTS]; static int n_terms, terms_full;

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

static long num(const unsigned char *p, int len)
{
  long v = 0; int i;
  for (i = 0; i < len; i++) {
    if (p[i] < '0' || p[i] > '9') return -1;
    v = v * 10 + (p[i] - '0');
  }
  return v;
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

/* osszeg egesz egysegre (tizedesek nelkul) */
static char *fmt_amt0(long long v, int code, char *out)
{
  int e = crncy_exp(code), k; long long div = 1;
  for (k = 0; k < e; k++) div *= 10;
  return fmt_n(v / div, out);
}

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
  else { fmt_d(100.0 * a / b, 2, out); strcat(out, "%"); }
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

static void printable(const unsigned char *p, int len, char *out)
{
  int i;
  for (i = 0; i < len; i++) out[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
  out[len] = 0;
}

static void rtrim(char *s)
{
  int k = (int)strlen(s) - 1;
  while (k >= 0 && s[k] == ' ') s[k--] = 0;
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

static char *fmt_hm(long long jts, char *out)
{
  TmX t;
  if (!jts || ts_local(jts, &t)) strcpy(out, "-");
  else sprintf(out, "%02d:%02d", t.h, t.mi);
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
    if (err == 1) return 0;
    fprintf(stderr, "READX hiba %d\n", err);
    return -1;
  }
  return cnt;
}
static void rd_close(void) { if (fnum >= 0) FILE_CLOSE_(fnum); }
#else
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
static void load_builtin_resp(void)
{
  int i;
  for (i = 0; resp_builtin[i].desc; i++)
    resp_desc[resp_builtin[i].code] = (char *)resp_builtin[i].desc;
}

static void load_resp(const char *fn)
{
  FILE *f = fopen(fn, "r"); char line[256];
  if (!f) { fprintf(stderr, "Nem nyithato meg: %s (beepitett megnevezesekkel folytatom)\n", fn); return; }
  while (fgets(line, sizeof line, f)) {
    char *p = line, *e; long c;
    while (*p == ' ') p++;
    if (*p == '#' || *p == '\n' || *p == 0) continue;
    c = strtol(p, &e, 10);
    if (e == p || c < 0 || c > 999) continue;
    while (*e == ' ' || *e == '\t') e++;
    e[strcspn(e, "\r\n")] = 0;
    resp_desc[c] = (char *)malloc(strlen(e) + 1);
    if (resp_desc[c]) strcpy(resp_desc[c], e);
  }
  fclose(f);
}

/* ---------- Osztalyozas ---------- */
static int classify(long tcde)
{
  switch (tcde) {
  case 10: case 11: return T_WD;
  case 30: return T_BAL;
  case 40: return T_XFR;
  case 20: return T_DEP;
  case 24: return T_DEPCB;
  case 50: case 51: return T_PAY;
  case 25: case 26: return T_TOPUP;
  case 70: return T_STMT;
  case 81: case 82: return T_PIN;
  case 3: case 4: case 60: case 61: case 62: return T_INQ;
  default: return T_OTH;
  }
}

static int msg_class(long typ)
{
  switch (typ) {
  case 220: return M_ADV;
  case 420: return M_REV;
  case 5400: return M_ADJ;
  default: return M_RESP;
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

/* engedelyezett-e: a valaszkod 2-3. jegye < limit, vagy 90-92 */
static int approved_code(long resp)
{
  long rs = resp % 100;
  return rs < opt_appr_limit || (rs >= 90 && rs <= 92);
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

static LnStat *get_ln(const unsigned char *p)
{
  int i; char ln[5];
  printable(p, 4, ln); rtrim(ln);
  if (!ln[0]) strcpy(ln, "????");
  for (i = 0; i < n_ln; i++) if (strcmp(lnst[i].ln, ln) == 0) return &lnst[i];
  if (n_ln >= MAX_LN) return NULL;
  memset(&lnst[n_ln], 0, sizeof(LnStat));
  strcpy(lnst[n_ln].ln, ln);
  return &lnst[n_ln++];
}

static Term *get_term(const unsigned char *r)
{
  char fiid[5], id[17]; unsigned long h = 5381; int i;
  printable(r + OFF_TERM_FIID, 4, fiid); rtrim(fiid);
  printable(r + OFF_TERM_ID, 16, id); rtrim(id);
  for (i = 0; fiid[i]; i++) h = h * 33 + (unsigned char)fiid[i];
  for (i = 0; id[i]; i++) h = h * 33 + (unsigned char)id[i];
  h %= TERM_SLOTS;
  for (i = 0; i < TERM_SLOTS; i++) {
    Term *t = &terms[(h + i) % TERM_SLOTS];
    if (!t->used) {
      if (n_terms >= TERM_SLOTS * 3 / 4) { terms_full = 1; return NULL; }
      t->used = 1; strcpy(t->fiid, fiid); strcpy(t->id, id); t->crncy = -1;
      n_terms++;
      return t;
    }
    if (strcmp(t->id, id) == 0 && strcmp(t->fiid, fiid) == 0) return t;
  }
  return NULL;
}

static void dump_rec(const unsigned char *r, int len, long n, long typ)
{
  char a[64], b[64], c[64], d[64], e[64];
  long long et = get64(r + OFF_EXIT_TIM), rt = get64(r + OFF_REENTRY_TIM);
  printable(r + OFF_REC_TYP, 2, a);
  printable(r + OFF_TYP, 4, b);
  printable(r + OFF_T_CDE, 6, c);
  printable(r + OFF_RESP_CDE, 3, d);
  printf("#%ld hossz=%d REC-TYP=%s TYP=%s TRAN-CDE=%s RESP=%s\n", n, len, a, b, c, d);
  printable(r + OFF_RTE_STAT, 2, a);
  printf("   ORIGINATOR=%c RESPONDER=%c RTE-STAT=%s DEST-ORDER=%c\n",
         r[OFF_ORIGINATOR], r[OFF_RESPONDER], a, r[OFF_DEST_ORDER]);
  printable(r + OFF_TERM_LN, 4, a);
  printable(r + OFF_TERM_FIID, 4, b);
  printable(r + OFF_TERM_ID, 16, c);
  printf("   TERM: LN='%s' FIID='%s' ID='%s'\n", a, b, c);
  printable(r + OFF_CRD_LN, 4, a);
  printable(r + OFF_CRD_FIID, 4, b);
  printable(r + OFF_TERM_TYP, 2, c);
  printf("   KARTYA: LN='%s' FIID='%s'   TERM-TYP=%s\n", a, b, c);
  printable(r + OFF_ORIG_CRNCY, 3, a);
  printable(r + OFF_RVSL_RSN, 2, e);
  if (typ == 420 || typ == 5400)
    printf("   AMT-1=%lld AMT-2=%lld CRNCY=%s RVSL-RSN=%s\n",
           get64(r + OFF_AMT_1), get64(r + OFF_AMT_2), a, e);
  else
    printf("   AMT-1=%lld (AMT-2/3 egyenleg, nem irom ki) CRNCY=%s\n", get64(r + OFF_AMT_1), a);
  printable(r + OFF_TERM_NAME_LOC, 25, b);
  printable(r + OFF_TERM_CITY, 13, c);
  printable(r + OFF_TERM_CNTRY, 2, d);
  printf("   HELY='%s' VAROS='%s' ORSZAG=%s\n", b, c, d);
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
  long rt_typ, typ, tcde, resp, rvsl;
  int ty, mc, dc, appr, h, mi, sec, code, fault;
  long long ts, et, rt;
  TmX t; Term *tm; Crncy *cr; LnStat *ls;

  n_rec++;
  if (len < 10) { n_short++; return; }
  rt_typ = num(r + OFF_REC_TYP, 2);
  if (rt_typ >= 20 && rt_typ <= 22) { n_excp[rt_typ - 20]++; return; }
  if (rt_typ == 4) {
    long ac;
    n_admin++;
    if (len < OFF_ADMIN_CDE + 2 || opt_dump) return;
    ac = num(r + OFF_ADMIN_CDE, 2);
    /* TERM-CASH: 01-04, 07, 08 = kazetta inicializalas/novelés/csokkentes */
    if (ac == 1 || ac == 2 || ac == 3 || ac == 4 || ac == 7 || ac == 8)
      if ((tm = get_term(r)) != NULL) tm->cashops++;
    return;
  }
  if (rt_typ != 1) { n_other++; return; }
  if (len < MIN_REC_LEN) { n_short++; return; }

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
  if (typ == 9980 || typ == 9991) { n_skip_typ++; return; }

  n_tran++;
  if (opt_dump) {
    if (n_tran <= opt_dump) dump_rec(r, len, n_tran, typ);
    return;
  }

  tcde = num(r + OFF_T_CDE, 2);
  resp = num(r + OFF_RESP_CDE, 3);
  if (resp < 0) resp = 999;
  appr = approved_code(resp);
  ty = classify(tcde);
  mc = msg_class(typ);
  dc = decision(r);
  rvsl = num(r + OFF_RVSL_RSN, 2);
  if (rvsl < 0) rvsl = 0;
  fault = is_fault((int)typ, (int)rvsl);

  /* a tipus-tablaban csak a valasz/advice uzenetek, a sztorno/korrekcio kulon */
  if (mc == M_RESP || mc == M_ADV) { by_type[ty].cnt++; if (appr) by_type[ty].appr++; }
  by_msg[mc].cnt++; if (appr) by_msg[mc].appr++;
  by_dec[dc].cnt++; if (appr) by_dec[dc].appr++;
  resp_cnt[resp][dc]++;
  if (r[OFF_RESP_CDE] == '1') n_retained++;
  if (mc == M_REV) rvsl_cnt[0][rvsl]++;
  if (mc == M_ADJ) rvsl_cnt[1][rvsl]++;

  if (memcmp(r + OFF_CRD_FIID, r + OFF_TERM_FIID, 4) == 0) { own_crd.cnt++; if (appr) own_crd.appr++; }
  else { frn_crd.cnt++; if (appr) frn_crd.appr++; }
  if ((ls = get_ln(r + OFF_CRD_LN)) != NULL) { ls->cnt++; if (appr) ls->appr++; }

  /* osszegek */
  code = (int)num(r + OFF_ORIG_CRNCY, 3);
  if (code < 0) code = 0;
  cr = get_crncy(code);
  tm = get_term(r);
  if (cr) {
    long long a1 = get64(r + OFF_AMT_1), a2 = get64(r + OFF_AMT_2);
    if (ty == T_WD && appr && (mc == M_RESP || mc == M_ADV)) {
      cr->wd += a1; cr->wd_cnt++;
      if (tm) {
        tm->wd_amt += a1; tm->wd_cnt++;
        if (tm->crncy < 0) tm->crncy = code; else if (tm->crncy != code) tm->mixed = 1;
      }
    }
    if (ty == T_WD && mc == M_REV) {          /* visszavont = kert - tenylegesen kiadott */
      long long d = a1 - a2;
      if (d < 0) d = 0;
      cr->rev += d;
      if (tm && tm->crncy == code) tm->wd_amt -= d;
    }
    if (ty == T_WD && mc == M_ADJ) {          /* korrekcio: uj - eredeti */
      cr->adj += a2 - a1;
      if (tm && tm->crncy == code) tm->wd_amt += a2 - a1;
    }
    if ((ty == T_DEP || ty == T_DEPCB) && appr && mc == M_RESP) cr->dep += a1;
  }

  /* ATM */
  if (tm) {
    tm->cnt++;
    if (!appr) tm->decl++;
    if (fault) tm->fault++;
    if (r[OFF_RESP_CDE] == '1') tm->retained++;
    if (ts > tm->last_ts) tm->last_ts = ts;
    if (!tm->loc[0]) { printable(r + OFF_TERM_NAME_LOC, 25, tm->loc); rtrim(tm->loc); }
  }

  /* ido */
  if (ts && !ts_local(ts, &t)) {
    if (!first_ts || ts < first_ts) first_ts = ts;
    if (ts > last_ts) last_ts = ts;
    h = t.h; mi = t.h * 60 + t.mi; sec = mi * 60 + t.s;
    hour[h].cnt++;
    if (!appr) hour[h].decl++;
    if (dc == D_STIP) { hour[h].stip++; minute_stip[mi]++; }
    if (mc == M_REV && rvsl == 1) hour[h].tmo++;
    if (fault) hour[h].fault++;
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

static void print_header(const char *title)
{
  char a[32], b[32];
  printf("\n");
  line(RPT_W, '=');
  printf("BASE24 ATM - %s\n", title);
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
  printf("Rekordok:  osszes %s, tranzakcio %s, adminisztrativ %s,\n",
         fmt_n(n_rec, a), fmt_n(n_tran, b), fmt_n(n_admin, c));
  printf("           egyeb %s, rovid %s\n", fmt_n(n_other + n_skip_typ, d), fmt_n(n_short, e));
  printf("Kivetelek: 20/21 hibas STM adat %ld/%ld, 22 egyeb %ld\n",
         n_excp[0], n_excp[1], n_excp[2]);
  printf("Engedelyezett: valaszkod 2-3. jegye < %02d, vagy 90-92\n\n", opt_appr_limit);

  head_ca("Tipus (0210/0220)");
  for (i = 0; i < NTYPE; i++) {
    if (!by_type[i].cnt) continue;
    row_ca(type_name[i], &by_type[i]);
    tot.cnt += by_type[i].cnt; tot.appr += by_type[i].appr;
  }
  line(76, '-');
  row_ca("OSSZESEN", &tot);

  printf("\n");
  head_ca("Uzenettipus");
  for (i = 0; i < NMSG; i++) if (by_msg[i].cnt) row_ca(msg_name[i], &by_msg[i]);

  printf("\n");
  head_ca("Kartya");
  row_ca("Sajat kartya (CRD=TERM FIID)", &own_crd);
  row_ca("Idegen kartya", &frn_crd);
  for (i = 0; i < n_ln; i++) {
    char nm[40]; CntAppr x;
    sprintf(nm, "  halozat (CRD.LN) %.4s", lnst[i].ln);
    x.cnt = lnst[i].cnt; x.appr = lnst[i].appr;
    row_ca(nm, &x);
  }

  printf("\n");
  head_ca("Dontes helye");
  for (i = 0; i < NDEC; i++) if (by_dec[i].cnt) row_ca(dec_long[i], &by_dec[i]);

  printf("\nElnyelt kartyak (valaszkod 1xx): %s db\n", fmt_n(n_retained, a));

  printf("\nKifizetes devizankent (a tranzakcio devizajaban)\n");
  line(RPT_W, '-');
  for (i = 0; i < n_crncy; i++) {
    Crncy *c0 = &crncy[i];
    const char *nm = crncy_name(c0->code);
    char cn[8];
    if (!c0->wd_cnt && !c0->rev && !c0->adj && !c0->dep) continue;
    if (nm) strcpy(cn, nm); else sprintf(cn, "%03d", c0->code);
    printf("%-4s %s engedelyezett keszpenzfelvetel\n", cn, fmt_n(c0->wd_cnt, a));
    printf("     Felvett osszeg    %18s   Sztornozott     %17s\n",
           fmt_amt(c0->wd, c0->code, b), fmt_amt(c0->rev, c0->code, c));
    printf("     Korrekcio         %18s   Befizetes       %17s\n",
           fmt_amt(c0->adj, c0->code, d), fmt_amt(c0->dep, c0->code, e));
    printf("     NETTO KIFIZETES   %18s\n", fmt_amt(c0->wd - c0->rev + c0->adj, c0->code, b));
  }
  line(RPT_W, '-');
  printf("Sztornozott = 0420: AMT-1 (kert) - AMT-2 (tenylegesen kiadott)\n");
  printf("Korrekcio   = 5400: AMT-2 (uj) - AMT-1 (eredeti)\n");
}

static long g_sort_tot[1000];
static int cmp_code(const void *a, const void *b)
{
  long x = g_sort_tot[*(const int *)a], y = g_sort_tot[*(const int *)b];
  return (y > x) - (y < x);
}

static void resp_table(int approved)
{
  int idx[1000], n = 0, i, k, used[NDEC], nused = 0, dw; long tot = 0;
  char a[32], b[32], c[16];
  for (k = 0; k < NDEC; k++) used[k] = 0;
  for (i = 0; i < 1000; i++) {
    long s = 0;
    if (approved_code(i) != approved) continue;
    for (k = 0; k < NDEC; k++) { s += resp_cnt[i][k]; if (resp_cnt[i][k]) used[k] = 1; }
    if (!s) continue;
    g_sort_tot[i] = s; tot += s; idx[n++] = i;
  }
  if (!n) { printf("  nincs\n"); return; }
  qsort(idx, n, sizeof(int), cmp_code);
  for (k = 0; k < NDEC; k++) if (used[k]) nused++;
  dw = RPT_W - 4 - 17 - nused * 7;
  if (dw > 45) dw = 45;
  if (dw < 12) dw = 12;
  printf("%-4s%-*s%8s%9s", "Kod", dw, "Megnevezes", "Darab", "Arany");
  for (k = 0; k < NDEC; k++) if (used[k]) printf("%7s", dec_name[k]);
  printf("\n");
  line(RPT_W, '-');
  for (i = 0; i < n; i++) {
    int code = idx[i]; char desc[64];
    strncpy(desc, resp_desc[code] ? resp_desc[code] : "", (size_t)(dw - 1)); desc[dw - 1] = 0;
    printf("%03d %-*s%8s%9s", code, dw, desc, fmt_n(g_sort_tot[code], a), pct(g_sort_tot[code], tot, b));
    for (k = 0; k < NDEC; k++) {
      if (!used[k]) continue;
      if (resp_cnt[code][k]) fmt_n(resp_cnt[code][k], c); else strcpy(c, "-");
      printf("%7s", c);
    }
    printf("\n");
  }
  line(RPT_W, '-');
  printf("    %-*s%8s\n", dw, "OSSZESEN", fmt_n(tot, a));
}

static void report_resp(void)
{
  long decl = 0, all = 0; int i; char a[32], b[32];
  for (i = 0; i < NMSG; i++) { all += by_msg[i].cnt; decl += by_msg[i].cnt - by_msg[i].appr; }
  print_header("VALASZKOD-ELEMZES");
  printf("Elutasitott: %s db (a forgalom %s-a)\n\n", fmt_n(decl, a), pct(decl, all, b));
  printf("A) Elutasito valaszkodok (1xx = a kartyat az ATM elnyelte)\n");
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

#define BAR_W 18

static void report_hourly(void)
{
  int h, i; long mx = 0, tot = 0, decl = 0, peak_h = 0;
  long pm = 0, pmi = 0, ps = 0, psi = 0;
  char a[32], b[32], c[32], d[32], e[32], f[32];
  static unsigned long all_hist[RT_MAX_MS + 1]; long all_n = 0; long long all_sum = 0;

  print_header("ORANKENTI FORGALOM");
  for (h = 0; h < 24; h++) {
    if (hour[h].cnt > mx) { mx = hour[h].cnt; peak_h = h; }
    tot += hour[h].cnt; decl += hour[h].decl;
  }
  printf("%-6s%8s%7s%6s%6s%6s%6s%6s\n", "Ora", "Darab", "Elut%", "STIP", "TOszt", "Hiba",
         "Atlms", "p95ms");
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
    printf("%02d-%02d %8s%7s%6s%6s%6s%6s%6s ", h, h + 1, fmt_n(x->cnt, a),
           x->cnt ? drs : "-", x->stip ? fmt_n(x->stip, b) : "-",
           x->tmo ? fmt_n(x->tmo, c) : "-", x->fault ? fmt_n(x->fault, d) : "-", e, f);
    for (i = 0; i < bar; i++) putchar('#');
    if (x->cnt && x->cnt >= 50 && decl && dr > 2.0 * 100.0 * decl / tot) printf(" <ELUT");
    if (h == peak_h && mx) printf(" <CSUCS");
    printf("\n");
    for (i = 0; i <= RT_MAX_MS; i++) all_hist[i] += x->rt_hist[i];
    all_n += x->rt_n; all_sum += x->rt_sum_us;
  }
  line(RPT_W, '-');
  printf("Elut%% = elutasitasi arany, TOszt = idotullepes miatti sztorno,\n");
  printf("Hiba = ATM/kapcsolati hiba miatti sztorno/korrekcio (lasd 5. riport),\n");
  printf("Atlms/p95ms = host valaszido (ms)\n\n");
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

/* ATM-ek rendezese azonosito szerint */
static Term *g_tl[TERM_SLOTS];
static int cmp_term_id(const void *a, const void *b)
{
  const Term *x = *(Term * const *)a, *y = *(Term * const *)b;
  int c = strcmp(x->fiid, y->fiid);
  return c ? c : strcmp(x->id, y->id);
}

/* figyelendo pontszam */
static long g_score[TERM_SLOTS];
static int cmp_term_score(const void *a, const void *b)
{
  long x = g_score[*(const int *)a], y = g_score[*(const int *)b];
  return (y > x) - (y < x);
}

static double avg_fault, avg_ret, avg_decl;

static int flag_fault(const Term *t)
{ return t->fault >= 3 && t->fault >= 2.0 * avg_fault; }
static int flag_ret(const Term *t)
{ return t->retained >= 3 && t->retained >= 2.0 * avg_ret; }
static int flag_decl(const Term *t)
{
  double dr = t->cnt ? 100.0 * t->decl / t->cnt : 0;
  return t->cnt >= 20 && dr > 10.0 && dr > 2.0 * avg_decl;
}
static int flag_stopped(const Term *t)
{ return t->cnt >= 10 && last_ts && t->last_ts && last_ts - t->last_ts > 3LL * 3600 * 1000000LL; }

static void report_atm(void)
{
  int i, n = 0, k, nfl = 0, idx[TERM_SLOTS];
  long tcnt = 0, tdecl = 0;
  char a[64], b[32], c[32], d[32], e[32], f[32];

  print_header("ATM-ENKENTI FORGALOM");
  for (i = 0; i < TERM_SLOTS; i++)
    if (terms[i].used && terms[i].cnt > 0) {
      g_tl[n++] = &terms[i]; tcnt += terms[i].cnt; tdecl += terms[i].decl;
    }
  qsort(g_tl, n, sizeof(Term *), cmp_term_id);
  avg_decl = tcnt ? 100.0 * tdecl / tcnt : 0;
  printf("ATM-ek szama: %d   (tranzakcio nelkuli ATM-ek nem szerepelnek)\n", n);
  if (terms_full) printf("FIGYELEM: tul sok ATM, a lista hianyos!\n");

  /* figyelendo ATM-ek: csak az atlagtol erdemben eltero esetek
     - hiba (sztorno/korrekcio): legalabb 3 es az ATM-atlag legalabb 2-szerese
     - elnyelt kartya: legalabb 3 es az atlag legalabb 2-szerese
     - elutasitas: legalabb 20 tranzakcio, 10% folott es az atlag 2-szerese
     - elhallgatott: legalabb 10 tranzakcio, de 3 oranal regebben volt az utolso */
  {
    long tf = 0, trt = 0;
    for (i = 0; i < n; i++) { tf += g_tl[i]->fault; trt += g_tl[i]->retained; }
    avg_fault = n ? (double)tf / n : 0;
    avg_ret = n ? (double)trt / n : 0;
  }
  for (i = 0; i < n; i++) {
    Term *t = g_tl[i]; long sc = 0;
    if (flag_fault(t))   sc += t->fault * 10;
    if (flag_ret(t))     sc += t->retained * 5;
    if (flag_decl(t))    sc += (long)(100.0 * t->decl / t->cnt);
    if (flag_stopped(t)) sc += 100;
    g_score[i] = sc;
    if (sc > 0) idx[nfl++] = i;
  }
  qsort(idx, nfl, sizeof(int), cmp_term_score);
  printf("\nA) Figyelendo ATM-ek (legfeljebb 30)\n");
  printf("%-11s%-20s%s\n", "Terminal", "Hely", "Ok");
  line(RPT_W, '-');
  if (!nfl) printf("  nincs\n");
  for (k = 0; k < nfl && k < 30; k++) {
    Term *t = g_tl[idx[k]]; char why[128] = "";
    if (flag_stopped(t)) { sprintf(a, "utolso tr. %s; ", fmt_hm(t->last_ts, b)); strcat(why, a); }
    if (flag_fault(t))   { sprintf(a, "hiba %ld; ", t->fault); strcat(why, a); }
    if (flag_ret(t))     { sprintf(a, "elnyelt %ld; ", t->retained); strcat(why, a); }
    if (flag_decl(t)) {
      sprintf(a, "elut. %s%%; ", fmt_d(100.0 * t->decl / t->cnt, 1, b)); strcat(why, a);
    }
    if (strlen(why) >= 2) why[strlen(why) - 2] = 0;
    printf("%-10.10s %-19.19s %.47s\n", t->id, t->loc, why);
  }
  line(RPT_W, '-');
  printf("Szempontok: hiba/elnyelt >= 3 es az ATM-atlag 2-szerese; elutasitas >10%%\n");
  printf("es az atlag 2-szerese; utolso tr. = 3 oranal regebben volt tranzakcio\n");
  printf("(atlag/ATM: hiba %s, elnyelt %s, elutasitas %s%%)\n",
         fmt_d(avg_fault, 1, a), fmt_d(avg_ret, 1, b), fmt_d(avg_decl, 1, c));

  printf("\nB) Minden ATM (azonosito szerint)\n");
  printf("%-10s %-12s%7s%7s%7s%13s%5s%5s%4s%6s\n", "Terminal", "Hely", "Darab", "Elut%",
         "Felv.", "Kifizetve", "Hiba", "Elny", "Kaz", "Utols");
  line(RPT_W, '-');
  for (i = 0; i < n; i++) {
    Term *t = g_tl[i]; char amt[32];
    if (t->wd_cnt) {
      fmt_amt0(t->wd_amt, t->crncy, amt);
      if (t->mixed) strcat(amt, "*");
    } else strcpy(amt, "-");
    printf("%-10.10s %-12.12s%7s%7s%7s%13s%5s%5s%4ld%6s\n", t->id, t->loc, fmt_n(t->cnt, a),
           t->cnt ? fmt_d(100.0 * t->decl / t->cnt, 1, b) : "-",
           t->wd_cnt ? fmt_n(t->wd_cnt, c) : "-", amt,
           t->fault ? fmt_n(t->fault, d) : "-", t->retained ? fmt_n(t->retained, e) : "-",
           t->cashops, fmt_hm(t->last_ts, f));
  }
  line(RPT_W, '-');
  printf("Felv. = engedelyezett keszpenzfelvetel (db), Kifizetve = netto kifizetett\n");
  printf("osszeg egesz egysegre (* = tobb deviza, csak az elso), Elny = elnyelt kartya,\n");
  printf("Kaz = kazettamuvelet (admin rekord), Utols = utolso tranzakcio ideje\n");
}

static void report_rvsl(void)
{
  int k, c, any; char a[32], b[32];
  print_header("MELLEKLET: SZTORNO- ES KORREKCIO-OKOK");
  for (k = 0; k < 2; k++) {
    long tot = 0;
    for (c = 0; c < 100; c++) tot += rvsl_cnt[k][c];
    printf("%s\n", k == 0 ? "A) Sztorno (0420) - RVSL-RSN" : "B) Korrekcio (5400) - RVSL-RSN");
    printf("%-4s%-36s%10s%9s  %s\n", "Kod", "Megnevezes", "Darab", "Arany", "Hiba?");
    line(70, '-');
    any = 0;
    for (c = 0; c < 100; c++) {
      if (!rvsl_cnt[k][c]) continue;
      any = 1;
      printf("%02d  %-36s%10s%9s  %s\n", c, rvsl_name(k == 0 ? 420 : 5400, c),
             fmt_n(rvsl_cnt[k][c], a), pct(rvsl_cnt[k][c], tot, b),
             is_fault(k == 0 ? 420 : 5400, c) ? "igen" : "");
    }
    if (!any) printf("  nem volt\n");
    line(70, '-');
    printf("\n");
  }
  printf("Hiba? = az ATM-enkenti riportban hibakent szamolva\n");
}

/* ---------- main ---------- */
static void usage(void)
{
  fprintf(stderr, "Hasznalat: TLFRPT [-a nn] [-r respfile] [-z perc] [-d n] tlf-fajl\n");
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
      case 'r': case 'R': opt_respfile = argv[++i]; break;
      case 'z': case 'Z': opt_tz_min = atol(argv[++i]); opt_tz_set = 1; break;
      case 'd': case 'D': opt_dump = atoi(argv[++i]); break;
      default: usage();
      }
    } else if (!opt_file) opt_file = argv[i];
    else usage();
  }
  if (!opt_file) usage();
  load_builtin_resp();
  if (opt_respfile) load_resp(opt_respfile);

  if (rd_open(opt_file)) return 1;
  while ((len = rd_next(buf, MAXREC)) > 0) process(buf, len);
  rd_close();
  if (len < 0) fprintf(stderr, "Olvasasi hiba, a riport hianyos lehet.\n");

  if (opt_dump) {
    printf("Bajtsorrend: %s. Tranzakcios rekordok: %ld, admin: %ld\n",
           big_endian ? "big-endian" : "little-endian", n_tran, n_admin);
    return 0;
  }
  if (n_tran == 0) {
    printf("Nincs tranzakcios rekord (REC-TYP 01) a fajlban: %s\n", opt_file);
    return 0;
  }
  report_summary();
  report_resp();
  report_hourly();
  report_atm();
  report_rvsl();
  return 0;
}
