# stef-shell

A POSIX shell written from scratch in C11.The project's purpose is to re-familiarize myself with the C language and basic OS concepts.
No dependency on `readline`, `libedit`, or any other shell runtime. Covers process creation, fd manipulation, pipes, signals, job control, and a line editor.

## Scope

- [x] Interactive REPL with EOF handling
- [x] Tokenizer (words, single/double quotes, operators)
- [x] Parser producing a command/pipeline AST
- [x] External command execution via `fork`/`execvp`/`waitpid`
- [x] Built-ins: `cd`, `exit`, `export`, `unset`, `pwd`, `env`
- [x] I/O redirection: `<`, `>`, `>>`, `2>`
- [ ] N-stage pipelines (`cmd1 | cmd2 | cmd3`)
- [ ] Signal handling: `SIGINT`, `SIGQUIT`, `SIGCHLD`
- [ ] Job control: `&`, `fg`, `bg`, `jobs`, `Ctrl-Z`
- [ ] Raw-mode line editor via `termios`: arrow keys, history, tab completion

**Non-goals:** scripting constructs (`if`, `while`), variable/parameter expansion, globbing, command substitution, aliases, startup files, Windows support.

## Build

```
make debug      # -O0 -g with AddressSanitizer + UBSan (default)
make release    # -O2, sanitizers off
make run        # build debug and run
make clean
```

Compiled with `-Wall -Wextra -Werror -Wpedantic -Wshadow -Wvla` against `-std=c11` and `_POSIX_C_SOURCE=200809L`.

Requires a POSIX system with `gcc` or `clang` and `make`. Developed on Ubuntu 24.04.

## Layout

```
src/        implementation (one module per concept: lexer, parser, executor, ...)
tests/      unit tests and an integration script comparing output against bash
Makefile    strict flags, auto header-dependency tracking, sanitizer-enabled debug
```
