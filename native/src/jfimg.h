#ifndef VIBE_JFIMG_H
#define VIBE_JFIMG_H
/*
 * Jellyfin images (posters, stills) for the panel: fetched on a thread of
 * their own, decoded (stb_image), scaled to the size asked for, kept in a
 * small in-memory cache. The UI thread asks every frame; nothing blocks it.
 */
#include <stdint.h>

#include "jf.h"

struct jfimg {
     int w, h;              /* as asked for */
     uint32_t *px;          /* ARGB, opaque, pitch = w */
};

/* the session to fetch with (server, token); a copy is kept. Call after
 * every connect, sign-in and sign-out. */
void jfimg_session(const struct jf_session *s);

/* the image of item `id` (kind "Primary", "Backdrop", "Thumb"; tag as
 * listed, may be ""), w x h pixels; NULL until it has arrived (it is asked
 * for then) or if it failed */
const struct jfimg *jfimg_get(const char *id, const char *kind, const char *tag, int w, int h);

/* 1 if an image arrived since the last call (time to redraw) */
int jfimg_changed(void);

#endif
