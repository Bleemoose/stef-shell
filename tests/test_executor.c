/*
 * Executor tests. Each test lexes + parses an input line, hands it to
 * execute(), and checks the returned status code. The child half runs real
 * programs (/bin/true, /bin/false, etc.), which exist on every POSIX system
 * including WSL.
 *
 * Redirection tests write to /tmp/stef_shell_test_* and unlink on the way
 * out. The TMP_PREFIX macro keeps the paths consistent and grep-able.
 *
 * Child-side allocations between fork and exec: there are none, so ASan's
 * skipped exit-time check on the _exit() path isn't hiding anything. The
 * parent's pipeline_t is freed after every test and ASan verifies that.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/executor.h"
#include "../src/lexer.h"
#include "../src/parser.h"

#define TMP_PREFIX "/tmp/stef_shell_test_"

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
 * for tests where the executor (or the child) prints an expected
 * diagnostic that would otherwise litter a successful run. */
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

/* --- file helpers for redir tests --------------------------------------- */

/* Read the entire file at `path` into a heap buffer (NUL-terminated). Caller
 * frees. Returns NULL if the file doesn't exist or can't be opened. */
static char *slurp(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	char *buf = malloc(4096);
	assert(buf);
	size_t n = fread(buf, 1, 4095, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Write `content` to `path`, truncating. Used to pre-seed a file for < tests. */
static void seed(const char *path, const char *content) {
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(content, f);
	fclose(f);
}

/* rm -f a path; ignore errors. */
static void rm(const char *path) {
	unlink(path);
}

/* --- happy paths -------------------------------------------------------- */

static void true_returns_0(void) {
	assert(run("/bin/true") == 0);
}

static void false_returns_1(void) {
	assert(run("/bin/false") == 1);
}

static void path_lookup_succeeds(void) {
	/* No absolute path: exercises execvp's PATH walk. */
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

/* --- empty / NYI -------------------------------------------------------- */

static void empty_pipeline_returns_0(void) {
	assert(run("") == 0);
}

static void whitespace_only_returns_0(void) {
	assert(run("   \t  ") == 0);
}

static void background_rejected(void) {
	assert(run_silent("sleep 0 &") == 1);
}

/* --- redirection (M5) --------------------------------------------------- */

static void stdout_redirect_truncate(void) {
	const char *out = TMP_PREFIX "out";
	rm(out);

	/* First write: creates the file. */
	assert(run("echo hi > " TMP_PREFIX "out") == 0);
	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "hi\n") == 0);
	free(s);

	/* Second write: O_TRUNC clobbers the first. */
	assert(run("echo bye > " TMP_PREFIX "out") == 0);
	s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "bye\n") == 0);
	free(s);

	rm(out);
}

static void stdout_redirect_append(void) {
	const char *out = TMP_PREFIX "append";
	rm(out);

	assert(run("echo one > " TMP_PREFIX "append") == 0);
	assert(run("echo two >> " TMP_PREFIX "append") == 0);

	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "one\ntwo\n") == 0);
	free(s);

	rm(out);
}

static void stdin_redirect(void) {
	const char *in  = TMP_PREFIX "in";
	const char *out = TMP_PREFIX "catout";
	rm(in);
	rm(out);

	seed(in, "hello from stdin\n");
	/* `cat < in > out` exercises both redirs in one command. */
	assert(run("cat < " TMP_PREFIX "in > " TMP_PREFIX "catout") == 0);

	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "hello from stdin\n") == 0);
	free(s);

	rm(in);
	rm(out);
}

static void stderr_redirect(void) {
	const char *err = TMP_PREFIX "err";
	rm(err);

	/* `ls` on a missing path prints to stderr and exits non-zero. */
	int status = run("ls /nonexistent-xyz-42 2> " TMP_PREFIX "err");
	assert(status != 0);

	char *s = slurp(err);
	assert(s != NULL);
	/* We don't pin the exact wording (differs across coreutils versions),
	 * but the path we asked for had better appear in the message. */
	assert(strstr(s, "nonexistent-xyz-42") != NULL);
	assert(strlen(s) > 0);
	free(s);

	rm(err);
}

static void open_failure_returns_1(void) {
	/* Child opens /nonexistent-xyz-42 for read, fails, _exit(1). */
	assert(run_silent("cat < /nonexistent-xyz-42") == 1);
}

static void builtin_redirect_pwd(void) {
	const char *out = TMP_PREFIX "pwd";
	rm(out);

	assert(run("pwd > " TMP_PREFIX "pwd") == 0);

	char cwd[4096];
	assert(getcwd(cwd, sizeof cwd) != NULL);

	char *s = slurp(out);
	assert(s != NULL);
	/* `pwd` prints "<cwd>\n". Compare the prefix; trailing newline is the only
	 * thing after. */
	size_t cwd_len = strlen(cwd);
	assert(strncmp(s, cwd, cwd_len) == 0);
	assert(s[cwd_len] == '\n');
	assert(s[cwd_len + 1] == '\0');
	free(s);

	/* Shell's own stdout must still work; the real check is that subsequent
	 * tests run without I/O weirdness. */
	fflush(stdout);

	rm(out);
}

static void multiple_redirects(void) {
	const char *out = TMP_PREFIX "mout";
	const char *err = TMP_PREFIX "merr";
	rm(out);
	rm(err);

	/* echo writes only to stdout; stderr redirect target should end up empty. */
	assert(run("echo hello > " TMP_PREFIX "mout 2> " TMP_PREFIX "merr") == 0);

	char *so = slurp(out);
	char *se = slurp(err);
	assert(so != NULL);
	assert(se != NULL);
	assert(strcmp(so, "hello\n") == 0);
	assert(strcmp(se, "") == 0);
	free(so);
	free(se);

	rm(out);
	rm(err);
}

static void append_mode_creates(void) {
	const char *out = TMP_PREFIX "newappend";
	rm(out);   /* ensure it really doesn't exist */

	/* >> on a non-existent path must create it, not error. */
	assert(run("echo fresh >> " TMP_PREFIX "newappend") == 0);
	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "fresh\n") == 0);
	free(s);

	rm(out);
}

/* --- pipelines (M6) ---------------------------------------------------- */

static void two_stage_pipe(void) {
	const char *out = TMP_PREFIX "p2";
	rm(out);

	assert(run("echo hi | cat > " TMP_PREFIX "p2") == 0);

	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "hi\n") == 0);
	free(s);
	rm(out);
}

static void three_stage_pipe(void) {
	const char *out = TMP_PREFIX "p3";
	rm(out);

	assert(run("echo hi | tr h H | cat > " TMP_PREFIX "p3") == 0);

	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "Hi\n") == 0);
	free(s);
	rm(out);
}

static void pipeline_exit_is_last(void) {
	/* Last stage wins; no pipefail. */
	assert(run("/bin/false | /bin/true") == 0);
	assert(run("/bin/true | /bin/false") == 1);
}

static void pipeline_with_redirect_on_first(void) {
	const char *in  = TMP_PREFIX "pin";
	const char *out = TMP_PREFIX "pout";
	rm(in);
	rm(out);

	seed(in, "abc\n");
	/* First stage reads from a file, last stage writes to one. The pipe
	 * in the middle ties cat's stdout to tr's stdin. */
	assert(run("cat < " TMP_PREFIX "pin | tr a-z A-Z > " TMP_PREFIX "pout") == 0);

	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "ABC\n") == 0);
	free(s);
	rm(in);
	rm(out);
}

static void pipeline_builtin_stage(void) {
	/* pwd in a pipeline runs in a forked child. Its output must still
	 * reach the next stage via the pipe. */
	const char *out = TMP_PREFIX "pbuiltin";
	rm(out);

	assert(run("pwd | cat > " TMP_PREFIX "pbuiltin") == 0);

	char cwd[4096];
	assert(getcwd(cwd, sizeof cwd) != NULL);
	char *s = slurp(out);
	assert(s != NULL);
	size_t cwd_len = strlen(cwd);
	assert(strncmp(s, cwd, cwd_len) == 0);
	assert(s[cwd_len] == '\n');
	assert(s[cwd_len + 1] == '\0');
	free(s);

	rm(out);
}

static void pipeline_first_not_found(void) {
	/* First stage execvp fails with ENOENT -> _exit(127). Its error is
	 * written to stderr (silenced here) and the write end of the pipe
	 * closes, so cat sees immediate EOF and exits 0. The pipeline's
	 * status is cat's (the last stage), which is 0. The real value of
	 * this test is that it doesn't hang: dangling pipe fds in the
	 * parent would keep cat blocked forever on its read(). */
	assert(run_silent("nonexistent-cmd-xyz-42 | cat") == 0);
}

static void pipeline_sigpipe_clean(void) {
	/* `yes` prints forever; `head -n 3` reads three lines and closes
	 * stdin. yes's next write() hits a pipe with no reader and the
	 * kernel raises SIGPIPE, which has its default disposition
	 * (terminate). head exits 0 as the last stage, so the pipeline
	 * status is 0. The test mainly proves the shell doesn't hang. */
	const char *out = TMP_PREFIX "psigp";
	rm(out);

	assert(run("yes | head -n 3 > " TMP_PREFIX "psigp") == 0);

	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "y\ny\ny\n") == 0);
	free(s);
	rm(out);
}

static void pipeline_four_stages(void) {
	/* N=4: tokenize `a b c` into one-per-line, reverse-sort, take top 2. */
	const char *out = TMP_PREFIX "p4";
	rm(out);

	assert(run("echo a b c | tr ' ' '\\n' | sort -r | head -n 2 > "
	           TMP_PREFIX "p4") == 0);

	char *s = slurp(out);
	assert(s != NULL);
	assert(strcmp(s, "c\nb\n") == 0);
	free(s);
	rm(out);
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
	RUN(background_rejected);

	RUN(stdout_redirect_truncate);
	RUN(stdout_redirect_append);
	RUN(stdin_redirect);
	RUN(stderr_redirect);
	RUN(open_failure_returns_1);
	RUN(builtin_redirect_pwd);
	RUN(multiple_redirects);
	RUN(append_mode_creates);

	RUN(two_stage_pipe);
	RUN(three_stage_pipe);
	RUN(pipeline_exit_is_last);
	RUN(pipeline_with_redirect_on_first);
	RUN(pipeline_builtin_stage);
	RUN(pipeline_first_not_found);
	RUN(pipeline_sigpipe_clean);
	RUN(pipeline_four_stages);

	printf("all tests passed\n");
	return 0;
}
