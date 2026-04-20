/*
 * stef-shell -- REPL driver.
 *
 * Read a line, lex it, parse it, execute it. Lex or parse errors print a
 * diagnostic and the loop continues at the next prompt; executor errors
 * (bad command, fork failure, etc.) are also non-fatal to the shell.
 * Only EOF exits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "executor.h"
#include "lexer.h"
#include "parser.h"

#define PROMPT        "stef-shell$ "
#define LINE_BUF_SIZE 4096

int main(void) {
	char buf[LINE_BUF_SIZE];

	for (;;) {
		fputs(PROMPT, stdout);
		fflush(stdout);

		if (!fgets(buf, sizeof buf, stdin)) {
			/* EOF or read error: print a newline so the next shell
			 * prompt starts on its own line, then exit. */
			fputc('\n', stdout);
			break;
		}

		/* fgets keeps the trailing '\n' -- strip it. */
		size_t n = strlen(buf);
		if (n > 0 && buf[n - 1] == '\n') {
			buf[--n] = '\0';
		}

		if (n == 0) {
			continue;
		}

		/* Lex. On failure, the lexer has already printed a diagnostic. */
		token_vec_t tokens;
		token_vec_init(&tokens);
		if (lex(buf, &tokens) != 0) {
			token_vec_free(&tokens);
			continue;
		}

		/* Parse. Same contract: diagnostic printed by parse() on failure. */
		pipeline_t pipe;
		if (parse(&tokens, &pipe) != 0) {
			token_vec_free(&tokens);
			continue;
		}

		/* Execute. execute() prints its own diagnostics on failure; we
		 * discard the exit status here (M4 adds a `status` builtin that
		 * exposes it). */
		(void)execute(&pipe);

		pipeline_free(&pipe);
		token_vec_free(&tokens);
	}

	return 0;
}
