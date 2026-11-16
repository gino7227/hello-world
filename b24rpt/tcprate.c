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
 * Parameterek: port [meresi-ido-mp] [R]
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

static void kv(Conn *c, int p, const char *t)
{
  const char *v = strchr(t, ':');
  if (!v) return;
  v++;
  if      (!strncmp(t, "data_segs_out:", 14))  c->dso[p] = to_ll(v);
  else if (!strncmp(t, "data_segs_in:", 13))   c->dsi[p] = to_ll(v);
  else if (!strncmp(t, "bytes_sent:", 11))     c->bsent[p] = to_ll(v);
  else if (!strncmp(t, "bytes_received:", 15)) c->brecv[p] = to_ll(v);
  else if (!strncmp(t, "bytes_retrans:", 14))  c->bretr[p] = to_ll(v);
  else if (p == 2) {
    if      (!strncmp(t, "rtt:", 4)) { c->rtt = atof(v); { const char *s = strchr(v, '/'); if (s) c->rttvar = atof(s + 1); } }
    else if (!strncmp(t, "minrtt:", 7))  c->minrtt = atof(v);
    else if (!strncmp(t, "lastrcv:", 8)) c->lastrcv = atol(v);
    else if (!strncmp(t, "lastsnd:", 8)) c->lastsnd = atol(v);
    else if (!strncmp(t, "cwnd:", 5))    c->cwnd = atol(v);
  }
}

static void blk_flush(void)
{
  int i, maxlen = 0, nmax = 0, wrapw = 0;
  size_t tot = 1; char *buf, *p, *tok[5];
  Conn *c = NULL;

  if (!blk_n) return;
  /* Ha a climcmd fix szelessegben tordelte a sorokat, a teljes szelessegu
     sorokat szokoz nelkul kell a kovetkezohoz fuzni. */
  for (i = 0; i < blk_n; i++) {
    int L = (int)strlen(blk[i]);
    tot += (size_t)L + 1;
    if (L > maxlen) { maxlen = L; nmax = 1; } else if (L == maxlen) nmax++;
  }
  if (maxlen >= 70 && nmax >= 2) wrapw = maxlen;
  buf = (char *)malloc(tot);
  if (!buf) { fprintf(stderr, "Keves memoria\n"); exit(1); }
  buf[0] = 0; p = buf;
  for (i = 0; i < blk_n; i++) {
    size_t L = strlen(blk[i]);
    memcpy(p, blk[i], L); p += L;
    if (!(wrapw && (int)L == wrapw)) *p++ = ' ';
    free(blk[i]);
  }
  *p = 0;
  blk_n = 0;

  /* szavakra bontas */
  p = buf;
  for (;;) {
    char *t;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;
    t = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) *p++ = 0;
    if (is_state(t)) {
      /* State Recv-Q Send-Q Local Peer */
      int k;
      for (k = 0; k < 4; k++) {
        while (*p && isspace((unsigned char)*p)) p++;
        tok[k] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
      }
      if (!*tok[3] || !strchr(tok[2], ':') || !strchr(tok[3], ':')) { c = NULL; continue; }
      c = get_conn(cur_clim, cur_ns, tok[2], tok[3]);
      if (c) {
        c->seen[cur_pass] = 1;
        if (cur_pass == 2) { c->recvq = atol(tok[0]); c->sendq = atol(tok[1]); }
      }
    } else if (c && strchr(t, ':')) {
      kv(c, cur_pass, t);
    }
  }
  free(buf);
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

/* "@@D <menet> yyyy mm dd hh mm ss cc" (TACL #CONTIME) -> "yyyy.mm.dd hh:mm:ss" */
static char t_text[3][24];
static void set_time_text(const char *s)
{
  int p, y, mo, d, h, mi, se;
  if (sscanf(s, "%d %d %d %d %d %d %d", &p, &y, &mo, &d, &h, &mi, &se) == 7 && p >= 1 && p <= 2)
    sprintf(t_text[p], "%04d.%02d.%02d %02d:%02d:%02d", y, mo, d, h, mi, se);
}

int main(int argc, char *argv[])
{
  static char ln[MAXLINE];
  double el;
  long long tin = 0, tout = 0, tbin = 0, tbout = 0;
  int i, k, nwarn = 0, n_sq = 0, n_cs = 0, n_rt = 0, n_new = 0, n_gone = 0, detail = 0;
  char a[32], b[32], c[32], d[32], e[32];
  char last_grp[64] = "";

  if (argc < 2) {
    fprintf(stderr, "Hasznalat: TCPRATE port [mp] [R]   (a bemenet: RUN /INV valtozo/)\n");
    return 2;
  }
  port = atoi(argv[1]);
  if (argc > 2) sec_param = atof(argv[2]);
  if (argc > 3 && (argv[3][0] == 'R' || argv[3][0] == 'r')) detail = 1;

  while (fgets(ln, MAXLINE, stdin)) {
    ln[strcspn(ln, "\r\n")] = 0;
    if (!strncmp(ln, "@@T ", 4)) {
      int p = atoi(ln + 4);
      const char *q = strchr(ln + 4, ' ');
      if (q && p >= 1 && p <= 2) t_snap[p] = to_ll(q + 1);
      continue;
    }
    if (!strncmp(ln, "@@D ", 4)) { set_time_text(ln + 4); continue; }
    if (!strncmp(ln, "@@S ", 4)) {
      blk_flush();
      cur_clim[0] = cur_ns[0] = 0;
      sscanf(ln + 4, "%d %16s %32s", &cur_pass, cur_clim, cur_ns);
      if (cur_pass < 1 || cur_pass > 2) cur_pass = 2;
      continue;
    }
    if (!strncmp(ln, "Termination Info", 16)) continue;
    if (cur_pass) blk_add(ln);
  }
  blk_flush();

  el = (t_snap[1] && t_snap[2] > t_snap[1]) ? (t_snap[2] - t_snap[1]) / 1e6 : sec_param;
  if (el <= 0) el = 1;
  qsort(conn, n_conn, sizeof(Conn), cmp_conn);

  /* ---- osszesites: uzenetek szama a meresi idoszakban ---- */
  for (i = 0; i < n_conn; i++) {
    Conn *x = &conn[i];
    char ip[40];
    long long din = 0, dout = 0, dbin = 0, dbout = 0;
    if (x->seen[1] && x->seen[2]) {
      din = x->dsi[2] - x->dsi[1];   dout = x->dso[2] - x->dso[1];
      dbin = x->brecv[2] - x->brecv[1]; dbout = x->bsent[2] - x->bsent[1];
      if (x->sendq > 0) n_sq++;
      if (x->lastrcv > 60000) n_cs++;
      if (x->bretr[2] > x->bretr[1]) n_rt++;
    } else if (x->seen[2]) {          /* kozben letrejott: minden forgalma ide tartozik */
      din = x->dsi[2]; dout = x->dso[2]; dbin = x->brecv[2]; dbout = x->bsent[2];
      n_new++;
    } else n_gone++;
    tin += din; tout += dout; tbin += dbin; tbout += dbout;
    ip_of(x->peer, ip);
    for (k = 0; k < n_peer; k++) if (!strcmp(peers[k].ip, ip)) break;
    if (k == n_peer && n_peer < MAXPEER) { memset(&peers[k], 0, sizeof(Peer)); strcpy(peers[k].ip, ip); n_peer++; }
    if (k < n_peer) {
      peers[k].n++;
      peers[k].in += din; peers[k].out += dout;
      peers[k].kbin += dbin / 1024.0; peers[k].kbout += dbout / 1024.0;
    }
  }

  /* ---- rovid osszefoglalo (alapertelmezes) ---- */
  printf("\n");
  line(RPT_W, '=');
  if (t_text[1][0] && t_text[2][0])
    printf("Port %d forgalma: %s - %s (%s perc)\n", port, t_text[1], t_text[2] + 11,
           fmt_d(el / 60.0, 1, a));
  else
    printf("Port %d forgalma az elmult %s percben\n", port, fmt_d(el / 60.0, 1, a));
  line(RPT_W, '=');
  printf("Beerkezo uzenet (a NonStop fele): %12s   (%s / mp)\n", fmt_n(tin, a), fmt_d(tin / el, 1, b));
  printf("Kimeno uzenet (a NonStoprol):     %12s   (%s / mp)\n", fmt_n(tout, a), fmt_d(tout / el, 1, b));
  printf("Kapcsolatok szama:                %12d\n", n_conn - n_gone);
  if (n_peer > 1) {
    printf("Megoszlas:");
    for (k = 0; k < n_peer; k++) {
      double sh = (tin + tout) > 0 ? 100.0 * (peers[k].in + peers[k].out) / (double)(tin + tout) : 0;
      printf("%s %s %s%%", k ? "," : "", peers[k].ip, fmt_d(sh, 0, a));
    }
    printf("\n");
  }
  if (!n_conn) printf("FIGYELEM: nincs kapcsolat ezen a porton!\n");
  else if (!n_sq && !n_cs && !n_rt && !n_gone) printf("Allapot: rendben\n");
  else {
    if (n_sq) printf("FIGYELEM: %d kapcsolaton nyugtazatlan adat all sorban\n", n_sq);
    if (n_cs) printf("FIGYELEM: %d kapcsolaton 1 percnel regebben jott adat\n", n_cs);
    if (n_rt) printf("Megjegyzes: %d kapcsolaton volt ujrakuldes\n", n_rt);
    if (n_gone) printf("Megjegyzes: %d kapcsolat megszunt a meres alatt\n", n_gone);
  }
  if (n_new) printf("Megjegyzes: %d uj kapcsolat jott letre a meres alatt\n", n_new);
  if (!detail) return 0;

  /* ---- reszletes (R parameter) ---- */
  printf("\nReszletek (Be/Ki = adatszegmens/mp; kis uzeneteknel 1 szegmens ~ 1 uzenet)\n");
  printf("%-22s%7s%7s%7s%7s%6s%8s%5s %s\n", "Tuloldal", "Be/mp", "Ki/mp", "rtt",
         "minrtt", "SendQ", "Utolso", "Retr", "Megj.");
  line(RPT_W, '-');
  for (i = 0; i < n_conn; i++) {
    Conn *x = &conn[i];
    char grp[64], ip[40], note[32] = "";
    double rin = 0, rout = 0;
    long long retr = 0;
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
  printf("Osszesen: %s KB be, %s KB ki\n", fmt_d(tbin / 1024.0, 0, a), fmt_d(tbout / 1024.0, 0, b));
  return 0;
}
