# fos-ai-bus-d + 默认模型插件
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE

BIN     := build/fosd
PLUGIN  := build/models/default_rule.so

all: $(BIN) $(PLUGIN)

$(BIN): main.c ../common/libfoscommon.a
	@mkdir -p build
	@$(CC) $(CFLAGS) -o $@ main.c ../common/libfoscommon.a $(LDLIBS)
	@echo "  LD    $@"

$(PLUGIN): models/model_default.c
	@mkdir -p build/models
	@$(CC) $(CFLAGS) -shared -fPIC -o $@ models/model_default.c
	@echo "  SO    $@"

clean:
	rm -rf build

.PHONY: all clean
