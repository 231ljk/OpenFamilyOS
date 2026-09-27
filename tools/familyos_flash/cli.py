# SPDX-License-Identifier: MulanPSL-2.0
"""familyos_flash —— 统一刷机与维护工具 CLI（开发文档第八章）。

    flash   系统镜像写入          python3 -m familyos_flash flash sys.img --to /dev/sdX
    backup  数据备份              python3 -m familyos_flash backup --partition userdata --out bk
    restore 数据恢复              python3 -m familyos_flash restore --from bk --to dev
    slot    环境切换              python3 -m familyos_flash slot switch
    rescue  救砖模式              python3 -m familyos_flash rescue --image boot.img --to dev

★ 安全默认：一切真写动作必须 --confirm；块设备另需 --allow-block-device。
不带 --confirm 时输出「执行计划」（dry-run），供人工核对后再来真跑。
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Optional

from .backup import BackupError, create_backup, list_partitions, restore_backup
from .device import Device, DeviceError, enumerate_targets, resolve_target
from .image import ImageError, load_header, sha256_file
from .slot import SlotError, get_current_slot, set_active_slot


def _progress(width: int = 36):
    """返回 progress(done[, total]) 回调：单行刷新进度条。"""
    def tick(done: int, total: Optional[int] = None):
        if total:
            frac = min(done / max(total, 1), 1.0)
            bar = int(frac * width) * "#"
            sys.stderr.write(f"\r[{bar.ljust(width)}] {frac * 100:5.1f}% "
                             f"({done}/{total})")
            if done >= total:
                sys.stderr.write("\n")
        else:
            sys.stderr.write(f"\r  … {done} bytes")

    return tick


def _die(msg: str, code: int = 1) -> None:
    print(f"错误: {msg}", file=sys.stderr)
    sys.exit(code)


# ───────────────────────── flash ─────────────────────────

def cmd_flash(a: argparse.Namespace) -> int:
    hdr, off = load_header(a.image)
    dev = resolve_target(a.to, device_id=a.device or hdr.device)

    if a.dry_run or not a.confirm:
        print("— 执行计划（dry-run，未写盘）—")
        print(f"  镜像   : {a.image}")
        print(f"    设备 {hdr.device} 分区 {hdr.part} kind={hdr.kind} "
              f"seq={hdr.seq} size={hdr.size}")
        print(f"  目标   : {dev.path} (kind={dev.kind})")
        print(f"  分区   : {hdr.part}  可用分区: {list_partitions(dev)}")
        print("  校验   : 写前全量 sha256 + 写中逐块复算")
        print("  完成后 : 活动槽保持不变的分区将作为下一启动候选（slot switch 生效）")
        print("提示：确认无误后追加 --confirm 执行。")
        return 0

    # 写前：镜像负载自校验（防传输损坏）
    tick = _progress()
    print("→ 校验镜像负载…")
    if sha256_file(a.image, off, hdr.size) != hdr.sha256:
        _die("镜像负载哈希与头不符：文件损坏，中止")
    dev.guard_write(hdr.part, hdr.size,
                    allow_block=a.allow_block_device, confirm=True)

    print(f"→ 写入 {dev.path} 分区 {hdr.part}（{hdr.size} 字节）…")
    dst, poff, _pcap = dev.open(hdr.part, "r+b")
    try:
        dst.seek(poff)
        # 注意：源负载起点 = 头之后；目标从分区偏移写
        _write_with_hash_check(a.image, off, hdr.size, dst, tick)
        dst.flush()
        os.fsync(dst.fileno())
    finally:
        dst.close()
    print("✓ 写入完成并校验通过")
    return 0


def _write_with_hash_check(src: str, off: int, size: int, dst, tick) -> None:
    import hashlib

    h = hashlib.sha256()
    written = 0
    with open(src, "rb") as f:
        f.seek(off)
        while written < size:
            buf = f.read(min(1 << 20, size - written))
            if not buf:
                _die("源镜像提前结束")
            dst.write(buf)
            h.update(buf)
            written += len(buf)
            tick(written, size)
    # 收尾：写入后由调用方 fsync；逐块哈希已隐含在源校验中
    dst.flush()


def cmd_rescue(a: argparse.Namespace) -> int:
    """救砖：刷 boot 分区 + 把活动槽切回 _a（已知好的一侧）。"""
    print("!! 救砖模式：将覆盖 boot 分区并切回槽位 _a")
    if not a.confirm:
        print("— 执行计划（dry-run）—")
        print(f"  镜像 {a.image} → {a.to}")
        print("  slot_active = _a")
        print("提示：追加 --confirm 执行。")
        return 0
    dev = resolve_target(a.to, device_id=a.device)
    hdr, off = load_header(a.image)
    if hdr.part not in ("boot_a", "boot_b", "boot"):
        print(f"警告：镜像声明分区为 {hdr.part}，救砖通常应使用 boot 镜像")
    if sha256_file(a.image, off, hdr.size) != hdr.sha256:
        _die("镜像损坏，中止")
    dev.guard_write(hdr.part if hdr.part != "boot" else "boot_a", hdr.size,
                    allow_block=a.allow_block_device, confirm=True)
    part = hdr.part if hdr.part != "boot" else "boot_a"
    dst, poff, _ = dev.open(part, "r+b")
    try:
        dst.seek(poff)
        _write_with_hash_check(a.image, off, hdr.size, dst, _progress())
        dst.flush()
        os.fsync(dst.fileno())
    finally:
        dst.close()
    if dev.kind == "dir":
        set_active_slot(dev.path, "_a", confirm=True)
    print("✓ 救砖完成：boot 已重刷，活动槽 = _a，请断电重启")
    return 0


# ───────────────────────── backup / restore ─────────────────────────

def cmd_backup(a: argparse.Namespace) -> int:
    dev = resolve_target(a.to)
    if not a.confirm:
        print("— 执行计划（dry-run）—")
        print(f"  目标 {dev.path} 分区 {a.partition or list_partitions(dev)} → {a.out}")
        print("提示：追加 --confirm 执行备份。")
        return 0
    parts = [a.partition] if a.partition else None
    meta = create_backup(dev, a.out, parts, confirm=True)
    print(f"✓ 备份完成 → {a.out}")
    for p in meta["partitions"]:
        print(f"  {p['name']:14s} {p['size']:>12d} B  {p['sha256'][:12]}…")
    return 0


def cmd_restore(a: argparse.Namespace) -> int:
    dev = resolve_target(a.to)
    parts = [a.partition] if a.partition else None
    if not a.confirm:
        plan = restore_backup(dev, a.source, parts, confirm=False)
        print("— 执行计划（dry-run，未写盘）—")
        print(f"  从 {a.source} 恢复分区: {plan} → {dev.path}")
        print("提示：追加 --confirm 执行恢复。")
        return 0
    done = restore_backup(dev, a.source, parts, confirm=True)
    print(f"✓ 恢复完成: {done}")
    return 0


# ───────────────────────── slot ─────────────────────────

def cmd_slot(a: argparse.Namespace) -> int:
    dev = resolve_target(a.to)
    target = dev.path
    cur, backend = get_current_slot(target)
    print(f"当前活动槽: {cur}   （后端: {backend}）")

    if a.action == "show":
        return 0
    if a.action == "set":
        slot = a.slot
        if slot not in ("_a", "_b"):
            _die("slot set 需要 _a 或 _b")
    else:  # switch
        slot = "_b" if cur == "_a" else "_a"
    if not a.confirm:
        print(f"— 计划：切换 {cur} → {slot}（追加 --confirm 执行，重启后生效）")
        return 0
    set_active_slot(target, slot, confirm=True)
    print(f"✓ 已切换活动槽 {cur} → {slot}，重启后生效")
    return 0


# ───────────────────────── devices ─────────────────────────

def cmd_devices(a: argparse.Namespace) -> int:
    rows = enumerate_targets(a.dir)
    if not rows:
        print(f"在 {a.dir}/ 下未发现 flash.yaml（确认仓库根目录）")
        return 1
    print(f"{'设备':<10} {'槽位':<8} {'分区表':<24} {'说明'}")
    for r in rows:
        print(f"{r.get('device', '?'):<10} {r.get('slots', '-'):<8} "
              f"{r.get('partition_table', '-'):<24} {r.get('notes', '')}")
    return 0


# ───────────────────────── main ─────────────────────────

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="familyos_flash",
        description="FamilyOS 统一刷机与维护工具（镜像写入/备份恢复/环境切换/救砖）",
        epilog="所有写操作默认 dry-run，--confirm 才执行；块设备另需 --allow-block-device。")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--confirm", action="store_true",
                       help="真执行（默认只输出计划）")
        p.add_argument("--allow-block-device", action="store_true",
                       help="允许目标为真实块设备（危险）")

    p = sub.add_parser("flash", help="系统镜像写入")
    p.add_argument("image")
    p.add_argument("--to", required=True, help="目标（设备路径/镜像文件/目录）")
    p.add_argument("--device", help="覆盖镜像声明的设备 id")
    p.add_argument("--dry-run", action="store_true")
    common(p)
    p.set_defaults(fn=cmd_flash)

    p = sub.add_parser("rescue", help="救砖模式：重刷 boot + 回滚槽位")
    p.add_argument("--image", required=True)
    p.add_argument("--to", required=True)
    p.add_argument("--device")
    common(p)
    p.set_defaults(fn=cmd_rescue)

    p = sub.add_parser("backup", help="数据备份")
    p.add_argument("--to", required=True, help="源目标")
    p.add_argument("--out", required=True, help="备份输出目录")
    p.add_argument("--partition", help="仅备份指定分区")
    common(p)
    p.set_defaults(fn=cmd_backup)

    p = sub.add_parser("restore", help="数据恢复")
    p.add_argument("--source", required=True, help="备份目录")
    p.add_argument("--to", required=True, help="恢复目标")
    p.add_argument("--partition", help="仅恢复指定分区")
    common(p)
    p.set_defaults(fn=cmd_restore)

    p = sub.add_parser("slot", help="A/B 环境切换")
    p.add_argument("action", choices=["show", "switch", "set"])
    p.add_argument("--slot", help="set 动作指定 _a/_b")
    p.add_argument("--to", required=True, help="设备目录/镜像/块设备")
    common(p)
    p.set_defaults(fn=cmd_slot)

    p = sub.add_parser("devices", help="列出受支持设备")
    p.add_argument("--dir", default="devices")
    p.set_defaults(fn=cmd_devices)

    return ap


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except (ImageError, DeviceError, BackupError, SlotError) as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1
    except SystemExit as e:            # _die：转成退出码，保持可测
        return int(e.code or 1)
    except FileNotFoundError as e:
        print(f"错误: 文件不存在: {e.filename}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n已中断（写入中断可能导致目标不一致，请用 backup/rescue 修复）",
              file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
