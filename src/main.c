/*
 * stef-shell -- M0 REPL skeleton
 *
 * Read a line, echo it back. Exit on EOF (Ctrl-D on an empty prompt).
 * No forking, no execution, no parsing yet -- this milestone exists only to
 * prove the toolchain works and to establish the main loop's shape.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
		if (n > 0 && buf[n - 1] == '\n'){
			buf[--n] = '\0';
		} 

		if (n == 0){
			continue;
		} 

		printf("you said: %s\n", buf);
	}

	return 0;
}
