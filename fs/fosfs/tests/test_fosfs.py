# SPDX-License-Identifier: MIT
"""fosfs 参考实现测试：往返/压缩/快照/回滚/损坏检测/compact/TLV。"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from fosfs import CorruptError, FosFSError, FosVolume  # noqa: E402


class FosfsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "t.fosvol")

    def tearDown(self):
        self.dir.cleanup()

    def test_put_get_roundtrip(self):
        v = FosVolume.create(self.path)
        d = v.put("a.txt", b"hello fosfs")
        self.assertEqual(v.get("a.txt"), b"hello fosfs")
        self.assertEqual(d, v._current_manifest()["files"]["a.txt"])

    def test_reopen_persist(self):
        v = FosVolume.create(self.path)
        v.put("a", b"one")
        v.put("b", b"two")
        v2 = FosVolume(self.path)
        self.assertEqual(v2.get("a"), b"one")
        self.assertEqual(v2.get("b"), b"two")

    def test_overwrite(self):
        v = FosVolume.create(self.path)
        v.put("a", b"old")
        v.put("a", b"new")
        self.assertEqual(v.get("a"), b"new")
        self.assertEqual(FosVolume(self.path).get("a"), b"new")

    def test_delete_and_ls(self):
        v = FosVolume.create(self.path)
        v.put("a", b"1")
        v.put("b", b"22")
        v.delete("a")
        self.assertFalse(v.exists("a"))
        self.assertEqual(v.ls(), {"b": 2})
        with self.assertRaises(FosFSError):
            v.delete("a")

    def test_compression_zlib(self):
        # 可压缩数据应比原始小，块类型走 COMP_ZLIB
        v = FosVolume.create(self.path)
        blob = b"abcdefgh" * 2048
        v.put("big", blob)
        size = os.path.getsize(self.path)
        self.assertLess(size, len(blob))  # 压缩生效

    def test_snapshot_rollback(self):
        v = FosVolume.create(self.path)
        v.put("a", b"one")
        v.snapshot("v1")
        v.put("a", b"two")
        v.put("b", b"bee")
        self.assertEqual(v.get("a"), b"two")
        v.rollback("v1")
        self.assertEqual(v.get("a"), b"one")
        self.assertFalse(v.exists("b"))
        self.assertEqual(FosVolume(self.path).get("a"), b"one")

    def test_snapshot_list_persist(self):
        v = FosVolume.create(self.path)
        v.put("a", b"x")
        v.snapshot("keep")
        self.assertEqual(FosVolume(self.path).snapshots(), ["keep"])

    def test_tlv_ttl(self):
        v = FosVolume.create(self.path)
        v.put("c", b"cache", ttl_ms=60000)
        self.assertEqual(v.ttl_of("c"), 60000)

    def test_verify_ok(self):
        v = FosVolume.create(self.path)
        v.put("a", b"1")
        v.snapshot("s")
        v.put("a", b"2")
        self.assertGreaterEqual(v.verify(), 4)

    def test_corruption_detected(self):
        v = FosVolume.create(self.path)
        v.put("a", b"hello world hello world")
        # 篡改最后一个数据块 payload 中部一个字节
        with open(self.path, "r+b") as f:
            f.seek(-4, os.SEEK_END)
            b = f.read(1)
            f.seek(-1, os.SEEK_END)
            f.write(bytes([b[0] ^ 0xFF]))
        with self.assertRaises(CorruptError):
            FosVolume(self.path)

    def test_compact_reclaims(self):
        v = FosVolume.create(self.path)
        for i in range(20):
            v.put("f", bytes([i]) * 10000)  # 反复覆盖产生孤儿块
        before = os.path.getsize(self.path)
        freed = v.compact()
        after = os.path.getsize(self.path)
        self.assertGreater(freed, 0)
        self.assertEqual(before - after, freed)
        self.assertEqual(v.get("f"), bytes([19]) * 10000)
        # compact 后仍完整可读、可校验
        self.assertEqual(FosVolume(self.path).get("f"), bytes([19]) * 10000)
        FosVolume(self.path).verify()

    def test_bad_name(self):
        v = FosVolume.create(self.path)
        with self.assertRaises(FosFSError):
            v.put("a/b", b"x")
        with self.assertRaises(FosFSError):
            v.put("", b"x")


if __name__ == "__main__":
    unittest.main(verbosity=2)
