# devices/phone — 手机（arm64）

## 配方

- 内核：Linux 6.6 LTS（arm64）
- 形态：virt-compat `fullscreen`；主屏 + 超级环下拉
- 通信栈：语音/蜂窝驱动高度机型相关，仓库仅到「可启动 + 基础 UI」层

## 状态：实验中

手机涉及大量 SoC 专有驱动（调制解调器、相机 ISP、音频 DSP），
开轮版当前提供：通用 ARM64 引导框架 + `mainline` 机型清单（见 Discussions）。
建议从 mainline 已支持机型（部分 PinePhone / 已解锁 bootloader 的旧旗舰）起步。

## 刷入

```bash
./tools/build-image.sh --device phone --out out/
sudo python3 tools/familyos_flash/cli.py rescue --image out/familyos-open-wheel-phone.fosimg \
     --to /dev/mmcblk0 --confirm --allow-block-device      # 换机前先救砖备份
```

## 已知问题

- 电源管理（深度休眠）是主要功耗瓶颈；
- 健康使用「前台判定」在通话界面需特殊处理（通话不计入应用时长）。
