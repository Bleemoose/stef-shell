/*
 * fork / exec / wait for a single command, plus I/O redirection.
 *
 * External commands: parent forks, child applies redirs (open/dup2/close)
 * and calls execvp. Exec-failure codes follow bash's convention: 127 for
 * not-found, 126 for not-executable, 1 otherwise.
 *
 * Builtins don't fork (they mutate shell state). If a builtin has redirs,
 * the shell saves the affected fds with dup(), applies, runs, restores.
 *
 * Pipelines and background are still stubbed; each will be wired up in its
 * own milestone.
 */

#include "executor.h"

#include "builtins.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Updated on every return from execute(). The `status` builtin reads this
 * through executor_last_status(). Module-local so no other code can mutate
 * it behind our back. */
static int last_status = 0;

int executor_last_status(void) {
	return last_status;
}

static void nyi(const char *feature) {
	fprintf(stderr, "stef-shell: %s not implemented yet\n", feature);
}

/* ---- redirection helpers ---------------------------------------------- */

/* Which stdio fd does this redirection kind target?
 *    <   -> stdin  (0)
 *    >   -> stdout (1)
 *    >>  -> stdout (1)
 *    2>  -> stderr (2)
 * The trailing `return -1` is unreachable (switch covers every enum value)
 * but keeps -Wreturn-type quiet. */
static int redir_fd(redir_kind_t k) {
	switch (k) {
		case R_IN:     return 0;
		case R_OUT:    return 1;
		case R_APPEND: return 1;
		case R_ERR:    return 2;
	}
	return -1;
}

/* Open the file named by `r` with the right flags for its kind. Creates
 * missing files for >, >>, and 2> (mode 0644 before umask); < requires the
 * file to exist. On failure, prints a diagnostic and returns -1. */
static int open_redir(const redir_t *r) {
	int flags;
	mode_t mode = 0644;
	switch (r->kind) {
		case R_IN:
			flags = O_RDONLY;
			break;
		case R_OUT:
		case R_ERR:
			flags = O_WRONLY | O_CREAT | O_TRUNC;
			break;
		case R_APPEND:
			flags = O_WRONLY | O_CREAT | O_APPEND;
			break;
		default:
			flags = 0;   /* unreachable */
	}
	int fd = open(r->target, flags, mode);
	if (fd < 0) {
		fprintf(stderr, "stef-shell: %s: %s\n", r->target, strerror(errno));
	}
	return fd;
}

/* Apply every redir in order to the current process's fd table. Intended to
 * run in the forked child before exec. Returns 0 on success, -1 on failure
 * (diagnostic already printed). Failure leaves some subset of redirs applied;
 * the caller should _exit() immediately on -1 rather than try to recover. */
static int apply_redirs_child(const command_t *c) {
	for (size_t i = 0; i < c->n_redirs; i++) {
		int fd = open_redir(&c->redirs[i]);
		if (fd < 0) {
			return -1;
		}
		int target = redir_fd(c->redirs[i].kind);
		if (dup2(fd, target) < 0) {
			fprintf(stderr, "stef-shell: dup2: %s\n", strerror(errno));
			close(fd);
			return -1;
		}
		close(fd);   /* target now points at the file; original fd redundant */
	}
	return 0;
}

/* Apply redirs around an in-shell builtin call and restore afterwards.
 * Precondition: c->argv[0] is a known builtin (caller checks via builtin_is).
 * Returns the builtin's status, or 1 if redirect setup failed.
 *
 * The fflush calls around the dup2 dance are load-bearing: stdio buffers
 * live on FILE*, not on the fd, so any pending bytes would otherwise leak
 * to whichever file fd 1/2 points at when they finally get flushed. */
static int run_builtin_with_redirs(const command_t *c) {
	fflush(stdout);
	fflush(stderr);

	int *saved = xmalloc(c->n_redirs * sizeof(int));
	for (size_t i = 0; i < c->n_redirs; i++) {
		saved[i] = -1;
	}

	int setup_failed = 0;
	for (size_t i = 0; i < c->n_redirs; i++) {
		int target = redir_fd(c->redirs[i].kind);
		saved[i] = dup(target);
		if (saved[i] < 0) {
			fprintf(stderr, "stef-shell: dup: %s\n", strerror(errno));
			setup_failed = 1;
			break;
		}
		int fd = open_redir(&c->redirs[i]);
		if (fd < 0) {
			setup_failed = 1;
			break;
		}
		if (dup2(fd, target) < 0) {
			fprintf(stderr, "stef-shell: dup2: %s\n", strerror(errno));
			close(fd);
			setup_failed = 1;
			break;
		}
		close(fd);
	}

	int status = 1;
	if (!setup_failed) {
		int builtin_status;
		if (builtin_try(c, &builtin_status)) {
			status = builtin_status;
		}
		/* If builtin_try returns 0 here, the contract was violated (caller
		 * failed to pre-check with builtin_is). Surface as status 1. */
	}

	/* Flush again so any pending output from the builtin actually lands
	 * on the redirect target, not on the about-to-be-restored fd. */
	fflush(stdout);
	fflush(stderr);

	/* Restore in reverse: matters only for future nested-redir scenarios,
	 * but it's the obviously-correct order so do it now. */
	for (ssize_t i = (ssize_t)c->n_redirs - 1; i >= 0; i--) {
		if (saved[i] < 0) {
			continue;   /* we never managed to dup this one */
		}
		int target = redir_fd(c->redirs[i].kind);
		dup2(saved[i], target);
		close(saved[i]);
	}

	free(saved);
	return status;
}

/* ---- external command path -------------------------------------------- */

/* Child side: apply redirs, exec the target, diagnose + _exit on failure.
 * Never returns. Exit codes: 127 for not-found, 126 for not-executable,
 * 1 otherwise (matches bash). */
static _Noreturn void child_exec(const command_t *c) {
	if (apply_redirs_child(c) != 0) {
		/* open/dup2 already printed the specific error. Exit 1 matches
		 * bash's convention for redirection-setup failure. */
		_exit(1);
	}

	execvp(c->argv[0], c->argv);

	/* execvp only returns on failure. Capture errno before any library
	 * call that might clobber it. */
	int saved_errno = errno;
	fprintf(stderr, "stef-shell: %s: %s\n", c->argv[0], strerror(saved_errno));

	int code;
	if (saved_errno == ENOENT) {
		code = 127;
	} else if (saved_errno == EACCES) {
		code = 126;
	} else {
		code = 1;
	}

	/* _exit, not exit: the child inherited the parent's stdio buffers; the
	 * full exit() would flush them and double-print anything the parent had
	 * queued. _exit terminates without running atexit handlers or flushing. */
	_exit(code);
}

/* Wait for one child, retrying on EINTR. Returns the exit status
 * (0-255 for normal exit, 128+N when killed by signal N). */
static int wait_for(pid_t pid) {
	int status;
	for (;;) {
		pid_t r = waitpid(pid, &status, 0);
		if (r >= 0) {
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		perror("stef-shell: waitpid");
		return 1;
	}

	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}
	/* Stopped or continued: not expected without WUNTRACED/WCONTINUED. */
	return 1;
}

/* ---- top-level dispatch ----------------------------------------------- */

int execute(const pipeline_t *p) {
	if (p->size == 0) {
		return last_status = 0;
	}
	if (p->size > 1) {
		nyi("pipelines");
		return last_status = 1;
	}
	if (p->background) {
		nyi("background execution");
		return last_status = 1;
	}

	const command_t *c = &p->commands[0];

	/* Builtin dispatch: always in-shell, with or without redirs. */
	if (c->argc > 0 && builtin_is(c->argv[0])) {
		if (c->n_redirs == 0) {
			int s;
			(void)builtin_try(c, &s);   /* guaranteed to return 1 here */
			return last_status = s;
		}
		return last_status = run_builtin_with_redirs(c);
	}

	/* External command: fork, child applies redirs and execs. */
	pid_t pid = fork();
	if (pid < 0) {
		perror("stef-shell: fork");
		return last_status = 1;
	}
	if (pid == 0) {
		child_exec(c);   /* _Noreturn */
	}

	return last_status = wait_for(pid);
}
