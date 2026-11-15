/* `spinel console`: each line compiled against the running program and
   dlopened into it. docs/internals/interactive-design.md, option 3. */
#include "console.h"

#if defined(_WIN32)
#include <stdio.h>
int console_main(int argc, char **argv, const char *self, const char *lib_dir) {
  (void)argc; (void)argv; (void)self; (void)lib_dir;
  fprintf(stderr, "spinel console: not available on Windows\n");
  return 2;
}
const char *g_console_host_dir = NULL;
int console_host_prepare(const char *work, CSplitResult *res) { (void)work; (void)res; return -1; }
#else

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <prism.h>
#include "csplit.h"
#include "sha256.h"

/* ---- small buffers ---- */

typedef struct { char *p; size_t len, cap; } Buf;

static void b_addn(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n + 1) nc *= 2;
    b->p = realloc(b->p, nc);
    if (!b->p) { fprintf(stderr, "spinel console: out of memory\n"); exit(1); }
    b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}
static void b_add(Buf *b, const char *s) { b_addn(b, s, strlen(s)); }
static void b_printf(Buf *b, const char *fmt, ...) {
  char tmp[8192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) { b_addn(b, tmp, (size_t)(n < 0 ? 0 : n)); return; }
  char *big = malloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b_addn(b, big, (size_t)n);
  free(big);
}
static char *b_take(Buf *b) { char *p = b->p ? b->p : strdup(""); b->p = NULL; b->len = b->cap = 0; return p; }

static char *read_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  Buf b = {0};
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) b_addn(&b, chunk, n);
  fclose(f);
  if (len_out) *len_out = b.len;
  return b_take(&b);
}
static int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fputs(text, f);
  return fclose(f);
}

static uint64_t fnv(const char *s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
  return h;
}

/* A set of strings (owned copies) with an int each. */
typedef struct { char **k; int *v; size_t cap, n; } SMap;
static int smap_slot(const SMap *m, const char *k, size_t n) {
  if (!m->cap) return -1;
  size_t i = (size_t)fnv(k, n) & (m->cap - 1);
  while (m->k[i]) {
    if (strlen(m->k[i]) == n && memcmp(m->k[i], k, n) == 0) return (int)i;
    i = (i + 1) & (m->cap - 1);
  }
  return -(int)i - 2;
}
static int *smap_getn(const SMap *m, const char *k, size_t n) {
  int s = smap_slot(m, k, n);
  return s >= 0 ? &m->v[s] : NULL;
}
static int *smap_get(const SMap *m, const char *k) { return smap_getn(m, k, strlen(k)); }
static void smap_putn(SMap *m, const char *k, size_t n, int v) {
  if ((m->n + 1) * 2 > m->cap) {
    SMap g = { NULL, NULL, m->cap ? m->cap * 2 : 64, 0 };
    g.k = calloc(g.cap, sizeof *g.k);
    g.v = calloc(g.cap, sizeof *g.v);
    for (size_t i = 0; i < m->cap; i++)
      if (m->k[i]) {
        int s = smap_slot(&g, m->k[i], strlen(m->k[i]));
        g.k[-s - 2] = m->k[i]; g.v[-s - 2] = m->v[i]; g.n++;
      }
    free(m->k); free(m->v);
    *m = g;
  }
  int s = smap_slot(m, k, n);
  if (s >= 0) { m->v[s] = v; return; }
  char *c = malloc(n + 1);
  memcpy(c, k, n); c[n] = 0;
  m->k[-s - 2] = c; m->v[-s - 2] = v; m->n++;
}
static void smap_put(SMap *m, const char *k, int v) { smap_putn(m, k, strlen(k), v); }
static void smap_free(SMap *m) {
  for (size_t i = 0; i < m->cap; i++) free(m->k[i]);
  free(m->k); free(m->v);
  memset(m, 0, sizeof *m);
}

/* ---- the C of a unit set, as top-level items ---- */

enum { CI_OTHER, CI_DECL, CI_FN, CI_DATA };
typedef struct {
  int kind;
  int unit;            /* index in CSet.units */
  int is_static;
  char name[256];      /* FN and DATA: the symbol */
  char *text;          /* owned, trimmed */
  size_t len;
} CItem;
typedef struct {
  CItem *it; int n, cap;
  char **units; int nunits;  /* unit file names, in link order */
  char *unit_rt;             /* per unit: 1 for the runtime's */
  char dir[4096];
  SMap by_name;              /* FN/DATA name -> item index */
} CSet;

static int is_ident(int c) { return isalnum(c) || c == '_'; }

/* `s` with each __attribute__((...)), __asm__(...) and
   __extension__ blanked out, into a new string */
static char *strip_attrs(const char *s, size_t n) {
  char *o = malloc(n + 1);
  size_t j = 0;
  for (size_t i = 0; i < n;) {
    const char *kw = NULL;
    if (!strncmp(s + i, "__attribute__", 13)) kw = "__attribute__";
    else if (!strncmp(s + i, "__asm__", 7)) kw = "__asm__";
    else if (!strncmp(s + i, "__extension__", 13)) { o[j++] = ' '; i += 13; continue; }
    if (kw && (i == 0 || !is_ident((unsigned char)s[i - 1]))) {
      size_t k = i + strlen(kw);
      while (k < n && isspace((unsigned char)s[k])) k++;
      if (k < n && s[k] == '(') {
        int d = 0;
        for (; k < n; k++) {
          if (s[k] == '(') d++;
          else if (s[k] == ')' && --d == 0) { k++; break; }
        }
        o[j++] = ' ';
        i = k;
        continue;
      }
    }
    o[j++] = s[i++];
  }
  o[j] = 0;
  return o;
}

/* The identifier that ends right before position `at` of `s` (skipping
   blanks), or "" */
static void ident_before(const char *s, size_t at, char *out, size_t outsz) {
  size_t e = at;
  while (e > 0 && isspace((unsigned char)s[e - 1])) e--;
  size_t b = e;
  while (b > 0 && is_ident((unsigned char)s[b - 1])) b--;
  size_t n = e - b < outsz - 1 ? e - b : outsz - 1;
  memcpy(out, s + b, n);
  out[n] = 0;
}

/* Past the stars and qualifiers of a `(*const _Nullable name` declarator. */
static const char *skip_ptr_quals(const char *q) {
  static const char *quals[] = { "const", "volatile", "restrict", "__restrict", "_Nullable", "_Nonnull", "_Null_unspecified", NULL };
  for (;;) {
    while (isspace((unsigned char)*q) || *q == '*') q++;
    int hit = 0;
    for (int i = 0; quals[i]; i++) {
      size_t n = strlen(quals[i]);
      if (!strncmp(q, quals[i], n) && !is_ident((unsigned char)q[n])) { q += n; hit = 1; break; }
    }
    if (!hit) return q;
  }
}

/* Kind and name of one item. */
static void citem_classify(CItem *ci, int is_fn) {
  char *t = strip_attrs(ci->text, ci->len);
  const char *p = t;
  while (isspace((unsigned char)*p)) p++;
  ci->name[0] = 0;
  ci->is_static = !strncmp(p, "static", 6) && !is_ident((unsigned char)p[6]);
  if (is_fn) {
    ci->kind = CI_FN;
    const char *lb = strchr(p, '(');
    if (lb) ident_before(p, (size_t)(lb - p), ci->name, sizeof ci->name);
    free(t);
    return;
  }
  if ((!strncmp(p, "extern", 6) && !is_ident((unsigned char)p[6])) ||
      !strncmp(p, "typedef", 7) || !strncmp(p, "_Static_assert", 14)) {
    ci->kind = !strncmp(p, "extern", 6) ? CI_DECL : CI_OTHER;
    free(t);
    return;
  }
  /* depth-0 `=` and `(` of the declaration */
  int d = 0;
  const char *eq = NULL, *paren = NULL, *brace = NULL;
  for (const char *q = p; *q; q++) {
    if (*q == '"' || *q == '\'') {
      char c = *q++;
      while (*q && *q != c) { if (*q == '\\' && q[1]) q++; q++; }
      if (!*q) break;
      continue;
    }
    if (d == 0 && *q == '=' && !eq) eq = q;
    if (d == 0 && *q == '(' && !paren && !eq) paren = q;
    if (d == 0 && *q == '{' && !brace && !eq) brace = q;
    if (*q == '(' || *q == '{' || *q == '[') d++;
    else if (*q == ')' || *q == '}' || *q == ']') d--;
  }
  if (!eq && (brace || !strncmp(p, "struct", 6) || !strncmp(p, "union", 5) || !strncmp(p, "enum", 4)) && !paren) {
    /* `struct X {...};` or a declaration of a type alone */
    ci->kind = brace ? CI_OTHER : CI_DECL;
    if (!brace) {
      /* `struct X *v;` is a tentative definition of v */
      const char *semi = strrchr(p, ';');
      size_t at = semi ? (size_t)(semi - p) : strlen(p);
      char nm[256];
      ident_before(p, at, nm, sizeof nm);
      if (nm[0] && strchr(p, '*')) { ci->kind = CI_DATA; strcpy(ci->name, nm); }
    }
    free(t);
    return;
  }
  if (!eq && paren) {
    /* a prototype, unless the parenthesis is a function pointer's `(*name)`;
       `(*name(params))(...)` is one too, of a function answering a pointer */
    const char *q = paren + 1;
    while (isspace((unsigned char)*q)) q++;
    if (*q != '*') { ci->kind = CI_DECL; free(t); return; }
    q = skip_ptr_quals(q);
    while (is_ident((unsigned char)*q)) q++;
    while (isspace((unsigned char)*q)) q++;
    if (*q == '(') { ci->kind = CI_DECL; free(t); return; }
  }
  ci->kind = CI_DATA;
  const char *end = eq ? eq : p + strlen(p);
  /* `(*const name[])`: the name follows the star */
  if (paren && paren < end) {
    const char *q = skip_ptr_quals(paren + 1);
    size_t n = 0;
    while (is_ident((unsigned char)q[n]) && n < sizeof ci->name - 1) n++;
    memcpy(ci->name, q, n);
    ci->name[n] = 0;
    free(t);
    return;
  }
  /* the last identifier before `=` or `;`, past any `[...]` */
  size_t at = (size_t)(end - p);
  while (at > 0 && (isspace((unsigned char)p[at - 1]) || p[at - 1] == ';')) at--;
  while (at > 0 && p[at - 1] == ']') {
    int dd = 0;
    while (at > 0) {
      at--;
      if (p[at] == ']') dd++;
      else if (p[at] == '[' && --dd == 0) break;
    }
    while (at > 0 && isspace((unsigned char)p[at - 1])) at--;
  }
  ident_before(p, at, ci->name, sizeof ci->name);
  free(t);
}

static void cset_push(CSet *s, int unit, const char *text, size_t n, int is_fn) {
  while (n && isspace((unsigned char)*text)) { text++; n--; }
  while (n && isspace((unsigned char)text[n - 1])) n--;
  if (!n) return;
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 1024;
    s->it = realloc(s->it, (size_t)s->cap * sizeof *s->it);
  }
  CItem *ci = &s->it[s->n];
  memset(ci, 0, sizeof *ci);
  ci->unit = unit;
  ci->text = malloc(n + 1);
  memcpy(ci->text, text, n);
  ci->text[n] = 0;
  ci->len = n;
  citem_classify(ci, is_fn);
  if ((ci->kind == CI_FN || ci->kind == CI_DATA) && ci->name[0] && !ci->is_static)
    smap_put(&s->by_name, ci->name, s->n);
  s->n++;
}

/* Split preprocessed C into its top-level items: a declaration ends at a
   depth-0 `;`, a function definition at the `}` that closes its body. */
static void cset_split(CSet *s, int unit, const char *src) {
  Buf t = {0};
  for (const char *l = src; *l;) {
    const char *e = strchr(l, '\n');
    size_t n = e ? (size_t)(e - l) + 1 : strlen(l);
    if (*l != '#') b_addn(&t, l, n);
    l += n;
  }
  const char *p = t.p ? t.p : "";
  size_t n = t.len, start = 0;
  int d = 0, is_fn = 0;
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '"' || c == '\'') {
      for (i++; i < n && p[i] != c; i++) if (p[i] == '\\') i++;
      continue;
    }
    if (c == '/' && i + 1 < n && p[i + 1] == '*') {
      const char *e = strstr(p + i + 2, "*/");
      i = e ? (size_t)(e - p) + 1 : n;
      continue;
    }
    if (c == '{' && d == 0) {
      /* a body when what precedes it is a declarator's closing `)` */
      char *h = strip_attrs(p + start, i - start);
      size_t hl = strlen(h);
      while (hl && isspace((unsigned char)h[hl - 1])) hl--;
      int has_eq = 0, dd = 0;
      for (size_t k = 0; k < hl; k++) {
        if (h[k] == '(') dd++;
        else if (h[k] == ')') dd--;
        else if (h[k] == '=' && dd == 0) has_eq = 1;
      }
      is_fn = hl && h[hl - 1] == ')' && !has_eq;
      free(h);
    }
    if (c == '(' || c == '{' || c == '[') d++;
    else if (c == ')' || c == '}' || c == ']') {
      d--;
      if (c == '}' && d == 0 && is_fn) {
        cset_push(s, unit, p + start, i + 1 - start, 1);
        start = i + 1;
        is_fn = 0;
      }
    }
    else if (c == ';' && d == 0) {
      cset_push(s, unit, p + start, i + 1 - start, 0);
      start = i + 1;
    }
  }
  if (start < n) cset_push(s, unit, p + start, n - start, 0);
  free(t.p);
}

static void cset_free(CSet *s) {
  for (int i = 0; i < s->n; i++) free(s->it[i].text);
  free(s->it);
  for (int i = 0; i < s->nunits; i++) free(s->units[i]);
  free(s->units);
  free(s->unit_rt);
  smap_free(&s->by_name);
  memset(s, 0, sizeof *s);
}

/* The units of a -c --incremental directory (units.txt), split. */
static int cset_load(CSet *s, const char *dir) {
  memset(s, 0, sizeof *s);
  snprintf(s->dir, sizeof s->dir, "%s", dir);
  char path[4200];
  snprintf(path, sizeof path, "%s/units.txt", dir);
  char *man = read_file(path, NULL);
  if (!man) return -1;
  int line = 0;
  for (char *l = man; *l; line++) {
    char *e = strchr(l, '\n');
    if (e) *e = 0;
    if (line >= 2) {
      char label[256], file[1024], hash[128], kind[32];
      if (sscanf(l, "%255s %1023s %127s %31s", label, file, hash, kind) == 4) {
        s->units = realloc(s->units, (size_t)(s->nunits + 1) * sizeof *s->units);
        s->unit_rt = realloc(s->unit_rt, (size_t)(s->nunits + 1));
        s->units[s->nunits] = strdup(file);
        s->unit_rt[s->nunits] = !strcmp(kind, "rt");
        snprintf(path, sizeof path, "%s/%s", dir, file);
        char *src = read_file(path, NULL);
        if (!src) { free(man); return -1; }
        cset_split(s, s->nunits, src);
        free(src);
        s->nunits++;
      }
    }
    if (!e) break;
    l = e + 1;
  }
  free(man);
  return 0;
}

/* The items of one header of the set. */
static int cset_load_header(CSet *s, const char *dir, const char *name) {
  memset(s, 0, sizeof *s);
  char path[4200];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  char *src = read_file(path, NULL);
  if (!src) return -1;
  cset_split(s, 0, src);
  free(src);
  return 0;
}

/* ---- trampolines ----

   In the host each program function f is three definitions: its body as
   f__spl0, a pointer f__spl_ptr to it, and f itself forwarding through the
   pointer. Code and tables keep naming f; a line that changes f's body
   loads f__spl<N> and repoints f__spl_ptr. */

/* `s` with each __attribute__((...)), __asm__(...) and __extension__
   blanked to spaces, in place, so positions keep. */
static void blank_attrs(char *s) {
  size_t n = strlen(s);
  for (size_t i = 0; i < n;) {
    size_t kl = !strncmp(s + i, "__attribute__", 13) ? 13 : !strncmp(s + i, "__asm__", 7) ? 7
              : !strncmp(s + i, "__extension__", 13) ? 13 : 0;
    if (!kl || (i && is_ident((unsigned char)s[i - 1])) || is_ident((unsigned char)s[i + kl])) { i++; continue; }
    size_t k = i + kl;
    if (kl == 13 && s[i + 2] == 'e') { memset(s + i, ' ', kl); i = k; continue; }
    while (k < n && isspace((unsigned char)s[k])) k++;
    if (k < n && s[k] == '(') {
      int d = 0;
      for (; k < n; k++) {
        if (s[k] == '(') d++;
        else if (s[k] == ')' && --d == 0) { k++; break; }
      }
    }
    memset(s + i, ' ', k - i);
    i = k;
  }
}

/* A function definition's parts for its forwarder. */
typedef struct {
  size_t head_len;     /* up to the body's `{` */
  size_t name_at;      /* the name's position in the head */
  int is_void;
  char args[4096];     /* the parameters' names, as a call passes them */
} FnShape;

/* 0, or -1 when no forwarder can be written: variadic, a function answering
   a function pointer, an unnamed parameter. */
static int fn_shape(const CItem *ci, FnShape *sh) {
  memset(sh, 0, sizeof *sh);
  if (!ci->name[0] || ci->is_static || !strcmp(ci->name, "main")) return -1;
  char *t = strdup(ci->text);
  blank_attrs(t);
  int rc = -1;
  char *brace = strchr(t, '{');
  if (!brace) goto out;
  sh->head_len = (size_t)(brace - t);
  *brace = 0;
  size_t nl = strlen(ci->name);
  char *nm = NULL;
  for (char *p = strstr(t, ci->name); p; p = strstr(p + 1, ci->name)) {
    if (p > t && is_ident((unsigned char)p[-1])) continue;
    if (is_ident((unsigned char)p[nl])) continue;
    const char *q = p + nl;
    while (isspace((unsigned char)*q)) q++;
    if (*q == '(') { nm = p; break; }
  }
  if (!nm) goto out;
  sh->name_at = (size_t)(nm - t);
  char *lp = strchr(nm, '(');
  int d = 0;
  char *rp = lp;
  for (; *rp; rp++) {
    if (*rp == '(') d++;
    else if (*rp == ')' && --d == 0) break;
  }
  if (!*rp) goto out;
  for (const char *q = rp + 1; *q; q++) if (!isspace((unsigned char)*q)) goto out;
  /* the return type: what precedes the name, less storage words */
  {
    char ret[1024];
    size_t rn = 0;
    for (const char *q = t; q < nm && rn < sizeof ret - 1;) {
      if (isspace((unsigned char)*q)) { q++; continue; }
      size_t wl = 0;
      while (is_ident((unsigned char)q[wl])) wl++;
      if (wl && ((wl == 6 && !strncmp(q, "extern", 6)) || (wl == 6 && !strncmp(q, "inline", 6)) ||
                 (wl == 8 && !strncmp(q, "__inline", 8)))) { q += wl; continue; }
      if (!wl) wl = 1;
      if (rn + wl >= sizeof ret - 1) break;
      if (rn) ret[rn++] = ' ';
      memcpy(ret + rn, q, wl);
      rn += wl;
      q += wl;
    }
    ret[rn] = 0;
    sh->is_void = !strcmp(ret, "void");
  }
  /* the parameters, split at depth-0 commas */
  *rp = 0;
  size_t an = 0;
  int nparams = 0;
  for (char *p = lp + 1; *p;) {
    char *e = p;
    d = 0;
    while (*e && !(d == 0 && *e == ',')) {
      if (*e == '(' || *e == '[') d++;
      else if (*e == ')' || *e == ']') d--;
      e++;
    }
    char save = *e;
    *e = 0;
    while (isspace((unsigned char)*p)) p++;
    size_t pl = strlen(p);
    while (pl && isspace((unsigned char)p[pl - 1])) p[--pl] = 0;
    if (!strcmp(p, "...")) goto out;
    if (!(nparams == 0 && !save && (!strcmp(p, "void") || !*p))) {
      char name[256] = "";
      char *paren = strchr(p, '(');
      if (paren) {
        const char *q = skip_ptr_quals(paren + 1);
        size_t k = 0;
        while (is_ident((unsigned char)q[k]) && k < sizeof name - 1) { name[k] = q[k]; k++; }
        name[k] = 0;
      }
      else {
        size_t at = pl;
        while (at > 0 && p[at - 1] == ']') {
          int dd = 0;
          while (at > 0) {
            at--;
            if (p[at] == ']') dd++;
            else if (p[at] == '[' && --dd == 0) break;
          }
          while (at > 0 && isspace((unsigned char)p[at - 1])) at--;
        }
        ident_before(p, at, name, sizeof name);
        /* a lone type word is an unnamed parameter */
        size_t b = 0;
        while (isspace((unsigned char)p[b])) b++;
        if (!strncmp(p + b, name, strlen(name)) && b + strlen(name) == at) name[0] = 0;
      }
      if (!name[0] || an + strlen(name) + 3 >= sizeof sh->args) goto out;
      an += (size_t)snprintf(sh->args + an, sizeof sh->args - an, "%s%s", nparams ? ", " : "", name);
    }
    nparams++;
    if (!save) break;
    p = e + 1;
  }
  rc = 0;
out:
  free(t);
  return rc;
}

/* The definition with its name spelled `name<suffix>`. */
static void emit_renamed(Buf *b, const CItem *ci, const FnShape *sh, const char *suffix) {
  b_addn(b, ci->text, sh->name_at);
  b_add(b, ci->name);
  b_add(b, suffix);
  b_add(b, ci->text + sh->name_at + strlen(ci->name));
  b_add(b, "\n");
}

/* f itself: the original head, forwarding through the pointer. */
static void emit_forwarder(Buf *b, const CItem *ci, const FnShape *sh) {
  b_addn(b, ci->text, sh->head_len);
  b_printf(b, "{ %s%s__spl_ptr(%s); }\n", sh->is_void ? "" : "return ", ci->name, sh->args);
}

/* All three, the pointer starting at the body named `name<suffix>`. */
static void emit_trampoline(Buf *b, const CItem *ci, const FnShape *sh, const char *suffix) {
  emit_renamed(b, ci, sh, suffix);
  b_printf(b, "__typeof__(%s%s) *%s__spl_ptr = %s%s;\n", ci->name, suffix, ci->name, ci->name, suffix);
  emit_forwarder(b, ci, sh);
}

const char *g_console_host_dir = NULL;

static int copy_file(const char *from, const char *to) {
  size_t n;
  char *t = read_file(from, &n);
  if (!t) return -1;
  FILE *f = fopen(to, "wb");
  int rc = f && fwrite(t, 1, n, f) == n ? 0 : -1;
  if (f && fclose(f)) rc = -1;
  free(t);
  return rc;
}

int console_host_prepare(const char *work, CSplitResult *res) {
  mkdir(g_console_host_dir, 0755);
  DIR *dp = opendir(work);
  if (!dp) return -1;
  struct dirent *de;
  while ((de = readdir(dp))) {
    size_t l = strlen(de->d_name);
    if (!((l > 2 && !strcmp(de->d_name + l - 2, ".c")) || (l > 2 && !strcmp(de->d_name + l - 2, ".h")) ||
          !strcmp(de->d_name, "units.txt"))) continue;
    char a[4400], b[4400];
    snprintf(a, sizeof a, "%s/%s", work, de->d_name);
    snprintf(b, sizeof b, "%s/%s", g_console_host_dir, de->d_name);
    if (copy_file(a, b)) { closedir(dp); return -1; }
  }
  closedir(dp);
  for (int k = 0; k < res->n; k++) {
    CSplitUnit *u = &res->units[k];
    if (u->is_rt) continue;
    char *src = read_file(u->path, NULL);
    if (!src) return -1;
    CSet s;
    memset(&s, 0, sizeof s);
    cset_split(&s, 0, src);
    Buf o = {0};
    /* the directives the split drops; line markers aside */
    for (const char *l = src; *l;) {
      const char *e = strchr(l, '\n');
      size_t n = e ? (size_t)(e - l) + 1 : strlen(l);
      if (*l == '#' && l[1] != ' ') b_addn(&o, l, n);
      l += n;
    }
    free(src);
    for (int i = 0; i < s.n; i++) {
      FnShape sh;
      if (s.it[i].kind == CI_FN && fn_shape(&s.it[i], &sh) == 0) emit_trampoline(&o, &s.it[i], &sh, "__spl0");
      else { b_addn(&o, s.it[i].text, s.it[i].len); b_add(&o, "\n"); }
    }
    cset_free(&s);
    int rc = write_file(u->path, o.p ? o.p : "");
    if (!rc) sha256_hex(o.p ? o.p : "", o.len, u->hash);
    free(o.p);
    if (rc) return -1;
  }
  return 0;
}

/* ---- the session ---- */

typedef struct {
  char *defs;     /* top-level definitions the line brought */
  char *body;     /* the line's statements, locals rewritten */
  char **locals;  /* console locals the line writes first */
  int nlocals;
  char *src;      /* as typed */
  int at_load;    /* its definitions run code at load */
  int stopped;    /* ^C stopped it: a replay would not end either */
} Line;

typedef struct {
  const char *self, *lib_dir;
  char *prog;                 /* the program's absolute path without .rb, or NULL */
  char **extra; int nextra;   /* compiler flags passed through (-I DIR ...) */
  char dir[4096];             /* the session directory */
  Line *lines; int nlines;    /* accepted */
  Line pend; int has_pend;    /* the last refused line, which :restart folds in */
  SMap locals;                /* the console locals */
  SMap tramp;                 /* functions behind a pointer a line can repoint */
  SMap renamed;               /* tables a line reloaded -> the line that did */
  int gen;                    /* builds made, for directory names */
  CSet cur;                   /* the definitions the process holds */
  char cur_dir[4200];
  Buf cflags;                 /* what a loaded unit is compiled with */
  const char *cc;
  pid_t host;
  FILE *req;
  int ack;
  int timing, keep;
  int interrupted;            /* the last host_call saw a ^C */
} Session;

static double now_s(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* Run argv with stdout and stderr in `log` (or inherited when NULL); the
   exit status, or -1. */
static int run(char *const argv[], const char *log) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (log) {
      int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    }
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) { dup2(nul, 0); close(nul); }
    execvp(argv[0], argv);
    _exit(127);
  }
  int st;
  while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The compiler's diagnostics, as the log holds them, minus its notes on
   what it wrote. */
static void show_log(const char *log) {
  char *t = read_file(log, NULL);
  if (!t) return;
  for (char *l = t; *l;) {
    char *e = strchr(l, '\n');
    if (e) *e = 0;
    if (strncmp(l, "Wrote ", 6) && !strstr(l, " -> ")) fprintf(stderr, "%s\n", l);
    if (!e) break;
    l = e + 1;
  }
  free(t);
}

/* The compiler with the session file, the pass-through flags, then `more`
   (NULL-ended). */
static int run_spinel(Session *s, const char *log, ...) {
  char *argv[64];
  int n = 0;
  char session_rb[4200];
  snprintf(session_rb, sizeof session_rb, "%s/session.rb", s->dir);
  argv[n++] = (char *)s->self;
  argv[n++] = session_rb;
  for (int i = 0; i < s->nextra && n < 40; i++) argv[n++] = s->extra[i];
  /* symbol ids stay where the running program has them */
  char seed[4200];
  snprintf(seed, sizeof seed, "%s/symbols.seed", s->dir);
  argv[n++] = "--console-symbols";
  argv[n++] = seed;
  va_list ap;
  va_start(ap, log);
  char *a;
  while ((a = va_arg(ap, char *)) && n < 63) argv[n++] = a;
  va_end(ap);
  argv[n] = NULL;
  return run(argv, log);
}

static void line_free(Line *l) {
  free(l->defs); free(l->body); free(l->src);
  for (int i = 0; i < l->nlocals; i++) free(l->locals[i]);
  free(l->locals);
  memset(l, 0, sizeof *l);
}

/* session.rb: the program, the lines' definitions, a method per line, and
   the call into the host loop. `extra` is a line not accepted yet (or NULL). */
static void write_session(Session *s, const Line *extra) {
  Buf b = {0};
  if (s->prog) b_printf(&b, "require_relative \"%s\"\n", s->prog);
  int n = s->nlines + (extra ? 1 : 0);
  for (int i = 0; i < n; i++) {
    const Line *l = i < s->nlines ? &s->lines[i] : extra;
    if (l->defs && *l->defs) b_printf(&b, "%s\n", l->defs);
  }
  for (int i = 0; i < n; i++) {
    const Line *l = i < s->nlines ? &s->lines[i] : extra;
    /* parentheses, not begin/end: `r = begin <a call that only raises> end`
       compiles to a void C local */
    b_printf(&b,
      "def __spl_l%d\n"
      "  __spl_r = (\n%s\n  )\n"
      "  print \"=> \"\n"
      "  p __spl_r\n"
      "  nil\n"
      "rescue => __spl_e\n"
      "  puts \"#{__spl_e.class}: #{__spl_e.message}\"\n"
      "rescue Interrupt\n"
      "  puts \"Interrupt\"\n"
      "ensure\n"
      "  $stdout.flush\n"
      "end\n",
      i + 1, l->body && *l->body ? l->body : "nil");
  }
  /* the lines are reached (and so compiled) through one dispatch; the host
     calls each by its symbol */
  b_add(&b, "def __spl_run(n)\n  case n\n  when 0 then nil\n");
  for (int i = 0; i < n; i++) b_printf(&b, "  when %d then __spl_l%d\n", i + 1, i + 1);
  b_add(&b, "  end\n  nil\nend\n"
            "__spl_run(ARGV.size) if ARGV.size > 1000000\n"
            "$stdout.flush\n"
            "module SpinelConsoleHost\n"
            "  ffi_func :sp_console_loop, [], :void\n"
            "end\n"
            "SpinelConsoleHost.sp_console_loop\n");
  char path[4200];
  snprintf(path, sizeof path, "%s/session.rb", s->dir);
  write_file(path, b.p);
  free(b.p);
}

/* ---- the host process ---- */

/* The host is out of the terminal's group, so a hangup does not reach it. */
static volatile pid_t live_host;
static void on_hup(int sig) {
  if (live_host > 0) kill(live_host, SIGKILL);
  _exit(128 + sig);
}

static void host_stop(Session *s) {
  if (s->req) { fclose(s->req); s->req = NULL; }
  if (s->ack >= 0) { close(s->ack); s->ack = -1; }
  if (s->host > 0) {
    int st;
    /* SIGKILL: a line stuck in a loop never acts on SIGTERM */
    live_host = 0;
    kill(s->host, SIGKILL);
    waitpid(s->host, &st, 0);
    s->host = 0;
  }
}

/* One request; 1 when the host ran it, 0 when it could not load it, -1
   when the host is gone. */
static volatile sig_atomic_t got_int;
static void on_int(int sig) { (void)sig; got_int = 1; }


static int host_call(Session *s, const char *path, const char *sym, int quiet) {
  if (!s->req) return -1;
  fprintf(s->req, "%s %s %s\n", path, sym, quiet ? "q" : "v");
  fflush(s->req);
  char c;
  ssize_t r;
  got_int = 0;
  int ints = 0;
  /* a second ^C kills a line that never reaches a point Interrupt is raised at */
  while ((r = read(s->ack, &c, 1)) < 0 && errno == EINTR)
    if (got_int) { got_int = 0; kill(s->host, ++ints > 1 ? SIGKILL : SIGINT); }
  s->interrupted = ints > 0;
  if (r != 1) {
    int st = 0;
    if (s->host > 0) waitpid(s->host, &st, 0);
    s->host = 0;
    fclose(s->req); s->req = NULL;
    close(s->ack); s->ack = -1;
    if (WIFEXITED(st)) fprintf(stderr, "the program exited (status %d); :restart starts it again\n", WEXITSTATUS(st));
    else fprintf(stderr, "the program died (signal %d); :restart starts it again\n", WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return -1;
  }
  return c == 'k';
}

static int host_start(Session *s, const char *bin) {
  int rq[2], ak[2];
  if (pipe(rq) || pipe(ak)) return -1;
  fflush(stdout);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    close(rq[1]); close(ak[0]);
    char a[32], b[32];
    snprintf(a, sizeof a, "%d", rq[0]);
    snprintf(b, sizeof b, "%d", ak[1]);
    setenv("SPINEL_CONSOLE_IN", a, 1);
    setenv("SPINEL_CONSOLE_OUT", b, 1);
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) { dup2(nul, 0); close(nul); }
    /* out of the terminal's group: a ^C reaches it only while it runs a line */
    setpgid(0, 0);
    execl(bin, bin, (char *)NULL);
    _exit(127);
  }
  close(rq[0]); close(ak[1]);
  s->host = pid;
  live_host = pid;
  s->req = fdopen(rq[1], "w");
  s->ack = ak[0];
  /* the top level has run once the loop answers */
  return host_call(s, "!", "!", 1) < 0 ? -1 : 0;
}

/* The compile flags of the session's build, from --print-build. */
static int read_cflags(Session *s, const char *log) {
  char out[4200];
  snprintf(out, sizeof out, "%s/build.txt", s->dir);
  if (run_spinel(s, out, "--print-build", (char *)NULL) != 0) { show_log(out); return -1; }
  (void)log;
  char *t = read_file(out, NULL);
  if (!t) return -1;
  free(s->cflags.p);
  memset(&s->cflags, 0, sizeof s->cflags);
  for (char *l = t; *l;) {
    char *e = strchr(l, '\n');
    if (e) *e = 0;
    char *sp = strchr(l, ' ');
    if (sp) {
      *sp = 0;
      const char *v = sp + 1;
      if (!strcmp(l, "cflag") || !strcmp(l, "define")) b_printf(&s->cflags, " '%s'", v);
      else if (!strcmp(l, "include")) b_printf(&s->cflags, " '-I%s'", v);
    }
    if (!e) break;
    l = e + 1;
  }
  free(t);
  return 0;
}

/* Build the host from the session as it stands and run its top level;
   then replay the accepted lines, their output hidden. */
static int session_boot(Session *s, int show_last) {
  double t0 = now_s();
  host_stop(s);
  write_session(s, NULL);
  char obj[4200], src[4200], log[4200], bin[4200], units[4200];
  snprintf(src, sizeof src, "%s/console/sp_console_host.c", s->lib_dir);
  snprintf(obj, sizeof obj, "%s/sp_console_host.o", s->dir);
  snprintf(log, sizeof log, "%s/build.log", s->dir);
  if (access(obj, F_OK) != 0) {
    char *cc_argv[] = { (char *)s->cc, "-c", "-O2", "-fPIC", src, "-o", obj, NULL };
    if (run(cc_argv, log) != 0) { show_log(log); return -1; }
  }
  s->gen++;
  snprintf(bin, sizeof bin, "%s/host%d", s->dir, s->gen);
  snprintf(units, sizeof units, "%s/u%d", s->dir, s->gen);
  if (run_spinel(s, log, "--incremental", "--console-host", units, "-o", bin, "--link", obj, (char *)NULL) != 0) {
    show_log(log);
    return -1;
  }
  if (read_cflags(s, log) != 0) return -1;
  cset_free(&s->cur);
  if (cset_load(&s->cur, units) != 0) { fprintf(stderr, "spinel console: cannot read %s\n", units); return -1; }
  snprintf(s->cur_dir, sizeof s->cur_dir, "%s", units);
  smap_free(&s->tramp);
  smap_free(&s->renamed);
  for (int i = 0; i < s->cur.n; i++) {
    FnShape sh;
    const CItem *ci = &s->cur.it[i];
    if (ci->kind == CI_FN && !s->cur.unit_rt[ci->unit] && fn_shape(ci, &sh) == 0) smap_put(&s->tramp, ci->name, 1);
  }
  if (s->timing) fprintf(stderr, "[console] boot %.2fs\n", now_s() - t0);
  if (host_start(s, bin) != 0) return -1;
  for (int i = 0; i < s->nlines; i++) {
    char sym[64];
    snprintf(sym, sizeof sym, "sp___spl_l%d", i + 1);
    if (s->lines[i].stopped) continue;
    if (host_call(s, "-", sym, !(show_last && i == s->nlines - 1)) < 0) return -1;
  }
  return 0;
}

/* ---- a line ---- */

typedef struct { size_t at, len; char *text; } Edit;
typedef struct {
  pm_parser_t *parser;
  Edit *edits; int nedits, cap;
  int depth;
  SMap *known;          /* console locals before the line */
  SMap fresh;           /* locals the line writes first */
} Rw;

static void rw_edit(Rw *r, const uint8_t *at, size_t len, const char *text) {
  if (r->nedits == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 16;
    r->edits = realloc(r->edits, (size_t)r->cap * sizeof *r->edits);
  }
  r->edits[r->nedits].at = (size_t)(at - r->parser->start);
  r->edits[r->nedits].len = len;
  r->edits[r->nedits].text = strdup(text);
  r->nedits++;
}

static void rw_local(Rw *r, pm_constant_id_t id, const uint8_t *at, size_t len, int writes, const char *prefix) {
  pm_constant_t *c = pm_constant_pool_id_to_constant(&r->parser->constant_pool, id);
  char name[256];
  size_t n = c->length < sizeof name - 1 ? c->length : sizeof name - 1;
  memcpy(name, c->start, n);
  name[n] = 0;
  if (writes && !smap_get(r->known, name)) smap_put(&r->fresh, name, 1);
  char text[300];
  snprintf(text, sizeof text, "%s$__spl_%s", prefix, name);
  rw_edit(r, at, len, text);
}

/* Each local of the line's own scope (or of the console's, which the parse
   sees as the scope around it) becomes its console global. A block opens a
   scope its locals' depth counts; a def or a class body is another scope. */
static bool rw_visit(const pm_node_t *node, void *data) {
  Rw *r = data;
  switch (PM_NODE_TYPE(node)) {
  case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE: case PM_SINGLETON_CLASS_NODE:
    return false;
  case PM_BLOCK_NODE: case PM_LAMBDA_NODE:
    r->depth++;
    pm_visit_child_nodes(node, rw_visit, r);
    r->depth--;
    return false;
  case PM_LOCAL_VARIABLE_READ_NODE: {
    const pm_local_variable_read_node_t *n = (const void *)node;
    if ((int)n->depth >= r->depth)
      rw_local(r, n->name, node->location.start, (size_t)(node->location.end - node->location.start), 0, "");
    return false;
  }
  case PM_LOCAL_VARIABLE_TARGET_NODE: {
    const pm_local_variable_target_node_t *n = (const void *)node;
    if ((int)n->depth >= r->depth)
      rw_local(r, n->name, node->location.start, (size_t)(node->location.end - node->location.start), 1, "");
    return false;
  }
  case PM_LOCAL_VARIABLE_WRITE_NODE: {
    const pm_local_variable_write_node_t *n = (const void *)node;
    if ((int)n->depth >= r->depth)
      rw_local(r, n->name, n->name_loc.start, (size_t)(n->name_loc.end - n->name_loc.start), 1, "");
    return true;
  }
  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    const pm_local_variable_operator_write_node_t *n = (const void *)node;
    if ((int)n->depth >= r->depth)
      rw_local(r, n->name, n->name_loc.start, (size_t)(n->name_loc.end - n->name_loc.start), 1, "");
    return true;
  }
  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    const pm_local_variable_and_write_node_t *n = (const void *)node;
    if ((int)n->depth >= r->depth)
      rw_local(r, n->name, n->name_loc.start, (size_t)(n->name_loc.end - n->name_loc.start), 1, "");
    return true;
  }
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    const pm_local_variable_or_write_node_t *n = (const void *)node;
    if ((int)n->depth >= r->depth)
      rw_local(r, n->name, n->name_loc.start, (size_t)(n->name_loc.end - n->name_loc.start), 1, "");
    return true;
  }
  case PM_ASSOC_NODE: {
    /* `{x:}` is `{x: x}`: the value is written after the key */
    const pm_assoc_node_t *n = (const void *)node;
    if (n->value && PM_NODE_TYPE(n->value) == PM_IMPLICIT_NODE) {
      const pm_node_t *v = ((const pm_implicit_node_t *)n->value)->value;
      if (v && PM_NODE_TYPE(v) == PM_LOCAL_VARIABLE_READ_NODE &&
          (int)((const pm_local_variable_read_node_t *)v)->depth >= r->depth)
        rw_local(r, ((const pm_local_variable_read_node_t *)v)->name, n->key->location.end, 0, 0, " ");
      return false;
    }
    return true;
  }
  case PM_IMPLICIT_NODE:
    return false;
  default:
    return true;
  }
}

static int edit_cmp(const void *a, const void *b) {
  const Edit *x = a, *y = b;
  return x->at < y->at ? -1 : x->at > y->at;
}

/* The text of [from, to) of the source with the edits inside it applied. */
static void apply_edits(Buf *out, const char *src, size_t from, size_t to, const Rw *r) {
  size_t at = from;
  for (int i = 0; i < r->nedits; i++) {
    const Edit *e = &r->edits[i];
    if (e->at < from || e->at + e->len > to || e->at < at) continue;
    b_addn(out, src + at, e->at - at);
    b_add(out, e->text);
    at = e->at + e->len;
  }
  b_addn(out, src + at, to - at);
}

static int is_definition(const pm_parser_t *p, const pm_node_t *n) {
  switch (PM_NODE_TYPE(n)) {
  case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE:
  case PM_CONSTANT_WRITE_NODE: case PM_CONSTANT_PATH_WRITE_NODE:
    return 1;
  case PM_CALL_NODE: {
    const pm_call_node_t *c = (const void *)n;
    if (c->receiver) return 0;
    pm_constant_t *k = pm_constant_pool_id_to_constant(&p->constant_pool, c->name);
    return (k->length == 7 && !memcmp(k->start, "require", 7)) ||
           (k->length == 16 && !memcmp(k->start, "require_relative", 16));
  }
  default:
    return 0;
  }
}

/* Does the definition run code when the program loads (a constant, a
   require, a class body statement)? Code the running process will never
   run, unlike a def, which only names code. */
static int runs_at_load(const pm_parser_t *p, const pm_node_t *n) {
  static const char *const decl[] = { "attr_reader", "attr_writer", "attr_accessor", "include", "extend",
    "prepend", "private", "public", "protected", "module_function", "private_constant", NULL };
  switch (PM_NODE_TYPE(n)) {
  case PM_DEF_NODE: case PM_ALIAS_METHOD_NODE:
    return 0;
  case PM_CLASS_NODE: case PM_MODULE_NODE: {
    const pm_node_t *b = PM_NODE_TYPE(n) == PM_CLASS_NODE ? ((const pm_class_node_t *)n)->body : ((const pm_module_node_t *)n)->body;
    if (!b) return 0;
    if (PM_NODE_TYPE(b) != PM_STATEMENTS_NODE) return 1;
    const pm_statements_node_t *st = (const void *)b;
    for (size_t i = 0; i < st->body.size; i++)
      if (runs_at_load(p, st->body.nodes[i])) return 1;
    return 0;
  }
  case PM_CALL_NODE: {
    const pm_call_node_t *c = (const void *)n;
    if (c->receiver) return 1;
    pm_constant_t *k = pm_constant_pool_id_to_constant(&p->constant_pool, c->name);
    for (int i = 0; decl[i]; i++)
      if (k->length == strlen(decl[i]) && !memcmp(k->start, decl[i], k->length)) return 0;
    return 1;
  }
  default:
    return 1;
  }
}

enum { LINE_OK, LINE_MORE, LINE_ERROR };

/* Parse a line: its definitions, its statements with the console locals
   rewritten, and the locals it brings. LINE_MORE when the input stops
   inside a construct. */
static int line_parse(Session *s, const char *src, Line *out) {
  memset(out, 0, sizeof *out);
  pm_options_t opts;
  memset(&opts, 0, sizeof opts);
  pm_options_line_set(&opts, 1);
  pm_options_frozen_string_literal_set(&opts, true);
  pm_options_version_set(&opts, "4.0", 3);
  pm_options_scopes_init(&opts, 1);
  pm_options_scope_t *sc = (pm_options_scope_t *)pm_options_scope_get(&opts, 0);
  pm_options_scope_init(sc, s->locals.n);
  size_t li = 0;
  for (size_t i = 0; i < s->locals.cap; i++)
    if (s->locals.k[i]) pm_string_constant_init(&sc->locals[li++], s->locals.k[i], strlen(s->locals.k[i]));
  pm_parser_t parser;
  size_t len = strlen(src);
  pm_parser_init(&parser, (const uint8_t *)src, len, &opts);
  pm_node_t *root = pm_parse(&parser);
  int rc = LINE_OK;
  if (parser.error_list.size) {
    size_t end = len;
    while (end && isspace((unsigned char)src[end - 1])) end--;
    rc = LINE_ERROR;
    /* more input may complete it, unless a token already went wrong */
    int open = 0, hard = 0;
    for (pm_diagnostic_t *d = (pm_diagnostic_t *)parser.error_list.head; d; d = (pm_diagnostic_t *)d->node.next) {
      size_t at = (size_t)(d->location.start - parser.start);
      if (at >= end || strstr(d->message, "end-of-input") || strstr(d->message, "unterminated") ||
          strstr(d->message, "to close") || strstr(d->message, "expected an `end`")) open = 1;
      if (at < end && !strncmp(d->message, "unexpected", 10) && !strstr(d->message, "end-of-input")) hard = 1;
    }
    if (open && !hard) rc = LINE_MORE;
    if (rc == LINE_ERROR)
      for (pm_diagnostic_t *d = (pm_diagnostic_t *)parser.error_list.head; d; d = (pm_diagnostic_t *)d->node.next) {
        int32_t ln = pm_newline_list_line_column(&parser.newline_list, d->location.start, 1).line;
        fprintf(stderr, "syntax error, line %d: %s\n", (int)ln, d->message);
      }
  }
  if (rc == LINE_OK) {
    Rw r = { &parser, NULL, 0, 0, 0, &s->locals, {0} };
    pm_visit_node(root, rw_visit, &r);
    qsort(r.edits, (size_t)r.nedits, sizeof *r.edits, edit_cmp);
    const pm_statements_node_t *st = ((const pm_program_node_t *)root)->statements;
    Buf defs = {0}, body = {0};
    for (size_t i = 0; st && i < st->body.size; i++) {
      const pm_node_t *n = st->body.nodes[i];
      size_t from = (size_t)(n->location.start - parser.start), to = (size_t)(n->location.end - parser.start);
      if (is_definition(&parser, n)) {
        out->at_load |= runs_at_load(&parser, n);
        b_addn(&defs, src + from, to - from);
        b_add(&defs, "\n");
      }
      else {
        b_add(&body, "    ");
        apply_edits(&body, src, from, to, &r);
        b_add(&body, "\n");
      }
    }
    /* a line that only defines answers what Ruby's statement does: a def
       its name, a constant its value */
    const pm_node_t *last = st && st->body.size ? st->body.nodes[st->body.size - 1] : NULL;
    if (!body.len && last && (PM_NODE_TYPE(last) == PM_DEF_NODE || PM_NODE_TYPE(last) == PM_CONSTANT_WRITE_NODE)) {
      pm_constant_id_t id = PM_NODE_TYPE(last) == PM_DEF_NODE ? ((const pm_def_node_t *)last)->name
                                                              : ((const pm_constant_write_node_t *)last)->name;
      pm_constant_t *k = pm_constant_pool_id_to_constant(&parser.constant_pool, id);
      int plain = 1;
      for (size_t i = 0; i < k->length; i++) if (!is_ident(k->start[i])) plain = 0;
      if (PM_NODE_TYPE(last) == PM_DEF_NODE && ((const pm_def_node_t *)last)->receiver) plain = 0;
      if (plain) b_printf(&body, "    %s%.*s", PM_NODE_TYPE(last) == PM_DEF_NODE ? ":" : "", (int)k->length, (const char *)k->start);
    }
    if (body.len && body.p[body.len - 1] == '\n') body.p[--body.len] = 0;
    out->defs = b_take(&defs);
    out->body = b_take(&body);
    out->src = strdup(src);
    for (size_t i = 0; i < r.fresh.cap; i++)
      if (r.fresh.k[i]) {
        out->locals = realloc(out->locals, (size_t)(out->nlocals + 1) * sizeof *out->locals);
        out->locals[out->nlocals++] = strdup(r.fresh.k[i]);
      }
    for (int i = 0; i < r.nedits; i++) free(r.edits[i].text);
    free(r.edits);
    smap_free(&r.fresh);
  }
  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  pm_options_free(&opts);
  return rc;
}

/* ---- comparing a build with what the process holds ---- */

/* Definitions a line may change without reloading them: the dispatch over
   the lines (the host calls a line by its symbol, not through it) and the
   table of frozen literals `-str` interns from (a literal a line brings
   is its own copy, as one a program never dedups is). */
static int skipped(const char *name) {
  return !strcmp(name, "sp___spl_run") || !strcmp(name, "sp_fzl_all") ||
         !strcmp(name, "sp_fzl_all_set");
}

/* The Ruby name of a C symbol for a refusal: from the symbol map, a
   console local, a global, a class layout. */
static void ruby_name_of(const char *map, const char *csym, char *out, size_t outsz) {
  if (!strncmp(csym, "gv___spl_", 9)) { snprintf(out, outsz, "local variable `%s` (a value of another type)", csym + 9); return; }
  if (!strncmp(csym, "gv_", 3)) { snprintf(out, outsz, "$%s (a value of another type)", csym + 3); return; }
  if (!strcmp(csym, "_sp_main_body") || !strcmp(csym, "main")) {
    snprintf(out, outsz, "the program's top level (a line whose code runs at load, such as a constant)");
    return;
  }
  if (map) {
    char key[300];
    snprintf(key, sizeof key, "\"c\":\"%s\"", csym);
    const char *p = strstr(map, key);
    if (p) {
      const char *r = strstr(p, "\"ruby\":\"");
      const char *f = strstr(p, "\"file\":\"");
      const char *l = strstr(p, "\"line\":");
      const char *eol = strchr(p, '}');
      if (r && r < eol) {
        r += 8;
        const char *re = strchr(r, '"');
        char file[1024] = "";
        if (f && f < eol) {
          f += 8;
          const char *fe = strchr(f, '"');
          const char *base = fe;
          while (base > f && base[-1] != '/') base--;
          snprintf(file, sizeof file, "%.*s", (int)(fe - base), base);
        }
        int line = l && l < eol ? atoi(l + 7) : 0;
        if (file[0] && strcmp(file, "session.rb"))
          snprintf(out, outsz, "%.*s (%s:%d)", (int)(re - r), r, file, line);
        else
          snprintf(out, outsz, "%.*s", (int)(re - r), r);
        return;
      }
    }
  }
  snprintf(out, outsz, "%s", csym);
}

/* The name a header item states, for a refusal: a struct's class, or the
   item's first words. */
static void header_item_name(const CItem *ci, char *out, size_t outsz) {
  const char *p = strstr(ci->text, "struct sp_");
  if (p && strchr(ci->text, '{')) {
    p += 10;
    size_t n = 0;
    while (is_ident((unsigned char)p[n])) n++;
    if (n > 2 && !strncmp(p + n - 2, "_s", 2)) n -= 2;
    snprintf(out, outsz, "the layout of %.*s", (int)n, p);
    return;
  }
  if (ci->name[0]) { snprintf(out, outsz, "%s", ci->name); return; }
  snprintf(out, outsz, "`%.60s`", ci->text);
}

/* Does `t` name any identifier in `m`? String literals aside. */
static int names_any(const char *t, const SMap *m) {
  if (!m->n) return 0;
  for (size_t k = 0; t[k];) {
    char c = t[k];
    if (c == '"' || c == '\'') {
      for (k++; t[k] && t[k] != c; k++) if (t[k] == '\\' && t[k + 1]) k++;
      if (t[k]) k++;
      continue;
    }
    if (is_ident((unsigned char)c) && !isdigit((unsigned char)c) && (k == 0 || !is_ident((unsigned char)t[k - 1]))) {
      size_t e = k;
      while (is_ident((unsigned char)t[e])) e++;
      if (smap_getn(m, t + k, e - k)) return 1;
      k = e;
    }
    else k++;
  }
  return 0;
}

/* A data definition no running code writes to: `const` ahead of its name. */
static int const_data(const CItem *ci) {
  char *t = strdup(ci->text);
  blank_attrs(t);
  char *eq = strchr(t, '=');
  if (eq) *eq = 0;
  int hit = 0;
  for (char *p = strstr(t, "const"); p; p = strstr(p + 1, "const"))
    if ((p == t || !is_ident((unsigned char)p[-1])) && !is_ident((unsigned char)p[5])) { hit = 1; break; }
  free(t);
  return hit;
}

enum { SEL_NONE, SEL_NEW, SEL_SWAP, SEL_REN, SEL_COPY };

static void add_why(char ***why, int *nwhy, const char *name) {
  *why = realloc(*why, (size_t)(*nwhy + 1) * sizeof **why);
  (*why)[(*nwhy)++] = strdup(name);
}

/* Compare the build in `dir` with what the process holds. 0 and the path of
   the shared object to load (in `so`, empty when nothing is new) when the
   line fits; -1 after printing what it would change.

   A new definition is loaded as it is (a new function behind a pointer of
   its own). A function whose body changed under the same head is loaded
   under a new name and its pointer repointed. A changed constant table is
   loaded under a new name too, and every function naming it with it. What
   else changed is a layout or a signature the running code was compiled
   against, and refuses the line. */
static int build_line_so(Session *s, const char *dir, int n, int at_load, char *so, size_t sosz) {
  so[0] = 0;
  CSet nw;
  if (cset_load(&nw, dir) != 0) { fprintf(stderr, "spinel console: cannot read %s\n", dir); return -1; }
  char **why = NULL;
  int nwhy = 0;
  CSet hdr[2];
  memset(hdr, 0, sizeof hdr);
  /* the headers: every declaration the process was built with still holds */
  const char *hdrs[] = { "sp_program.h", "sp_rt.h" };
  for (int h = 0; h < 2; h++) {
    CSet ho;
    if (cset_load_header(&ho, s->cur_dir, hdrs[h]) != 0 || cset_load_header(&hdr[h], dir, hdrs[h]) != 0) {
      cset_free(&ho);
      continue;
    }
    SMap texts = {0};
    for (int i = 0; i < hdr[h].n; i++) smap_putn(&texts, hdr[h].it[i].text, hdr[h].it[i].len, 1);
    for (int i = 0; i < ho.n; i++)
      if (!smap_getn(&texts, ho.it[i].text, ho.it[i].len)) {
        char nm[300];
        header_item_name(&ho.it[i], nm, sizeof nm);
        add_why(&why, &nwhy, nm);
      }
    smap_free(&texts);
    cset_free(&ho);
  }
  char suffix[32];
  snprintf(suffix, sizeof suffix, "__spl%d", n);
  unsigned char *sel = calloc((size_t)nw.n + 1, 1);
  SMap ren = {0};   /* tables this line renames */
  for (int i = 0; i < nw.n; i++) {
    CItem *ci = &nw.it[i];
    if ((ci->kind != CI_FN && ci->kind != CI_DATA) || ci->is_static || !ci->name[0] || skipped(ci->name)) continue;
    int *o = smap_get(&s->cur.by_name, ci->name);
    if (!o) { sel[i] = SEL_NEW; continue; }
    const CItem *old = &s->cur.it[*o];
    if (old->len == ci->len && !memcmp(old->text, ci->text, ci->len)) continue;
    /* the top level ran already: what changed there matters only when the
       line brings code for it to run */
    if (!strcmp(ci->name, "_sp_main_body") && !at_load) continue;
    if (ci->kind == CI_FN) {
      FnShape a, b;
      if (smap_get(&s->tramp, ci->name) && strcmp(ci->name, "_sp_main_body") &&
          old->kind == CI_FN && fn_shape(old, &a) == 0 && fn_shape(ci, &b) == 0 &&
          a.head_len == b.head_len && !memcmp(old->text, ci->text, a.head_len))
        sel[i] = SEL_SWAP;
      else add_why(&why, &nwhy, ci->name);
    }
    else if (old->kind == CI_DATA && const_data(ci)) { sel[i] = SEL_REN; smap_put(&ren, ci->name, 1); }
    else add_why(&why, &nwhy, ci->name);
  }
  /* what names a renamed table follows it */
  for (int grew = 1; grew && ren.n;) {
    grew = 0;
    for (int i = 0; i < nw.n; i++) {
      CItem *ci = &nw.it[i];
      if (sel[i] || (ci->kind != CI_FN && ci->kind != CI_DATA) || !ci->name[0] || skipped(ci->name)) continue;
      if (!names_any(ci->text, &ren)) continue;
      if (ci->is_static) add_why(&why, &nwhy, ci->name);
      else if (ci->kind == CI_FN && smap_get(&s->tramp, ci->name) && strcmp(ci->name, "_sp_main_body")) sel[i] = SEL_SWAP;
      else if (ci->kind == CI_DATA && const_data(ci)) { sel[i] = SEL_REN; smap_put(&ren, ci->name, 1); }
      else add_why(&why, &nwhy, ci->name);
      if (sel[i]) grew = 1;
      else sel[i] = SEL_COPY + 1;   /* refused; not visited again */
    }
  }
  for (int i = 0; i < nw.n; i++) if (sel[i] > SEL_COPY) sel[i] = SEL_NONE;
  for (int h = 0; h < 2; h++) {
    for (int i = 0; i < hdr[h].n; i++)
      if (names_any(hdr[h].it[i].text, &ren)) {
        char nm[300];
        header_item_name(&hdr[h].it[i], nm, sizeof nm);
        add_why(&why, &nwhy, nm);
      }
    cset_free(&hdr[h]);
  }
  if (nwhy) {
    char *map = NULL;
    char mp[4200], log[4200];
    snprintf(mp, sizeof mp, "%s/symbols.json", s->dir);
    snprintf(log, sizeof log, "%s/map.log", s->dir);
    if (run_spinel(s, log, "--emit-symbol-map", "-o", mp, (char *)NULL) == 0) map = read_file(mp, NULL);
    fprintf(stderr, "refused: this line changes code the running program already holds:\n");
    SMap said = {0};
    for (int i = 0; i < nwhy; i++) {
      char nm[1400];
      ruby_name_of(map, why[i], nm, sizeof nm);
      if (!smap_get(&said, nm)) { fprintf(stderr, "  %s\n", nm); smap_put(&said, nm, 1); }
      free(why[i]);
    }
    smap_free(&said);
    fprintf(stderr, ":restart folds the line into the program and replays the session\n");
    free(map); free(why); free(sel);
    smap_free(&ren);
    cset_free(&nw);
    return -1;
  }
  free(why);
  /* a static definition the loaded code names comes along, as its own copy */
  {
    unsigned char *scanned = calloc((size_t)nw.n + 1, 1);
    SMap used = {0};
    for (int grew = 1; grew;) {
      grew = 0;
      for (int i = 0; i < nw.n; i++) {
        if (!sel[i] || scanned[i]) continue;
        scanned[i] = 1;
        const char *t = nw.it[i].text;
        for (size_t k = 0; t[k];) {
          if (is_ident((unsigned char)t[k]) && !isdigit((unsigned char)t[k]) && (k == 0 || !is_ident((unsigned char)t[k - 1]))) {
            size_t e = k;
            while (is_ident((unsigned char)t[e])) e++;
            smap_putn(&used, t + k, e - k, 1);
            k = e;
          }
          else k++;
        }
      }
      for (int i = 0; i < nw.n; i++)
        if (!sel[i] && nw.it[i].is_static && (nw.it[i].kind == CI_FN || nw.it[i].kind == CI_DATA) &&
            nw.it[i].name[0] && smap_get(&used, nw.it[i].name)) { sel[i] = SEL_COPY; grew = 1; }
    }
    smap_free(&used);
    free(scanned);
  }
  int any = 0;
  unsigned char *unit_used = calloc((size_t)nw.nunits + 1, 1);
  for (int i = 0; i < nw.n; i++) if (sel[i]) { any = 1; unit_used[nw.it[i].unit] = 1; }
  for (size_t k = 0; k < ren.cap; k++) if (ren.k[k]) smap_put(&s->renamed, ren.k[k], n);
  SMap fresh = {0};   /* functions this line puts behind a pointer */
  if (any) {
    Buf c = {0};
    for (size_t k = 0; k < s->renamed.cap; k++)
      if (s->renamed.k[k]) b_printf(&c, "#define %s %s__spl%d\n", s->renamed.k[k], s->renamed.k[k], s->renamed.v[k]);
    b_add(&c, "#include \"sp_program.h\"\n");
    SMap seen = {0};
    Buf ctor = {0};
    for (int i = 0; i < nw.n; i++) {
      CItem *ci = &nw.it[i];
      if (!unit_used[ci->unit]) continue;
      FnShape sh;
      if (ci->kind == CI_FN || ci->kind == CI_DATA) {
        if (!sel[i]) continue;
        if (ci->kind == CI_FN && sel[i] == SEL_NEW && fn_shape(ci, &sh) == 0) {
          emit_trampoline(&c, ci, &sh, suffix);
          smap_put(&fresh, ci->name, 1);
          continue;
        }
        if (sel[i] == SEL_SWAP && fn_shape(ci, &sh) == 0) {
          emit_renamed(&c, ci, &sh, suffix);
          b_printf(&c, "extern __typeof__(%s%s) *%s__spl_ptr;\n", ci->name, suffix, ci->name);
          b_printf(&ctor, "  %s__spl_ptr = %s%s;\n", ci->name, ci->name, suffix);
          continue;
        }
      }
      else {
        if (smap_getn(&seen, ci->text, ci->len)) continue;
        smap_putn(&seen, ci->text, ci->len, 1);
      }
      b_addn(&c, ci->text, ci->len);
      b_add(&c, "\n");
    }
    smap_free(&seen);
    if (ctor.len)
      b_printf(&c, "__attribute__((constructor)) static void sp_console_install_%d(void) {\n%s}\n", n, ctor.p);
    free(ctor.p);
    char cpath[4200], log[4200];
    snprintf(cpath, sizeof cpath, "%s/line%d.c", dir, n);
    snprintf(so, sosz, "%s/line%d.so", dir, n);
    snprintf(log, sizeof log, "%s/cc.log", dir);
    write_file(cpath, c.p);
    free(c.p);
    Buf cmd = {0};
    b_printf(&cmd, "%s -O1 -w -fPIC -shared", s->cc);
#if defined(__APPLE__)
    b_add(&cmd, " -undefined dynamic_lookup");
#endif
    b_printf(&cmd, "%s '-I%s' '%s' -o '%s'", s->cflags.p ? s->cflags.p : "", dir, cpath, so);
    char *sh[] = { "/bin/sh", "-c", cmd.p, NULL };
    int rc = run(sh, log);
    free(cmd.p);
    if (rc != 0) {
      fprintf(stderr, "spinel console: the line's C did not compile (%s):\n", cpath);
      show_log(log);
      so[0] = 0;
      any = -1;
    }
  }
  free(unit_used);
  free(sel);
  smap_free(&ren);
  if (any < 0) { smap_free(&fresh); cset_free(&nw); return -1; }
  for (size_t k = 0; k < fresh.cap; k++) if (fresh.k[k]) smap_put(&s->tramp, fresh.k[k], 1);
  smap_free(&fresh);
  cset_free(&s->cur);
  s->cur = nw;
  snprintf(s->cur_dir, sizeof s->cur_dir, "%s", dir);
  return 0;
}

/* Compile a line against the session and run it in the process. */
static void line_run(Session *s, Line *l) {
  double t0 = now_s();
  int n = s->nlines + 1;
  write_session(s, l);
  char dir[4200], log[4200];
  s->gen++;
  snprintf(dir, sizeof dir, "%s/u%d", s->dir, s->gen);
  snprintf(log, sizeof log, "%s/line.log", s->dir);
  if (run_spinel(s, log, "-c", "--incremental", "-o", dir, (char *)NULL) != 0) {
    show_log(log);
    line_free(l);
    return;
  }
  double t1 = now_s();
  char so[4200];
  if (build_line_so(s, dir, n, l->at_load, so, sizeof so) != 0) {
    if (s->has_pend) line_free(&s->pend);
    s->pend = *l;
    s->has_pend = 1;
    return;
  }
  double t2 = now_s();
  /* accepted: the process holds its code from here on */
  s->lines = realloc(s->lines, (size_t)n * sizeof *s->lines);
  s->lines[s->nlines++] = *l;
  for (int i = 0; i < l->nlocals; i++) smap_put(&s->locals, l->locals[i], 1);
  if (s->has_pend) { line_free(&s->pend); s->has_pend = 0; }
  char sym[64];
  snprintf(sym, sizeof sym, "sp___spl_l%d", n);
  host_call(s, so[0] ? so : "-", sym, 0);
  s->lines[s->nlines - 1].stopped = s->interrupted;
  if (s->timing)
    fprintf(stderr, "[console] compile %.2fs, load %.2fs, run %.2fs\n", t1 - t0, t2 - t1, now_s() - t2);
}

static void rm_rf(const char *dir) {
  char *argv[] = { "rm", "-rf", (char *)dir, NULL };
  run(argv, NULL);
}

/* ---- line editing on a terminal ---- */

static struct termios ed_cooked;
static int ed_raw_on;
static char **ed_hist;
static int ed_nhist;
static char ed_hist_path[4096];

static void ed_restore(void) {
  if (ed_raw_on) { tcsetattr(0, TCSANOW, &ed_cooked); ed_raw_on = 0; }
}

static int ed_raw(void) {
  if (tcgetattr(0, &ed_cooked) != 0) return -1;
  struct termios t = ed_cooked;
  t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  if (tcsetattr(0, TCSANOW, &t) != 0) return -1;
  ed_raw_on = 1;
  return 0;
}

static void ed_hist_add(const char *l, int save) {
  if (!*l || (ed_nhist && !strcmp(ed_hist[ed_nhist - 1], l))) return;
  ed_hist = realloc(ed_hist, (size_t)(ed_nhist + 1) * sizeof *ed_hist);
  ed_hist[ed_nhist++] = strdup(l);
  if (save && ed_hist_path[0]) {
    FILE *f = fopen(ed_hist_path, "a");
    if (f) { fprintf(f, "%s\n", l); fclose(f); }
  }
}

static void ed_hist_load(void) {
  const char *home = getenv("HOME");
  if (!home || !*home) return;
  snprintf(ed_hist_path, sizeof ed_hist_path, "%s/.spinel_console_history", home);
  char *t = read_file(ed_hist_path, NULL);
  if (!t) return;
  for (char *l = t; *l;) {
    char *e = strchr(l, '\n');
    if (e) *e = 0;
    ed_hist_add(l, 0);
    if (!e) break;
    l = e + 1;
  }
  free(t);
}

static void ed_hist_free(void) {
  for (int i = 0; i < ed_nhist; i++) free(ed_hist[i]);
  free(ed_hist);
  ed_hist = NULL;
  ed_nhist = 0;
}

/* Columns between two byte offsets: one per UTF-8 character. */
static int ed_cols(const char *s, size_t from, size_t to) {
  int n = 0;
  for (size_t i = from; i < to; i++) if (((unsigned char)s[i] & 0xc0) != 0x80) n++;
  return n;
}

static void ed_refresh(const char *prompt, const Buf *b, size_t pos) {
  Buf o = {0};
  b_add(&o, "\r");
  b_add(&o, prompt);
  if (b->len) b_addn(&o, b->p, b->len);
  b_add(&o, "\x1b[K");
  int back = ed_cols(b->p ? b->p : "", pos, b->len);
  if (back) b_printf(&o, "\x1b[%dD", back);
  if (write(1, o.p, o.len) < 0) {}
  free(o.p);
}

static void ed_set(Buf *b, size_t *pos, const char *s) {
  b->len = 0;
  b_add(b, s);
  *pos = b->len;
}

static void ed_delete(Buf *b, size_t at, size_t n) {
  memmove(b->p + at, b->p + at + n, b->len - at - n);
  b->len -= n;
  b->p[b->len] = 0;
}

enum { ED_LINE, ED_EOF, ED_CANCEL };

/* One line typed at `prompt`, into `out` with its newline. */
static int ed_read(const char *prompt, Buf *out) {
  fflush(stdout);
  if (ed_raw() != 0) {
    fputs(prompt, stdout);
    fflush(stdout);
    char buf[65536];
    if (!fgets(buf, sizeof buf, stdin)) return ED_EOF;
    b_add(out, buf);
    return ED_LINE;
  }
  Buf b = {0};
  b_add(&b, "");
  size_t pos = 0;
  int hi = ed_nhist;     /* the history entry shown; ed_nhist is the edit itself */
  char *draft = NULL;
  int rc = ED_LINE;
  ed_refresh(prompt, &b, pos);
  for (;;) {
    unsigned char c;
    ssize_t r = read(0, &c, 1);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { rc = ED_EOF; break; }
    int key = c;
    if (c == 27) {
      unsigned char q[2];
      if (read(0, &q[0], 1) != 1) continue;
      if (q[0] != '[' && q[0] != 'O') continue;
      if (read(0, &q[1], 1) != 1) continue;
      if (q[1] >= '0' && q[1] <= '9') {
        unsigned char t;
        if (read(0, &t, 1) != 1 || t != '~') continue;
        key = q[1] == '3' ? 0x104 : (q[1] == '1' || q[1] == '7') ? 1 : (q[1] == '4' || q[1] == '8') ? 5 : -1;
      }
      else key = q[1] == 'A' ? 16 : q[1] == 'B' ? 14 : q[1] == 'C' ? 6 : q[1] == 'D' ? 2 :
                 q[1] == 'H' ? 1 : q[1] == 'F' ? 5 : -1;
    }
    switch (key) {
    case '\r': case '\n':
      goto done;
    case 3:   /* ^C */
      rc = ED_CANCEL;
      goto done;
    case 4:   /* ^D: end of input on an empty line */
      if (!b.len) { rc = ED_EOF; goto done; }
      /* fall through */
    case 0x104:
      if (pos < b.len) {
        size_t n = 1;
        while (pos + n < b.len && ((unsigned char)b.p[pos + n] & 0xc0) == 0x80) n++;
        ed_delete(&b, pos, n);
      }
      break;
    case 127: case 8:
      if (pos) {
        size_t s = pos - 1;
        while (s && ((unsigned char)b.p[s] & 0xc0) == 0x80) s--;
        ed_delete(&b, s, pos - s);
        pos = s;
      }
      break;
    case 2:   /* left */
      if (pos) { pos--; while (pos && ((unsigned char)b.p[pos] & 0xc0) == 0x80) pos--; }
      break;
    case 6:   /* right */
      if (pos < b.len) { pos++; while (pos < b.len && ((unsigned char)b.p[pos] & 0xc0) == 0x80) pos++; }
      break;
    case 1: pos = 0; break;
    case 5: pos = b.len; break;
    case 11: b.len = pos; b.p[pos] = 0; break;
    case 21: ed_delete(&b, 0, pos); pos = 0; break;
    case 23: {
      size_t s = pos;
      while (s && b.p[s - 1] == ' ') s--;
      while (s && b.p[s - 1] != ' ') s--;
      ed_delete(&b, s, pos - s);
      pos = s;
      break;
    }
    case 12:
      if (write(1, "\x1b[H\x1b[2J", 7) < 0) {}
      break;
    case 16: case 14: {   /* history: up, down */
      int to = key == 16 ? hi - 1 : hi + 1;
      if (to < 0 || to > ed_nhist) break;
      if (hi == ed_nhist) { free(draft); draft = strdup(b.p); }
      hi = to;
      ed_set(&b, &pos, hi == ed_nhist ? (draft ? draft : "") : ed_hist[hi]);
      break;
    }
    default:
      if (key >= 32 && key < 256 && key != 127) {
        char ch = (char)key;
        b_add(&b, " ");
        memmove(b.p + pos + 1, b.p + pos, b.len - pos - 1);
        b.p[pos++] = ch;
      }
      break;
    }
    ed_refresh(prompt, &b, pos);
  }
done:
  ed_restore();
  if (write(1, rc == ED_CANCEL ? "^C\r\n" : "\r\n", rc == ED_CANCEL ? 4 : 2) < 0) {}
  if (rc == ED_LINE) {
    ed_hist_add(b.p, 1);
    b_add(out, b.p);
    b_add(out, "\n");
  }
  free(draft);
  free(b.p);
  return rc;
}

static void usage(void) {
  fprintf(stderr,
    "Usage: spinel console [FILE.rb] [-I DIR]...\n"
    "Runs FILE's top level, then compiles each line against the running\n"
    "program and loads it there: its classes, objects and the line's locals\n"
    "carry over. Commands: :restart (fold a refused line into the program and\n"
    "replay the session), :quit (or exit, or end of input).\n");
}

int console_main(int argc, char **argv, const char *self, const char *lib_dir) {
  Session s;
  memset(&s, 0, sizeof s);
  s.self = self;
  s.lib_dir = lib_dir;
  s.ack = -1;
  s.cc = getenv("CC") && *getenv("CC") ? getenv("CC") : "cc";
  s.timing = getenv("SPINEL_CONSOLE_TIMING") != NULL;
  s.keep = getenv("SPINEL_CONSOLE_KEEP") != NULL;
  s.extra = calloc((size_t)argc + 1, sizeof *s.extra);
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
    if (!strcmp(a, "-I") && i + 1 < argc) { s.extra[s.nextra++] = argv[i]; s.extra[s.nextra++] = argv[++i]; continue; }
    if (!strncmp(a, "-I", 2) || !strncmp(a, "--", 2)) { s.extra[s.nextra++] = argv[i]; continue; }
    if (s.prog) { usage(); return 2; }
    char rp[4096];
    if (!realpath(a, rp)) { fprintf(stderr, "spinel console: %s: %s\n", a, strerror(errno)); return 2; }
    size_t n = strlen(rp);
    if (n > 3 && !strcmp(rp + n - 3, ".rb")) rp[n - 3] = 0;
    s.prog = strdup(rp);
  }
  const char *tmp = getenv("TMPDIR");
  snprintf(s.dir, sizeof s.dir, "%s/spinel-console.XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(s.dir)) { fprintf(stderr, "spinel console: %s\n", strerror(errno)); return 1; }
  signal(SIGPIPE, SIG_IGN);
  /* ^C interrupts the running line, not the session; no SA_RESTART, so the
     wait for the host sees it */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_int;
  sigaction(SIGINT, &sa, NULL);
  signal(SIGHUP, on_hup);
  signal(SIGTERM, on_hup);
  int rc = 0;
  if (session_boot(&s, 0) != 0) {
    rc = 1;
    goto done;
  }
  int tty = isatty(0) && isatty(1);
  if (tty) { ed_hist_load(); atexit(ed_restore); }
  Buf in = {0};
  char buf[65536];
  for (;;) {
    if (tty) {
      int r = ed_read(in.len ? "spinel* " : "spinel> ", &in);
      if (r == ED_EOF) break;
      if (r == ED_CANCEL) { if (in.p) { in.len = 0; in.p[0] = 0; } continue; }
    }
    else {
      if (!fgets(buf, sizeof buf, stdin)) break;
      b_add(&in, buf);
    }
    if (!in.len || in.p[strspn(in.p, " \t\r\n")] == 0) { in.len = 0; in.p[0] = 0; continue; }
    char cmd[64], rest[8];
    if (sscanf(in.p, " %63s %7s", cmd, rest) == 1) {
      if (!strcmp(cmd, ":quit") || !strcmp(cmd, ":q") || !strcmp(cmd, "exit") || !strcmp(cmd, "quit")) break;
      if (!strcmp(cmd, ":help")) { usage(); in.len = 0; in.p[0] = 0; continue; }
      if (!strcmp(cmd, ":restart")) {
        int folded = s.has_pend;
        if (s.has_pend) {
          s.lines = realloc(s.lines, (size_t)(s.nlines + 1) * sizeof *s.lines);
          Line *l = &s.lines[s.nlines++];
          *l = s.pend;
          for (int i = 0; i < l->nlocals; i++) smap_put(&s.locals, l->locals[i], 1);
          s.has_pend = 0;
        }
        fprintf(stderr, "restarting: the program's top level and %d line%s run again\n", s.nlines, s.nlines == 1 ? "" : "s");
        if (session_boot(&s, folded) != 0) fprintf(stderr, "spinel console: the restart failed\n");
        in.len = 0; in.p[0] = 0;
        continue;
      }
    }
    if (!s.req && session_boot(&s, 0) != 0) { in.len = 0; in.p[0] = 0; continue; }
    Line l;
    int r = line_parse(&s, in.p, &l);
    if (r == LINE_MORE) continue;
    in.len = 0; in.p[0] = 0;
    if (r == LINE_ERROR) continue;
    line_run(&s, &l);
  }
  free(in.p);
done:
  host_stop(&s);
  if (!s.keep) rm_rf(s.dir);
  else fprintf(stderr, "session kept in %s\n", s.dir);
  for (int i = 0; i < s.nlines; i++) line_free(&s.lines[i]);
  free(s.lines);
  if (s.has_pend) line_free(&s.pend);
  cset_free(&s.cur);
  smap_free(&s.locals);
  smap_free(&s.tramp);
  ed_hist_free();
  smap_free(&s.renamed);
  free(s.cflags.p);
  free(s.prog);
  free(s.extra);
  return rc;
}

#endif
