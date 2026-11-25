/*
 * vibe-gam - talk to app_man (MediaTek's "GAM") as a client, the way the
 * vendor's own native sample UI does (/3rd/bin/launcher.bin, "dockui").
 * See gam.c and docs/findings/06-native-route.md.
 *
 * usage: vibe-gam [--user=...] [--name=N] info
 *        vibe-gam ... --yes cmd DST CMD PARA        GAM_SendCmd
 *        vibe-gam ... --yes raw MSG                 GAM_SendRawMsg
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "gam.h"

static void dump(const char *tag, int type, const void *data, int len)
{
     printf("[%s] type=0x%x len=%d", tag, type, len);
     if (data && len > 0) {
          const unsigned char *p = data;
          printf(" text='");
          for (int i = 0; i < len && i < 200; i++)
               putchar(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
          printf("'");
     }
     printf("\n");
     fflush(stdout);
}
static void on_sync(int type, const void *data, int len)  { dump("sync", type, data, len); }
static void on_async(int type, const void *data, int len) { dump("async", type, data, len); }

int main(int argc, char **argv)
{
     const char *user = NULL, *name = "vibe";
     int yes = 0, i, ret = 2, r;

     for (i = 1; i < argc && argv[i][0] == '-'; i++) {
          if (!strncmp(argv[i], "--user=", 7))      user = argv[i] + 7;
          else if (!strncmp(argv[i], "--name=", 7)) name = argv[i] + 7;
          else if (!strcmp(argv[i], "--yes"))       yes = 1;
          else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
     }
     if (i >= argc) {
          fprintf(stderr, "usage: vibe-gam [--user=..] [--name=N] info | --yes cmd DST CMD PARA | --yes raw MSG\n");
          return 2;
     }
     const char *cmd = argv[i++];
     int nargs = argc - i;
     char **args = argv + i;
     if ((!strcmp(cmd, "cmd") || !strcmp(cmd, "raw")) && !yes) {
          fprintf(stderr, "vibe-gam: %s sends a command to app_man; add --yes\n", cmd);
          return 2;
     }

     setvbuf(stdout, NULL, _IOLBF, 0);
     alarm(20);
     if (user && vibe_drop_privileges(user))
          return 1;
     if (gam_open(name, on_sync, on_async))
          return 1;
     printf("GAM client '%s' up\n", name);

     if (!strcmp(cmd, "info") && nargs == 0) {
          char buf[256];
          int n, st;
          gam_default_process(buf, sizeof(buf)); printf("default process: '%s'\n", buf);
          gam_focus_process(buf, sizeof(buf));   printf("focus process:   '%s'\n", buf);
          gam_active_process(buf, sizeof(buf));  printf("active process:  '%s'\n", buf);
          n = gam_process_count();
          printf("process count %d\n", n);
          for (int k = 0; k < n && k < 128; k++) {
               gam_process(k, buf, sizeof(buf), &st);
               printf("  %-24s state=%d\n", buf, st);
          }
          ret = 0;
     } else if (!strcmp(cmd, "cmd") && nargs == 3) {
          r = gam_send_cmd(args[0], args[1], args[2]);
          printf("GAM_SendCmd {%s -> %s: %s=%s} r=%d\n", name, args[0], args[1], args[2], r);
          sleep(2);   /* let async answers arrive */
          ret = 0;
     } else if (!strcmp(cmd, "raw") && nargs == 1) {
          r = gam_send_raw(args[0]);
          printf("GAM_SendRawMsg '%s' r=%d\n", args[0], r);
          sleep(2);
          ret = 0;
     } else {
          fprintf(stderr, "vibe-gam: bad command\n");
     }

     gam_close();
     alarm(0);
     return ret;
}
