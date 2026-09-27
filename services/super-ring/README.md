# super-ring — 超级环守护进程

`fos-super-ring-d`：FamilyOS 的全局设备协同入口的服务端。

## 定位

灵感来自业界的融合中心 / 超级终端。目标只有一个：让不同设备之间的
**连接、投屏、互传、控制**变得简单 —— 轻点进入设备全景视图。

- **发现**：mDNS（局域网）、BLE 广播、蓝牙经典、星闪（经 home-bridge
  转译登记）多路信号统一收敛到本服务；
- **登记**：在线设备表与协同会话保存在内核 `super_ring` 模块
  （`/dev/fos_ring`），本机任何 UI 都能用同一视图，不各说各话；
  内核模块缺失时自动降级为进程内存表（dev 环境友好）；
- **开放连接**：非本生态的蓝牙外设（耳机、音箱等）通过超级环接入控制，
  在登记表中体现为 `bt_audio` 类型设备；
- **传输**：投屏流、文件分片、剪贴板内容**不走本服务**。本服务只建
  「会话」并交换端点信息，数据由发起应用直连传输（带宽路径最短）。

## 接口（docs/ipc-protocol.md）

socket：`/run/fos/super-ring.sock`

| 命令 | 说明 |
| --- | --- |
| `devices.list` | 设备全景视图：所有在线/近期离线设备 + 活动会话 |
| `device.heartbeat {name,type,link,caps}` | 设备上线/心跳（发现层调用） |
| `device.drop {id}` | 设备下线 |
| `session.open {kind,src,dst}` | 建立会话（cast/xfer/clip/net/ctrl） |
| `session.close {id}` | 结束会话 |

## 安全

- 会话建立的前提是**预配对**：两台设备必须在同一账号下，或经用户端
  PIN 码确认（PIN 校验在账号服务，本服务只认已配对设备表）。
- socket 权限 0660 (root:fos)；写命令要求对端 uid ∈ {0, fosd}。

## 构建与自测

```bash
make            # 生成 build/fosd
./build/fosd --self-test
```
