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
/* modeljs.system.SwitchOffTv([reason]); note: no "sl2" (read in libcobalt).
 * Only with gui.loop.finished = 1: before that it left the TV in a standby
 * POWER did not wake (docs/findings/06). */
#define BIZ_ACTION_POWER_OFF  "kilby.action.system.switch.off.tv"
#define BIZ_POWER_OFF_SLEEP_TIMER 4      /* the stock UI's reason at sleep-timer end */
/* 1 screen on, 0 off (standby) */
#define BIZ_KEY_SCREEN_STATE  "kilby.sl2.i32.system.enum.screen.state"
/* stock SDK: system.uiLoadFinish(1) -> modeljs.system.setGuiLoopFinished(1).
 * 1 with the stock UI up; a blank page never sets it. */
#define BIZ_KEY_GUI_LOOP_FINISHED "kilby.sl2.i32.system.gui.loop.finished"

/* picture: keys and actions read in libcobalt (modeljs_atsc::Video) */
#define BIZ_KEY_PICTURE_MODE  "kilby.sl2.i32.video.enum.picture.adjustment"
#define BIZ_ACTION_PICTURE_MODE "kilby.sl2.action.video.set.picture.adjustment"
#define BIZ_KEY_BACKLIGHT     "kilby.sl2.i32.video.backlight"
#define BIZ_KEY_BRIGHTNESS    "kilby.sl2.i32.video.brightness"
#define BIZ_KEY_CONTRAST      "kilby.sl2.i32.video.contrast"
#define BIZ_KEY_COLOUR        "kilby.sl2.i32.video.colour.intensity"
#define BIZ_KEY_SHARPNESS     "kilby.sl2.i32.video.sharpness"
/* sound (modeljs_atsc::Sound, read in libcobalt) */
#define BIZ_KEY_SPEAKER       "kilby.sl2.i32.sound.speaker.arc.mode"   /* 1 TV, 2 ARC, 3 optical */
#define BIZ_KEY_EARC          "kilby.sl2.i32.sound.earc.switch.value"  /* 0/1 */
#define BIZ_KEY_SOUND_MODE    "kilby.sl2.i32.sound.enum.adjust"        /* 0 Standard .. 5 Sport */
#define BIZ_KEY_ARC_DEVICE    "kilby.sl2.i32.sound.arcdev.exist"
/* game (modeljs_atsc::Video) */
#define BIZ_KEY_GAME_MODE     "kilby.sl2.i32.video.game.mode.enable"   /* 0/1 */
#define BIZ_KEY_IGR           "kilby.sl2.i32.video.enum.instant.game.response" /* 0 off 1 on 2 auto */
#define BIZ_KEY_IGR_SUPPORTED "kilby.sl2.i32.video.instant.game.response.supported"
/* sleep timer, minutes (modeljs_atsc::TimerFunction::SetHotelmodeSleepTimer) */
#define BIZ_KEY_SLEEP_TIMER   "kilby.sl2.i32.timerfunctions.hotelmode.sleep.timer"
/* modeljs.source.InputRename([sourceId, name]) */
#define BIZ_ACTION_RENAME     "kilby.sl2.action.source.input.rename"

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
/* stock: modeljs.video.SetPictureAdjustment([mode, 1, 1]); modes 0..6 =
 * Dynamic, Standard, Cinema Night, Cinema Day, Game, Sport, Calibrated */
int biz_set_picture_mode(int mode);
/* stock: source.InputRename([sourceId, rename]) */
int biz_rename_input(const char *id, const char *name);
/* stock: system.SwitchOffTv([reason]) */
int biz_switch_off_tv(int reason);

#endif
