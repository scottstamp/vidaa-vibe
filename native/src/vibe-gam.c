/*
 * vibe-gam - talk to app_man (MediaTek's "GAM") as a client, the way the
 * vendor's own native sample UI does (/3rd/bin/launcher.bin, "dockui").
 * /basic/libgam_client.so is loaded with dlopen.
 *
 * Prototypes, from the library's own __PRETTY_FUNCTION__ strings:
 *   int GAM_ClientInit(GAM_APPCTRL_HANDLER_t*, const char *name);
 *   int GAM_ClientUninit();
 *   int GAM_GetDefaultProcess(char*), GAM_GetFocusProcess(char*),
 *       GAM_GetActiveProcess(char*);
 *   int GAM_LoadProcessList(); GAM_GetProcessListCount(unsigned*);
 *   int GAM_GetProcessListElement(unsigned, char*);
 *   int GAM_GetProcessState(const char*, GAMProcessState*);
 *   int GAM_SendCmd(GAM_CLIENT_SEND_CMD_T*);  int GAM_SendRawMsg(const char*);
 * The handler struct, as dockui fills it (its .data):
 *   { onSync(type, data, len), onAsync(type, data, len), "deprecated", "-", 1, 0... }
 * The send-command struct, as dockui announces itself:
 *   { src = "dockui", dst = "am", 0, cmd = "started", para = "dockui" }
 *
 * usage: vibe-gam [--user=...] [--name=N] info
 *        vibe-gam ... --yes cmd DST CMD PARA        GAM_SendCmd
 *        vibe-gam ... --yes raw MSG                 GAM_SendRawMsg
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

typedef void (*gam_cb)(int type, const void *data, int len);

/* layout copied from dockui's initialised data; slack at the end */
struct gam_handler {
     gam_cb      on_sync;
     gam_cb      on_async;
     const char *a;      /* "deprecated" */
     const char *b;      /* "-" */
     int         c;      /* 1 */
     int         pad[27];
};

struct gam_send_cmd {
     const char *src;
     const char *dst;
     int         zero;
     const char *cmd;
     const char *para;
     int         pad[16];
};

static struct {
     void *lib;
     int (*client_init)(struct gam_handler *, const char *);
     int (*client_uninit)(void);
     int (*get_default)(char *);
     int (*get_focus)(char *);
     int (*get_active)(char *);
     int (*load_list)(void);
     int (*list_count)(unsigned *);
     int (*list_element)(unsigned, char *);
     int (*proc_state)(const char *, int *);
     int (*send_cmd)(struct gam_send_cmd *);
     int (*send_raw)(const char *);
} g;

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

static int gam_open(void)
{
     g.lib = dlopen("/basic/libgam_client.so", RTLD_NOW | RTLD_GLOBAL);
     if (!g.lib) {
          fprintf(stderr, "vibe-gam: dlopen: %s\n", dlerror());
          return -1;
     }
#define SYM(f, n) if (!(*(void **)&g.f = dlsym(g.lib, n))) { fprintf(stderr, "missing %s\n", n); return -1; }
     SYM(client_init, "GAM_ClientInit");
     SYM(client_uninit, "GAM_ClientUninit");
     SYM(get_default, "GAM_GetDefaultProcess");
     SYM(get_focus, "GAM_GetFocusProcess");
     SYM(get_active, "GAM_GetActiveProcess");
     SYM(load_list, "GAM_LoadProcessList");
     SYM(list_count, "GAM_GetProcessListCount");
     SYM(list_element, "GAM_GetProcessListElement");
     SYM(proc_state, "GAM_GetProcessState");
     SYM(send_cmd, "GAM_SendCmd");
     SYM(send_raw, "GAM_SendRawMsg");
#undef SYM
     return 0;
}

static int cmd_info(void)
{
     static char buf[4096];
     unsigned n = 0;
     int r;

     memset(buf, 0, sizeof(buf)); r = g.get_default(buf); printf("default process: '%s' (r=%d)\n", buf, r);
     memset(buf, 0, sizeof(buf)); r = g.get_focus(buf);   printf("focus process:   '%s' (r=%d)\n", buf, r);
     memset(buf, 0, sizeof(buf)); r = g.get_active(buf);  printf("active process:  '%s' (r=%d)\n", buf, r);
     r = g.load_list();
     printf("LoadProcessList r=%d\n", r);
     r = g.list_count(&n);
     printf("process count %u (r=%d)\n", n, r);
     for (unsigned i = 0; i < n && i < 128; i++) {
          int st[64] = { -1 };
          memset(buf, 0, sizeof(buf));
          g.list_element(i, buf);
          r = g.proc_state(buf, st);
          printf("  %-24s state=%d (r=%d)\n", buf, st[0], r);
     }
     return 0;
}

int main(int argc, char **argv)
{
     static struct gam_handler h;
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
     if (gam_open())
          return 1;

     h.on_sync = on_sync;
     h.on_async = on_async;
     h.a = "deprecated";
     h.b = "-";
     h.c = 1;
     r = g.client_init(&h, name);
     printf("GAM_ClientInit('%s') r=%d\n", name, r);
     if (r != 0) {
          fprintf(stderr, "vibe-gam: ClientInit failed\n");
          return 1;
     }

     if (!strcmp(cmd, "info") && nargs == 0) {
          ret = cmd_info();
     } else if (!strcmp(cmd, "cmd") && nargs == 3) {
          struct gam_send_cmd c;
          memset(&c, 0, sizeof(c));
          c.src = name; c.dst = args[0]; c.cmd = args[1]; c.para = args[2];
          r = g.send_cmd(&c);
          printf("GAM_SendCmd {%s -> %s: %s=%s} r=%d\n", name, args[0], args[1], args[2], r);
          sleep(2);   /* let async answers arrive */
          ret = 0;
     } else if (!strcmp(cmd, "raw") && nargs == 1) {
          r = g.send_raw(args[0]);
          printf("GAM_SendRawMsg '%s' r=%d\n", args[0], r);
          sleep(2);
          ret = 0;
     } else {
          fprintf(stderr, "vibe-gam: bad command\n");
     }

     r = g.client_uninit();
     printf("GAM_ClientUninit r=%d\n", r);
     alarm(0);
     return ret;
}
