# 构建指南（BUILD）

> 开轮版基于 **Linux 原生内核**（不改源码树，能力全部以模块叠加）。本文覆盖：环境准备、内核模块、用户态服务、
> Python 组件、镜像打包与刷机。目标设备支持矩阵见 [`devices/`](devices)。

## 1. 环境准备

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) \
    python3 python3-pytest pkg-config systemd-dev
```

### Fedora / RHEL

```bash
sudo dnf install -y gcc make kernel-devel kernel-headers \
    python3 pytest systemd-devel
```

### Arch

```bash
sudo pacman -S base-devel linux-headers python pytest
```

### Windows / macOS（仅限开发与测试 Python 组件）

```bash
python -m pip install pytest
make test-python        # 或: python -m pytest pkg/tests tools/tests -q
```

内核模块与 C 服务**必须在 Linux 上构建**；Windows 用户建议 WSL2 + `linux-headers`。

## 2. 内核层

开轮版**使用 Linux 原生内核，不 fork、不魔改源码树**，能力全部以「基线 + 可加载模块」的方式叠加：

```bash
make kernel             # 构建 kernel/ 下全部模块
sudo insmod kernel/super_ring/super_ring.ko
sudo insmod kernel/health_usage/health_usage.ko
```

- 内核基线版本：见 [`kernel/README.md`](kernel/README.md)（当前对齐 6.x LTS）。
- 配置片段（Kconfig）与补丁组织方式同见该文档。

## 3. 用户态服务

```bash
make services           # 构建 services/ 下全部守护进程
sudo make services-install   # 安装到 /usr/{bin,lib,share} 并注册 systemd 单元
```

每个服务对应一个 systemd unit（`services/*/systemd/*.service`），互相通过
Unix socket 行协议通信（帧格式见 [`docs/ipc-protocol.md`](docs/ipc-protocol.md)）。

## 4. Python 组件

`pkg/`（.fos 打包与转包）、`tools/familyos_flash/`（刷机维护）、
`fs/fosfs/`（存储格式参考实现）均为零第三方依赖的纯标准库实现：

```bash
python3 -m pkg build examples/hello-app      # 打一个 .fos 包
python3 tools/familyos_flash/cli.py --help   # 刷机与维护工具
make test-python                             # 全部单元测试
```

## 5. 系统镜像

开轮版镜像 = Linux 基线 rootfs + FamilyOS 模块 + 服务 + 工具：

```bash
./tools/build-image.sh --device pc --out out/familyos-open-wheel-<device>.img
```

脚本说明与依赖见 [`tools/build-image.sh`](tools/build-image.sh) 头部注释。

### 5.1 从源码一键构建「完整可启动系统」（推荐）

`tools/build-full-os.sh` 是**全源码构建链**：下载 Linux 6.6 LTS 与 busybox 源码，
编译内核（含 `kernel/` 两个自研模块）、7 个守护进程与默认模型，组装
initramfs/rootfs，最终用 grub 生成**可启动 ISO**（BIOS/UEFI）：

```bash
# 依赖（Debian/Ubuntu）：
sudo apt install -y build-essential wget bc flex bison libssl-dev libelf-dev \
    cpio xorriso grub-pc-bin grub-efi-ia32-bin grub-efi-amd64-bin mtools systemd-dev

make full-os                      # 等价于 ./tools/build-full-os.sh
# 产出: out/familyos-open-wheel-<version>-x86_64.iso
# 验证: qemu-system-x86_64 -cdrom out/familyos-open-wheel-*.iso -m 512M -boot d
```

构建链每次在 CI（`build-full-os` job）自动跑通，**源码 → 完整可启动系统**的
闭环由 CI 持续验证；最新可启动镜像以 Actions artifact 形式提供。

## 6. 刷机与维护

统一使用 [`tools/`](tools) 下的 `familyos_flash` 工具：

| 能力 | 命令 |
| --- | --- |
| 系统镜像写入 | `python3 tools/familyos_flash/cli.py flash <image> --to <disk>` |
| 数据备份/恢复 | `... backup --partition userdata --out file.fosbak` / `restore ...` |
| 环境切换（A/B） | `... slot switch` |
| 救砖模式 | `... rescue --image <image>` |

所有真机写操作默认 `--dry-run`，加 `--confirm` 才会执行 —— 防止误刷。

## 7. 目标设备支持矩阵

见 [`devices/README.md`](devices/README.md)。

## 8. CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) 在 ubuntu-latest 上执行：
内核模块编译（modpost）→ C 服务编译与冒烟测试 → Python 全量测试 →
**全源码构建完整可启动系统**（`build-full-os`，产出可启动 ISO 并上传 artifact）。
