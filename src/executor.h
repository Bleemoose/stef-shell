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
 * Scope: one single-command, foreground, no-redirect pipeline. Pipelines,
 * background, and redirects are rejected with a diagnostic. Each will be
 * implemented in its own milestone (M5 redirects, M6 pipes, M8 job control).
 */
int execute(const pipeline_t *p);

/*
 * Last exit status returned by execute(). Zero at shell startup; updated on
 * every execute() call (including NYI rejections, fork failures, and builtin
 * returns). Exposed so the `status` builtin can print it -- the M4 stand-in
 * for $? until variable expansion arrives.
 */
int executor_last_status(void);

#endif /* STEF_EXECUTOR_H */
