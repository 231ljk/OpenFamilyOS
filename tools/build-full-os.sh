#!/usr/bin/env bash
# SPDX-License-Identifier: MulanPSL-2.0
# build-full-os.sh — 从源码一键构建完整可启动的 FamilyOS 系统（x86_64）
#
# 本脚本是「源码 → 完整操作系统」的构建链：
#   1. 下载 Linux 6.6 LTS 源码并编译内核（含 kernel/ 两个自研模块）
#   2. 下载 busybox 源码并静态编译（基础用户态）
#   3. 编译 services/ 全部 7 个守护进程 + ai-bus 默认模型
#   4. 组装 initramfs/rootfs（init 脚本、glibc 依赖、fos 模块与服务）
#   5. grub-mkrescue 生成可启动 ISO（BIOS/UEFI）
#
# 产出：<out>/familyos-open-wheel-<version>-x86_64.iso
#
# 依赖（Debian/Ubuntu）：
#   apt install build-essential wget bc flex bison libssl-dev libelf-dev cpio \
#               xorriso grub-pc-bin grub-efi-ia32-bin grub-efi-amd64-bin
# 用法：./tools/build-full-os.sh [--out DIR] [--arch x86_64]
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$TOP/out"
ARCH="x86_64"
JOBS="$(nproc 2>/dev/null || echo 2)"
KERNEL_MAJOR="6.6"
BUSYBOX_CHANNEL="1.36"

while [ $# -gt 0 ]; do
  case "$1" in
    --out)  OUT="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help)
      sed -n '3,22p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

[ "$ARCH" = "x86_64" ] || { echo "当前仅支持 --arch x86_64" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"; RFS="$WORK/rootfs"
mkdir -p "$SRC" "$RFS" "$OUT"

say() { echo; echo "==> $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# ---------- 0. 工具链自检 ----------
for t in gcc make wget tar cpio xz grub-mkrescue; do
  command -v "$t" >/dev/null || die "缺少工具: $t（见脚本头部依赖说明）"
done

# ---------- 1. 解析最新内核 / busybox 版本 ----------
say "[1/6] 解析 Linux $KERNEL_MAJOR / busybox $BUSYBOX_CHANNEL 最新版本"
KVER=$(wget -qO- "https://cdn.kernel.org/pub/linux/kernel/v6.x/" \
  | grep -oP "linux-$KERNEL_MAJOR\.\d+\.tar\.xz" | sort -V | tail -1 \
  | sed 's/linux-\(.*\)\.tar\.xz/\1/')
[ -n "$KVER" ] || die "无法解析 Linux $KERNEL_MAJOR 最新版本（网络问题？）"
BVER=$(wget -qO- "https://busybox.net/downloads/" \
  | grep -oP "busybox-$BUSYBOX_CHANNEL\.\d+\.tar\.bz2" | sort -V | tail -1 \
  | sed 's/busybox-\(.*\)\.tar\.bz2/\1/')
[ -n "$BVER" ] || die "无法解析 busybox $BUSYBOX_CHANNEL 最新版本"
FOS_VERSION=$(grep -oP 'FOS_VERSION\s*=\s*\K[0-9.]+' "$TOP/Makefile" 2>/dev/null || echo "0.1.0")
say "内核 $KVER · busybox $BVER · FamilyOS $FOS_VERSION"

# ---------- 2. 编译内核（含 fos 模块） ----------
say "[2/6] 下载并编译 Linux $KVER"
wget -q "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" -O "$SRC/kernel.tar.xz"
tar -xf "$SRC/kernel.tar.xz" -C "$SRC"
KS="$SRC/linux-$KVER"; cd "$KS"
make ARCH=$ARCH x86_64_defconfig >/dev/null
# 最小可启动覆盖配置：initramfs / devtmpfs / 虚拟化驱动（QEMU 可跑）/ 9p
cat >> .config <<'EOF'
CONFIG_BLK_DEV_INITRD=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_EXT4_FS=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_NET=y
CONFIG_9P_FS=y
CONFIG_9P_VIRTIO=y
CONFIG_NET_9P=y
CONFIG_TMPFS=y
CONFIG_FW_LOADER=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
EOF
make ARCH=$ARCH olddefconfig >/dev/null
make ARCH=$ARCH prepare >/dev/null
make ARCH=$ARCH modules_prepare >/dev/null

say "    编译 kernel/ 自研模块（out-of-tree，KDIR=$KS）"
make ARCH=$ARCH -C "$TOP/kernel" KDIR="$KS" modules >/dev/null
mkdir -p "$RFS/lib/modules"
cp "$TOP/kernel/super_ring/super_ring.ko" \
   "$TOP/kernel/health_usage/health_usage.ko" "$RFS/lib/modules/"

say "    编译内核镜像"
make ARCH=$ARCH -j"$JOBS" bzImage >/dev/null

# ---------- 3. 编译 busybox（静态） ----------
say "[3/6] 下载并静态编译 busybox $BVER"
wget -q "https://busybox.net/downloads/busybox-$BVER.tar.bz2" -O "$SRC/busybox.tar.bz2"
tar -xf "$SRC/busybox.tar.bz2" -C "$SRC"
cd "$SRC/busybox-$BVER"
make ARCH=$ARCH defconfig >/dev/null
sed -i 's|^# CONFIG_STATIC is not set|CONFIG_STATIC=y|' .config
make ARCH=$ARCH -j"$JOBS" >/dev/null
mkdir -p "$RFS/bin"
cp busybox "$RFS/bin/"
cd "$RFS/bin" && ./busybox --install -s

# ---------- 4. 编译 FamilyOS 服务与模型 ----------
say "[4/6] 编译 services/（7 个守护进程 + 默认模型）"
make -C "$TOP/services" clean >/dev/null 2>&1 || true
make -C "$TOP/services" all >/dev/null
mkdir -p "$RFS/sbin" "$RFS/usr/lib/fos/models"
for s in super-ring ai-bus home-bridge voice virt-compat migration health; do
  cp "$TOP/services/$s/build/fosd" "$RFS/sbin/fos-$s-d"
done
cp "$TOP/services/ai-bus/build/models/"*.so "$RFS/usr/lib/fos/models/" 2>/dev/null || true

# 收集 glibc 动态依赖（服务为动态链接）
say "    收集动态链接依赖"
mkdir -p "$RFS/lib/x86_64-linux-gnu" "$RFS/lib64"
for f in "$RFS"/sbin/fos-*-d; do
  ldd "$f" 2>/dev/null | awk '/=> \//{print $3} /^\s*\/lib/{print $1}' | sort -u
done | sort -u | while read -r lib; do
  base="${lib##*/}"
  case "$lib" in
    /lib64/*)      cp -L "$lib" "$RFS/lib64/$base" 2>/dev/null || true ;;
    /lib/*|/usr/lib/*) cp -L "$lib" "$RFS/lib/x86_64-linux-gnu/$base" 2>/dev/null || true ;;
  esac
done
LD="$(ls /lib64/ld-linux-x86-64.so.2 2>/dev/null || echo /lib/ld-linux-x86-64.so.2)"
cp -L "$LD" "$RFS/lib64/$(basename "$LD")" 2>/dev/null || true

# ---------- 5. 组装 initramfs ----------
say "[5/6] 组装 initramfs/rootfs"
mkdir -p "$RFS/proc" "$RFS/sys" "$RFS/dev" "$RFS/tmp" "$RFS/run" \
         "$RFS/etc/init.d" "$RFS/var/lib/fos"
cat > "$RFS/init" <<'INIT'
#!/bin/sh
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mount -t tmpfs tmpfs /tmp 2>/dev/null
mount -t tmpfs tmpfs /run 2>/dev/null
mkdir -p /run/fos /var/lib/fos
insmod /lib/modules/super_ring.ko   2>/dev/null || true
insmod /lib/modules/health_usage.ko 2>/dev/null || true
for s in super-ring ai-bus home-bridge voice virt-compat migration health; do
  /sbin/fos-$s-d --sock /run/fos/$s.sock &
done
echo "FamilyOS (Open Wheel Edition) $(cat /etc/fos-version 2>/dev/null) — /run/fos/*.sock 服务已启动"
exec setsid cttyhack sh -c 'echo; echo "控制台就绪（Ctrl-D 退出）"; exec /bin/sh'
INIT
chmod +x "$RFS/init"
echo "$FOS_VERSION" > "$RFS/etc/fos-version"
cat > "$RFS/etc/hostname" <<'EOF'
familyos
EOF
cat > "$RFS/etc/fstab" <<'EOF'
proc     /proc     proc     defaults 0 0
sysfs    /sys      sysfs    defaults 0 0
devtmpfs /dev      devtmpfs defaults 0 0
tmpfs    /tmp      tmpfs    defaults 0 0
tmpfs    /run      tmpfs    defaults 0 0
EOF

say "    打包 initramfs.cpio.gz"
( cd "$RFS" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$OUT/initramfs.cpio.gz" )

# ---------- 6. 生成可启动 ISO ----------
say "[6/6] grub-mkrescue 生成 ISO"
ISO="$OUT/familyos-open-wheel-$FOS_VERSION-$ARCH.iso"
mkdir -p "$WORK/iso/boot/grub"
cp "$KS/arch/x86/boot/bzImage" "$WORK/iso/boot/vmlinuz"
cp "$OUT/initramfs.cpio.gz"    "$WORK/iso/boot/initramfs.gz"
cat > "$WORK/iso/boot/grub/grub.cfg" <<'EOF'
set timeout=3
set default=0
menuentry "FamilyOS (Open Wheel Edition)" {
  linux  /boot/vmlinuz console=tty0 console=ttyS0,115200 quiet
  initrd /boot/initramfs.gz
}
EOF
grub-mkrescue -o "$ISO" "$WORK/iso" >/dev/null 2>&1
chmod 644 "$ISO"
ls -lh "$ISO" "$OUT/initramfs.cpio.gz"
echo
echo "完成。可启动镜像: $ISO"
echo "  QEMU 验证: qemu-system-x86_64 -cdrom $ISO -m 512M -boot d"
