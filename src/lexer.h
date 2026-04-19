#ifndef STEF_LEXER_H
#define STEF_LEXER_H

#include <stddef.h>

typedef enum {
	TOK_WORD,       /* quoted or unquoted run of characters */
	TOK_PIPE,       /* |   */
	TOK_LT,         /* <   */
	TOK_GT,         /* >   */
	TOK_GT_GT,      /* >>  */
	TOK_AMP,        /* &   */
	TOK_STDERR_GT,  /* 2>  (only at a word boundary)                   */
	TOK_END,        /* end of input; always the last token in a vec    */
} tok_kind_t;

typedef struct {
	tok_kind_t kind;
	/* Heap-allocated, NUL-terminated, owned by this token.
	 * Non-NULL only for TOK_WORD; NULL for all other kinds. */
	char *text;
} token_t;

typedef struct {
	token_t *data;
	size_t len;
	size_t cap;
} token_vec_t;

void token_vec_init(token_vec_t *v);
void token_vec_free(token_vec_t *v);

/*
 * Tokenize `line` (NUL-terminated) into `*out`.
 *
 * On success: returns 0 and *out ends with a TOK_END token.
 * On lex error (unterminated quote, trailing backslash): prints a diagnostic
 * to stderr, returns -1, and leaves *out containing whatever tokens were
 * emitted before the error. The caller must call token_vec_free in both
 * cases.
 */
int lex(const char *line, token_vec_t *out);

/* Human-readable name for a token kind; never NULL. */
const char *tok_kind_name(tok_kind_t k);

#endif /* STEF_LEXER_H */
