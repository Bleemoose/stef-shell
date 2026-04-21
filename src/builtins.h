#ifndef STEF_BUILTINS_H
#define STEF_BUILTINS_H

#include "parser.h"

/*
 * Built-in commands: execute inside the shell process instead of a forked
 * child. Mandatory for anything that mutates shell state (cd, export, exit):
 * a child's chdir/setenv/exit would die with the child and leave the parent
 * unchanged. Also used for cheap introspection (pwd, env, help, status)
 * where skipping fork/exec is a real latency win.
 *
 * Dispatch happens in the executor *before* fork(). If the first argv slot
 * names a builtin, builtin_try runs it in-process, writes its exit status
 * through *status_out, and returns 1. Otherwise it returns 0 and the caller
 * proceeds with the normal fork/exec path.
 */

/*
 * Try to run `c` as a builtin. Returns:
 *   1 -> handled; *status_out holds the builtin's exit code (0 on success,
 *        non-zero on failure; diagnostic already on stderr).
 *   0 -> not a builtin; *status_out is untouched; caller should fork/exec.
 *
 * Safe to call on a command with argc == 0 (returns 0).
 */
int builtin_try(const command_t *c, int *status_out);

/*
 * Peek: is `name` registered in the builtins table? Used by the executor to
 * decide whether a command with redirections needs the in-shell save/restore
 * dance (builtin) or can let the forked child apply redirs itself (external).
 * Returns 1 if known, 0 otherwise. `name == NULL` -> 0.
 */
int builtin_is(const char *name);

#endif /* STEF_BUILTINS_H */
