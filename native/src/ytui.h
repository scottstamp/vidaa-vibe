#ifndef VIBE_YTUI_H
#define VIBE_YTUI_H
/*
 * The YouTube panel inside vibe-ui: Home (continue watching, the followed
 * channels' new videos), Search, History; a channel's videos. OK on a
 * video plays it at once (resuming); its details and actions (channel,
 * follow, from the start, description) are in the player's overlay.
 * Playback runs in the vibe-play child, with SponsorBlock segments skipped
 * (sponsor, self-promotion, interaction reminders, non-music) and shown on
 * the progress bar.
 *
 * Everything that has to keep up with YouTube runs in the helper
 * (helper/ythelper.py, on a home machine; yt.h). Same threading and host
 * as the Jellyfin panel (jfui.h): one worker thread, started on first open.
 *
 * Switch: /var/local/vibe/yt.off hides the panel's entry (checked at start).
 */
#include "jfui.h"

int  ytui_available(void);
/* at vibe-ui's start: the threads, so a linked phone can cast while the
 * panel is closed */
void ytui_start(void);
/* a phone asked to play while the panel was closed: open it (once) */
int  ytui_wants_open(void);
void ytui_open(const struct jfui_host *h, double now);
void ytui_close(double now);
int  ytui_playing(void);
void ytui_draw(struct canvas *c, double now);
int  ytui_key(int key, int repeat, double now);
int  ytui_tick(double now);

#endif
