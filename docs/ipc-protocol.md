# FamilyOS 服务间 IPC 协议（v1）

> 适用范围：`services/` 下全部守护进程与本机客户端（CLI、设置界面、语音助手路由）。
> 实现：`services/common/fos_ipc.c`（服务端/客户端均为 ~200 行，零第三方依赖）。

## 设计原则

1. **通用标准优先**：传输用 Unix domain socket（本机）；编码用 JSON 行。
   不发明二进制帧格式 —— 协议可读、可 curl、可被任何语言客户端接入。
2. **功能克制**：一行一个请求、一个响应；无流控、无多路复用。
   高吞吐场景（投屏流、文件互传）不走本协议，由超级环另立数据通道。
3. **崩溃友好**：服务端 poll 单线程 + 每客户端缓冲；客户端断开即清理。

## 地址约定

| 服务 | Socket 路径 |
| --- | --- |
| 超级环 | `/run/fos/super-ring.sock` |
| AI 模型总线 | `/run/fos/ai-bus.sock` |
| 智能家居桥接 | `/run/fos/home-bridge.sock` |
| 语音助手 | `/run/fos/voice.sock` |
| 虚拟化兼容层 | `/run/fos/virt-compat.sock` |
| 换机迁移 | `/run/fos/migration.sock` |
| 健康使用 | `/run/fos/health.sock` |

开发环境可用环境变量 `FOS_RUN_DIR` 重定位（单元测试即如此）。

## 报文

### 请求（客户端 → 服务端，单行）

```json
{"id":1,"cmd":"devices.list","args":{...}}
```

| 字段 | 必选 | 说明 |
| --- | --- | --- |
| `id` | 是 | 整数，响应原样回带，便于并发对账 |
| `cmd` | 是 | `域.动作`，点分命名，如 `session.open`、`device.heartbeat` |
| `args` | 否 | 对象，命令参数 |

### 响应（服务端 → 客户端，单行）

```json
{"id":1,"ok":true,"result":{...}}
{"id":2,"ok":false,"error":{"code":-32601,"msg":"unknown cmd: foo.bar"}}
```

### 事件（服务端主动推送，无 id）

```json
{"event":"device.online","data":{...}}
```

## 错误码（对齐 JSON-RPC 习惯）

| code | 含义 |
| --- | --- |
| -32600 | 请求非法（缺 id/cmd、JSON 解析失败） |
| -32601 | 命令不存在 |
| -32602 | 参数错误 |
| -32603 | 内部错误 |
| -32000 | 依赖服务不可用（如语音助手找不到 AI 总线） |

## 各服务命令一览

### super-ring
`devices.list` / `device.heartbeat {name,type,link,caps}` / `device.drop {id}`
`session.open {kind,src,dst}` / `session.close {id}`

### ai-bus
`models.list` / `model.load {path}`
`chat {model?,messages:[{role,content}],max_tokens?}`（模型可替换：不传 model 用默认）

### home-bridge
`bridge.list` / `device.add {id,proto,name}` / `device.remove {id}`
`device.command {id,action,params}`（action 透传给厂商适配器）

### voice
`asr {audio_b64?}`（参考实现返回占位文本）/ `nlup {text}`（意图解析）
`run {text}`（端到端：解析→路由到 super-ring / home-bridge → TTS 文本）

### virt-compat
`info`（架构/运行环境） / `app.run {arch,path,args}` / `mode.set {mode:window|fullscreen}`

### migration
`export {out,include:[apps,files,settings,accounts]}` /
`import {src}` / `status`（进度轮询）

### health
`usage.list` / `usage.add {pkg,uid,ms}` / `block.set {pkg,level}` /
`block.clear {pkg}` / `can.launch {pkg,uid}`（放行判断，供包管理与桌面调用）

## 安全边界

- socket 权限 `0660`，属主 `root:fos`；受限用户（青少年档）经 setgroups 加入 `fos` 组后
  仅允许 `health.can.launch` 与只读命令 —— 写命令由服务端按 peer uid 校验（SO_PEERCRED）。
- 本协议只服务本机进程；跨设备通道在超级环之上另有 TLS 预配对（见 `services/super-ring/README.md`）。
