# devices/tablet — 平板（arm64）

## 配方

- 内核：Linux 6.6 LTS（arm64）
- 形态：10~13 寸触屏；virt-compat `fullscreen`（学习应用全屏独占）
- 教育：主战场之一 —— `edu/` 课程默认设备含 tablet
- 护眼：健康使用附加亮度/时长提醒（定时经 health 通道）

## 刷入

```bash
./tools/build-image.sh --device tablet --out out/
sudo python3 tools/familyos_flash/cli.py flash out/familyos-open-wheel-tablet.fosimg \
     --to /dev/mmcblk0 --confirm --allow-block-device
```

## 已知问题

- 触摸屏固件差异大，参考配方假设标准 HID 触摸屏；
- 电源键唤醒需设备特定 DTS（社区贡献 `rootfs/` 模板）。
