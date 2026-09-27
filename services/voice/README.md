# voice — 跨端语音助手

`fos-voice-d`：设备内控制、跨设备控制、场景联动；**模型可替换**。

## 流水线

```
语音/文本
  │ asr（占位：接 whisper 类本地模型插件后替换）
  ▼
意图解析
  ├─ 规则表（离线兜底，内置 8 条演示规则）
  └─ ai-bus chat（自然语言路径；换模型 = 换语音大脑）
  ▼
路由执行
  ├─ home.*  → fos-home-bridge-d（device.command）
  ├─ ring.*  → fos-super-ring-d（session.open：投屏/互传）
  ├─ scene.* → 场景表（一句话触发一串动作）
  └─ sys.*   → 本机（音量/亮度，参考实现返回引导文本）
  ▼
TTS 文本（播放层负责合成，本服务输出文本）
```

## IPC 命令

| 命令 | 说明 |
| --- | --- |
| `asr {audio_b64?}` | 语音→文本（参考实现返回占位） |
| `nlup {text}` | 文本→意图 `{intent,slot,via}` |
| `run {text}` | 端到端：解析→路由→`{tts,intent,slot,exec_rc}` |

## 试试

```bash
./build/fosd --self-test
# 起一个带规则路径的实例（home-bridge 不在线时，路由会给出友好提示）
./build/fosd --sock /tmp/fos/voice.sock &
echo '{"id":1,"cmd":"run","args":{"text":"打开客厅灯"}}' \
  | nc -U /tmp/fos/voice.sock
```

## 模型替换接缝

意图解析的模型调用集中在 `parse_intent_model()` 一个函数里。
社区接更聪明的本地模型时只需要：
1. 给 ai-bus 换/加模型插件（`services/ai-bus/README.md`）；
2. 补全该函数里标注 `TODO(社区接入点)` 的响应解析段（含 JSON 转义）。
