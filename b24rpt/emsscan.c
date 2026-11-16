/*
 * EMSSCAN - EMS esemenyek kozott hibauzenetek keresese
 *
 * Bemenet: az EMSDIST printing distributor altal szovegge alakitott EMS log
 * (TEXTOUT fajl, lasd az EMSERR makrot). A ZZEVnnnn (843-as kodu) fajlok
 * binarisak, kozvetlenul nem olvashatok; ezeket az EMSDIST alakitja szovegge.
 *
 * Hasznalat (Guardian):
 *   RUN EMSSCAN /OUT $S.#EMSERR/ $DATA.EMSRPT.EMSTXT
 *   RUN EMSSCAN $DATA.EMSRPT.EMSTXT HSM SECDEV1      (sajat keresoszavak)
 *
 * Ha a fajlnev utan szavakat adsz meg, a beepitett lista helyett azokat
 * keresi (kis/nagybetu mindegy).
 *
 * Az EMSDIST szoveges formatuma (pelda):
 *   26-11-15 09:00:16 \EH08.$P1XML       *ACI.1042.1000        000111 Mgr PPD:
 *                                          \EH08.$P1ZN  Generator: P1Z^XML
 *                                          SENDMSG: Unable to send and wait ...
 *   - uj esemeny: a sor elejen "yy-mm-dd" datum
 *   - folytatosor: szokozzel kezdodik
 *   - '*' a subsystem elott: kritikus (kiemelt) esemeny
 *
 * Talalat: ha az esemeny szovegeben szerepel egy keresoszo, vagy kritikus.
 * A talalatokat esemenytipusonkent (processz + subsystem + esemenyszam)
 * csoportositja: darabszam, elso es utolso elofordulas, egy minta szovege.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef __TANDEM
#include <cextdecs(FILE_OPEN_,READX,FILE_GETINFO_,FILE_CLOSE_)>
#endif

#define MAXLINE     512
#define MAXEVLINES  30           /* egy esemeny max. sora */
#define SAMPLE_L    12           /* mintaszoveg max. sora */
#define SAMPLE_W    74           /* mintaszoveg sorhossza */
#define MAXGRP      500          /* kulonbozo esemenytipusok */
#define MAXSRC      300
#define RPT_W       79

static const char *default_words[] = {
  "ERROR", "FAIL", "ABEND", "ABORT", "TRAP", "HALT", "DOWN", "TIMEOUT",
  "TIME-OUT", "TIMED OUT", "UNABLE", "CANNOT", "UNAVAILABLE",
  "NOT AVAILABLE", "NOT RESPONDING", "STOPPED", "DEADLOCK", "VIOLATION",
  "CRITICAL", "LOST", "DISCONNECT", "OVERFLOW", "FULL", NULL };

static const char *words[64];
static int  nwords;
static long word_cnt[64];
static long n_lines, n_events, n_hits, n_crit;
static char first_ts[20], last_ts[20];

/* esemenytipus-csoport */
typedef struct {
  char key[64];                  /* processz subsystem esemenyszam */
  char proc[24], subsys[24], evnum[8];
  int  crit;
  long cnt;
  char first[20], last[20];
  int  ns;
  char sample[SAMPLE_L][SAMPLE_W + 1];
} Grp;
static Grp grp[MAXGRP]; static int n_grp, grp_full;

typedef struct { char name[48]; long cnt; } Src;
static Src src[MAXSRC]; static int n_src;

static char ev[MAXEVLINES][MAXLINE];
static int  ev_n;

/* ---------- Fajl olvasas: soronkent ---------- */
#ifdef __TANDEM
static short fnum = -1;
static int rd_open(const char *name)
{
  short err = FILE_OPEN_(name, (short)strlen(name), &fnum, 1, 0);
  if (err) { fprintf(stderr, "FILE_OPEN_ hiba %d: %s\n", err, name); return -1; }
  return 0;
}
static int rd_line(char *buf, int max)       /* 1 = sor, 0 = EOF, -1 = hiba */
{
  unsigned short cnt = 0; short err = 0;
  READX(fnum, buf, (unsigned short)(max - 1), &cnt);
  FILE_GETINFO_(fnum, &err);
  if (err == 1) return 0;
  if (err != 0) { fprintf(stderr, "READX hiba %d\n", err); return -1; }
  buf[cnt] = 0;
  return 1;
}
static void rd_close(void) { if (fnum >= 0) FILE_CLOSE_(fnum); }
#else
static FILE *rf;
static int rd_open(const char *name)
{
  rf = fopen(name, "r");
  if (!rf) { perror(name); return -1; }
  return 0;
}
static int rd_line(char *buf, int max)
{
  if (!fgets(buf, max, rf)) return 0;
  buf[strcspn(buf, "\r\n")] = 0;
  return 1;
}
static void rd_close(void) { if (rf) fclose(rf); }
#endif

/* ---------- Segedfuggvenyek ---------- */
static void rtrim(char *s)
{
  int k = (int)strlen(s) - 1;
  while (k >= 0 && (s[k] == ' ' || s[k] == '\t')) s[k--] = 0;
}

static int contains_ci(const char *hay, const char *needle)
{
  size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t i;
    for (i = 0; i < n; i++)
      if (!hay[i] || toupper((unsigned char)hay[i]) != toupper((unsigned char)needle[i])) break;
    if (i == n) return 1;
  }
  return 0;
}

static char *fmt_n(long v, char *out)
{
  char tmp[32]; int i, j = 0, len;
  sprintf(tmp, "%ld", v);
  len = (int)strlen(tmp);
  for (i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0) out[j++] = ' ';
    out[j++] = tmp[i];
  }
  out[j] = 0;
  return out;
}

static void line(int n, char c) { while (n-- > 0) putchar(c); putchar('\n'); }

/* uj esemeny: a sor elejen "yy-mm-dd" */
static int is_event_start(const char *s)
{
  return isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && s[2] == '-' &&
         isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4]) && s[5] == '-' &&
         isdigit((unsigned char)s[6]) && isdigit((unsigned char)s[7]);
}

/* az elso sor szavakra bontasa */
static int split(const char *s, char *tmp, char **tok, int maxt)
{
  int nt = 0, i = 0;
  { size_t L = strlen(s); if (L > MAXLINE - 1) L = MAXLINE - 1; memcpy(tmp, s, L); tmp[L] = 0; }
  while (tmp[i] && nt < maxt) {
    while (tmp[i] == ' ') tmp[i++] = 0;
    if (!tmp[i]) break;
    tok[nt++] = &tmp[i];
    while (tmp[i] && tmp[i] != ' ') i++;
  }
  return nt;
}

static void count_src(const char *name)
{
  int i;
  for (i = 0; i < n_src; i++) if (strcmp(src[i].name, name) == 0) { src[i].cnt++; return; }
  if (n_src < MAXSRC) { strcpy(src[n_src].name, name); src[n_src].cnt = 1; n_src++; }
}

/* mintaszoveg: az elso sor az esemenyszam utani resz, majd a folytatosorok
   bal oldali szokozok nelkul, SAMPLE_W szelessegre tordelve */
static void add_sample(Grp *g, const char *text)
{
  const char *p = text;
  while (*p == ' ') p++;
  while (*p && g->ns < SAMPLE_L) {
    int n = (int)strlen(p);
    if (n > SAMPLE_W) {
      int cut = SAMPLE_W;
      while (cut > 20 && p[cut] != ' ') cut--;
      if (cut <= 20) cut = SAMPLE_W;
      strncpy(g->sample[g->ns], p, (size_t)cut); g->sample[g->ns][cut] = 0;
      g->ns++;
      p += cut;
      while (*p == ' ') p++;
    } else {
      strcpy(g->sample[g->ns++], p);
      break;
    }
  }
}

static void flush_event(void)
{
  char tmp[MAXLINE], *tok[12], ts[20], proc[24], subsys[24], evnum[8], key[64];
  int nt, i, k, hit = 0, crit = 0, matched[64];
  Grp *g = NULL;

  if (ev_n == 0) return;
  n_events++;
  for (k = 0; k < nwords; k++) matched[k] = 0;
  for (i = 0; i < ev_n; i++)
    for (k = 0; k < nwords; k++)
      if (!matched[k] && contains_ci(ev[i], words[k])) { matched[k] = 1; hit = 1; }

  /* elso sor: datum ido \NODE.$PROC [*]SUBSYS ESEMENYSZAM szoveg... */
  nt = split(ev[0], tmp, tok, 12);
  ts[0] = proc[0] = subsys[0] = evnum[0] = 0;
  if (nt >= 2) sprintf(ts, "%.8s %.8s", tok[0], tok[1]);
  if (nt >= 3) {
    char *pp = strchr(tok[2], '.');
    strncat(proc, (tok[2][0] == '\\' && pp) ? pp + 1 : tok[2], 23);
  }
  if (nt >= 4) {
    char *s = tok[3];
    if (*s == '*') { crit = 1; s++; }
    strncat(subsys, s, 23);
  }
  if (nt >= 5 && isdigit((unsigned char)tok[4][0])) strncat(evnum, tok[4], 7);

  if (ts[0]) {
    if (!first_ts[0]) strcpy(first_ts, ts);
    strcpy(last_ts, ts);
  }
  if (!(hit || crit)) { ev_n = 0; return; }

  n_hits++;
  if (crit) n_crit++;
  for (k = 0; k < nwords; k++) if (matched[k]) word_cnt[k]++;
  {
    char sname[48];
    sprintf(sname, "%.20s %.23s", proc[0] ? proc : "?", subsys[0] ? subsys : "?");
    count_src(sname);
  }

  sprintf(key, "%s|%s|%s", proc, subsys, evnum);
  for (i = 0; i < n_grp; i++) if (strcmp(grp[i].key, key) == 0) { g = &grp[i]; break; }
  if (!g) {
    if (n_grp >= MAXGRP) { grp_full = 1; ev_n = 0; return; }
    g = &grp[n_grp++];
    memset(g, 0, sizeof(Grp));
    strcpy(g->key, key); strcpy(g->proc, proc); strcpy(g->subsys, subsys);
    strcpy(g->evnum, evnum); strcpy(g->first, ts);
    /* minta: az elso sor esemenyszam utani resze + folytatosorok */
    {
      const char *q = subsys[0] ? strstr(ev[0], subsys) : NULL, *p = NULL;
      if (q && evnum[0]) { p = strstr(q, evnum); if (p) p += strlen(evnum); }
      if (p) add_sample(g, p);
      for (i = 1; i < ev_n && g->ns < SAMPLE_L; i++) add_sample(g, ev[i]);
    }
  }
  g->cnt++;
  if (crit) g->crit = 1;
  if (ts[0]) strcpy(g->last, ts);
  ev_n = 0;
}

static int cmp_src(const void *a, const void *b)
{
  long x = ((const Src *)a)->cnt, y = ((const Src *)b)->cnt;
  return (y > x) - (y < x);
}

/* kritikusak elol, azon belul darabszam szerint csokkeno */
static int cmp_grp(const void *a, const void *b)
{
  const Grp *x = (const Grp *)a, *y = (const Grp *)b;
  if (x->crit != y->crit) return y->crit - x->crit;
  return (y->cnt > x->cnt) - (y->cnt < x->cnt);
}

int main(int argc, char *argv[])
{
  char buf[MAXLINE], a[32], b[32], c[32], d[32];
  int i, k, r;

  if (argc < 2) {
    fprintf(stderr, "Hasznalat: EMSSCAN ems-szovegfajl [keresoszo ...]\n");
    return 2;
  }
  if (argc > 2) for (i = 2; i < argc && nwords < 63; i++) words[nwords++] = argv[i];
  else for (i = 0; default_words[i]; i++) words[nwords++] = default_words[i];

  if (rd_open(argv[1])) return 1;
  while ((r = rd_line(buf, MAXLINE)) > 0) {
    n_lines++;
    rtrim(buf);
    if (!buf[0]) continue;
    if (is_event_start(buf)) flush_event();
    else if (ev_n == 0) continue;            /* esemeny elotti sorok */
    if (ev_n < MAXEVLINES) { strcpy(ev[ev_n], buf); ev_n++; }
  }
  flush_event();
  rd_close();
  if (r < 0) fprintf(stderr, "Olvasasi hiba, a riport hianyos lehet.\n");

  printf("\n");
  line(RPT_W, '=');
  printf("EMS HIBAKERESES\n");
  printf("Forras:  %s\n", argv[1]);
  printf("Idoszak: %s - %s\n", first_ts[0] ? first_ts : "-", last_ts[0] ? last_ts : "-");
  line(RPT_W, '=');
  printf("Sorok: %s, esemenyek: %s, talalat: %s (ebbol kritikus: %s)\n",
         fmt_n(n_lines, a), fmt_n(n_events, b), fmt_n(n_hits, c), fmt_n(n_crit, d));
  printf("Kulonbozo esemenytipus a talalatok kozott: %d%s\n\n", n_grp,
         grp_full ? " (tobb is volt, a lista hianyos)" : "");

  printf("A) Keresoszavak\n");
  printf("%-30s%10s\n", "Keresoszo", "Esemeny");
  line(40, '-');
  for (i = 0; i < nwords; i++)
    if (word_cnt[i]) printf("%-30.30s%10s\n", words[i], fmt_n(word_cnt[i], a));
  line(40, '-');

  printf("\nB) Forrasok (processz, subsystem) - legfeljebb 30\n");
  printf("%-50s%10s\n", "Forras", "Talalat");
  line(60, '-');
  qsort(src, n_src, sizeof(Src), cmp_src);
  for (i = 0; i < n_src && i < 30; i++)
    printf("%-50.50s%10s\n", src[i].name, fmt_n(src[i].cnt, a));
  line(60, '-');

  printf("\nC) Esemenytipusok (kritikusak elol, majd gyakorisag szerint)\n");
  line(RPT_W, '-');
  qsort(grp, n_grp, sizeof(Grp), cmp_grp);
  for (i = 0; i < n_grp; i++) {
    Grp *g = &grp[i];
    printf("%s%s %s %s   %s db\n", g->crit ? "*" : " ", g->proc, g->subsys,
           g->evnum, fmt_n(g->cnt, a));
    if (g->cnt > 1) printf("  elso: %s   utolso: %s\n", g->first, g->last);
    else printf("  ido:  %s\n", g->first);
    for (k = 0; k < g->ns; k++) printf("    %s\n", g->sample[k]);
    printf("\n");
  }
  if (!n_grp) printf("  nincs talalat\n");
  line(RPT_W, '-');
  printf("* = kritikus (kiemelt) EMS esemeny\n");
  return 0;
}
