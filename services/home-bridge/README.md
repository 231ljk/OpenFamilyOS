# home-bridge — 智能家居桥接层

`fos-home-bridge-d`：**只做桥接不做硬件**。它是整个系统的设备连接枢纽。

## 兼容什么

| 协议族 | 内置参考适配器 | 真实接入方式 |
| --- | --- | --- |
| 鸿蒙星闪 NearLink | `nearlink`（echo） | 社区适配器 .so（厂商 SDK 封装） |
| 运营商协议（andlink 等） | `carrier`（echo） | 同上 |
| 各厂商私有协议 | `vendor`（echo） | 同上 |

内置适配器只演示接口形态并**不发出真实报文** —— 桥接层保持中立，
协议实现的正确性交给社区与厂商认证。

## 适配器插件接口（`adapter.h`）

```c
const char *fos_adapter_proto(void);   /* 本适配器负责的协议名 */
int fos_adapter_send(dev_id, action, params_json, reply, rsz);
```

放进 `/usr/lib/fos/home-adapters/`（或 `FOS_ADAPTER_DIR`），服务启动
自动 dlopen；`device.add` 时按 `proto` 字段路由。

## 统一动作语义

跨协议只暴露一套动词，适配器负责映射到厂商私有报文：

`on` / `off` / `toggle` / `set_level {v}` / `set_temp {c}` / `mode {m}` …

这样语音助手（`services/voice`）和超级环（`services/super-ring`）
不需要知道设备用什么协议 —— 这正是「桥接枢纽」的意义。

## IPC 命令

| 命令 | 说明 |
| --- | --- |
| `bridge.list` | 适配器与设备一览 |
| `device.add {id,proto,name,model}` | 登记（幂等） |
| `device.remove {id}` | 移除 |
| `device.command {id,action,params}` | 统一下发，透传适配器回复 |

## 与其他模块的协作

- 设备上线后由发现层（或手动）调用超级环 `device.heartbeat`，
  type 填 `iot`，能力位带 `FOS_CAP_HOME_CTRL` → 全景视图可见；
- 语音助手把「把客厅灯调暗」解析成 `device.command {id, "set_level", …}`。
