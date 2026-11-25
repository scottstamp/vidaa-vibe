/* app_man client over /basic/libgam_client.so. See gam.h. */
#define _GNU_SOURCE
#include "gam.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

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
     int   inited;
     char  name[64];
     struct gam_handler h;
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

static void nop_cb(int type, const void *data, int len) { (void)type; (void)data; (void)len; }

int gam_open(const char *name, gam_cb on_sync, gam_cb on_async)
{
     int r;

     if (g.inited)
          return 0;
     g.lib = dlopen("/basic/libgam_client.so", RTLD_NOW | RTLD_GLOBAL);
     if (!g.lib) {
          fprintf(stderr, "gam: dlopen: %s\n", dlerror());
          return -1;
     }
#define SYM(f, n) if (!(*(void **)&g.f = dlsym(g.lib, n))) { fprintf(stderr, "gam: missing %s\n", n); return -1; }
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
     snprintf(g.name, sizeof(g.name), "%s", name);
     g.h.on_sync = on_sync ? on_sync : nop_cb;
     g.h.on_async = on_async ? on_async : nop_cb;
     g.h.a = "deprecated";
     g.h.b = "-";
     g.h.c = 1;
     r = g.client_init(&g.h, g.name);
     if (r != 0) {
          fprintf(stderr, "gam: GAM_ClientInit('%s') = %d\n", g.name, r);
          return -1;
     }
     g.inited = 1;
     return 0;
}

void gam_close(void)
{
     if (g.inited) {
          g.client_uninit();
          g.inited = 0;
     }
}

int gam_send_cmd(const char *dst, const char *cmd, const char *para)
{
     struct gam_send_cmd c;
     memset(&c, 0, sizeof(c));
     c.src = g.name;
     c.dst = dst;
     c.cmd = cmd;
     c.para = para;
     return g.send_cmd(&c);
}

int gam_send_raw(const char *msg) { return g.send_raw(msg); }

/* the library writes names without a length: use a big buffer, then copy */
static int get_name(int (*fn)(char *), char *out, int size)
{
     static char buf[4096];
     int r;
     memset(buf, 0, sizeof(buf));
     r = fn(buf);
     snprintf(out, size, "%s", buf);
     return r;
}

int gam_default_process(char *out, int size) { return get_name(g.get_default, out, size); }
int gam_focus_process(char *out, int size)   { return get_name(g.get_focus, out, size); }
int gam_active_process(char *out, int size)  { return get_name(g.get_active, out, size); }

int gam_process_count(void)
{
     unsigned n = 0;
     g.load_list();
     g.list_count(&n);
     return (int)n;
}

int gam_process(int i, char *name, int size, int *state)
{
     static char buf[4096];
     int st[64];
     memset(buf, 0, sizeof(buf));
     memset(st, 0xff, sizeof(st));
     g.list_element((unsigned)i, buf);
     g.proc_state(buf, st);
     snprintf(name, size, "%s", buf);
     *state = st[0];
     return 0;
}
