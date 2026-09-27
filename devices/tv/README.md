# devices/tv — 电视（arm64 / x86_64）

## 配方

- 内核：Linux 6.6 LTS
- 形态：10 英尺体验；1080p/4K 输出；遥控器即超级环 `ctrl` 会话
- 教育：大屏课程 + 3D 闯关的沉浸出口
- 健康使用：儿童档 —— 观看时长走 `health` 同一记账体系

## 交互约定

| 输入 | 通道 |
| --- | --- |
| 蓝牙/红外遥控 | 超级环登记 `ctrl` 能力设备 |
| 手机当遥控器 | 超级环会话（`services/super-ring`） |
| 语音 | 电视麦克风阵列 → `voice` 服务（本地 ASR 插件） |

## 刷入

```bash
./tools/build-image.sh --device tv --out out/
# 多数电视盒子经 recovery/USB 引导，具体见机型 rootfs 模板
```

## 已知问题

- HDMI CEC 控制零散依赖内核补丁（`kernel/patches/` 收社区贡献）；
- 部分盒子 GPU 为专有驱动，降级路径：`ssdlt`/`simpledrm`。
