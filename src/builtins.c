/*
 * Built-in commands.
 *
 * Each builtin is a static function with the signature
 *     int builtin_xxx(int argc, char **argv);
 * returning the shell-convention exit code (0 success, 1+ failure, 2 for
 * usage errors a la bash). Diagnostics go to stderr as
 *     "stef-shell: <name>: <message>\n"
 * matching the executor's format.
 *
 * Registration is data-driven: the `builtins[]` table is the single source
 * of truth for names, function pointers, and `help` summaries. Adding a
 * builtin means adding a row -- no changes to dispatch or help.
 */

#include "builtins.h"

#include "executor.h"
#include "parser.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* environ is declared by unistd.h on most systems but POSIX says to declare
 * it yourself if you use it. Cheap insurance against a picky libc. */
extern char **environ;

typedef int (*builtin_fn)(int argc, char **argv);

typedef struct {
	const char *name;
	builtin_fn  fn;
	const char *summary;
} builtin_t;

static int builtin_exit  (int argc, char **argv);
static int builtin_cd    (int argc, char **argv);
static int builtin_pwd   (int argc, char **argv);
static int builtin_export(int argc, char **argv);
static int builtin_unset (int argc, char **argv);
static int builtin_env   (int argc, char **argv);
static int builtin_help  (int argc, char **argv);
static int builtin_status(int argc, char **argv);

static const builtin_t builtins[] = {
	{ "exit",   builtin_exit,   "Exit the shell"                          },
	{ "cd",     builtin_cd,     "Change directory"                        },
	{ "pwd",    builtin_pwd,    "Print working directory"                 },
	{ "export", builtin_export, "Set or show environment variables"       },
	{ "unset",  builtin_unset,  "Remove an environment variable"          },
	{ "env",    builtin_env,    "Print the environment"                   },
	{ "help",   builtin_help,   "List built-in commands"                  },
	{ "status", builtin_status, "Print the last command's exit status"    },
};

#define N_BUILTINS (sizeof(builtins) / sizeof(builtins[0]))

int builtin_try(const command_t *c, int *status_out) {
	if (c->argc == 0) {
		return 0;
	}
	for (size_t i = 0; i < N_BUILTINS; i++) {
		if (strcmp(c->argv[0], builtins[i].name) == 0) {
			*status_out = builtins[i].fn((int)c->argc, c->argv);
			return 1;
		}
	}
	return 0;
}

/* ---- exit -------------------------------------------------------------- */

static int builtin_exit(int argc, char **argv) {
	int code = executor_last_status();
	if (argc >= 3) {
		fprintf(stderr, "stef-shell: exit: too many arguments\n");
		return 1;
	}
	if (argc == 2) {
		char *end;
		errno = 0;
		long v = strtol(argv[1], &end, 10);
		if (errno != 0 || *end != '\0' || v < 0 || v > 255) {
			fprintf(stderr, "stef-shell: exit: %s: numeric argument required\n",
			        argv[1]);
			/* Stay in the shell. bash exits 2 here; we prefer not to drop
			 * the user out of an interactive session over a typo. */
			return 2;
		}
		code = (int)v;
	}
	exit(code);   /* shell process: full exit() is correct, flushes stdio */
}

/* ---- cd ---------------------------------------------------------------- */

static int builtin_cd(int argc, char **argv) {
	if (argc > 2) {
		fprintf(stderr, "stef-shell: cd: too many arguments\n");
		return 1;
	}

	const char *target;
	char       *heap_target = NULL;   /* set iff we built the path ourselves */
	int         echo_target = 0;      /* `cd -` echoes the destination */

	if (argc == 1) {
		target = getenv("HOME");
		if (!target) {
			fprintf(stderr, "stef-shell: cd: HOME not set\n");
			return 1;
		}
	} else if (strcmp(argv[1], "-") == 0) {
		target = getenv("OLDPWD");
		if (!target) {
			fprintf(stderr, "stef-shell: cd: OLDPWD not set\n");
			return 1;
		}
		echo_target = 1;
	} else if (argv[1][0] == '~') {
		/* Only ~ and ~/... -- no ~user. Splice $HOME in for the '~'. */
		const char *home = getenv("HOME");
		if (!home) {
			fprintf(stderr, "stef-shell: cd: HOME not set\n");
			return 1;
		}
		size_t hlen = strlen(home);
		size_t rest = strlen(argv[1] + 1);   /* everything after the ~ */
		heap_target = xmalloc(hlen + rest + 1);
		memcpy(heap_target, home, hlen);
		memcpy(heap_target + hlen, argv[1] + 1, rest + 1);   /* +1 for NUL */
		target = heap_target;
	} else {
		target = argv[1];
	}

	/* Snapshot the current cwd *before* chdir so we can set OLDPWD to a
	 * resolved absolute path regardless of whether the user typed one. */
	char old_cwd[PATH_MAX];
	int have_old = (getcwd(old_cwd, sizeof old_cwd) != NULL);

	if (chdir(target) != 0) {
		fprintf(stderr, "stef-shell: cd: %s: %s\n", target, strerror(errno));
		free(heap_target);
		return 1;
	}

	if (echo_target) {
		puts(target);
	}

	if (have_old) {
		setenv("OLDPWD", old_cwd, 1);
	}
	char new_cwd[PATH_MAX];
	if (getcwd(new_cwd, sizeof new_cwd)) {
		setenv("PWD", new_cwd, 1);
	}

	free(heap_target);
	return 0;
}

/* ---- pwd --------------------------------------------------------------- */

static int builtin_pwd(int argc, char **argv) {
	(void)argc;
	(void)argv;
	char buf[PATH_MAX];
	if (!getcwd(buf, sizeof buf)) {
		fprintf(stderr, "stef-shell: pwd: %s\n", strerror(errno));
		return 1;
	}
	puts(buf);
	return 0;
}

/* ---- export ------------------------------------------------------------ */

static int builtin_export(int argc, char **argv) {
	if (argc == 1) {
		for (char **e = environ; *e; e++) {
			puts(*e);
		}
		return 0;
	}
	int rc = 0;
	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq) {
			/* `export NAME` without an assignment: in a full shell this
			 * would mark the variable as exported. We don't have shell
			 * variables yet, so treat it as a no-op rather than an error. */
			continue;
		}
		/* Temporarily split "NAME=VAL" at the '=' so we can pass NAME to
		 * setenv without allocating. We own the mutation window because
		 * argv strings here are the parser's xstrdup copies. */
		*eq = '\0';
		if (setenv(argv[i], eq + 1, 1) != 0) {
			fprintf(stderr, "stef-shell: export: %s: %s\n",
			        argv[i], strerror(errno));
			rc = 1;
		}
		*eq = '=';
	}
	return rc;
}

/* ---- unset ------------------------------------------------------------- */

static int builtin_unset(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (unsetenv(argv[i]) != 0) {
			fprintf(stderr, "stef-shell: unset: %s: %s\n",
			        argv[i], strerror(errno));
			return 1;
		}
	}
	return 0;
}

/* ---- env --------------------------------------------------------------- */

static int builtin_env(int argc, char **argv) {
	(void)argc;
	(void)argv;
	for (char **e = environ; *e; e++) {
		puts(*e);
	}
	return 0;
}

/* ---- help -------------------------------------------------------------- */

static int builtin_help(int argc, char **argv) {
	(void)argc;
	(void)argv;
	for (size_t i = 0; i < N_BUILTINS; i++) {
		printf("  %-8s  %s\n", builtins[i].name, builtins[i].summary);
	}
	return 0;
}

/* ---- status ------------------------------------------------------------ */

static int builtin_status(int argc, char **argv) {
	(void)argc;
	(void)argv;
	printf("%d\n", executor_last_status());
	return 0;
}
