/*
 * vibe-model - command-line access to the TV's "kilby" model service
 * (biz.c: the vendor's C facade in libbizAgent.so).
 *
 * usage: vibe-model [--user=...] [--timeout=S] get-int KEY
 *        vibe-model ... get-str KEY
 *        vibe-model ... get-vstr KEY
 *        vibe-model ... inputs
 *        vibe-model ... --yes set-int KEY VALUE
 *        vibe-model ... --yes action KEY [ARG...]
 *        vibe-model ... --yes switch-input ID          e.g. HDMI2
 * Anything that changes state needs --yes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "biz.h"
#include "common.h"

static void usage(void)
{
     fprintf(stderr,
             "usage: vibe-model [--user=UID:GID[:G,...]] [--yes] [--timeout=S]\n"
             "         get-int KEY | get-str KEY | get-vstr KEY | inputs |\n"
             "         set-int KEY VALUE | action KEY [ARG...] | switch-input ID\n");
}

int main(int argc, char **argv)
{
     const char *user = NULL;
     int yes = 0, timeout = 15, i, ret = 2;

     for (i = 1; i < argc && argv[i][0] == '-'; i++) {
          if (!strncmp(argv[i], "--user=", 7))          user = argv[i] + 7;
          else if (!strcmp(argv[i], "--yes"))           yes = 1;
          else if (!strncmp(argv[i], "--timeout=", 10)) timeout = atoi(argv[i] + 10);
          else { usage(); return 2; }
     }
     if (i >= argc) { usage(); return 2; }
     const char *cmd = argv[i++];
     int nargs = argc - i;
     char **args = argv + i;
     int writes = !strcmp(cmd, "set-int") || !strcmp(cmd, "action") || !strcmp(cmd, "switch-input");

     if (writes && !yes) {
          fprintf(stderr, "vibe-model: %s changes the TV's state; add --yes\n", cmd);
          return 2;
     }
     setvbuf(stdout, NULL, _IOLBF, 0);
     /* the model client starts threads and a connection; never hang here */
     alarm(timeout > 0 && timeout < 120 ? (unsigned)timeout : 15);

     if (user && vibe_drop_privileges(user))
          return 1;
     if (biz_open())
          return 1;

     if (!strcmp(cmd, "get-int") && nargs == 1) {
          int v = 0, r = biz_get_int(args[0], &v);
          printf("%s = %d (r=%d)\n", args[0], v, r);
          ret = 0;
     } else if (!strcmp(cmd, "get-str") && nargs == 1) {
          static char s[BIZ_STR_MAX];
          int r = biz_get_str(args[0], s);
          printf("%s = '%s' (r=%d)\n", args[0], s, r);
          ret = 0;
     } else if (!strcmp(cmd, "get-vstr") && nargs == 1) {
          char **v = biz_vec_alloc();
          int r = v ? biz_get_vstr(args[0], v) : -1;
          printf("%s (r=%d):\n", args[0], r);
          for (int k = 0; v && k < BIZ_VEC_MAX; k++)
               if (v[k][0])
                    printf("  [%d] '%s'\n", k, v[k]);
          ret = 0;
     } else if (!strcmp(cmd, "inputs") && nargs == 0) {
          struct biz_input in[BIZ_MAX_INPUTS];
          char cur[32] = "";
          int n = biz_inputs(in, BIZ_MAX_INPUTS);
          biz_current_input(cur, sizeof(cur));
          printf("current: '%s'\n", cur);
          for (int k = 0; k < n; k++)
               printf("  %-8s signal=%d label='%s'\n", in[k].id, in[k].signal, in[k].label);
          ret = 0;
     } else if (!strcmp(cmd, "set-int") && nargs == 2) {
          int r = biz_set_int(args[0], atoi(args[1]));
          printf("set %s = %d -> r=%d\n", args[0], atoi(args[1]), r);
          ret = 0;
     } else if (!strcmp(cmd, "action") && nargs >= 1) {
          int r = biz_action(args[0], nargs - 1, (const char **)args + 1);
          printf("action %s argc=%d -> r=%d\n", args[0], nargs - 1, r);
          ret = 0;
     } else if (!strcmp(cmd, "switch-input") && nargs == 1) {
          int r = biz_switch_input(args[0]);
          printf("switch-input %s -> r=%d\n", args[0], r);
          ret = 0;
     } else {
          usage();
     }

     biz_close();
     alarm(0);
     return ret;
}
