/*
 * stef-shell -- REPL driver.
 *
 * Read a line, lex it, parse it, and print the resulting pipeline AST.
 * Execution is not wired up yet (that's M3); until then the shell is an
 * interactive parser/lexer debugger. Lex or parse errors print a diagnostic
 * and the loop continues at the next prompt; only EOF exits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

		/* No executor yet show the parse tree so the user can see
		 * exactly what the shell understood.*/
		pipeline_print(stdout, &pipe);

		pipeline_free(&pipe);
		token_vec_free(&tokens);
	}

	return 0;
}
