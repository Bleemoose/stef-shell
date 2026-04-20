/*
 * Executor tests. Each test lexes + parses an input line, hands it to
 * execute(), and checks the returned status code. The child half runs real
 * programs (/bin/true, /bin/false, etc.) -- we rely on them being present,
 * which they are on every POSIX system including WSL.
 *
 * ASan note: the child calls _exit() on exec-failure paths, which bypasses
 * ASan's exit-time leak check. The child never allocates between fork and
 * exec, so there's nothing to leak on its side. The parent's allocations
 * (the pipeline_t built by parse) are freed after every test, which ASan
 * does verify.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/executor.h"
#include "../src/lexer.h"
#include "../src/parser.h"

/* Lex + parse the input, run execute(), free the pipeline, return status.
 * Aborts the test on any lex/parse failure. */
static int run(const char *input) {
	token_vec_t v;
	token_vec_init(&v);
	if (lex(input, &v) != 0) {
		fprintf(stderr, "FAIL: %s -- lex failed\n", input);
		abort();
	}
	pipeline_t p;
	if (parse(&v, &p) != 0) {
		fprintf(stderr, "FAIL: %s -- parse failed\n", input);
		abort();
	}
	token_vec_free(&v);

	int status = execute(&p);

	pipeline_free(&p);
	return status;
}

/* Same as run(), but silences stderr for the duration of execute(). Used
 * for tests where the executor (or the child) prints an expected diagnostic
 * -- we don't want that littering a successful run. */
static int run_silent(const char *input) {
	token_vec_t v;
	token_vec_init(&v);
	assert(lex(input, &v) == 0);
	pipeline_t p;
	assert(parse(&v, &p) == 0);
	token_vec_free(&v);

	fflush(stderr);
	int saved = dup(fileno(stderr));
	FILE *devnull = freopen("/dev/null", "w", stderr);
	(void)devnull;

	int status = execute(&p);

	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);

	pipeline_free(&p);
	return status;
}

/* --- happy paths -------------------------------------------------------- */

static void true_returns_0(void) {
	assert(run("/bin/true") == 0);
}

static void false_returns_1(void) {
	assert(run("/bin/false") == 1);
}

static void path_lookup_succeeds(void) {
	/* No absolute path -- exercises execvp's PATH walk. */
	assert(run("true") == 0);
}

static void args_are_passed(void) {
	/* /bin/false ignores its args; this just proves argv > argv[0] survives. */
	assert(run("false one two three") == 1);
}

/* --- error paths -------------------------------------------------------- */

static void command_not_found_returns_127(void) {
	/* The child prints "stef-shell: ...: No such file or directory"; silence it. */
	assert(run_silent("nonexistent-command-xyz-42") == 127);
}

static void absolute_path_not_found_returns_127(void) {
	assert(run_silent("/bin/nonexistent-xyz-42") == 127);
}

/* --- M3 non-scope: explicit NYI rejections ------------------------------ */

static void empty_pipeline_returns_0(void) {
	assert(run("") == 0);
}

static void whitespace_only_returns_0(void) {
	assert(run("   \t  ") == 0);
}

static void pipelines_rejected(void) {
	assert(run_silent("ls | wc") == 1);
}

static void background_rejected(void) {
	assert(run_silent("sleep 0 &") == 1);
}

static void redirects_rejected(void) {
	assert(run_silent("echo hi > /tmp/stef_shell_never_created") == 1);
}

/* --- runner ------------------------------------------------------------- */

#define RUN(name) do { printf("  %s\n", #name); name(); } while (0)

int main(void) {
	RUN(true_returns_0);
	RUN(false_returns_1);
	RUN(path_lookup_succeeds);
	RUN(args_are_passed);
	RUN(command_not_found_returns_127);
	RUN(absolute_path_not_found_returns_127);
	RUN(empty_pipeline_returns_0);
	RUN(whitespace_only_returns_0);
	RUN(pipelines_rejected);
	RUN(background_rejected);
	RUN(redirects_rejected);

	printf("all tests passed\n");
	return 0;
}
