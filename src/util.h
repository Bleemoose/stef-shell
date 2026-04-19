#ifndef STEF_UTIL_H
#define STEF_UTIL_H

#include <stddef.h>

/*
 * Abort-on-OOM allocators.
 *
 * In a shell, there is no sensible recovery from malloc failure: we can't
 * parse the next command without memory. So we crash cleanly with a
 * diagnostic instead of propagating NULL through every call site.
 */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Print "stef-shell: <fmt>\n" to stderr, then exit(1). Never returns. */
_Noreturn void die(const char *fmt, ...);

/*
 * Growable NUL-terminated character buffer.
 *
 * Used by the lexer to accumulate a word one char at a time: quoted segments,
 * escapes, and unquoted chars all call str_buf_push, and the final text is
 * handed off with str_buf_take (which transfers ownership and empties the
 * buffer so it can be reused for the next word).
 */
typedef struct {
	char *data;    /* NUL-terminated; NULL when empty & never pushed */
	size_t len;    /* chars stored, excluding terminator */
	size_t cap;    /* bytes allocated */
} str_buf_t;

void  str_buf_init(str_buf_t *b);
void  str_buf_push(str_buf_t *b, char c);
char *str_buf_take(str_buf_t *b);  /* caller owns result; buf becomes empty */
void  str_buf_free(str_buf_t *b);

#endif /* STEF_UTIL_H */
