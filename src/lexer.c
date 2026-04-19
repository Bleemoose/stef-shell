/*
 * Lexer -- state machine over a NUL-terminated input line.
 *
 * State transitions (summary):
 *
 *   START ---- whitespace ----> START
 *   START ---- '|'        ----> emit PIPE,  stay START
 *   START ---- '<'        ----> emit LT,    stay START
 *   START ---- '>'        ----> check next: '>' -> GT_GT, else GT
 *   START ---- '&'        ----> emit AMP,   stay START
 *   START ---- '2' '>'    ----> emit STDERR_GT (only at a word boundary)
 *   START ---- '\''       ----> IN_SQUOTE
 *   START ---- '"'        ----> IN_DQUOTE
 *   START ---- '\\'       ----> BS_UNQUOTED
 *   START ---- other      ----> IN_WORD, push char
 *
 *   IN_WORD ---- whitespace|op ----> flush WORD, reprocess char in START
 *   IN_WORD ---- quote         ----> enter quote state (word continues)
 *   IN_WORD ---- '\\'          ----> BS_UNQUOTED
 *   IN_WORD ---- other         ----> push char
 *
 *   IN_SQUOTE: everything literal until '\''; unterminated at EOF is error.
 *   IN_DQUOTE: literal except '\\' (which goes to BS_DQUOTE) and '"' (ends).
 *   BS_UNQUOTED: next char (any) is appended literally; EOF is error.
 *   BS_DQUOTE:   per POSIX, '\\' only escapes {", \\, $, `}; other chars
 *                are preserved along with the leading backslash.
 *
 * Adjacent quoting (e.g. a"b"c'd') concatenates into a single WORD: quoting
 * only changes how chars are interpreted, it doesn't split words.
 */

#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* ---- token_vec ---------------------------------------------------------- */

#define TOK_VEC_INIT_CAP 8

void token_vec_init(token_vec_t *v) {
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
}

void token_vec_free(token_vec_t *v) {
	for (size_t i = 0; i < v->len; i++){
		free(v->data[i].text);
	}
	free(v->data);
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
}

static void token_vec_push(token_vec_t *v, tok_kind_t k, char *text) {
	if (v->len == v->cap) {
		if (v->cap == 0) {
			v->cap = TOK_VEC_INIT_CAP;
		} else {
			v->cap = v->cap * 2;
		}
		v->data = xrealloc(v->data, v->cap * sizeof(v->data[0]));
	}
	v->data[v->len].kind = k;
	v->data[v->len].text = text;
	v->len++;
}

const char *tok_kind_name(tok_kind_t k) {
	switch (k) {
		case TOK_WORD:       return "WORD";
		case TOK_PIPE:       return "PIPE";
		case TOK_LT:         return "LT";
		case TOK_GT:         return "GT";
		case TOK_GT_GT:      return "GT_GT";
		case TOK_AMP:        return "AMP";
		case TOK_STDERR_GT:  return "STDERR_GT";
		case TOK_END:        return "END";
	}
	return "???";
}

/* ---- lex ---------------------------------------------------------------- */

typedef enum {
	S_START,
	S_IN_WORD,
	S_IN_SQUOTE,
	S_IN_DQUOTE,
	S_BS_UNQUOTED,
	S_BS_DQUOTE,
} lex_state_t;

static int is_whitespace(int ch) {
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static int is_op_start(int ch) {
	return ch == '|' || ch == '<' || ch == '>' || ch == '&';
}

static void lex_err(const char *msg, size_t col) {
	fprintf(stderr, "stef-shell: lex error at column %zu: %s\n", col + 1, msg);
}

/* Flush the accumulated word (if any) as a TOK_WORD. Takes the three pieces
 * of state it touches explicitly so the data flow is visible at every call
 * site and so the function is debuggable (breakable, type-checked). */
static void flush_word(token_vec_t *out, str_buf_t *sb, int *have_word) {
	if (*have_word) {
		token_vec_push(out, TOK_WORD, str_buf_take(sb));
		*have_word = 0;
	}
}

int lex(const char *line, token_vec_t *out) {
	size_t i = 0;
	lex_state_t st = S_START;
	str_buf_t sb;
	str_buf_init(&sb);
	int have_word = 0;

	for (;;) {
		char c = line[i];

		switch (st) {
		case S_START:
			if (c == '\0') {
				goto done;
			} else if (is_whitespace(c)) {
				i++;
			} else if (c == '|') {
				token_vec_push(out, TOK_PIPE, NULL);
				i++;
			} else if (c == '<') {
				token_vec_push(out, TOK_LT, NULL);
				i++;
			} else if (c == '>') {
				if (line[i + 1] == '>') {
					token_vec_push(out, TOK_GT_GT, NULL);
					i += 2;
				} else {
					token_vec_push(out, TOK_GT, NULL);
					i++;
				}
			} else if (c == '&') {
				token_vec_push(out, TOK_AMP, NULL);
				i++;
			} else if (c == '2' && line[i + 1] == '>') {
				/* Stderr redirect recognized only at word boundary. */
				token_vec_push(out, TOK_STDERR_GT, NULL);
				i += 2;
			} else if (c == '\'') {
				have_word = 1;
				st = S_IN_SQUOTE;
				i++;
			} else if (c == '"') {
				have_word = 1;
				st = S_IN_DQUOTE;
				i++;
			} else if (c == '\\') {
				have_word = 1;
				st = S_BS_UNQUOTED;
				i++;
			} else {
				have_word = 1;
				str_buf_push(&sb, c);
				st = S_IN_WORD;
				i++;
			}
			break;

		case S_IN_WORD:
			if (c == '\0') {
				flush_word(out, &sb, &have_word);
				goto done;
			} else if (is_whitespace(c)) {
				flush_word(out, &sb, &have_word);
				st = S_START;
				i++;
			} else if (is_op_start(c)) {
				/* Operator terminates the word; reprocess in START. */
				flush_word(out, &sb, &have_word);
				st = S_START;
			} else if (c == '\'') {
				st = S_IN_SQUOTE;
				i++;
			} else if (c == '"') {
				st = S_IN_DQUOTE;
				i++;
			} else if (c == '\\') {
				st = S_BS_UNQUOTED;
				i++;
			} else {
				str_buf_push(&sb, c);
				i++;
			}
			break;

		case S_IN_SQUOTE:
			if (c == '\0') {
				lex_err("unterminated single quote", i);
				str_buf_free(&sb);
				return -1;
			} else if (c == '\'') {
				st = S_IN_WORD;
				i++;
			} else {
				str_buf_push(&sb, c);
				i++;
			}
			break;

		case S_IN_DQUOTE:
			if (c == '\0') {
				lex_err("unterminated double quote", i);
				str_buf_free(&sb);
				return -1;
			} else if (c == '"') {
				st = S_IN_WORD;
				i++;
			} else if (c == '\\') {
				st = S_BS_DQUOTE;
				i++;
			} else {
				str_buf_push(&sb, c);
				i++;
			}
			break;

		case S_BS_UNQUOTED:
			if (c == '\0') {
				lex_err("trailing backslash", i);
				str_buf_free(&sb);
				return -1;
			}
			str_buf_push(&sb, c);
			st = S_IN_WORD;
			i++;
			break;

		case S_BS_DQUOTE:
			if (c == '\0') {
				lex_err("unterminated double quote", i);
				str_buf_free(&sb);
				return -1;
			}
			/* POSIX: inside "...", '\\' only escapes {", \, $, `}.
			 * Other chars keep both the backslash and the char. */
			if (c == '"' || c == '\\' || c == '$' || c == '`') {
				str_buf_push(&sb, c);
			} else {
				str_buf_push(&sb, '\\');
				str_buf_push(&sb, c);
			}
			st = S_IN_DQUOTE;
			i++;
			break;
		}
	}

done:
	token_vec_push(out, TOK_END, NULL);
	str_buf_free(&sb);
	return 0;
}
