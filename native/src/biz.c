/* The kilby model through libbizAgent.so. See biz.h. */
#define _GNU_SOURCE
#include "biz.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef void *(*get_instance_fn)(void);
typedef void  (*release_instance_fn)(void **);
typedef int   (*get_int_fn)(void *, const char *, int *);
typedef int   (*set_int_fn)(void *, const char *, const int *);  /* pointer: measured */
typedef int   (*get_str_fn)(void *, const char *, char *);
typedef int   (*get_vstr_fn)(void *, const char *, char **, int);
typedef int   (*on_action_fn)(void *, const char *, unsigned *, const char **, int);

static struct {
     void *lib, *h;
     get_instance_fn     get_instance;
     release_instance_fn release_instance;
     get_int_fn          get_int;
     set_int_fn          set_int;
     get_str_fn          get_str;
     get_vstr_fn         get_vstr;
     on_action_fn        on_action;
} biz;

int biz_open(void)
{
     if (biz.h)
          return 0;
     biz.lib = dlopen("libbizAgent.so", RTLD_NOW | RTLD_GLOBAL);
     if (!biz.lib) {
          fprintf(stderr, "biz: dlopen: %s\n", dlerror());
          return -1;
     }
#define SYM(field, name) \
     if (!(*(void **)&biz.field = dlsym(biz.lib, name))) { \
          fprintf(stderr, "biz: missing %s\n", name); return -1; }
     SYM(get_instance, "getInstance");
     SYM(release_instance, "releaseInstance");
     SYM(get_int, "getIntValue");
     SYM(set_int, "setIntValue");
     SYM(get_str, "getStringValue");
     SYM(get_vstr, "getVectorStringValue");
     SYM(on_action, "onAction");
#undef SYM
     biz.h = biz.get_instance();
     if (!biz.h) {
          fprintf(stderr, "biz: getInstance returned NULL\n");
          return -1;
     }
     return 0;
}

void biz_close(void)
{
     if (biz.h)
          biz.release_instance(&biz.h);
}

int biz_get_int(const char *key, int *out)        { return biz.get_int(biz.h, key, out); }
int biz_set_int(const char *key, int v)           { return biz.set_int(biz.h, key, &v); }

int biz_get_str(const char *key, char *out)
{
     memset(out, 0, BIZ_STR_MAX);
     return biz.get_str(biz.h, key, out);
}

int biz_get_vstr(const char *key, char **out)
{
     for (int i = 0; i < BIZ_VEC_MAX; i++)
          memset(out[i], 0, BIZ_STR_MAX);
     return biz.get_vstr(biz.h, key, out, BIZ_VEC_MAX);
}

int biz_action(const char *key, int argc, const char **argv)
{
     unsigned id = 0;
     return biz.on_action(biz.h, key, &id, argv, argc);
}

char **biz_vec_alloc(void)
{
     char **v = calloc(BIZ_VEC_MAX, sizeof(char *));
     if (!v)
          return NULL;
     for (int i = 0; i < BIZ_VEC_MAX; i++)
          if (!(v[i] = calloc(1, BIZ_STR_MAX))) {
               biz_vec_free(v);
               return NULL;
          }
     return v;
}

void biz_vec_free(char **v)
{
     if (!v)
          return;
     for (int i = 0; i < BIZ_VEC_MAX; i++)
          free(v[i]);
     free(v);
}

int biz_inputs(struct biz_input *out, int max)
{
     static char **v;
     int n = 0;

     if (!v && !(v = biz_vec_alloc()))
          return 0;
     if (biz_get_vstr(BIZ_KEY_INPUT_LIST, v) != 0)
          return 0;
     /* groups of four: id, signal ("2" = present), display name, "Original" */
     for (int i = 0; i + 3 < BIZ_VEC_MAX && v[i][0] && n < max; i += 4) {
          snprintf(out[n].id, sizeof(out[n].id), "%s", v[i]);
          snprintf(out[n].label, sizeof(out[n].label), "%s", v[i + 2][0] ? v[i + 2] : v[i]);
          out[n].signal = atoi(v[i + 1]) == 2;
          n++;
     }
     return n;
}

int biz_current_input(char *out, int size)
{
     static char buf[BIZ_STR_MAX];
     biz_get_str(BIZ_KEY_INPUT_CURRENT, buf);
     if (!buf[0])
          return -1;
     snprintf(out, size, "%s", buf);
     return 0;
}

int biz_switch_input(const char *id)
{
     char ts[32];
     struct timeval tv;
     gettimeofday(&tv, NULL);
     snprintf(ts, sizeof(ts), "%lld", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
     const char *a[3] = { ts, "9", id };   /* 9 = SOURCE_SWITCH_REASON_SWITCH_SOURCE */
     return biz_action(BIZ_ACTION_SWITCH, 3, a);
}

int biz_set_picture_mode(int mode)
{
     char m[16];
     snprintf(m, sizeof(m), "%d", mode);
     const char *a[3] = { m, "1", "1" };
     return biz_action(BIZ_ACTION_PICTURE_MODE, 3, a);
}

int biz_rename_input(const char *id, const char *name)
{
     const char *a[2] = { id, name };
     return biz_action(BIZ_ACTION_RENAME, 2, a);
}

int biz_switch_off_tv(int reason)
{
     char r[16];
     snprintf(r, sizeof(r), "%d", reason);
     const char *a[1] = { r };
     return biz_action(BIZ_ACTION_POWER_OFF, 1, a);
}

int biz_reset_picture(void)
{
     const char *a[1] = { "1" };
     return biz_action(BIZ_ACTION_PICTURE_RESET, 1, a);
}
