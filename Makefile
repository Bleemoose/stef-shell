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

.PHONY: all debug release run clean
all: debug

debug:   CFLAGS  := $(BASE) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer
debug:   LDFLAGS := -fsanitize=address,undefined
debug:   $(BIN)

release: CFLAGS  := $(BASE) -O2 -DNDEBUG
release: LDFLAGS :=
release: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: debug
	./$(BIN)

clean:
	rm -rf $(BUILD) $(BIN)

-include $(DEPS)
