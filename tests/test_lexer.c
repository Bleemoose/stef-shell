/*
 * Lexer tests. Plain assertions, no framework.
 *
 * Each test calls either expect_ok (input should lex successfully into an
 * expected token sequence) or expect_err (input should cause a lex error).
 * assert() aborts on failure, which AddressSanitizer enjoys, so any hit
 * gives us a full stack trace.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/lexer.h"

/* Compare the lexer's output against an expected sequence. */
static void expect_ok(const char *input,
                      const tok_kind_t *exp_kinds,
                      const char *const *exp_texts,
                      size_t exp_n) {
	token_vec_t v;
	token_vec_init(&v);
	int r = lex(input, &v);
	if (r != 0) {
		fprintf(stderr, "FAIL: %s -- lex returned %d (expected 0)\n", input, r);
		abort();
	}
	if (v.len != exp_n) {
		fprintf(stderr, "FAIL: %s -- got %zu tokens, expected %zu\n",
		        input, v.len, exp_n);
		abort();
	}
	for (size_t i = 0; i < exp_n; i++) {
		if (v.data[i].kind != exp_kinds[i]) {
			fprintf(stderr, "FAIL: %s -- token %zu kind %s (expected %s)\n",
			        input, i, tok_kind_name(v.data[i].kind),
			        tok_kind_name(exp_kinds[i]));
			abort();
		}
		if (exp_kinds[i] == TOK_WORD) {
			if (!v.data[i].text || strcmp(v.data[i].text, exp_texts[i]) != 0) {
				fprintf(stderr, "FAIL: %s -- token %zu text \"%s\" "
				                "(expected \"%s\")\n",
				        input, i,
				        v.data[i].text ? v.data[i].text : "(null)",
				        exp_texts[i]);
				abort();
			}
		} else {
			assert(v.data[i].text == NULL);
		}
	}
	token_vec_free(&v);
}

/* Negative test. Silences stderr so a successful run has clean output. */
static void expect_err(const char *input) {
	token_vec_t v;
	token_vec_init(&v);

	/* Redirect stderr to /dev/null for the duration of lex(). */
	fflush(stderr);
	int saved = dup(fileno(stderr));
	FILE *devnull = freopen("/dev/null", "w", stderr);
	(void)devnull;

	int r = lex(input, &v);

	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);

	if (r == 0) {
		fprintf(stderr, "FAIL: %s -- lex returned 0 (expected error)\n", input);
		abort();
	}
	token_vec_free(&v);
}

/* --- test cases --------------------------------------------------------- */

#define KS(...)  (tok_kind_t[]){__VA_ARGS__}
#define TS(...)  (const char *const[]){__VA_ARGS__}

static void empty_input(void) {
	expect_ok("", KS(TOK_END), TS(NULL), 1);
}

static void single_word(void) {
	expect_ok("foo", KS(TOK_WORD, TOK_END), TS("foo", NULL), 2);
}

static void multiple_words(void) {
	expect_ok("one two three",
	          KS(TOK_WORD, TOK_WORD, TOK_WORD, TOK_END),
	          TS("one", "two", "three", NULL), 4);
}

static void whitespace_runs(void) {
	expect_ok("  a\tb  ",
	          KS(TOK_WORD, TOK_WORD, TOK_END),
	          TS("a", "b", NULL), 3);
}

static void pipe_alone(void) {
	expect_ok("|", KS(TOK_PIPE, TOK_END), TS(NULL, NULL), 2);
}

static void all_operators(void) {
	expect_ok("| < > >> & 2>",
	          KS(TOK_PIPE, TOK_LT, TOK_GT, TOK_GT_GT, TOK_AMP, TOK_STDERR_GT, TOK_END),
	          TS(NULL, NULL, NULL, NULL, NULL, NULL, NULL), 7);
}

static void op_glued_to_word(void) {
	expect_ok("ls|wc",
	          KS(TOK_WORD, TOK_PIPE, TOK_WORD, TOK_END),
	          TS("ls", NULL, "wc", NULL), 4);
}

static void gt_gt_vs_gt(void) {
	expect_ok("echo>>out",
	          KS(TOK_WORD, TOK_GT_GT, TOK_WORD, TOK_END),
	          TS("echo", NULL, "out", NULL), 4);
}

static void stderr_redirect(void) {
	expect_ok("ls 2>err",
	          KS(TOK_WORD, TOK_STDERR_GT, TOK_WORD, TOK_END),
	          TS("ls", NULL, "err", NULL), 4);
}

static void two_as_word(void) {
	expect_ok("echo 2 > out",
	          KS(TOK_WORD, TOK_WORD, TOK_GT, TOK_WORD, TOK_END),
	          TS("echo", "2", NULL, "out", NULL), 5);
}

static void two_inside_word(void) {
	expect_ok("foo2bar",
	          KS(TOK_WORD, TOK_END),
	          TS("foo2bar", NULL), 2);
}

static void single_quoted_spaces(void) {
	expect_ok("'hi there'",
	          KS(TOK_WORD, TOK_END),
	          TS("hi there", NULL), 2);
}

static void double_quoted_spaces(void) {
	expect_ok("\"hi there\"",
	          KS(TOK_WORD, TOK_END),
	          TS("hi there", NULL), 2);
}

static void adjacent_quoting(void) {
	expect_ok("a\"b\"c'd'",
	          KS(TOK_WORD, TOK_END),
	          TS("abcd", NULL), 2);
}

static void backslash_escapes_space(void) {
	expect_ok("a\\ b",
	          KS(TOK_WORD, TOK_END),
	          TS("a b", NULL), 2);
}

static void backslash_in_dquote_escapes_dquote(void) {
	expect_ok("\"a\\\"b\"",
	          KS(TOK_WORD, TOK_END),
	          TS("a\"b", NULL), 2);
}

static void backslash_in_squote_is_literal(void) {
	expect_ok("'a\\b'",
	          KS(TOK_WORD, TOK_END),
	          TS("a\\b", NULL), 2);
}

static void empty_squote_makes_empty_word(void) {
	expect_ok("''", KS(TOK_WORD, TOK_END), TS("", NULL), 2);
}

static void realistic_pipeline(void) {
	expect_ok("echo \"hi there\" | grep foo > out.txt",
	          KS(TOK_WORD, TOK_WORD, TOK_PIPE, TOK_WORD, TOK_WORD,
	             TOK_GT, TOK_WORD, TOK_END),
	          TS("echo", "hi there", NULL, "grep", "foo", NULL, "out.txt", NULL),
	          8);
}

static void unterminated_squote(void)       { expect_err("'hi there"); }
static void unterminated_dquote(void)       { expect_err("\"hi there"); }
static void unterminated_dquote_bs(void)    { expect_err("\"hi\\"); }
static void trailing_backslash(void)        { expect_err("foo\\"); }

/* --- runner ------------------------------------------------------------- */

#define RUN(name) do { printf("  %s\n", #name); name(); } while (0)

int main(void) {
	RUN(empty_input);
	RUN(single_word);
	RUN(multiple_words);
	RUN(whitespace_runs);
	RUN(pipe_alone);
	RUN(all_operators);
	RUN(op_glued_to_word);
	RUN(gt_gt_vs_gt);
	RUN(stderr_redirect);
	RUN(two_as_word);
	RUN(two_inside_word);
	RUN(single_quoted_spaces);
	RUN(double_quoted_spaces);
	RUN(adjacent_quoting);
	RUN(backslash_escapes_space);
	RUN(backslash_in_dquote_escapes_dquote);
	RUN(backslash_in_squote_is_literal);
	RUN(empty_squote_makes_empty_word);
	RUN(realistic_pipeline);
	RUN(unterminated_squote);
	RUN(unterminated_dquote);
	RUN(unterminated_dquote_bs);
	RUN(trailing_backslash);

	printf("all tests passed\n");
	return 0;
}
