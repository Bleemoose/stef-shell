/*
 * Executor -- fork/exec/wait for a single command.
 *
 * The shape: the parent calls fork(); the child calls execvp() to replace
 * itself with the target program; the parent calls waitpid() to reap the
 * child and decode its exit status. If execvp fails, the child prints a
 * diagnostic and _exits with a POSIX-shaped status code (127 for not-found,
 * 126 for not-executable, 1 otherwise).
 *
 * Scope: one single-command, foreground, no-redirect pipeline. Anything
 * richer returns an explicit "not yet implemented" diagnostic and status 1,
 * so the shell stays usable while later milestones fill those in.
 */

#include "executor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void nyi(const char *feature) {
	fprintf(stderr, "stef-shell: %s not implemented yet\n", feature);
}

/* Run the child half of fork: exec the target; on failure, diagnose and
 * _exit with a POSIX-shaped code. Never returns. */
static _Noreturn void child_exec(const command_t *c) {
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

/* Wait for one child, retrying if a signal interrupts the syscall. Returns
 * the bash-style status code (0-255 exit, 128+N signal). */
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
	/* Stopped or continued -- shouldn't happen without WUNTRACED/WCONTINUED,
	 * but be defensive. */
	return 1;
}

int execute(const pipeline_t *p) {
	if (p->size == 0) {
		return 0;
	}
	if (p->size > 1) {
		nyi("pipelines");
		return 1;
	}
	if (p->background) {
		nyi("background execution");
		return 1;
	}

	const command_t *c = &p->commands[0];
	if (c->n_redirs > 0) {
		nyi("redirections");
		return 1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("stef-shell: fork");
		return 1;
	}
	if (pid == 0) {
		child_exec(c);   /* _Noreturn */
	}

	return wait_for(pid);
}
