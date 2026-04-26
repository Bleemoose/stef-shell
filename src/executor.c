/*
 * fork / exec / wait, plus I/O redirection and N-stage pipelines.
 *
 * External commands: parent forks, child applies redirs (open/dup2/close)
 * and calls execvp. Exec-failure codes follow bash's convention: 127 for
 * not-found, 126 for not-executable, 1 otherwise.
 *
 * Builtins in a single-command pipeline don't fork (they mutate shell
 * state). If a builtin has redirs, the shell saves the affected fds with
 * dup(), applies, runs, restores. Builtins in a multi-stage pipeline run
 * in a forked child: the redir-aware stage helper dispatches to them
 * directly and _exits with their status. Consequence: `cd /tmp | cat`
 * does not change the shell's cwd (matches bash).
 *
 * Pipelines: N commands -> N-1 pipes, one fork per stage, all stages run
 * concurrently. Close discipline is strict: each child dup2's its two
 * ends into fd 0/1 and closes every other pipe fd before exec, the
 * parent closes both ends of every pipe after forking the stage that
 * needed them. A single dangling write-end anywhere would keep readers
 * blocked on EOF forever. Pipeline exit status is the last stage's
 * status (no pipefail).
 *
 * Background is still stubbed.
 */

#include "executor.h"

#include "builtins.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

/* Restore default dispositions for the signals the shell ignores so a
 * forked child can be killed by Ctrl-C / Ctrl-\. POSIX preserves SIG_IGN
 * across execve, so without this a piped `sleep 100` would outlive
 * Ctrl-C and the pipeline would hang. Called from every child right
 * before it execs (or calls a builtin). */
static void reset_default_signals(void) {
	signal(SIGINT,  SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
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
	reset_default_signals();

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
static int wait_for_pid(pid_t pid) {
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

/* ---- pipeline path ---------------------------------------------------- */

/* Run one stage of an N-stage pipeline. Called in the forked child; never
 * returns.
 *
 *   in_fd        -> dup'd onto stdin, then closed. -1 means leave stdin
 *                   alone (first stage with no `<` redirect inherits the
 *                   shell's stdin).
 *   out_fd       -> dup'd onto stdout, then closed. -1 means leave stdout
 *                   alone (last stage with no `>` redirect).
 *   other_ends[] -> pipe fds still open in this process that belong to
 *                   *other* stages; must be closed before exec or the
 *                   reader at the far end never sees EOF.
 *
 * Per-command redirs are applied after the pipe wiring so that a
 * user-specified `> out` on a non-last stage overrides the pipe
 * connection (POSIX: later redirs win, and the pipe wiring is
 * effectively the earliest). */
static _Noreturn void run_pipeline_child(const command_t *c,
                                         int in_fd, int out_fd,
                                         const int *other_ends,
                                         size_t n_other) {
	reset_default_signals();

	if (in_fd >= 0) {
		if (dup2(in_fd, 0) < 0) {
			fprintf(stderr, "stef-shell: dup2: %s\n", strerror(errno));
			_exit(1);
		}
		close(in_fd);
	}
	if (out_fd >= 0) {
		if (dup2(out_fd, 1) < 0) {
			fprintf(stderr, "stef-shell: dup2: %s\n", strerror(errno));
			_exit(1);
		}
		close(out_fd);
	}
	for (size_t i = 0; i < n_other; i++) {
		close(other_ends[i]);
	}

	if (apply_redirs_child(c) != 0) {
		_exit(1);
	}

	/* Forked-builtin path: no exec, just call the builtin and _exit. Any
	 * state it mutated (cwd, environ) dies with this process. Flush
	 * before _exit or the builtin's stdio-buffered output (puts/printf)
	 * never reaches the pipe -- _exit skips stdio cleanup. */
	if (c->argc > 0 && builtin_is(c->argv[0])) {
		int s = 1;
		(void)builtin_try(c, &s);
		fflush(stdout);
		fflush(stderr);
		_exit(s);
	}

	execvp(c->argv[0], c->argv);

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
	_exit(code);
}

/* Drive an N-stage pipeline: for each stage create one pipe (except the
 * last), fork, hand the right fds to run_pipeline_child, and close the
 * parent's copies immediately. After all stages are forked, reap every
 * child; the pipeline's status is the last stage's status.
 *
 * Close discipline for the parent is what keeps the pipeline from
 * hanging:
 *   - prev_read (read end of the pipe feeding the current stage) is
 *     closed after the fork: only the child needs it.
 *   - pipefd[1] (write end of the pipe feeding the NEXT stage) is
 *     closed after the fork for the same reason.
 *   - pipefd[0] is kept as prev_read for the next iteration, then
 *     closed there.
 * End result: after the loop, the parent holds zero pipe fds. */
static int run_pipeline(const pipeline_t *p) {
	/* Drain the parent's stdio buffers before forking. Otherwise every
	 * child inherits them, and a forked-builtin stage that flushes
	 * before _exit would push the parent's pending bytes into the pipe
	 * (duplicated output), while not flushing would drop the child's
	 * own output on the floor. */
	fflush(stdout);
	fflush(stderr);

	size_t n = p->size;
	pid_t *pids = xmalloc(n * sizeof(pid_t));
	int prev_read = -1;

	for (size_t i = 0; i < n; i++) {
		int pipefd[2] = { -1, -1 };
		int is_last = (i + 1 == n);

		if (!is_last) {
			if (pipe(pipefd) < 0) {
				perror("stef-shell: pipe");
				if (prev_read >= 0) close(prev_read);
				for (size_t j = 0; j < i; j++) {
					(void)waitpid(pids[j], NULL, 0);
				}
				free(pids);
				return 1;
			}
		}

		/* The only "other end" the child sees is pipefd[0]: the read end
		 * of the pipe feeding the NEXT stage. The child must close it so
		 * only the next stage's child ever holds it. prev_read is passed
		 * as in_fd (dup2'd + closed in the child); pipefd[1] is out_fd
		 * (same). */
		int other_ends[1];
		size_t n_other = 0;
		if (!is_last) {
			other_ends[n_other++] = pipefd[0];
		}

		pid_t pid = fork();
		if (pid < 0) {
			perror("stef-shell: fork");
			if (!is_last) {
				close(pipefd[0]);
				close(pipefd[1]);
			}
			if (prev_read >= 0) close(prev_read);
			for (size_t j = 0; j < i; j++) {
				(void)waitpid(pids[j], NULL, 0);
			}
			free(pids);
			return 1;
		}

		if (pid == 0) {
			int in_fd  = prev_read;
			int out_fd = is_last ? -1 : pipefd[1];
			run_pipeline_child(&p->commands[i], in_fd, out_fd,
			                   other_ends, n_other);
		}

		/* Parent: drop every pipe fd we no longer need. */
		if (prev_read >= 0) {
			close(prev_read);
		}
		if (!is_last) {
			close(pipefd[1]);
			prev_read = pipefd[0];   /* hand read end to next iteration */
		} else {
			prev_read = -1;
		}

		pids[i] = pid;
	}

	/* Reap every stage; keep only the last one's status. */
	int last = 1;
	for (size_t i = 0; i < n; i++) {
		int s = wait_for_pid(pids[i]);
		if (i + 1 == n) {
			last = s;
		}
	}
	free(pids);
	return last;
}

/* ---- top-level dispatch ----------------------------------------------- */

int execute(const pipeline_t *p) {
	if (p->size == 0) {
		return last_status = 0;
	}
	if (p->background) {
		nyi("background execution");
		return last_status = 1;
	}

	/* Single-command fast path: builtins run in-shell (M4/M5), externals
	 * fork once. This is the only way `cd`, `export`, and friends actually
	 * mutate the shell's state. */
	if (p->size == 1) {
		const command_t *c = &p->commands[0];

		if (c->argc > 0 && builtin_is(c->argv[0])) {
			if (c->n_redirs == 0) {
				int s;
				(void)builtin_try(c, &s);
				return last_status = s;
			}
			return last_status = run_builtin_with_redirs(c);
		}

		pid_t pid = fork();
		if (pid < 0) {
			perror("stef-shell: fork");
			return last_status = 1;
		}
		if (pid == 0) {
			child_exec(c);   /* _Noreturn */
		}
		return last_status = wait_for_pid(pid);
	}

	return last_status = run_pipeline(p);
}
