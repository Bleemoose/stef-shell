#ifndef STEF_PARSER_H
#define STEF_PARSER_H

#include <stddef.h>
#include <stdio.h>

#include "lexer.h"

/*
 * Parser: token stream -> pipeline AST.
 *
 * Grammar (small enough to hand-roll; no tables, no parser generator):
 *
 *   pipeline := command ('|' command)* ('&')? END
 *   command  := (WORD | redir)+
 *   redir    := ('<' | '>' | '>>' | '2>') WORD
 *
 * Semantics:
 *   - A command needs at least one WORD; "| foo", "foo |", and "&" alone are
 *     all syntax errors.
 *   - Redirections may appear anywhere inside a command, not just at the end
 *     (matches POSIX: `echo > out hi` is legal).
 *   - '&' may appear at most once, and only as the final token before END.
 *
 * Ownership:
 *   parse() copies every WORD's text (xstrdup) into the AST. The token_vec_t
 *   passed in is unmodified and remains the caller's responsibility to free.
 *   The resulting pipeline_t owns every string and every array it holds;
 *   pipeline_free walks the whole tree.
 */

typedef enum {
	R_IN,       /* <   */
	R_OUT,      /* >   */
	R_APPEND,   /* >>  */
	R_ERR,      /* 2>  */
} redir_kind_t;

typedef struct {
	redir_kind_t kind;
	char *target;           /* heap-owned filename */
} redir_t;

typedef struct {
	/* NULL-terminated, heap-owned. Ready to hand to execvp.
	 * argv[argc] == NULL; argv is NULL only when argc == 0. */
	char   **argv;
	size_t   argc;

	redir_t *redirs;
	size_t   n_redirs;
} command_t;

typedef struct {
	command_t *commands;
	size_t     size;
	int        background;   /* 1 iff the pipeline ended with '&' */
} pipeline_t;

void pipeline_init(pipeline_t *p);
void pipeline_free(pipeline_t *p);

/*
 * Parse `tokens` into `*out`. `*out` is always initialized on entry, whether
 * the result is success or failure.
 *
 * Returns 0 on success. For an empty input (tokens == [TOK_END]), *out is an
 * empty pipeline (size == 0) and the return is 0.
 *
 * On parse error: prints a diagnostic to stderr, returns -1, and leaves *out
 * in a clean (freed, zeroed) state. Caller still owns the token_vec_t.
 */
int parse(const token_vec_t *tokens, pipeline_t *out);

/* Human-readable name for a redir kind; never NULL. */
const char *redir_kind_name(redir_kind_t k);

/*
 * Debug dump of a pipeline to `f`. Format is multi-line and indented for
 * humans; not a parseable serialization. Useful as a stand-in for the
 * executor during development.
 */
void pipeline_print(FILE *f, const pipeline_t *p);

#endif /* STEF_PARSER_H */
