/*
 * TCPRATE - TCP kapcsolatok forgalma a CLIM-en (ss -tin ket pillanatkepbol)
 *
 * A TCPRATE TACL makro hivja: ket pillanatkepet keszit a climcmd/ss
 * paranccsal, es az IN fajlon (RUN /INV valtozo/) adja at ennek a programnak.
 *
 * Bemenet (soronkent):
 *   @@T <menet> <JULIANTIMESTAMP>          a menet kezdete (1 vagy 2)
 *   @@S <menet> <clim> <namespace>         egy ss kimenet kezdete
 *   ... az ss -tin kimenete ...
 *
 *   @@D <menet> yyyy mm dd hh mm ss cc     ugyanez helyi idoben (#CONTIME)
 *
 * Uzemmodok:
 *   TCPRATE port [mp] [R]                 egy meres (TCPRATE makro)
 *   TCPRATE L naplo port:nev [port:nev..] naplosor portonkent (DAILYLOOP, 5 perc)
 *   TCPRATE D naplo                       orankenti riport a napi naplobol
 *
 * A menet sorszamat a @@T sorok sorrendje adja (elso = 1, masodik = 2).
 *
 * Kimenet (max. 79 oszlop):
 *   rovid osszefoglalo: a meresi idoszakban beerkezett es kimeno uzenetek
 *   szama, kapcsolatok szama, megoszlas tuloldalankent, allapot
 *   R parameterrel ezen felul: kapcsolatonkenti es tuloldal szerinti reszletek
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef __TANDEM
#include <cextdecs(FILE_OPEN_,READX,WRITEX,FILE_GETINFO_,FILE_CLOSE_)>
#endif

#define TCPRATE_VER "2026-11-16 v5 (tordeles-turo)"
#define MAXLINE   8192
#define MAXCONN   2000
#define MAXPEER   200
#define RPT_W     79

typedef struct {
  char key[160];                 /* clim|ns|local|peer */
  char clim[17], ns[33], local[48], peer[48];
  int  seen[3];                  /* [1], [2] menetben latott */
  long long dso[3], dsi[3], bsent[3], brecv[3], bretr[3];
  double rtt, rttvar, minrtt;
  long sendq, recvq, lastrcv, lastsnd, cwnd;
} Conn;

static Conn conn[MAXCONN]; static int n_conn;
static long long t_snap[3];
static int  port;
static double sec_param = 60;

/* ---------- segedek ---------- */
static void line(int n, char c) { while (n-- > 0) putchar(c); putchar('\n'); }

static char *fmt_d(double v, int dec, char *out)
{
  char *c;
  sprintf(out, "%.*f", dec, v);
  if ((c = strchr(out, '.')) != NULL) *c = ',';
  return out;
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

/* sajat 64 bites szamolvaso (az atoll C99, nem minden fordito ismeri) */
static long long to_ll(const char *s)
{
  long long v = 0; int neg = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return neg ? -v : v;
}

static int is_state(const char *t)
{
  static const char *st[] = { "ESTAB", "SYN-SENT", "SYN-RECV", "FIN-WAIT-1",
    "FIN-WAIT-2", "TIME-WAIT", "CLOSE-WAIT", "LAST-ACK", "CLOSING", "CLOSED",
    "LISTEN", "UNCONN", NULL };
  int i;
  for (i = 0; st[i]; i++) if (strcmp(t, st[i]) == 0) return 1;
  return 0;
}

static Conn *get_conn(const char *clim, const char *ns, const char *local, const char *peer)
{
  char key[160]; int i;
  sprintf(key, "%.16s|%.32s|%.47s|%.47s", clim, ns, local, peer);
  for (i = 0; i < n_conn; i++) if (strcmp(conn[i].key, key) == 0) return &conn[i];
  if (n_conn >= MAXCONN) return NULL;
  memset(&conn[n_conn], 0, sizeof(Conn));
  strcpy(conn[n_conn].key, key);
  sprintf(conn[n_conn].clim, "%.16s", clim);
  sprintf(conn[n_conn].ns, "%.32s", ns);
  sprintf(conn[n_conn].local, "%.47s", local);
  sprintf(conn[n_conn].peer, "%.47s", peer);
  return &conn[n_conn++];
}

/* ---------- egy ss blokk feldolgozasa ---------- */
static char **blk; static int blk_n, blk_cap;
static int cur_pass; static char cur_clim[17], cur_ns[33];

static void blk_add(const char *s)
{
  if (blk_n >= blk_cap) {
    blk_cap = blk_cap ? blk_cap * 2 : 256;
    blk = (char **)realloc(blk, (size_t)blk_cap * sizeof(char *));
    if (!blk) { fprintf(stderr, "Keves memoria\n"); exit(1); }
  }
  blk[blk_n] = (char *)malloc(strlen(s) + 1);
  if (blk[blk_n]) strcpy(blk[blk_n++], s);
}

/* kulcs keresese a kapcsolat osszefuzott szovegeben. A tordeles miatt a kulcs
   az elozo szohoz ragadhat (pl. "18.1Mbpslastsnd:"), ezert csak az "rtt:"
   kulcsnal tiltjuk a betu-elotagot (hogy ne a "minrtt:"-t talalja meg). */
static const char *find_key(const char *buf, const char *key)
{
  const char *p = buf;
  size_t n = strlen(key);
  int guard = !strcmp(key, "rtt:");     /* "minrtt:", "rcv_rtt:" kizarasa */
  while ((p = strstr(p, key)) != NULL) {
    if (!guard || p == buf || !(isalpha((unsigned char)p[-1]) || p[-1] == '_')) return p + n;
    p += n;
  }
  return NULL;
}

static void parse_info(Conn *c, int pas, const char *buf)
{
  const char *v;
  if ((v = find_key(buf, "data_segs_out:")) != NULL)  c->dso[pas] = to_ll(v);
  if ((v = find_key(buf, "data_segs_in:")) != NULL)   c->dsi[pas] = to_ll(v);
  if ((v = find_key(buf, "bytes_sent:")) != NULL)     c->bsent[pas] = to_ll(v);
  if ((v = find_key(buf, "bytes_received:")) != NULL) c->brecv[pas] = to_ll(v);
  if ((v = find_key(buf, "bytes_retrans:")) != NULL)  c->bretr[pas] = to_ll(v);
  if (pas == 2) {
    if ((v = find_key(buf, "rtt:")) != NULL) {
      const char *sl = strchr(v, '/');
      c->rtt = atof(v);
      if (sl) c->rttvar = atof(sl + 1);
    }
    if ((v = find_key(buf, "minrtt:")) != NULL)  c->minrtt = atof(v);
    if ((v = find_key(buf, "lastrcv:")) != NULL) c->lastrcv = (long)to_ll(v);
    if ((v = find_key(buf, "lastsnd:")) != NULL) c->lastsnd = (long)to_ll(v);
    if ((v = find_key(buf, "cwnd:")) != NULL)    c->cwnd = (long)to_ll(v);
  }
}

/* elso szo (szokozok atugrasa utan) */
static void first_word(const char *s, char *out, int max)
{
  int k = 0;
  while (*s && isspace((unsigned char)*s)) s++;
  while (*s && !isspace((unsigned char)*s) && k < max - 1) out[k++] = *s++;
  out[k] = 0;
}

static int count_words(const char *s)
{
  int n = 0;
  while (*s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) break;
    n++;
    while (*s && !isspace((unsigned char)*s)) s++;
  }
  return n;
}

/* Egy ss blokk feldolgozasa. A climcmd fajlba irt kimeneteben a hosszu sorok
   barhol eltorhetnek (szam vagy kulcs kozepen is), es a sorvegi szokozok
   elveszhetnek. Ezert:
   - uj kapcsolat: allapot-szoval (ESTAB stb.) kezdodo sor; ha abban nincs meg
     mind a 4 mezo (Recv-Q Send-Q Local Peer), a kovetkezo sor hozza ragad;
   - a kapcsolat tobbi sora elvalaszto nelkul osszefuzve (szokozzel kezdodo
     sor ele szokoz kerul), es a kulcsokat szovegkereses talalja meg. */
/* van-e a szovegben ket ':'-ot tartalmazo szo (rosszul osszeragasztott cim) */
static int bad_addr(const char *h)
{
  int colons = 0;
  for (; *h; h++) {
    if (isspace((unsigned char)*h)) colons = 0;
    else if (*h == ':' && ++colons > 1) return 1;
  }
  return 0;
}

static void blk_flush(void)
{
  int i, j;
  size_t tot = 1;
  char *info = NULL, *ip = NULL, first[32];
  Conn *c = NULL;

  if (!blk_n) return;
  for (i = 0; i < blk_n; i++) tot += strlen(blk[i]) + 2;
  info = (char *)malloc(tot);
  if (!info) { fprintf(stderr, "Keves memoria\n"); exit(1); }

  /* Szabaly: a valodi ss kimenetben minden folytatosor szokozzel/tabbal
     kezdodik. Ha egy sor nem igy kezdodik (es nem allapot-szo), akkor az
     elozo sor folytatasa, amit a tordeles vagott el: elvalaszto nelkul
     ragad hozza. */
  for (i = 0; i < blk_n; i++) {
    first_word(blk[i], first, sizeof first);
    if (is_state(first) && !isspace((unsigned char)blk[i][0])) {
      char hdr[1024], tmp[1024], *tok[6];
      int nt;
      if (c && ip) { *ip = 0; parse_info(c, cur_pass, info); }
      c = NULL;
      sprintf(hdr, "%.1000s", blk[i]);
      while (i + 1 < blk_n) {
        char f2[32];
        const char *nx = blk[i + 1];
        first_word(nx, f2, sizeof f2);
        if (is_state(f2) && !isspace((unsigned char)nx[0])) break;
        if (isspace((unsigned char)nx[0])) {
          /* szokozzel kezdodo sor: csak akkor a fejlec resze, ha meg hianyzik mezo */
          if (count_words(hdr) >= 5) break;
          strncat(hdr, " ", sizeof hdr - strlen(hdr) - 1);
          strncat(hdr, nx, sizeof hdr - strlen(hdr) - 1);
        } else {
          /* elvagott fejlec: ragasztas; ha igy hibas cim jonne ki (ket ':'),
             akkor a vagas szokoznel volt -> szokozzel */
          size_t L = strlen(hdr);
          strncat(hdr, nx, sizeof hdr - L - 1);
          if (bad_addr(hdr)) {
            hdr[L] = 0;
            strncat(hdr, " ", sizeof hdr - strlen(hdr) - 1);
            strncat(hdr, nx, sizeof hdr - strlen(hdr) - 1);
          }
        }
        i++;
      }
      strcpy(tmp, hdr);
      nt = 0;
      {
        char *q = tmp;
        while (*q && nt < 6) {
          while (*q && isspace((unsigned char)*q)) *q++ = 0;
          if (!*q) break;
          tok[nt++] = q;
          while (*q && !isspace((unsigned char)*q)) q++;
        }
      }
      if (nt >= 5 && strchr(tok[3], ':') && strchr(tok[4], ':')) {
        c = get_conn(cur_clim, cur_ns, tok[3], tok[4]);
        if (c) {
          c->seen[cur_pass] = 1;
          if (cur_pass == 2) { c->recvq = atol(tok[1]); c->sendq = atol(tok[2]); }
        }
      }
      ip = info; *ip = 0;
      continue;
    }
    if (!c || !ip) continue;
    if (isspace((unsigned char)blk[i][0])) *ip++ = ' ';
    for (j = 0; blk[i][j]; j++) *ip++ = blk[i][j];
    *ip = 0;
  }
  if (c && ip) { *ip = 0; parse_info(c, cur_pass, info); }
  for (i = 0; i < blk_n; i++) free(blk[i]);
  blk_n = 0;
  free(info);
}

/* ---------- riport ---------- */
static char *ip_of(const char *addr, char *out)
{
  const char *col = strrchr(addr, ':');
  size_t n = col ? (size_t)(col - addr) : strlen(addr);
  if (n > 39) n = 39;
  memcpy(out, addr, n); out[n] = 0;
  return out;
}

typedef struct { char ip[40]; int n; double in, out; double kbin, kbout; } Peer;
static Peer peers[MAXPEER]; static int n_peer;

static int cmp_conn(const void *a, const void *b)
{
  const Conn *x = (const Conn *)a, *y = (const Conn *)b;
  int r = strcmp(x->clim, y->clim);
  if (!r) r = strcmp(x->ns, y->ns);
  if (!r) r = strcmp(x->peer, y->peer);
  return r;
}

/* "@@D <menet> yyyy mm dd hh mm ss cc" (TACL #CONTIME) */
static char t_text[3][24];       /* "yyyy.mm.dd hh:mm:ss" */
static char t_iso[3][24];        /* "yyyy-mm-dd hh:mm:ss" (naplo) */
static void set_time_text(int p, const char *s)
{
  int q, y, mo, d, h, mi, se;
  if (p < 1 || p > 2) return;
  if (sscanf(s, "%d %d %d %d %d %d %d", &q, &y, &mo, &d, &h, &mi, &se) == 7) {
    sprintf(t_text[p], "%04d.%02d.%02d %02d:%02d:%02d", y, mo, d, h, mi, se);
    sprintf(t_iso[p], "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, se);
  }
}

/* ---------- bemenet beolvasasa (stdin = TACL /INV/) ---------- */
static void read_input(void)
{
  static char ln[MAXLINE];
  int npass = 0;
  while (fgets(ln, MAXLINE, stdin)) {
    ln[strcspn(ln, "\r\n")] = 0;
    if (!strncmp(ln, "@@T ", 4)) {
      const char *q = strchr(ln + 4, ' ');
      blk_flush();
      if (npass < 2) npass++;
      cur_pass = npass;
      if (q) t_snap[npass] = to_ll(q + 1);
      continue;
    }
    if (!strncmp(ln, "@@D ", 4)) { set_time_text(npass, ln + 4); continue; }
    if (!strncmp(ln, "@@S ", 4)) {
      int dummy;
      blk_flush();
      cur_clim[0] = cur_ns[0] = 0;
      sscanf(ln + 4, "%d %16s %32s", &dummy, cur_clim, cur_ns);
      if (!cur_pass) cur_pass = npass ? npass : 1;
      continue;
    }
    if (!strncmp(ln, "Termination Info", 16)) continue;
    if (cur_pass) blk_add(ln);
  }
  blk_flush();
}

static int port_of(const char *addr)
{
  const char *c = strrchr(addr, ':');
  return c ? atoi(c + 1) : -1;
}

/* egy port osszesitett forgalma a ket pillanatkep kozott */
typedef struct {
  long long in, out, bin, bout;
  int conns, sq, cs, rt, gone, nw;
} Stat;

static void stat_port(int p, Stat *st)
{
  int i;
  memset(st, 0, sizeof(Stat));
  for (i = 0; i < n_conn; i++) {
    Conn *x = &conn[i];
    if (p > 0 && port_of(x->local) != p && port_of(x->peer) != p) continue;
    if (x->seen[1] && x->seen[2]) {
      /* visszaugro szamlalo (pl. ujraepult kapcsolat ugyanazzal a porttal): 0 */
      if (x->dsi[2] >= x->dsi[1]) st->in += x->dsi[2] - x->dsi[1];
      if (x->dso[2] >= x->dso[1]) st->out += x->dso[2] - x->dso[1];
      if (x->brecv[2] >= x->brecv[1]) st->bin += x->brecv[2] - x->brecv[1];
      if (x->bsent[2] >= x->bsent[1]) st->bout += x->bsent[2] - x->bsent[1];
      st->conns++;
      if (x->sendq > 0) st->sq++;
      if (x->lastrcv > 60000) st->cs++;
      if (x->bretr[2] > x->bretr[1]) st->rt++;
    } else if (x->seen[2]) {          /* kozben letrejott: minden forgalma ide tartozik */
      st->in += x->dsi[2]; st->out += x->dso[2];
      st->bin += x->brecv[2]; st->bout += x->bsent[2];
      st->conns++; st->nw++;
    } else st->gone++;
  }
}

/* ---------- naplofajl irasa / olvasasa ---------- */
#ifdef __TANDEM
static short lfnum = -1;
static int log_open(const char *name, int write)
{
  short err = FILE_OPEN_(name, (short)strlen(name), &lfnum, (short)(write ? 2 : 1), 0);
  if (err) { fprintf(stderr, "FILE_OPEN_ hiba %d: %s\n", err, name); return -1; }
  return 0;
}
static int log_write(const char *s)
{
  short err = 0;
  WRITEX(lfnum, (char *)s, (unsigned short)strlen(s));
  FILE_GETINFO_(lfnum, &err);
  if (err) { fprintf(stderr, "WRITEX hiba %d\n", err); return -1; }
  return 0;
}
static int log_read(char *buf, int max)
{
  unsigned short cnt = 0; short err = 0;
  READX(lfnum, buf, (unsigned short)(max - 1), &cnt);
  FILE_GETINFO_(lfnum, &err);
  if (err == 1) return 0;
  if (err) { fprintf(stderr, "READX hiba %d\n", err); return -1; }
  buf[cnt] = 0;
  return 1;
}
static void log_close(void) { if (lfnum >= 0) FILE_CLOSE_(lfnum); }
#else
static FILE *lf;
static int log_open(const char *name, int write)
{
  lf = fopen(name, write ? "a" : "r");
  if (!lf) { perror(name); return -1; }
  return 0;
}
static int log_write(const char *s) { fprintf(lf, "%s\n", s); return 0; }
static int log_read(char *buf, int max)
{
  if (!fgets(buf, max, lf)) return 0;
  buf[strcspn(buf, "\r\n")] = 0;
  return 1;
}
static void log_close(void) { if (lf) fclose(lf); }
#endif

/* L uzemmod: portonkent egy naplosor
   "yyyy-mm-dd hh:mm:ss port nev mp be ki kapcs sq cs rt megsz uj" */
static int mode_log(int argc, char *argv[])
{
  int i; double el;
  if (argc < 4) { fprintf(stderr, "Hasznalat: TCPRATE L naplo port:nev ...\n"); return 2; }
  read_input();
  if (!t_snap[1] || !t_snap[2]) return 0;      /* nincs elozo pillanatkep */
  el = (t_snap[2] - t_snap[1]) / 1e6;
  if (el <= 0) return 0;
  if (log_open(argv[2], 1)) return 1;
  for (i = 3; i < argc; i++) {
    char nm[17] = "", rec[200], a[24];
    int p = atoi(argv[i]);
    const char *c = strchr(argv[i], ':');
    Stat st;
    if (c) sprintf(nm, "%.16s", c + 1); else sprintf(nm, "%d", p);
    stat_port(p, &st);
    sprintf(a, "%.0f", el);
    sprintf(rec, "%s %d %s %s %lld %lld %d %d %d %d %d %d",
            t_iso[2][0] ? t_iso[2] : "0000-00-00 00:00:00", p, nm, a,
            st.in, st.out, st.conns, st.sq, st.cs, st.rt, st.gone, st.nw);
    log_write(rec);
    printf("%s\n", rec);
  }
  log_close();
  return 0;
}

/* D uzemmod: orankenti riport a napi naplobol */
#define MAXP 16
typedef struct {
  int port; char nm[17];
  long long in[24], out[24];
  double pk_in[24], pk_out[24];
  int conns[24], warn[24], nsamp[24];
} PortDay;
static PortDay pd[MAXP]; static int n_pd;

static int mode_daily(int argc, char *argv[])
{
  static char ln[512];
  char day[16] = "", a[32], b[32], c[32], d[32];
  int i, h, r;
  if (argc < 3) { fprintf(stderr, "Hasznalat: TCPRATE D naplo\n"); return 2; }
  if (log_open(argv[2], 0)) return 1;
  while ((r = log_read(ln, sizeof ln)) > 0) {
    char dt[16], tm[16], nm[17];
    int p, el, conns, sq, cs, rt, gone, nw, k;
    long long in, out;
    char sin[32], sout[32];
    if (sscanf(ln, "%15s %15s %d %16s %d %31s %31s %d %d %d %d %d %d", dt, tm, &p, nm, &el,
               sin, sout, &conns, &sq, &cs, &rt, &gone, &nw) != 13) continue;
    in = to_ll(sin); out = to_ll(sout);
    h = atoi(tm);
    if (h < 0 || h > 23 || el <= 0) continue;
    if (!day[0]) strcpy(day, dt);
    for (k = 0; k < n_pd; k++) if (pd[k].port == p) break;
    if (k == n_pd) {
      if (n_pd >= MAXP) continue;
      memset(&pd[k], 0, sizeof(PortDay)); pd[k].port = p; strcpy(pd[k].nm, nm); n_pd++;
    }
    pd[k].in[h] += in; pd[k].out[h] += out;
    if ((double)in / el > pd[k].pk_in[h]) pd[k].pk_in[h] = (double)in / el;
    if ((double)out / el > pd[k].pk_out[h]) pd[k].pk_out[h] = (double)out / el;
    if (conns > pd[k].conns[h]) pd[k].conns[h] = conns;
    if (sq || cs || gone) pd[k].warn[h]++;
    pd[k].nsamp[h]++;
  }
  log_close();

  printf("\n");
  line(RPT_W, '=');
  printf("TCP FORGALOM ORANKENT %s\n", day);
  printf("Forras:  %s (5 percenkenti meres)\n", argv[2]);
  line(RPT_W, '=');
  if (!n_pd) { printf("Nincs adat a naplofajlban.\n"); return 0; }
  for (i = 0; i < n_pd; i++) {
    long long tin = 0, tout = 0; double pk = 0; int pkh = -1, tw = 0, missing = 0;
    printf("\nPort %d (%s)\n", pd[i].port, pd[i].nm);
    printf("%-7s%11s%11s%8s%13s%6s%6s%6s\n", "Ora", "Be (db)", "Ki (db)", "Be/mp",
           "Csucs Be/mp", "Kapcs", "Figy", "Meres");
    line(RPT_W, '-');
    for (h = 0; h < 24; h++) {
      if (!pd[i].nsamp[h]) { missing++; continue; }
      {
        char w[16];
        if (pd[i].warn[h]) sprintf(w, "%d", pd[i].warn[h]); else strcpy(w, "-");
        printf("%02d-%02d  %11s%11s%8s%13s%6d%6s%6d\n", h, h + 1,
               fmt_n(pd[i].in[h], a), fmt_n(pd[i].out[h], b),
               fmt_d(pd[i].in[h] / 3600.0, 1, c), fmt_d(pd[i].pk_in[h], 1, d),
               pd[i].conns[h], w, pd[i].nsamp[h]);
      }
      tin += pd[i].in[h]; tout += pd[i].out[h]; tw += pd[i].warn[h];
      if (pd[i].pk_in[h] > pk) { pk = pd[i].pk_in[h]; pkh = h; }
    }
    line(RPT_W, '-');
    printf("Osszesen: %s be, %s ki", fmt_n(tin, a), fmt_n(tout, b));
    if (pkh >= 0) printf(", csucs %s be/mp (%02d-%02d ora)", fmt_d(pk, 1, c), pkh, pkh + 1);
    printf("\n");
    if (tw) printf("Figyelmeztetes %d meresnel (sorban allo adat, csend vagy megszunt kapcs.)\n", tw);
    if (missing) printf("%d oranal nincs meres (pl. a DAILYLOOP nem futott)\n", missing);
  }
  printf("\nCsucs Be/mp = a legforgalmasabb 5 perc atlaga az adott oraban;\n");
  printf("Meres = az oraban vegzett 5 perces meresek szama (teljes ora: 12)\n");
  return 0;
}

/* ---------- egyszeri meres (TCPRATE makro) ---------- */
static int mode_once(int argc, char *argv[])
{
  double el;
  int i, k, nwarn = 0, detail = 0;
  char a[32], b[32], c[32], d[32], e[32];
  char last_grp[64] = "";
  Stat st;

  port = atoi(argv[1]);
  if (argc > 2) sec_param = atof(argv[2]);
  if (argc > 3 && (argv[3][0] == 'R' || argv[3][0] == 'r')) detail = 1;
  read_input();

  el = (t_snap[1] && t_snap[2] > t_snap[1]) ? (t_snap[2] - t_snap[1]) / 1e6 : sec_param;
  if (el <= 0) el = 1;
  qsort(conn, n_conn, sizeof(Conn), cmp_conn);
  stat_port(port, &st);

  /* tuloldal szerinti bontas */
  for (i = 0; i < n_conn; i++) {
    Conn *x = &conn[i];
    char ip[40];
    long long din = 0, dout = 0, dbin = 0, dbout = 0;
    if (port_of(x->local) != port && port_of(x->peer) != port) continue;
    if (x->seen[1] && x->seen[2]) {
      din = x->dsi[2] - x->dsi[1]; dout = x->dso[2] - x->dso[1];
      dbin = x->brecv[2] - x->brecv[1]; dbout = x->bsent[2] - x->bsent[1];
    } else if (x->seen[2]) { din = x->dsi[2]; dout = x->dso[2]; dbin = x->brecv[2]; dbout = x->bsent[2]; }
    ip_of(x->peer, ip);
    for (k = 0; k < n_peer; k++) if (!strcmp(peers[k].ip, ip)) break;
    if (k == n_peer && n_peer < MAXPEER) { memset(&peers[k], 0, sizeof(Peer)); strcpy(peers[k].ip, ip); n_peer++; }
    if (k < n_peer) {
      peers[k].n++;
      peers[k].in += din; peers[k].out += dout;
      peers[k].kbin += dbin / 1024.0; peers[k].kbout += dbout / 1024.0;
    }
  }

  printf("\n");
  line(RPT_W, '=');
  if (t_text[1][0] && t_text[2][0])
    printf("Port %d forgalma: %s - %s (%s perc)\n", port, t_text[1], t_text[2] + 11,
           fmt_d(el / 60.0, 1, a));
  else
    printf("Port %d forgalma az elmult %s percben\n", port, fmt_d(el / 60.0, 1, a));
  line(RPT_W, '=');
  printf("Beerkezo uzenet (a NonStop fele): %12s   (%s / mp)\n", fmt_n(st.in, a), fmt_d(st.in / el, 1, b));
  printf("Kimeno uzenet (a NonStoprol):     %12s   (%s / mp)\n", fmt_n(st.out, a), fmt_d(st.out / el, 1, b));
  printf("Kapcsolatok szama:                %12d\n", st.conns);
  if (n_peer > 1) {
    printf("Megoszlas:");
    for (k = 0; k < n_peer; k++) {
      double sh = (st.in + st.out) > 0 ? 100.0 * (peers[k].in + peers[k].out) / (double)(st.in + st.out) : 0;
      printf("%s %s %s%%", k ? "," : "", peers[k].ip, fmt_d(sh, 0, a));
    }
    printf("\n");
  }
  if (!st.conns && !st.gone) printf("FIGYELEM: nincs kapcsolat ezen a porton!\n");
  else if (!st.sq && !st.cs && !st.rt && !st.gone) printf("Allapot: rendben\n");
  else {
    if (st.sq) printf("FIGYELEM: %d kapcsolaton nyugtazatlan adat all sorban\n", st.sq);
    if (st.cs) printf("FIGYELEM: %d kapcsolaton 1 percnel regebben jott adat\n", st.cs);
    if (st.rt) printf("Megjegyzes: %d kapcsolaton volt ujrakuldes\n", st.rt);
    if (st.gone) printf("Megjegyzes: %d kapcsolat megszunt a meres alatt\n", st.gone);
  }
  if (st.nw) printf("Megjegyzes: %d uj kapcsolat jott letre a meres alatt\n", st.nw);
  printf("(TCPRATE %s)\n", TCPRATE_VER);
  if (!detail) return 0;

  printf("\nReszletek (Be/Ki = adatszegmens/mp; kis uzeneteknel 1 szegmens ~ 1 uzenet)\n");
  printf("%-22s%7s%7s%7s%7s%6s%8s%5s %s\n", "Tuloldal", "Be/mp", "Ki/mp", "rtt",
         "minrtt", "SendQ", "Utolso", "Retr", "Megj.");
  line(RPT_W, '-');
  for (i = 0; i < n_conn; i++) {
    Conn *x = &conn[i];
    char grp[64], ip[40], note[32] = "";
    double rin = 0, rout = 0;
    long long retr = 0;
    if (port_of(x->local) != port && port_of(x->peer) != port) continue;
    sprintf(grp, "%s / %s", x->clim, x->ns);
    if (strcmp(grp, last_grp)) {
      printf("%s  (helyi: %s)\n", grp, x->local);
      strcpy(last_grp, grp);
    }
    if (x->seen[1] && x->seen[2]) {
      rin = (x->dsi[2] - x->dsi[1]) / el;
      rout = (x->dso[2] - x->dso[1]) / el;
      retr = x->bretr[2] - x->bretr[1];
      if (x->sendq > 0) strcat(note, "SQ ");
      if (x->lastrcv > 60000) strcat(note, "CS ");
      if (retr > 0) strcat(note, "RT ");
    } else if (x->seen[2]) strcat(note, "UJ ");
    else strcat(note, "MSZ ");
    if (note[0]) { nwarn++; note[strlen(note) - 1] = 0; }
    printf("%-22.22s%7s%7s%7s%7s%6ld%8s%5s %s\n", x->peer,
           x->seen[1] && x->seen[2] ? fmt_d(rin, 1, a) : "-",
           x->seen[1] && x->seen[2] ? fmt_d(rout, 1, b) : "-",
           x->seen[2] ? fmt_d(x->rtt, 1, c) : "-",
           x->seen[2] ? fmt_d(x->minrtt, 2, d) : "-",
           x->sendq,
           x->seen[2] ? (sprintf(e, "%lds", x->lastrcv / 1000), e) : "-",
           retr > 0 ? (sprintf(ip, "%lld", retr), ip) : "-", note);
  }
  line(RPT_W, '-');
  printf("rtt/minrtt ms; Utolso = mp az utolso fogadas ota; Retr = ujrakuldott bajt\n");
  printf("Megj.: SQ = nyugtazatlan adat all sorban, CS = 60 mp-nel regebben jott\n");
  printf("adat (csend), RT = volt ujrakuldes, UJ / MSZ = kozben jott letre / szunt meg\n");

  printf("\nTuloldal szerint (a meresi idoszakban)\n");
  printf("%-24s%6s%11s%11s%9s%9s\n", "IP-cim", "Kapcs", "Be (db)", "Ki (db)", "Be KB", "Ki KB");
  line(RPT_W, '-');
  for (k = 0; k < n_peer; k++)
    printf("%-24.24s%6d%11s%11s%9s%9s\n", peers[k].ip, peers[k].n,
           fmt_n((long long)peers[k].in, a), fmt_n((long long)peers[k].out, b),
           fmt_d(peers[k].kbin, 0, c), fmt_d(peers[k].kbout, 0, d));
  line(RPT_W, '-');
  printf("Osszesen: %s KB be, %s KB ki\n", fmt_d(st.bin / 1024.0, 0, a), fmt_d(st.bout / 1024.0, 0, b));
  return 0;
}

int main(int argc, char *argv[])
{
  if (argc >= 2 && !strcmp(argv[1], "L")) return mode_log(argc, argv);
  if (argc >= 2 && !strcmp(argv[1], "D")) return mode_daily(argc, argv);
  if (argc < 2) {
    fprintf(stderr, "TCPRATE %s\n", TCPRATE_VER);
    fprintf(stderr, "Hasznalat: TCPRATE port [mp] [R]\n"
                    "           TCPRATE L naplo port:nev [port:nev ...]\n"
                    "           TCPRATE D naplo\n");
    return 2;
  }
  return mode_once(argc, argv);
}
