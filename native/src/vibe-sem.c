/*
 * vibe-sem: post a named POSIX semaphore (glibc: /dev/shm/sem.NAME).
 *
 *   vibe-sem post NAME
 *
 * Used at boot to post GUICLOSE, the semaphore Hisense's boot_animation
 * polls to stop the VIDAA animation (systemserver's
 * ApplicationManager::stopAnimation and tvmain post the same one, about
 * 2 s later). Never creates a semaphore: if it does not exist, nothing
 * happens (exit 1). docs/findings/06-native-route.md, "Boot splashes".
 */
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
     if (argc != 3 || strcmp(argv[1], "post") != 0) {
          fprintf(stderr, "usage: vibe-sem post NAME\n");
          return 2;
     }
     sem_t *s = sem_open(argv[2], 0);
     if (s == SEM_FAILED) {
          fprintf(stderr, "vibe-sem: %s: %s\n", argv[2], strerror(errno));
          return 1;
     }
     int r = sem_post(s);
     sem_close(s);
     if (r != 0) {
          fprintf(stderr, "vibe-sem: post %s: %s\n", argv[2], strerror(errno));
          return 1;
     }
     return 0;
}
