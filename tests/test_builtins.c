/*
 * Builtins tests. Each test lexes + parses an input line and hands the
 * first command's argv/argc to builtin_try directly. That lets us assert
 * both on the returned status and on the in-process side effects (cwd
 * moved, environ mutated).
 *
 * Process-state hygiene: main() snapshots getcwd() and a couple of env
 * vars we know each test touches, and restores them before returning.
 * That way repeat runs stay deterministic and this test leaves no visible
 * trail in whatever process launched it (make, CI, the user's shell).
 *
 * `exit` is deliberately not tested here -- the success path calls exit()
 * for real, which would kill the test runner. We trust the numeric-parse
 * branch by inspection and cover interactive behavior in the manual demo.
 */

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/builtins.h"
#include "../src/lexer.h"
#include "../src/parser.h"

/* Build a pipeline from `input`, return it via *out; abort on any parse
 * error (these inputs are hard-coded, so failure is a test bug). */
static void build(const char *input, pipeline_t *out) {
	token_vec_t v;
	token_vec_init(&v);
	if (lex(input, &v) != 0) {
		fprintf(stderr, "FAIL: lex(%s)\n", input);
		abort();
	}
	if (parse(&v, out) != 0) {
		fprintf(stderr, "FAIL: parse(%s)\n", input);
		abort();
	}
	token_vec_free(&v);
}

/* Run the first command of `input` as a builtin, silencing stdout +
 * stderr (we only care about the returned status and observable side
 * effects, not the printed output). Asserts it *was* handled (returns 1)
 * and returns the builtin's exit status. */
static int run_builtin_silent(const char *input) {
	pipeline_t p;
	build(input, &p);
	assert(p.size == 1);

	fflush(stdout);
	fflush(stderr);
	int sout = dup(fileno(stdout));
	int serr = dup(fileno(stderr));
	FILE *n1 = freopen("/dev/null", "w", stdout);
	FILE *n2 = freopen("/dev/null", "w", stderr);
	(void)n1;
	(void)n2;

	int status = -1;
	int handled = builtin_try(&p.commands[0], &status);

	fflush(stdout);
	fflush(stderr);
	dup2(sout, fileno(stdout));
	dup2(serr, fileno(stderr));
	close(sout);
	close(serr);

	assert(handled == 1);
	pipeline_free(&p);
	return status;
}

/* --- dispatch ----------------------------------------------------------- */

static void unknown_is_not_a_builtin(void) {
	pipeline_t p;
	build("definitely-not-a-builtin", &p);

	int status = 0xDEAD;
	int handled = builtin_try(&p.commands[0], &status);
	assert(handled == 0);
	assert(status == 0xDEAD);   /* untouched on miss */

	pipeline_free(&p);
}

static void empty_argv_is_not_handled(void) {
	/* argc == 0 path. Construct manually; no parser input gets here. */
	command_t c = { .argv = NULL, .argc = 0, .redirs = NULL, .n_redirs = 0 };
	int status = 0xBEEF;
	int handled = builtin_try(&c, &status);
	assert(handled == 0);
	assert(status == 0xBEEF);
}

/* --- pwd / help / status ----------------------------------------------- */

static void pwd_returns_0(void) {
	assert(run_builtin_silent("pwd") == 0);
}

static void help_returns_0(void) {
	assert(run_builtin_silent("help") == 0);
}

static void status_returns_0(void) {
	assert(run_builtin_silent("status") == 0);
}

/* --- cd ---------------------------------------------------------------- */

static void cd_absolute_path_works(void) {
	assert(run_builtin_silent("cd /tmp") == 0);
	char buf[PATH_MAX];
	assert(getcwd(buf, sizeof buf));
	/* On macOS /tmp resolves to /private/tmp, but on Linux/WSL it's /tmp. */
	assert(strcmp(buf, "/tmp") == 0 || strcmp(buf, "/private/tmp") == 0);
}

static void cd_missing_dir_fails(void) {
	assert(run_builtin_silent("cd /no/such/path/xyz_42") == 1);
}

static void cd_dash_toggles(void) {
	/* Set up: remember where we are, go to /tmp, then `cd -` back. */
	char start[PATH_MAX];
	assert(getcwd(start, sizeof start));

	assert(run_builtin_silent("cd /tmp") == 0);
	assert(run_builtin_silent("cd -") == 0);

	char back[PATH_MAX];
	assert(getcwd(back, sizeof back));
	assert(strcmp(back, start) == 0);
}

static void cd_tilde_expands_to_home(void) {
	const char *home = getenv("HOME");
	if (!home) {
		/* HOME really should be set in any test environment, but if it
		 * isn't, skip rather than fail this test. */
		return;
	}
	assert(run_builtin_silent("cd ~") == 0);
	char buf[PATH_MAX];
	assert(getcwd(buf, sizeof buf));
	assert(strcmp(buf, home) == 0);
}

static void cd_too_many_args_fails(void) {
	assert(run_builtin_silent("cd a b") == 1);
}

/* --- export / unset / env --------------------------------------------- */

static void export_sets_variable(void) {
	unsetenv("STEF_TEST_VAR");
	assert(run_builtin_silent("export STEF_TEST_VAR=hello") == 0);
	const char *v = getenv("STEF_TEST_VAR");
	assert(v != NULL);
	assert(strcmp(v, "hello") == 0);
	unsetenv("STEF_TEST_VAR");
}

static void export_no_args_lists(void) {
	assert(run_builtin_silent("export") == 0);
}

static void unset_removes_variable(void) {
	setenv("STEF_TEST_VAR", "gone-soon", 1);
	assert(getenv("STEF_TEST_VAR") != NULL);

	assert(run_builtin_silent("unset STEF_TEST_VAR") == 0);
	assert(getenv("STEF_TEST_VAR") == NULL);
}

static void env_returns_0(void) {
	assert(run_builtin_silent("env") == 0);
}

/* --- runner ------------------------------------------------------------ */

#define RUN(name) do { printf("  %s\n", #name); name(); } while (0)

int main(void) {
	/* Snapshot process state we may mutate, restore at the end. */
	char start_cwd[PATH_MAX];
	assert(getcwd(start_cwd, sizeof start_cwd));
	char *saved_oldpwd = NULL;
	const char *e = getenv("OLDPWD");
	if (e) saved_oldpwd = strdup(e);

	RUN(unknown_is_not_a_builtin);
	RUN(empty_argv_is_not_handled);

	RUN(pwd_returns_0);
	RUN(help_returns_0);
	RUN(status_returns_0);

	RUN(cd_absolute_path_works);
	RUN(cd_missing_dir_fails);
	RUN(cd_dash_toggles);
	RUN(cd_tilde_expands_to_home);
	RUN(cd_too_many_args_fails);

	RUN(export_sets_variable);
	RUN(export_no_args_lists);
	RUN(unset_removes_variable);
	RUN(env_returns_0);

	/* Restore. */
	if (chdir(start_cwd) != 0) {
		perror("test_builtins: chdir back");
	}
	if (saved_oldpwd) {
		setenv("OLDPWD", saved_oldpwd, 1);
		free(saved_oldpwd);
	} else {
		unsetenv("OLDPWD");
	}
	unsetenv("STEF_TEST_VAR");

	printf("all tests passed\n");
	return 0;
}
