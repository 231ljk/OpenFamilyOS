# SPDX-License-Identifier: MIT
"""familyos_flash 测试：镜像格式 / 目标守卫 / A/B 槽 / 备份恢复 / CLI。

全部在临时目录内用「目录槽位」与「镜像文件」模拟设备，
绝不触碰真实块设备——守卫逻辑本身就是被测试对象。
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from familyos_flash import cli                     # noqa: E402
from familyos_flash.backup import (BackupError, create_backup,   # noqa: E402
                                   list_partitions, restore_backup)
from familyos_flash.device import (Device, DeviceError,            # noqa: E402
                                   enumerate_targets, resolve_target)
from familyos_flash.image import (ImageError, load_header,        # noqa: E402
                                  make_placeholder, sha256_file)
from familyos_flash.slot import (SlotError, get_current_slot,     # noqa: E402
                                 set_active_slot)

PAYLOAD = bytes(range(256)) * 64          # 16 KiB，可压缩性无所谓


class TempCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.w = self.dir.name

    def p(self, *parts):
        return os.path.join(self.w, *parts)

    def tearDown(self):
        self.dir.cleanup()


# ─────────────────── image ───────────────────

class TestImage(TempCase):
    def test_header_roundtrip(self):
        img = self.p("sys.fosimg")
        hdr = make_placeholder(img, device="phone-x1", part="system_a",
                               payload=PAYLOAD)
        got, off = load_header(img)
        self.assertEqual(got.device, "phone-x1")
        self.assertEqual(got.part, "system_a")
        self.assertEqual(got.size, len(PAYLOAD))
        self.assertEqual(off, 4096)
        self.assertEqual(got.sha256, hdr.sha256)

    def test_truncated_detected(self):
        img = self.p("cut.fosimg")
        make_placeholder(img, payload=PAYLOAD)
        size = os.path.getsize(img)
        with open(img, "r+b") as f:
            f.truncate(size - 100)
        with self.assertRaises(ImageError):
            load_header(img)

    def test_sha256_window(self):
        img = self.p("h.fosimg")
        hdr = make_placeholder(img, payload=PAYLOAD)
        self.assertEqual(sha256_file(img, 4096, hdr.size), hdr.sha256)
        # 篡改负载 → 校验不符
        with open(img, "r+b") as f:
            f.seek(4096 + 10)
            f.write(b"\xff\xff\xff\xff")
        self.assertNotEqual(sha256_file(img, 4096, hdr.size), hdr.sha256)

    def test_not_image(self):
        junk = self.p("junk")
        with open(junk, "wb") as f:
            f.write(b"x" * 100)
        with self.assertRaises(ImageError):
            load_header(junk)


# ─────────────────── device ───────────────────

class TestDevice(TempCase):
    def test_probe_kinds(self):
        d = self.p("devslot")
        os.makedirs(d)
        self.assertEqual(Device.probe(d).kind, "dir")
        f = self.p("disk.img")
        open(f, "wb").close()
        self.assertEqual(Device.probe(f).kind, "file")
        # 块设备路径模式（Windows 下不存在该路径，probe 归为 file/缺省）
        dev = Device(path="/dev/sdz", kind="block")
        self.assertEqual(dev.kind, "block")

    def test_partition_table_file(self):
        f = self.p("disk2.img")
        with open(f, "wb") as x:
            x.write(b"\0" * 1024 * 1024)
        with open(f + ".tab", "w", encoding="utf-8") as x:
            x.write("boot_a 0 65536\nsystem_a 65536 262144\n")
        dev = Device.probe(f)
        self.assertEqual([p.name for p in dev.parts],
                         ["boot_a", "system_a"])
        self.assertEqual(dev.partition("system_a").offset, 65536)
        with self.assertRaises(DeviceError):
            dev.partition("nope")

    def test_guard_requires_confirm(self):
        dev = Device(path=self.p("x.img"), kind="file")
        with self.assertRaises(DeviceError):
            dev.guard_write("system_a", 10, confirm=False)

    def test_guard_blocks_block_device(self):
        dev = Device(path="/dev/nvme0n1", kind="block")
        with self.assertRaises(DeviceError) as cm:
            dev.guard_write("system_a", 10, allow_block=False, confirm=True)
        self.assertIn("块设备", str(cm.exception))

    def test_guard_capacity(self):
        f = self.p("small.img")
        with open(f, "wb") as x:
            x.write(b"\0" * 1024)
        dev = Device.probe(f)
        with self.assertRaises(ImageError):
            dev.guard_write("whole", 1024 * 1024, confirm=True)

    def test_dir_slot_open(self):
        d = self.p("slotdir")
        os.makedirs(d)
        dev = Device.probe(d)
        fh, off, _ = dev.open("system_a", "r+b")
        self.assertEqual(off, 0)
        self.assertEqual(fh.name, os.path.join(d, "system_a.img"))
        fh.close()
        # dir 设备动态发现已存在的分区文件
        self.assertEqual(list_partitions(dev), ["system_a"])

    def test_enumerate_targets(self):
        root = self.p("devices")
        os.makedirs(os.path.join(root, "phone-x1"))
        with open(os.path.join(root, "phone-x1", "flash.yaml"),
                  "w", encoding="utf-8") as f:
            f.write("device: phone-x1\nslots: _a/_b\n"
                    "partition_table: partition-table.txt\n")
        rows = enumerate_targets(root)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["device"], "phone-x1")
        self.assertEqual(enumerate_targets(self.p("nope")), [])


# ─────────────────── slot ───────────────────

class TestSlot(TempCase):
    def test_file_backend_switch(self):
        d = self.p("devslot")
        os.makedirs(d)
        cur, backend = get_current_slot(d)
        self.assertEqual(cur, "_a")
        self.assertEqual(backend, "file")

        with self.assertRaises(SlotError):
            set_active_slot(d, "_b", confirm=False)     # 未确认
        set_active_slot(d, "_b", confirm=True)
        self.assertEqual(get_current_slot(d)[0], "_b")

        with self.assertRaises(SlotError):
            set_active_slot(d, "_c", confirm=True)      # 非法槽

    def test_unknown_state_defaults_a(self):
        d = self.p("devslot2")
        os.makedirs(d)
        with open(os.path.join(d, "slot_current"), "w") as f:
            f.write("garbage")
        self.assertEqual(get_current_slot(d)[0], "_a")


# ─────────────────── backup / restore ───────────────────

class TestBackupRestore(TempCase):
    def _make_dir_dev(self):
        d = self.p("devslot")
        os.makedirs(d)
        with open(os.path.join(d, "system_a.img"), "wb") as f:
            f.write(b"SYSTEM-OLD" * 100)
        with open(os.path.join(d, "boot_a.img"), "wb") as f:
            f.write(b"BOOT-DATA" * 50)
        return d, Device.probe(d)

    def test_backup_plan_requires_confirm(self):
        d, dev = self._make_dir_dev()
        with self.assertRaises(BackupError):
            create_backup(dev, self.p("bk"), confirm=False)
        self.assertFalse(os.path.exists(self.p("bk")))

    def test_backup_then_overwrite_then_restore(self):
        d, dev = self._make_dir_dev()
        meta = create_backup(dev, self.p("bk"),
                             partitions=["system_a"], confirm=True)
        self.assertEqual(len(meta["partitions"]), 1)
        self.assertTrue(os.path.isfile(self.p("bk", "system_a.img")))
        with open(self.p("bk", "meta.json"), encoding="utf-8") as f:
            json.load(f)

        # 覆盖破坏原内容
        with open(os.path.join(d, "system_a.img"), "wb") as f:
            f.write(b"WRONG" * 300)

        # dry-run：只回计划，不写
        plan = restore_backup(dev, self.p("bk"), ["system_a"], confirm=False)
        self.assertEqual(plan, ["system_a"])
        with open(os.path.join(d, "system_a.img"), "rb") as f:
            self.assertTrue(f.read().startswith(b"WRONG"))

        # 真恢复
        done = restore_backup(dev, self.p("bk"), ["system_a"], confirm=True)
        self.assertEqual(done, ["system_a"])
        with open(os.path.join(d, "system_a.img"), "rb") as f:
            self.assertEqual(f.read(), b"SYSTEM-OLD" * 100)

    def test_restore_rejects_corrupt_backup(self):
        _d, dev = self._make_dir_dev()
        create_backup(dev, self.p("bk2"), ["system_a"], confirm=True)
        fp = self.p("bk2", "system_a.img")
        with open(fp, "r+b") as f:
            f.seek(5)
            f.write(b"EVIL")
        with self.assertRaises(BackupError):
            restore_backup(dev, self.p("bk2"), confirm=True)

    def test_backup_out_exists_rejected(self):
        _d, dev = self._make_dir_dev()
        os.makedirs(self.p("bk3"))
        with self.assertRaises(BackupError):
            create_backup(dev, self.p("bk3"), confirm=True)

    def test_backup_missing_partition(self):
        _d, dev = self._make_dir_dev()
        with self.assertRaises(BackupError):
            create_backup(dev, self.p("bk4"), ["nonexistent"],
                          confirm=True)


# ─────────────────── CLI ───────────────────

class TestCli(TempCase):
    def _slot_dev(self):
        d = self.p("devslot")
        os.makedirs(d)
        with open(os.path.join(d, "system_a.img"), "wb") as f:
            f.write(b"\0" * 4096)
        return d

    def test_flash_dry_run_then_confirm(self):
        d = self._slot_dev()
        img = self.p("sys.fosimg")
        make_placeholder(img, device="pc", part="system_a", payload=PAYLOAD)

        self.assertEqual(
            cli.main(["flash", img, "--to", d]), 0)       # dry-run 默认
        with open(os.path.join(d, "system_a.img"), "rb") as f:
            self.assertEqual(f.read(4), b"\0\0\0\0")      # 未被写入

        self.assertEqual(
            cli.main(["flash", img, "--to", d, "--confirm"]), 0)
        with open(os.path.join(d, "system_a.img"), "rb") as f:
            self.assertEqual(f.read(len(PAYLOAD)), PAYLOAD)

    def test_flash_detects_corrupt_image(self):
        d = self._slot_dev()
        img = self.p("bad.fosimg")
        make_placeholder(img, part="system_a", payload=PAYLOAD)
        with open(img, "r+b") as f:
            f.seek(5000)
            f.write(b"\xde\xad\xbe\xef")
        self.assertEqual(
            cli.main(["flash", img, "--to", d, "--confirm"]), 1)

    def test_rescue_dry_run_and_confirm(self):
        d = self._slot_dev()
        with open(os.path.join(d, "boot_a.img"), "wb") as f:
            f.write(b"\0" * 2048)
        img = self.p("boot.fosimg")
        make_placeholder(img, part="boot_a", payload=PAYLOAD[:2048])

        self.assertEqual(
            cli.main(["rescue", "--image", img, "--to", d]), 0)
        self.assertEqual(
            cli.main(["rescue", "--image", img, "--to", d,
                      "--confirm"]), 0)
        self.assertEqual(get_current_slot(d)[0], "_a")

    def test_slot_cli(self):
        d = self._slot_dev()
        self.assertEqual(cli.main(["slot", "show", "--to", d]), 0)
        self.assertEqual(cli.main(["slot", "switch", "--to", d]), 0)  # dry-run
        self.assertEqual(get_current_slot(d)[0], "_a")
        self.assertEqual(cli.main(["slot", "switch", "--to", d,
                                   "--confirm"]), 0)
        self.assertEqual(get_current_slot(d)[0], "_b")
        self.assertEqual(cli.main(["slot", "set", "--slot", "_a",
                                   "--to", d, "--confirm"]), 0)

    def test_backup_restore_cli(self):
        d = self._slot_dev()
        self.assertEqual(cli.main(["backup", "--to", d, "--out",
                                   self.p("bk")]), 0)          # dry-run
        self.assertFalse(os.path.exists(self.p("bk")))
        self.assertEqual(cli.main(["backup", "--to", d, "--out",
                                   self.p("bk"), "--confirm"]), 0)
        self.assertEqual(cli.main(["restore", "--source", self.p("bk"),
                                   "--to", d]), 0)             # dry-run
        self.assertEqual(cli.main(["restore", "--source", self.p("bk"),
                                   "--to", d, "--confirm"]), 0)

    def test_devices_cli(self):
        self.assertEqual(cli.main(["devices", "--dir",
                                   self.p("nodir")]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
