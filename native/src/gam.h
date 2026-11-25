#ifndef VIBE_GAM_H
#define VIBE_GAM_H
/*
 * app_man as a client: MediaTek's GAM, /basic/libgam_client.so via dlopen.
 * Prototypes and struct layouts: docs/findings/06-native-route.md
 * ("app_man as a client: GAM"), taken from the library's
 * __PRETTY_FUNCTION__ strings and the vendor sample UI dockui.
 */

/* callback: (message type, payload, payload length); runs on a binder thread */
typedef void (*gam_cb)(int type, const void *data, int len);

/* load the library and GAM_ClientInit(name); 0 = ok */
int  gam_open(const char *name, gam_cb on_sync, gam_cb on_async);
void gam_close(void);

/* GAM_SendCmd({src = our name, dst, 0, cmd, para}) */
int gam_send_cmd(const char *dst, const char *cmd, const char *para);
int gam_send_raw(const char *msg);

int gam_default_process(char *out, int size);
int gam_focus_process(char *out, int size);
int gam_active_process(char *out, int size);

/* process list: count, then name and state of element i (state -1 if unknown) */
int gam_process_count(void);
int gam_process(int i, char *name, int size, int *state);

#endif
