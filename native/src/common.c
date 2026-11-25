/* Shared helpers for the native/ binaries. */
#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double vibe_now_s(void)
{
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* spec: UID:GID[:G1,G2,...] */
int vibe_drop_privileges(const char *spec)
{
     unsigned long uid, gid;
     gid_t groups[64];
     int ngroups = 0;
     char *end;
     const char *p = spec;

     uid = strtoul(p, &end, 10);
     if (end == p || *end != ':') goto bad;
     p = end + 1;
     gid = strtoul(p, &end, 10);
     if (end == p || (*end != ':' && *end != 0)) goto bad;
     if (*end == ':') {
          p = end + 1;
          while (*p && ngroups < 64) {
               groups[ngroups++] = (gid_t)strtoul(p, &end, 10);
               if (end == p) goto bad;
               if (*end && *end != ',') goto bad;
               p = (*end == ',') ? end + 1 : end;
          }
     }
     if (getuid() != 0) {
          fprintf(stderr, "vibe: --user needs root to start with\n");
          return -1;
     }
     if (setgroups(ngroups, groups) || setgid((gid_t)gid) || setuid((uid_t)uid)) {
          fprintf(stderr, "vibe: dropping to %lu:%lu failed: %s\n", uid, gid, strerror(errno));
          return -1;
     }
     if (setuid(0) == 0) {
          fprintf(stderr, "vibe: could regain root after drop, refusing to go on\n");
          return -1;
     }
     return 0;
bad:
     fprintf(stderr, "vibe: bad --user '%s' (want UID:GID[:G1,G2,...])\n", spec);
     return -1;
}
