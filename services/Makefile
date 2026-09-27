# FamilyOS 用户态服务构建（C / POSIX，仅依赖 libc 与 libdl）
#
#   make all               编译全部服务与公共库
#   make test              编译并跑冒烟测试（Linux）
#   make services-install  安装二进制与 systemd 单元（需 root）
#
# 目标平台：Linux（glibc 或 musl）。Windows/macOS 不参与编译。

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS  += -lpthread -ldl

SERVICES := super-ring ai-bus home-bridge voice virt-compat migration health health
SBINDIR  ?= /usr/bin
UNITDIR  ?= /usr/lib/systemd/system

ALL_BINS := $(addsuffix /build/fosd,$(SERVICES))

.PHONY: all test clean services-install $(SERVICES)

all: $(SERVICES)

# 公共库先编
common/libfoscommon.a:
	@$(MAKE) -C common

define SERVICE_rule
$(1): common/libfoscommon.a
	@$$(MAKE) -C $(1)
endef
$(foreach s,$(SERVICES),$(eval $(call SERVICE_rule,$(s))))

test: all
	@echo "== 冒烟测试：每个守护进程以 --self-test 启动并立即退出 =="
	@for s in $(SERVICES); do \
		./$$s/build/fosd --self-test || exit 1; \
		echo "  [ok] $$s"; \
	done

services-install: all
	install -d $(DESTDIR)$(SBINDIR)
	@for s in $(SERVICES); do \
		install -m 0755 $$s/build/fosd $(DESTDIR)$(SBINDIR)/fos-$$s-d; \
	done
	install -d $(DESTDIR)$(UNITDIR)
	@for s in $(SERVICES); do \
		install -m 0644 $$s/systemd/*.service $(DESTDIR)$(UNITDIR)/; \
	done
	@echo "已安装。启用： systemctl enable --now fos-<service>-d"

clean:
	@for s in $(SERVICES) common; do $(MAKE) -C $$s clean; done
