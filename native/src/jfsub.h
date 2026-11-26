#ifndef VIBE_JFSUB_H
#define VIBE_JFSUB_H
/*
 * Text subtitles for the Jellyfin player: the server converts any text
 * track (embedded or a sidecar file) to SRT on request; this parses it into
 * timed cues, plain text (styling tags dropped), for vibe-ui to draw.
 */
#include <stddef.h>

struct jf_cue {
     int  start_ms, end_ms;
     char *text;              /* lines separated by '\n' */
};

struct jf_subs {
     struct jf_cue *c;
     int n;
};

/* SRT (also takes WebVTT-style "." in the times); 0 = ok (maybe no cues) */
int  jf_subs_parse(const char *data, size_t len, struct jf_subs *out);
/* the cue showing at ms, or NULL */
const struct jf_cue *jf_subs_at(const struct jf_subs *s, int ms);
void jf_subs_free(struct jf_subs *s);

#endif
