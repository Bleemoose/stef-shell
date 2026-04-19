# stef-shell
#
#   make debug   -> -O0 -g + AddressSanitizer + UBSan  (default)
#   make release -> -O2, no sanitizers
#   make run     -> build debug and run interactively
#   make clean   -> remove build artifacts
#
# The .d files are auto-generated header-dependency files (-MMD -MP).
# Touching a header causes the right .c files to rebuild automatically.

CC       := cc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Werror -Wpedantic -Wshadow -Wvla
BASE     := $(CSTD) $(WARN) -D_POSIX_C_SOURCE=200809L

SRC_DIR  := src
BUILD    := build
BIN      := stef-shell

SOURCES  := $(wildcard $(SRC_DIR)/*.c)
OBJECTS  := $(SOURCES:$(SRC_DIR)/%.c=$(BUILD)/%.o)
DEPS     := $(OBJECTS:.o=.d)

# Test binaries: one per tests/test_*.c. Each links against every object in
# src/ except main.o (each test file supplies its own main()). Adding a new
# test is just `touch tests/test_foo.c` -- the wildcard picks it up.
TEST_SRCS        := $(wildcard tests/test_*.c)
TEST_BINS        := $(TEST_SRCS:tests/%.c=$(BUILD)/%)
NON_MAIN_OBJECTS := $(filter-out $(BUILD)/main.o, $(OBJECTS))

.PHONY: all debug release run test clean
all: debug

debug:   CFLAGS  := $(BASE) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer
debug:   LDFLAGS := -fsanitize=address,undefined
debug:   $(BIN)

release: CFLAGS  := $(BASE) -O2 -DNDEBUG
release: LDFLAGS :=
release: $(BIN)

# Tests always build with sanitizers; any memory error is a failure.
test:    CFLAGS  := $(BASE) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer
test:    LDFLAGS := -fsanitize=address,undefined
test:    $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "==> $$t"; $$t || exit 1; done

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

# Pattern rule: build/test_foo from tests/test_foo.c + all non-main src objects.
$(BUILD)/test_%: tests/test_%.c $(NON_MAIN_OBJECTS) | $(BUILD)
	$(CC) $(CFLAGS) $< $(NON_MAIN_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: debug
	./$(BIN)

clean:
	rm -rf $(BUILD) $(BIN)

-include $(DEPS)
