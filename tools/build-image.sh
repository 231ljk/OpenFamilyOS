#!/usr/bin/env bash
# SPDX-License-Identifier: MulanPSL-2.0
# build-image.sh — FamilyOS 开轮版系统镜像构建（参考流程）
#
# 产出：<out>/familyos-open-wheel-<device>.fosimg
#   = rootfs 模板（rootfs/<device>/） + services 安装 + 镜像头（sha256）
#
# 依赖：Linux + debootstrap（或自备 rootfs tarball）+ 已 make services
# 用法：./tools/build-image.sh --device pc [--out out/] [--rootfs PATH]
#
# 本脚本刻意保持「可读的流程骨架」而非黑盒：每一步都能单独重跑，
# 社区按设备配方扩展（见 devices/<name>/README.md）。
set -euo pipefail

DEVICE="pc"
OUT="out"
ROOTFS=""
TOP="$(cd "$(dirname "$0")/.." && pwd)"

while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    --rootfs) ROOTFS="$2"; shift 2 ;;
    -h|--help)
      echo "用法: $0 --device pc|tablet|phone|tv|watch [--out DIR] [--rootfs PATH]"
      exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/rootfs"
mkdir -p "$STAGE" "$OUT"

echo "==> [1/4] 基线 rootfs（device=$DEVICE）"
if [ -n "$ROOTFS" ]; then
  tar -xf "$ROOTFS" -C "$STAGE"
else
  command -v debootstrap >/dev/null || {
    echo "缺少 debootstrap：安装它（apt install debootstrap）或用 --rootfs 自备基线" >&2
    exit 1
  }
  # 基线版本与 devices/$DEVICE/README.md 的配方一致
  debootstrap --variant=minbase --arch=arm64 bookworm "$STAGE" \
    "http://deb.debian.org/debian" || \
    debootstrap --variant=minbase --arch=amd64 bookworm "$STAGE" \
      "http://deb.debian.org/debian"
fi

echo "==> [2/4] 安装 FamilyOS 服务与工具"
make -C "$TOP/services" all
DESTDIR="$STAGE" make -C "$TOP/services" services-install
mkdir -p "$STAGE/usr/lib/fos/models" "$STAGE/etc/fos"
cp "$TOP/services/ai-bus/build/models/"*.so \
   "$STAGE/usr/lib/fos/models/" 2>/dev/null || true
for svc in super-ring ai-bus home-bridge voice virt-compat migration health; do
  ln -sf "/usr/lib/systemd/system/fos-$svc-d.service" \
    "$STAGE/etc/systemd/system/multi-user.target.wants/fos-$svc-d.service"
done

echo "==> [3/4] 打包镜像（ext4 环回或直接 tar，取决于设备配方）"
IMG="$WORK/rootfs.img"
SIZE=$(( $(du -sb "$STAGE" | cut -f1) * 13 / 10 ))           # 1.3x 余量
SIZE=$(( (SIZE + 65535) / 65536 * 65536 ))
dd if=/dev/zero of="$IMG" bs=1M count=0 seek=$((SIZE/1048576)) status=none
if command -v mkfs.ext4 >/dev/null && command -v fakeroot >/dev/null; then
  mnt="$WORK/mnt"; mkdir -p "$mnt"
  mkfs.ext4 -q -d "$STAGE" "$IMG" 2>/dev/null || {
    # -d 需要 e2fsprogs ≥1.43；失败退 loop 挂载（需 root）
    mkfs.ext4 -q "$IMG"
    [ "$(id -u)" = 0 ] || { echo "需要 root 做环回挂载" >&2; exit 1; }
    mount -o loop "$IMG" "$mnt"; cp -a "$STAGE/." "$mnt/"; umount "$mnt"; }
fi
PAYLOAD="$IMG"; [ -f "$IMG" ] || PAYLOAD="$STAGE"   # 兜底：直接以目录为负载

echo "==> [4/4] 生成 .fosimg（镜像头 + 负载，见 tools/familyos_flash/image.py）"
python3 - "$TOP" "$DEVICE" "$OUT" "$PAYLOAD" <<'PY'
import hashlib, os, sys, time
sys.path.insert(0, os.path.join(sys.argv[1], "tools"))
from familyos_flash.image import ImageHeader

top, device, out, payload = sys.argv[1:5]
if os.path.isdir(payload):                      # 目录 → tar 作负载
    tmp = payload + ".tar"
    os.system(f"tar -C {payload} -cf {tmp} .")
    payload = tmp

with open(payload, "rb") as f:
    h = hashlib.sha256()
    for chunk in iter(lambda: f.read(1 << 20), b""):
        h.update(chunk)
hdr = ImageHeader(device=device, part="system_a", kind="tar"
                  if payload.endswith(".tar") else "ext4",
                  size=os.path.getsize(payload), sha256=h.digest(),
                  seq=int(time.time()), created_ms=int(time.time()*1000))
dst = os.path.join(out, f"familyos-open-wheel-{device}.fosimg")
with open(dst, "wb") as f:
    f.write(hdr.pack())
    with open(payload, "rb") as s:
        for chunk in iter(lambda: s.read(1 << 20), b""):
            f.write(chunk)
print(f"✓ {dst}（{hdr.size} 字节负载, sha256={hdr.sha256.hex()[:16]}…）")
print(f"  刷机: python3 tools/familyos_flash/cli.py flash {dst} --to <目标> --confirm")
PY

echo "完成。镜像: $OUT/familyos-open-wheel-$DEVICE.fosimg"
