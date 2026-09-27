# services/common —— 静态库 libfoscommon.a
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE

OBJS := fos_json.o fos_ipc.o

libfoscommon.a: $(OBJS)
	@ar rcs $@ $(OBJS)

clean:
	rm -f $(OBJS) libfoscommon.a
