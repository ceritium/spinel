/* The loop a `spinel console` program runs after its top level (console.c).
   Each request is one line, "<path> <symbol> <v|q>": dlopen the path ("-"
   for a symbol the binary itself holds), call the symbol, and answer one
   byte. "q" runs it with stdout sent to /dev/null (a replayed line); "!"
   only answers. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void sp_console_loop(void) {
  const char *in = getenv("SPINEL_CONSOLE_IN"), *out = getenv("SPINEL_CONSOLE_OUT");
  if (!in || !out) return;
  FILE *rq = fdopen(atoi(in), "r");
  int ack = atoi(out);
  if (!rq) return;
  char req[8192];
  while (fgets(req, sizeof req, rq)) {
    req[strcspn(req, "\n")] = 0;
    char *sym = strchr(req, ' ');
    if (!sym) break;
    *sym++ = 0;
    char *mode = strchr(sym, ' ');
    if (mode) *mode++ = 0;
    int quiet = mode && *mode == 'q';
    if (strcmp(req, "!") == 0) {   /* a ping: the top level has run */
      char k = 'k';
      if (write(ack, &k, 1) != 1) break;
      continue;
    }
    void *h = strcmp(req, "-") == 0 ? dlopen(NULL, RTLD_NOW) : dlopen(req, RTLD_NOW | RTLD_GLOBAL);
    void (*fn)(void) = h ? (void (*)(void))dlsym(h, sym) : NULL;
    char status = 'k';
    if (!fn) {
      fprintf(stderr, "console: %s\n", dlerror());
      status = 'e';
    }
    else {
      int saved = -1;
      if (quiet) {
        fflush(stdout);
        saved = dup(1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); close(nul); }
      }
      fn();
      fflush(stdout);
      if (saved >= 0) { dup2(saved, 1); close(saved); }
    }
    if (write(ack, &status, 1) != 1) break;
  }
}
