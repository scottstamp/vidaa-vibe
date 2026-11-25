#ifndef VIBE_HTTP_H
#define VIBE_HTTP_H
/*
 * Minimal HTTP client over the TV's own libcurl, loaded with dlopen (no
 * curl headers or link stubs needed at build time). Blocking; one request
 * per call; safe to call from one worker thread at a time per handle (each
 * call makes its own easy handle).
 */
#include <stddef.h>

struct http_buf {
     char  *data;     /* NUL-terminated (extra byte not counted in len) */
     size_t len;
};

/* load libcurl; 0 on success. Called by http_request if needed. */
int  http_init(void);

/*
 * method: "GET", "POST", "DELETE"; body may be NULL (POST with an empty
 * body sends Content-Length: 0). headers: NULL-terminated list of
 * "Name: value" strings, may be NULL. On return *status is the HTTP status
 * (0 if no response). Returns 0 if a response was received (any status),
 * -1 on transport failure (message in err, if given, size >= 128).
 * timeout_ms 0 = 15 s.
 */
int  http_request(const char *method, const char *url, const char *const *headers,
                  const char *body, long timeout_ms,
                  struct http_buf *out, long *status, char *err);

void http_buf_free(struct http_buf *b);

/* percent-encode s into out (size n); returns out */
char *http_escape(const char *s, char *out, size_t n);

#endif
