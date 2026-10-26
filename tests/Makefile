CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I..

BUILD = build
TESTS = $(basename $(notdir $(wildcard test_*.c)))
SRCS = $(filter-out %_furi.c,$(wildcard ../src/gen3/*.c)) $(wildcard fake_*.c)

.PHONY: all clean
all: $(addprefix run-,$(TESTS))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: %.c $(wildcard *.h) $(SRCS) $(wildcard ../src/gen3/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -DTEST_NAME='"$*"' $< $(SRCS) -o $@

run-%: $(BUILD)/%
	./$<

clean:
	rm -rf $(BUILD)
