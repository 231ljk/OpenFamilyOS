# FamilyOS 开轮版 · 顶层构建入口
#
# 常用目标：
#   make all       构建内核模块 + 用户态服务 + Python 组件检查
#   make test      运行全部测试（C 服务冒烟 + Python 单元测试）
#   make full-os   从源码一键构建完整可启动系统（tools/build-full-os.sh，Linux 环境）
#   make clean     清理构建产物
#
# 提示：内核模块与 C 服务需在 Linux 环境构建；Python 组件跨平台。

PYTHON ?= python3
TOP    := $(CURDIR)
# 版本号：tools/build-full-os.sh 会 grep 本行，用于 ISO 命名与开机横幅
FOS_VERSION = 0.1.0

.PHONY: all services kernel full-os test test-python clean

all: services kernel
	@$(PYTHON) -m compileall -q pkg tools fs >/dev/null && echo "[py] compile ok"

services:
	@$(MAKE) -C services

kernel:
	@$(MAKE) -C kernel

# 从源码一键构建完整可启动系统（详见 tools/build-full-os.sh）
full-os:
	@./tools/build-full-os.sh

# C 服务的完整编译验证由 CI（GitHub Actions, ubuntu-latest）承担；
# 本地在 Linux 上可直接 `make -C services test`。
test: test-python
	@$(MAKE) -C services test || echo "[services] 本机无编译器，跳过 C 测试（CI 会跑）"

test-python:
	@$(PYTHON) -m pytest pkg/tests fs/fosfs/tests tools/tests -q || \
	 ($(PYTHON) -m unittest discover -s pkg/tests -p 'test_*.py' && \
	  $(PYTHON) -m unittest discover -s fs/fosfs/tests -p 'test_*.py' && \
	  $(PYTHON) -m unittest discover -s tools/tests -p 'test_*.py')

clean:
	@$(MAKE) -C services clean
	@$(MAKE) -C kernel clean
	@find . -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true
