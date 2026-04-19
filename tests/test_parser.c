/*
 * Parser tests. Plain assertions, no framework.
 *
 * Each test lexes an input, parses it, and asserts directly on the resulting
 * pipeline_t. We don't go through macros like the lexer tests do (KS/TS) --
 * the AST has nested arrays, so inline expected values would be ugly.
 * Per-field asserts read fine.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/lexer.h"
#include "../src/parser.h"

/* --- harness ------------------------------------------------------------ */

/* Lex + parse. On any failure, abort with a diagnostic. */
static void do_parse_ok(const char *input, pipeline_t *out) {
	token_vec_t v;
	token_vec_init(&v);
	if (lex(input, &v) != 0) {
		fprintf(stderr, "FAIL: %s -- lex failed\n", input);
		abort();
	}
	if (parse(&v, out) != 0) {
		fprintf(stderr, "FAIL: %s -- parse failed\n", input);
		abort();
	}
	token_vec_free(&v);
}

/* Lex + parse expecting a parse error. Silences stderr during the call so a
 * successful negative test has clean output (same trick as test_lexer.c). */
static void expect_parse_err(const char *input) {
	token_vec_t v;
	token_vec_init(&v);
	if (lex(input, &v) != 0) {
		fprintf(stderr, "FAIL: %s -- lex failed (expected parse error)\n", input);
		abort();
	}

	fflush(stderr);
	int saved = dup(fileno(stderr));
	FILE *devnull = freopen("/dev/null", "w", stderr);
	(void)devnull;

	pipeline_t p;
	int r = parse(&v, &p);

	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);

	if (r == 0) {
		fprintf(stderr, "FAIL: %s -- parse returned 0 (expected error)\n", input);
		pipeline_free(&p);
		token_vec_free(&v);
		abort();
	}
	/* On error, parse() leaves *out cleanly freed -- nothing to free here. */
	token_vec_free(&v);
}

/* Check that cmd.argv matches a NULL-terminated expected list. */
static void assert_argv(const command_t *cmd, const char *const *expected,
                        const char *input, size_t cmd_idx) {
	size_t i = 0;
	while (expected[i] != NULL) {
		if (i >= cmd->argc) {
			fprintf(stderr, "FAIL: %s -- cmd %zu argv too short at %zu\n",
			        input, cmd_idx, i);
			abort();
		}
		if (strcmp(cmd->argv[i], expected[i]) != 0) {
			fprintf(stderr, "FAIL: %s -- cmd %zu argv[%zu] = \"%s\" (expected \"%s\")\n",
			        input, cmd_idx, i, cmd->argv[i], expected[i]);
			abort();
		}
		i++;
	}
	if (cmd->argc != i) {
		fprintf(stderr, "FAIL: %s -- cmd %zu argc = %zu (expected %zu)\n",
		        input, cmd_idx, cmd->argc, i);
		abort();
	}
	assert(cmd->argv[cmd->argc] == NULL);
}

/* --- test cases --------------------------------------------------------- */

static void empty_input(void) {
	pipeline_t p;
	do_parse_ok("", &p);
	assert(p.n == 0);
	assert(p.background == 0);
	assert(p.commands == NULL);
	pipeline_free(&p);
}

static void whitespace_only(void) {
	pipeline_t p;
	do_parse_ok("   \t  ", &p);
	assert(p.n == 0);
	pipeline_free(&p);
}

static void single_command(void) {
	pipeline_t p;
	do_parse_ok("ls", &p);
	assert(p.n == 1);
	assert(p.background == 0);
	assert_argv(&p.commands[0], (const char *[]){"ls", NULL}, "ls", 0);
	assert(p.commands[0].n_redirs == 0);
	pipeline_free(&p);
}

static void command_with_args(void) {
	pipeline_t p;
	do_parse_ok("echo hi there", &p);
	assert(p.n == 1);
	assert_argv(&p.commands[0],
	            (const char *[]){"echo", "hi", "there", NULL},
	            "echo hi there", 0);
	pipeline_free(&p);
}

static void two_stage_pipe(void) {
	pipeline_t p;
	do_parse_ok("ls | wc", &p);
	assert(p.n == 2);
	assert(p.background == 0);
	assert_argv(&p.commands[0], (const char *[]){"ls", NULL}, "ls|wc", 0);
	assert_argv(&p.commands[1], (const char *[]){"wc", NULL}, "ls|wc", 1);
	pipeline_free(&p);
}

static void three_stage_pipe(void) {
	pipeline_t p;
	do_parse_ok("ls -la | grep foo | wc -l", &p);
	assert(p.n == 3);
	assert_argv(&p.commands[0], (const char *[]){"ls", "-la", NULL}, "3-stage", 0);
	assert_argv(&p.commands[1], (const char *[]){"grep", "foo", NULL}, "3-stage", 1);
	assert_argv(&p.commands[2], (const char *[]){"wc", "-l", NULL}, "3-stage", 2);
	pipeline_free(&p);
}

static void background(void) {
	pipeline_t p;
	do_parse_ok("sleep 10 &", &p);
	assert(p.n == 1);
	assert(p.background == 1);
	assert_argv(&p.commands[0], (const char *[]){"sleep", "10", NULL}, "sleep 10 &", 0);
	pipeline_free(&p);
}

static void background_pipeline(void) {
	pipeline_t p;
	do_parse_ok("yes | head &", &p);
	assert(p.n == 2);
	assert(p.background == 1);
	pipeline_free(&p);
}

static void redirect_out(void) {
	pipeline_t p;
	do_parse_ok("echo hi > out.txt", &p);
	assert(p.n == 1);
	assert_argv(&p.commands[0], (const char *[]){"echo", "hi", NULL}, "> out", 0);
	assert(p.commands[0].n_redirs == 1);
	assert(p.commands[0].redirs[0].kind == R_OUT);
	assert(strcmp(p.commands[0].redirs[0].target, "out.txt") == 0);
	pipeline_free(&p);
}

static void redirect_in(void) {
	pipeline_t p;
	do_parse_ok("wc < in.txt", &p);
	assert(p.commands[0].n_redirs == 1);
	assert(p.commands[0].redirs[0].kind == R_IN);
	assert(strcmp(p.commands[0].redirs[0].target, "in.txt") == 0);
	pipeline_free(&p);
}

static void redirect_append(void) {
	pipeline_t p;
	do_parse_ok("echo hi >> log", &p);
	assert(p.commands[0].n_redirs == 1);
	assert(p.commands[0].redirs[0].kind == R_APPEND);
	assert(strcmp(p.commands[0].redirs[0].target, "log") == 0);
	pipeline_free(&p);
}

static void redirect_stderr(void) {
	pipeline_t p;
	do_parse_ok("ls 2> err", &p);
	assert(p.commands[0].n_redirs == 1);
	assert(p.commands[0].redirs[0].kind == R_ERR);
	assert(strcmp(p.commands[0].redirs[0].target, "err") == 0);
	pipeline_free(&p);
}

static void redirect_in_middle(void) {
	/* POSIX allows redirects anywhere in the command -- the executor uses
	 * them once, regardless of where they appeared. */
	pipeline_t p;
	do_parse_ok("echo > out hi", &p);
	assert(p.n == 1);
	assert_argv(&p.commands[0], (const char *[]){"echo", "hi", NULL}, "echo > out hi", 0);
	assert(p.commands[0].n_redirs == 1);
	assert(p.commands[0].redirs[0].kind == R_OUT);
	assert(strcmp(p.commands[0].redirs[0].target, "out") == 0);
	pipeline_free(&p);
}

static void multiple_redirects(void) {
	pipeline_t p;
	do_parse_ok("prog < in > out 2> err", &p);
	assert(p.commands[0].n_redirs == 3);
	assert(p.commands[0].redirs[0].kind == R_IN);
	assert(strcmp(p.commands[0].redirs[0].target, "in") == 0);
	assert(p.commands[0].redirs[1].kind == R_OUT);
	assert(strcmp(p.commands[0].redirs[1].target, "out") == 0);
	assert(p.commands[0].redirs[2].kind == R_ERR);
	assert(strcmp(p.commands[0].redirs[2].target, "err") == 0);
	pipeline_free(&p);
}

static void pipe_with_redirects(void) {
	pipeline_t p;
	do_parse_ok("grep foo < in | wc -l > out", &p);
	assert(p.n == 2);
	assert(p.commands[0].n_redirs == 1);
	assert(p.commands[0].redirs[0].kind == R_IN);
	assert(p.commands[1].n_redirs == 1);
	assert(p.commands[1].redirs[0].kind == R_OUT);
	assert(strcmp(p.commands[1].redirs[0].target, "out") == 0);
	pipeline_free(&p);
}

static void quoted_words_survive(void) {
	pipeline_t p;
	do_parse_ok("echo \"hi there\" | grep 'foo bar'", &p);
	assert(p.n == 2);
	assert_argv(&p.commands[0], (const char *[]){"echo", "hi there", NULL}, "quoted", 0);
	assert_argv(&p.commands[1], (const char *[]){"grep", "foo bar", NULL}, "quoted", 1);
	pipeline_free(&p);
}

/* --- negative cases ----------------------------------------------------- */

static void err_leading_pipe(void)     { expect_parse_err("| foo"); }
static void err_trailing_pipe(void)    { expect_parse_err("foo |"); }
static void err_double_pipe(void)      { expect_parse_err("foo || bar"); }
static void err_amp_alone(void)        { expect_parse_err("&"); }
static void err_leading_amp(void)      { expect_parse_err("& foo"); }
static void err_amp_then_cmd(void)     { expect_parse_err("foo & bar"); }
static void err_redir_no_target(void)  { expect_parse_err("echo >"); }
static void err_redir_eof(void)        { expect_parse_err("cat <"); }
static void err_redir_then_pipe(void)  { expect_parse_err("echo > | wc"); }
static void err_redir_then_amp(void)   { expect_parse_err("echo > &"); }
static void err_redir_only(void)       { expect_parse_err("> out"); }

/* --- runner ------------------------------------------------------------- */

#define RUN(name) do { printf("  %s\n", #name); name(); } while (0)

int main(void) {
	RUN(empty_input);
	RUN(whitespace_only);
	RUN(single_command);
	RUN(command_with_args);
	RUN(two_stage_pipe);
	RUN(three_stage_pipe);
	RUN(background);
	RUN(background_pipeline);
	RUN(redirect_out);
	RUN(redirect_in);
	RUN(redirect_append);
	RUN(redirect_stderr);
	RUN(redirect_in_middle);
	RUN(multiple_redirects);
	RUN(pipe_with_redirects);
	RUN(quoted_words_survive);

	RUN(err_leading_pipe);
	RUN(err_trailing_pipe);
	RUN(err_double_pipe);
	RUN(err_amp_alone);
	RUN(err_leading_amp);
	RUN(err_amp_then_cmd);
	RUN(err_redir_no_target);
	RUN(err_redir_eof);
	RUN(err_redir_then_pipe);
	RUN(err_redir_then_amp);
	RUN(err_redir_only);

	printf("all tests passed\n");
	return 0;
}
