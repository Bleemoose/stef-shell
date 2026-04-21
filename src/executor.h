#ifndef STEF_EXECUTOR_H
#define STEF_EXECUTOR_H

#include "parser.h"

/*
 * Execute a pipeline and return its exit status.
 *
 * Convention for the return value:
 *   0        -> success
 *   1..127   -> command-side failure (127 = not found, 126 = not executable,
 *               everything else = whatever the program returned)
 *   128 + N  -> child killed by signal N (bash convention)
 *
 * Shell-side errors (fork failure, waitpid failure, unsupported feature) are
 * reported to stderr and surfaced as status 1. Never aborts; the REPL always
 * gets control back.
 *
 * Scope: one single-command, foreground pipeline with I/O redirection
 * (<, >, >>, 2>) applied to either an external command or a builtin.
 * Pipelines (`|`) and background (`&`) are rejected with a diagnostic and
 * surface as status 1 until their milestones land (M6 pipes, M8 job control).
 */
int execute(const pipeline_t *p);

/*
 * Last exit status returned by execute(). Zero at shell startup; updated on
 * every execute() call (including NYI rejections, fork failures, and builtin
 * returns). The `status` builtin reads this (stand-in for $? until variable
 * expansion lands).
 */
int executor_last_status(void);

#endif /* STEF_EXECUTOR_H */
