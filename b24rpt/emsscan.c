/*
 * EMSSCAN - EMS esemenyek kozott hibauzenetek keresese
 *
 * Bemenet: az EMSDIST printing distributor altal szovegge alakitott EMS log
 * (TEXTOUT fajl, lasd az EMSERR makrot). A ZZEVnnnn (843-as kodu) fajlok
 * binarisak, kozvetlenul nem olvashatok; ezeket az EMSDIST alakitja szovegge.
 *
 * Hasznalat (Guardian):
 *   RUN EMSSCAN /OUT $S.#EMSERR/ $DATA.EMSRPT.EMSTXT
 *   RUN EMSSCAN $DATA.EMSRPT.EMSTXT DOWN TIMEOUT      (sajat keresoszavak)
 *
 * Ha a fajlnev utan szavakat adsz meg, a beepitett lista helyett azokat
 * keresi (kis/nagybetu mindegy).
 *
 * Mukodes:
 *   - Egy esemeny: a szamjeggyel (vagy '*'-gal) kezdodo sor es az utana
 *     kovetkezo, nem szamjeggyel kezdodo folytatosorok.
 *   - Talalat: ha az esemeny barmelyik soraban szerepel egy keresoszo, vagy az
 *     esemeny '*'-gal kezdodik (kritikus/kiemelt esemeny).
 *   - Kimenet: osszesito (keresoszavankent, forrasonkent), majd a talalt
 *     esemenyek teljes szovege.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef __TANDEM
#include <cextdecs(FILE_OPEN_,READX,FILE_GETINFO_,FILE_CLOSE_)>
#endif

#define MAXLINE     512
#define MAXEVLINES  40           /* egy esemeny max. sora */
#define MAXSRC      400          /* kulonbozo forrasok szama */
#define MAXSHOW     2000         /* legfeljebb ennyi esemenyt ir ki teljes szoveggel */
#define RPT_W       79

static const char *default_words[] = {
  "ERROR", "FAIL", "ABEND", "ABORT", "TRAP", "HALT", "DOWN", "TIMEOUT",
  "TIME-OUT", "TIMED OUT", "UNABLE", "CANNOT", "NOT AVAILABLE",
  "NOT RESPONDING", "STOPPED", "DEADLOCK", "VIOLATION", "CRITICAL",
  "LOST", "DISCONNECT", "OVERFLOW", "FULL", NULL };

static const char *words[64];
static int  nwords;
static long word_cnt[64];
static long n_lines, n_events, n_hits, n_crit, n_shown;

typedef struct { char name[48]; long cnt; } Src;
static Src src[MAXSRC]; static int n_src;

/* aktualis esemeny */
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
  while (k >= 0 && (s[k] == ' ' || s[k] == '\t' || s[k] == 0)) s[k--] = 0;
}

/* kis/nagybetu-fuggetlen reszszoveg kereses */
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

/* uj esemeny kezdete: (opcionalis '*' es szokozok utan) szamjegy */
static int is_event_start(const char *s)
{
  while (*s == ' ') s++;
  if (*s == '*') return 1;
  return isdigit((unsigned char)*s) != 0;
}

/* forras: az esemeny elso soraban a datum/ido utani elso ket szo
   (tipikusan a processz neve es a subsystem), pl. "$ZHOME TANDEM.EMS.L01" */
static void event_source(const char *s, char *out)
{
  char tmp[MAXLINE], *tok[8]; int nt = 0, i, start = 0;
  strncpy(tmp, s, MAXLINE - 1); tmp[MAXLINE - 1] = 0;
  for (i = 0; tmp[i] && nt < 8; ) {
    while (tmp[i] == ' ') tmp[i++] = 0;
    if (!tmp[i]) break;
    tok[nt++] = &tmp[i];
    while (tmp[i] && tmp[i] != ' ') i++;
  }
  /* '*', datum es ido atugrasa: szamjeggyel vagy '*'-gal kezdodo tokenek */
  while (start < nt && (tok[start][0] == '*' || isdigit((unsigned char)tok[start][0]))) start++;
  out[0] = 0;
  if (start < nt) {
    strncat(out, tok[start], 23);
    if (start + 1 < nt) { strcat(out, " "); strncat(out, tok[start + 1], 23); }
  }
  if (!out[0]) strcpy(out, "(ismeretlen)");
}

static void count_src(const char *name)
{
  int i;
  for (i = 0; i < n_src; i++) if (strcmp(src[i].name, name) == 0) { src[i].cnt++; return; }
  if (n_src < MAXSRC) { strcpy(src[n_src].name, name); src[n_src].cnt = 1; n_src++; }
}

/* 1. menet: szamlalas; 2. menet: a talalt esemenyek kiirasa */
static int pass;

static void flush_event(void)
{
  int i, k, hit = 0, crit = 0, matched[64];
  if (ev_n == 0) return;
  if (pass == 1) n_events++;
  for (k = 0; k < nwords; k++) matched[k] = 0;
  {
    const char *p = ev[0];
    while (*p == ' ') p++;
    if (*p == '*') crit = 1;
  }
  for (i = 0; i < ev_n; i++)
    for (k = 0; k < nwords; k++)
      if (!matched[k] && contains_ci(ev[i], words[k])) { matched[k] = 1; hit = 1; }
  if (hit || crit) {
    char sname[64];
    if (pass == 1) {
      n_hits++;
      if (crit) n_crit++;
      for (k = 0; k < nwords; k++) if (matched[k]) word_cnt[k]++;
      event_source(ev[0], sname);
      count_src(sname);
    } else if (n_shown < MAXSHOW) {
      for (i = 0; i < ev_n; i++) printf("%s\n", ev[i]);
      printf("\n");
      n_shown++;
    }
  }
  ev_n = 0;
}

static int cmp_src(const void *a, const void *b)
{
  long x = ((const Src *)a)->cnt, y = ((const Src *)b)->cnt;
  return (y > x) - (y < x);
}

static int scan(const char *fn, int which)
{
  char buf[MAXLINE]; int r;
  pass = which; ev_n = 0;
  if (rd_open(fn)) return -1;
  while ((r = rd_line(buf, MAXLINE)) > 0) {
    if (pass == 1) n_lines++;
    rtrim(buf);
    if (!buf[0]) continue;
    if (is_event_start(buf)) flush_event();
    if (ev_n < MAXEVLINES) { strcpy(ev[ev_n], buf); ev_n++; }
  }
  flush_event();
  rd_close();
  if (r < 0) fprintf(stderr, "Olvasasi hiba, a riport hianyos lehet.\n");
  return 0;
}

int main(int argc, char *argv[])
{
  char a[32], b[32], c[32], d[32];
  int i;

  if (argc < 2) {
    fprintf(stderr, "Hasznalat: EMSSCAN ems-szovegfajl [keresoszo ...]\n");
    return 2;
  }
  if (argc > 2) {
    for (i = 2; i < argc && nwords < 63; i++) words[nwords++] = argv[i];
  } else {
    for (i = 0; default_words[i]; i++) words[nwords++] = default_words[i];
  }

  if (scan(argv[1], 1)) return 1;

  printf("\n");
  line(RPT_W, '=');
  printf("EMS HIBAKERESES\n");
  printf("Forras:  %s\n", argv[1]);
  line(RPT_W, '=');
  printf("Sorok: %s, esemenyek: %s, talalat: %s (ebbol kritikus '*': %s)\n\n",
         fmt_n(n_lines, a), fmt_n(n_events, b), fmt_n(n_hits, c), fmt_n(n_crit, d));

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

  printf("\nC) Talalt esemenyek");
  if (n_hits > MAXSHOW) printf(" (az elso %d)", MAXSHOW);
  printf("\n");
  line(RPT_W, '-');
  scan(argv[1], 2);
  if (!n_hits) printf("  nincs talalat\n");
  return 0;
}
