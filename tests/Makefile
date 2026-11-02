CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I..
CFLAGS += -I../gba/helper/include -DGEN3_TESTING

BUILD = build
TESTS = $(basename $(notdir $(wildcard test_*.c)))
HELPER_SRCS = $(wildcard ../gba/helper/src/*.c)
SRCS = $(filter-out %_furi.c,$(wildcard ../src/gen3/*.c)) $(wildcard fake_*.c) $(HELPER_SRCS)

.PHONY: all clean cart-tables
all: $(addprefix run-,$(TESTS))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: %.c $(wildcard *.h) $(SRCS) $(wildcard ../src/gen3/*.h) $(wildcard ../gba/helper/include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -DTEST_NAME='"$*"' $< $(SRCS) -o $@

run-%: $(BUILD)/%
	./$<

clean:
	rm -rf $(BUILD)

# The Cartridge app's table reader is gen3_data.c compiled in cart mode, which the suite includes
# itself, so the normal gen3_data.c stays out of its link.
$(BUILD)/test_gen3_cart_tables: test_gen3_cart_tables.c $(wildcard *.h) $(SRCS) $(wildcard ../src/gen3/*.h) ../src/gen3/gen3_data.c ../files/gen3_cart_tables.bin | $(BUILD)
	$(CC) $(CFLAGS) -DTEST_NAME='"test_gen3_cart_tables"' $< $(filter-out ../src/gen3/gen3_data.c,$(SRCS)) -o $@

# Regenerates files/gen3_cart_tables.bin from the in-code tables
cart-tables: | $(BUILD)
	$(CC) $(CFLAGS) gen_cart_tables.c ../src/gen3/gen3_data_gen.c -o $(BUILD)/gen_cart_tables
	$(BUILD)/gen_cart_tables ../files/gen3_cart_tables.bin
