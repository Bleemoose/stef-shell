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

# Test binary links everything in src/ except main.o (tests supply their own main).
TEST_SRC         := tests/test_lexer.c
TEST_BIN         := $(BUILD)/test_lexer
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
test:    $(TEST_BIN)
	$(TEST_BIN)

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(TEST_BIN): $(TEST_SRC) $(NON_MAIN_OBJECTS) | $(BUILD)
	$(CC) $(CFLAGS) $(TEST_SRC) $(NON_MAIN_OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: debug
	./$(BIN)

clean:
	rm -rf $(BUILD) $(BIN)

-include $(DEPS)
