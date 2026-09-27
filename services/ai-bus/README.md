# ai-bus — 统一模型总线

`fos-ai-bus-d`：开轮版 AI 能力的唯一入口。

## 四条硬承诺

开轮版不绑定任何云端大模型：

1. **统一模型总线接口** —— 规范模型接入方式（[`ai_plugin.h`](ai_plugin.h)）；
2. **默认搭载可替换的开源模型**，全部本地运行；
3. **不联网、不烧 Token、不上传用户数据**（本进程零网络调用；
   systemd 单元可进一步 `PrivateNetwork=yes` 强隔离）；
4. **社区可自行接入任意符合接口规范的模型**。

## 模型插件 ABI

模型 = 一个 `.so`，导出 5 个符号：

```c
int  fos_model_abi(void);                      /* 返回 FOS_MODEL_ABI_VERSION */
int  fos_model_info(name*, version*, desc*);   /* 元数据 */
int  fos_model_load(void **h);                 /* 可选：初始化 */
int  fos_model_chat(h, msgs, n, max_tok, &out);/* 核心：一次对话 */
void fos_model_unload(h);                      /* 可选：释放 */
```

用 llama.cpp、whisper.cpp、ONNX Runtime 或自研推理引擎包一层，
即可让全家设备用上你自己的本地模型。`models/model_default.c`
是一个 60 行的最小示例（规则模型），拿来抄结构最快。

## 部署

```bash
# 编译总线与默认模型
make

# 放入系统模型目录（或 FOS_MODEL_DIR 指定）
sudo mkdir -p /usr/lib/fos/models
sudo cp build/models/default_rule.so /usr/lib/fos/models/

# 启动
./build/fosd --sock /tmp/fos/ai-bus.sock &

# 对话
python3 - <<'EOF'
import json, socket
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/fos/ai-bus.sock")
s.sendall(json.dumps({"id":1,"cmd":"chat","args":{"messages":[
    {"role":"user","content":"你好"}]}}).encode()+b"\n")
print(s.recv(4096).decode())
EOF
```

## IPC 命令

| 命令 | 说明 |
| --- | --- |
| `models.list` | 已加载模型及隐私声明 |
| `chat {model?,messages,max_tokens?}` | 一次对话；不传 model 用默认 |
| `model.load {path}` | 运行时热插一个模型 .so |

## 参考实现的边界

- 单线程串行推理（插件不保证线程安全）；高并发场景社区可换
  每模型 worker 进程池，接口不变；
- 无流式输出（chat 一次返回全文）；`chat` 响应含 `text` 即符合约定，
  流式可在 ABI v2 提案（见 Discussions）。
