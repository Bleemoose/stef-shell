/*
 * Parser -- straight-line recursive descent over the token stream.
 *
 * Single pass, single index `i`. The outer loop parses commands separated by
 * PIPE; a trailing AMP flips `background`; END terminates. An inner loop
 * consumes WORDs and redirects in any order, matching POSIX where
 * `echo > out hi` is a legal way to say "run echo with arg 'hi', stdout to
 * 'out'".
 *
 * Every WORD is xstrdup'd into the AST, so the caller can free the
 * token_vec_t immediately after parse() returns.
 */

#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* ---- growable array helpers -------------------------------------------- */

#define ARGV_INIT_CAP   4
#define REDIR_INIT_CAP  2
#define CMD_INIT_CAP    2

/* Keep argv NULL-terminated after every push. The capacity must be at least
 * argc + 2 (one for the new entry, one for the terminator). */
static void command_push_arg(command_t *c, size_t *cap, char *word) {
	if (c->argc + 1 >= *cap) {
		if (*cap == 0) {
			*cap = ARGV_INIT_CAP;
		} else {
			*cap = *cap * 2;
		}
		c->argv = xrealloc(c->argv, *cap * sizeof(char *));
	}
	//c->argv[c->argc++] = word;
	//Apparently in C x++ will first execute the command with the old value the increase ++
	c->argv[c->argc] = word;
	c->argc++;
	c->argv[c->argc] = NULL; 

}

static void command_push_redir(command_t *c, size_t *cap, redir_kind_t k, char *target) {
	if (c->n_redirs == *cap) {
		if (*cap == 0) {
			*cap = REDIR_INIT_CAP;
		} else {
			*cap = *cap * 2;
		}
		c->redirs = xrealloc(c->redirs, *cap * sizeof(redir_t));
	}
	c->redirs[c->n_redirs].kind = k;
	c->redirs[c->n_redirs].target = target;
	c->n_redirs++;
}

static void pipeline_push_command(pipeline_t *p, size_t *cap, command_t cmd) {
	if (p->size == *cap) {
		if (*cap == 0) {
			*cap = CMD_INIT_CAP;
		} else {
			*cap = *cap * 2;
		}
		p->commands = xrealloc(p->commands, *cap * sizeof(command_t));
	}
	p->commands[p->size++] = cmd;
}

/* ---- lifecycle --------------------------------------------------------- */

void pipeline_init(pipeline_t *p) {
	p->commands = NULL;
	p->size = 0;
	p->background = 0;
}

static void command_free(command_t *c) {
	for (size_t i = 0; i < c->argc; i++) {
		free(c->argv[i]);
	}
	free(c->argv);
	for (size_t i = 0; i < c->n_redirs; i++) {
		free(c->redirs[i].target);
	}
	free(c->redirs);
}

void pipeline_free(pipeline_t *p) {
	for (size_t i = 0; i < p->size; i++) {
		command_free(&p->commands[i]);
	}
	free(p->commands);
	pipeline_init(p);
}

/* ---- parsing ----------------------------------------------------------- */

const char *redir_kind_name(redir_kind_t k) {
	switch (k) {
		case R_IN:      return "<";
		case R_OUT:     return ">";
		case R_APPEND:  return ">>";
		case R_ERR:     return "2>";
	}
	return "???";
}

void pipeline_print(FILE *f, const pipeline_t *p) {
	if (p->size == 0) {
		fprintf(f, "pipeline (empty)\n");
		return;
	}
	fprintf(f, "pipeline (background=%d, size=%zu):\n", p->background, p->size);
	for (size_t i = 0; i < p->size; i++) {
		const command_t *c = &p->commands[i];
		fprintf(f, "  command %zu:\n", i);

		fprintf(f, "    argv: [");
		for (size_t j = 0; j < c->argc; j++) {
			if (j > 0) {
				fprintf(f, ", ");
			}
			fprintf(f, "\"%s\"", c->argv[j]);
		}
		fprintf(f, "]\n");

		if (c->n_redirs > 0) {
			fprintf(f, "    redirs:\n");
			for (size_t j = 0; j < c->n_redirs; j++) {
				fprintf(f, "      %s \"%s\"\n",
				        redir_kind_name(c->redirs[j].kind),
				        c->redirs[j].target);
			}
		}
	}
}

static void parse_err(const char *msg, const token_t *tok) {
	if (tok->kind == TOK_WORD && tok->text) {
		fprintf(stderr, "stef-shell: parse error: %s (near WORD \"%s\")\n",
		        msg, tok->text);
	} else {
		fprintf(stderr, "stef-shell: parse error: %s (near %s)\n",
		        msg, tok_kind_name(tok->kind));
	}
}

static int redir_kind_from_tok(tok_kind_t k, redir_kind_t *out) {
	switch (k) {
		case TOK_LT:        *out = R_IN;     return 1;
		case TOK_GT:        *out = R_OUT;    return 1;
		case TOK_GT_GT:     *out = R_APPEND; return 1;
		case TOK_STDERR_GT: *out = R_ERR;    return 1;
		default:            return 0;
	}
}

int parse(const token_vec_t *tokens, pipeline_t *out) {
	pipeline_init(out);

	/* Defensive: the lexer guarantees a TOK_END terminator, but if a caller
	 * hands us an empty vec treat it as empty input. */
	if (tokens->len == 0 || tokens->data[0].kind == TOK_END) {
		return 0;
	}

	size_t i = 0;
	size_t cmd_cap = 0;

	for (;;) {
		/* Parse one command. */
		command_t cmd;
		cmd.argv = NULL;
		cmd.argc = 0;
		cmd.redirs = NULL;
		cmd.n_redirs = 0;
		size_t argv_cap = 0;
		size_t redir_cap = 0;

		for (;;) {
			const token_t *tok = &tokens->data[i];
			redir_kind_t redir_kind;

			if (tok->kind == TOK_WORD) {
				command_push_arg(&cmd, &argv_cap, xstrdup(tok->text));
				i++;
			} else if (redir_kind_from_tok(tok->kind, &redir_kind)) {
				i++;
				const token_t *target = &tokens->data[i];
				if (target->kind != TOK_WORD) {
					parse_err("expected filename after redirect", target);
					command_free(&cmd);
					pipeline_free(out);
					return -1;
				}
				command_push_redir(&cmd, &redir_cap, redir_kind, xstrdup(target->text));
				i++;
			} else {
				/* PIPE, AMP, END -- end of this command. */
				break;
			}
		}

		if (cmd.argc == 0) {
			parse_err("empty command", &tokens->data[i]);
			command_free(&cmd);
			pipeline_free(out);
			return -1;
		}

		pipeline_push_command(out, &cmd_cap, cmd);

		const token_t *sep = &tokens->data[i];
		if (sep->kind == TOK_PIPE) {
			i++;
			continue;   /* parse next command */
		}
		if (sep->kind == TOK_AMP) {
			out->background = 1;
			i++;
			if (tokens->data[i].kind != TOK_END) {
				parse_err("unexpected token after '&'", &tokens->data[i]);
				pipeline_free(out);
				return -1;
			}
			return 0;
		}
		if (sep->kind == TOK_END) {
			return 0;
		}
		/* Unreachable: the inner loop only breaks on PIPE/AMP/END. */
		parse_err("unexpected token", sep);
		pipeline_free(out);
		return -1;
	}
}
