#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("stef-shell: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

void *xmalloc(size_t n) {
	void *p = malloc(n);
	if (!p) die("out of memory (malloc %zu)", n);
	return p;
}

void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n);
	if (!q) die("out of memory (realloc %zu)", n);
	return q;
}

char *xstrdup(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

/* ---- str_buf ------------------------------------------------------------ */

#define STR_BUF_INIT_CAP 16

void str_buf_init(str_buf_t *b) {
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

/* Ensure the buffer can hold `need` bytes total (including NUL). */
static void str_buf_reserve(str_buf_t *b, size_t need) {
	if (need <= b->cap) {
		return;
	}
	size_t c;
	if (b->cap == 0) {
		c = STR_BUF_INIT_CAP;
	} else {
		c = b->cap;
	}
	while (c < need) {
		c *= 2;
	}
	b->data = xrealloc(b->data, c);
	b->cap = c;
}

void str_buf_push(str_buf_t *b, char c) {
	/* Need room for the new char plus a terminating NUL. */
	str_buf_reserve(b, b->len + 2);
	b->data[b->len++] = c;
	b->data[b->len] = '\0';
}

char *str_buf_take(str_buf_t *b) {
	/* Empty string is still a valid result -- allocate if needed. */
	if (!b->data) {
		b->data = xmalloc(1);
		b->data[0] = '\0';
		b->cap = 1;
	}
	char *out = b->data;
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
	return out;
}

void str_buf_free(str_buf_t *b) {
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}
