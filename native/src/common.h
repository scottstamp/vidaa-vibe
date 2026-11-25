#ifndef VIBE_COMMON_H
#define VIBE_COMMON_H

/* monotonic seconds */
double vibe_now_s(void);

/*
 * Drop from root to UID:GID[:G1,G2,...] (setgroups, setgid, setuid) and
 * verify root cannot be regained. Returns 0 or -1 (message on stderr).
 */
int vibe_drop_privileges(const char *spec);

/*
 * The vendor DirectFB copies its own sizeof() into structs we pass; its data
 * structs were not measured, so give every library-filled buffer slack.
 */
#define PADDED(type, name) union { type v; char pad_[sizeof(type) + 512]; } name

#endif
