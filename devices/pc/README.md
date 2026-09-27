# devices/pc — 电脑（x86_64）

## 配方

- 内核：Linux 6.6 LTS（x86_64）+ `kernel/` 开轮模块
- 引导：UEFI（GRUB/systemd-boot 均可）
- 显示：Wayland 合成器（参考 weston，桌面壳社区自选）
- virt-compat 形态：`window` —— 多应用窗口化并行（含 x86 翻译应用）

## 构建与刷入

```bash
./tools/build-image.sh --device pc --out out/
sudo python3 tools/familyos_flash/cli.py flash out/familyos-open-wheel-pc.fosimg \
     --to /dev/sdX --confirm --allow-block-device
```

## 已知问题

- 闭源 GPU 驱动不在仓库范围（社区按设备文档自装）；
- 休眠路径（s2idle/S3）随机型差异大，健康使用计时依赖唤醒事件校准。
