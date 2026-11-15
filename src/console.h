#ifndef SPINEL_CONSOLE_H
#define SPINEL_CONSOLE_H

/* `spinel console [FILE.rb] [-I DIR ...]`: each line is compiled against the
   running program and loaded into it (console.c). `self` is this compiler's
   path, `lib_dir` the runtime's directory. Answers the exit status. */
int console_main(int argc, char **argv, const char *self, const char *lib_dir);

#include "csplit.h"
/* --console-host DIR: the unit set the host is built from is copied to DIR,
   then each program function in it is rewritten to be reached through a
   pointer a loaded line can repoint. 0, or -1. */
extern const char *g_console_host_dir;
int console_host_prepare(const char *work, CSplitResult *res);

#endif
