# fos-super-ring-d
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE

BIN := build/fosd

$(BIN): main.c ../common/libfoscommon.a
	@mkdir -p build
	@$(CC) $(CFLAGS) -o $@ main.c ../common/libfoscommon.a $(LDLIBS)
	@echo "  LD    $@"

clean:
	rm -rf build

.PHONY: clean
