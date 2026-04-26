/* REPL: read a line, lex, parse, execute. Only EOF exits; every other
 * error is reported and the prompt returns. */

#include <signal.h>
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

	/* Ignore SIGINT and SIGQUIT in the shell itself so Ctrl-C / Ctrl-\ at
	 * the prompt or during a running pipeline hits the children, not us.
	 * Each forked child resets both to SIG_DFL before exec / before calling
	 * a builtin (POSIX preserves SIG_IGN across execve, so the reset is
	 * mandatory or piped `sleep` would refuse to die). */
	signal(SIGINT,  SIG_IGN);
	signal(SIGQUIT, SIG_IGN);

	for (;;) {
		fputs(PROMPT, stdout);
		fflush(stdout);

		if (!fgets(buf, sizeof buf, stdin)) {
			/* EOF or read error: print a newline so the next shell
			 * prompt starts on its own line, then exit. */
			fputc('\n', stdout);
			break;
		}

		/* fgets keeps the trailing newline; strip it. */
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
