# FamilyOS 内核模块 out-of-tree 构建
#
# 用法：
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) clean
#
# 顶层 `make kernel` 会调用本 Makefile（自动定位内核构建树）。

obj-m += super_ring/
obj-m += health_usage/

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: modules clean

modules:
	@test -d "$(KDIR)" || { \
		echo "kernel/Makefile: 未找到内核构建树 $(KDIR)"; \
		echo "  Debian/Ubuntu: sudo apt install build-essential linux-headers-\$$(uname -r)"; \
		echo "  Fedora:        sudo dnf install gcc make kernel-devel"; \
		exit 1; }
	@$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	@$(MAKE) -C $(KDIR) M=$(CURDIR) clean
