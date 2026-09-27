# SPDX-License-Identifier: MIT
"""A/B 环境切换（slot）。

真实系统里这是 bootloader 的事；本参考实现支持两种后端：

  1. u-boot 环境变量（真机）：fw_printenv / fw_setenv 读写
     ``familyos_slot_active``；
  2. 文件模拟（开发）：``<dir>/slot_current`` 存 ``_a``/``_b``。

探测顺序：存在 fw_printenv 用真后端，否则文件模拟。
回滚语义：刷入新镜像只填充「非活动槽」，slot switch 才生效 ——
救砖时切回旧槽即可，这就是环境切换与刷机的解耦。
"""

from __future__ import annotations

import os
import shutil
import subprocess

_SLOT_VAR = "familyos_slot_active"


class SlotError(Exception):
    """槽位操作失败。"""


def _fw_tools() -> bool:
    return shutil.which("fw_printenv") is not None


def _file_state(target_dir: str) -> str:
    fn = os.path.join(target_dir, "slot_current")
    if not os.path.isfile(fn):
        return "_a"
    with open(fn, encoding="utf-8") as f:
        v = f.read().strip()
    return v if v in ("_a", "_b") else "_a"


def get_current_slot(target: str) -> tuple[str, str]:
    """返回 (slot, backend)。backend: u-boot / file。"""
    if _fw_tools():
        try:
            r = subprocess.run(["fw_printenv", "-n", _SLOT_VAR],
                               capture_output=True, text=True, check=True)
            v = r.stdout.strip()
            return (v if v in ("_a", "_b") else "_a"), "u-boot"
        except (subprocess.SubprocessError, OSError):
            raise SlotError("fw_printenv 调用失败（环境未初始化？）") from None
    return _file_state(target), "file"


def _other(slot: str) -> str:
    return "_b" if slot == "_a" else "_a"


def set_active_slot(target: str, slot: str, *, confirm: bool = False) -> str:
    """切换活动槽。返回新槽位。"""
    if slot not in ("_a", "_b"):
        raise SlotError(f"非法槽位: {slot}（只认 _a/_b）")
    if not confirm:
        raise SlotError("未确认切换：请加 --confirm（切换后需重启生效）。")
    if _fw_tools():
        if os.geteuid() != 0:
            raise SlotError("写 bootloader 环境需要 root。")
        try:
            subprocess.run(["fw_setenv", _SLOT_VAR, slot], check=True,
                           capture_output=True)
        except (subprocess.SubprocessError, OSError) as e:
            raise SlotError(f"fw_setenv 失败: {e}") from e
    else:
        with open(os.path.join(target, "slot_current"), "w",
                  encoding="utf-8") as f:
            f.write(slot)
    return slot


def switch_slot(target: str, *, confirm: bool = False) -> str:
    """切到另一个槽（救砖回滚 = 反复调用本函数）。"""
    cur, _ = get_current_slot(target)
    return set_active_slot(target, _other(cur), confirm=confirm)
