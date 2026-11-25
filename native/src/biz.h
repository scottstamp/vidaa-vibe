#ifndef VIBE_BIZ_H
#define VIBE_BIZ_H
/*
 * The TV's "kilby" model service, through the vendor's C facade in
 * /usr/local/lib/libbizAgent.so, loaded with dlopen. Signatures and quirks:
 * docs/findings/06-native-route.md ("TV functions").
 */

#define BIZ_STR_MAX  8192   /* the facade strcpy()s with no length */
#define BIZ_VEC_MAX  256

int  biz_open(void);
void biz_close(void);

/* each returns the facade's own result code (see the findings) */
int biz_get_int(const char *key, int *out);
int biz_get_str(const char *key, char *out /* BIZ_STR_MAX */);
/* fills out[0..BIZ_VEC_MAX) (each BIZ_STR_MAX, zeroed first); 0 = ok */
int biz_get_vstr(const char *key, char **out);
int biz_set_int(const char *key, int v);
int biz_action(const char *key, int argc, const char **argv);

/* string vector helpers */
char **biz_vec_alloc(void);
void   biz_vec_free(char **v);

/* ---- typed helpers over known keys ---- */

#define BIZ_KEY_INPUT_LIST    "kilby.sl2.vstr.source.input.name"
#define BIZ_KEY_INPUT_CURRENT "kilby.sl2.str.source.input.current"
#define BIZ_KEY_VOLUME        "kilby.sl2.i32.sound.main.volume"
#define BIZ_KEY_MUTE          "kilby.sl2.i32.sound.main.mute"
#define BIZ_ACTION_SWITCH     "kilby.sl2.action.source.switch"
/* modeljs.system.SwitchOffTv([1]); note: no "sl2" (read in libcobalt) */
#define BIZ_ACTION_POWER_OFF  "kilby.action.system.switch.off.tv"
/* stock SDK: system.uiLoadFinish(1) -> modeljs.system.setGuiLoopFinished(1).
 * 1 with the stock UI up; a blank page never sets it. */
#define BIZ_KEY_GUI_LOOP_FINISHED "kilby.sl2.i32.system.gui.loop.finished"

#define BIZ_MAX_INPUTS 16

struct biz_input {
     char id[32];       /* "HDMI1" */
     char label[64];    /* user-visible name ("HDMI1", "ANT/CABLE") */
     int  signal;       /* 1 if the model reports a signal */
};

/* returns the number of inputs (0 on failure) */
int biz_inputs(struct biz_input *out, int max);
/* current input id into out (size >= 32); returns 0 on success */
int biz_current_input(char *out, int size);
/* what the stock UI does: action source.switch [now_ms, "9", id] */
int biz_switch_input(const char *id);

#endif
