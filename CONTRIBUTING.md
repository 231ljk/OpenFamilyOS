# 贡献指南（CONTRIBUTING）

感谢你参与 FamilyOS 开轮版。开始前请通读本文。

## 行为准则

讨论对事不对人。项目理念是「科技服务每一个人」，社区同样服务每一个人。

## 开发环境

- 一个 Linux 环境（本机发行版或 WSL2 均可）用于编译内核模块与 C 服务
- `gcc` / `make` / `linux-headers-$(uname -r)`
- Python ≥ 3.10（`pkg/`、`tools/`、`fs/fosfs/` 的参考实现与测试）

```bash
git clone https://github.com/231ljk/OpenFamilyOS.git
cd OpenFamilyOS
make all && make test
```

## 代码风格

### C（kernel/ 与 services/）

- **内核代码**遵循 Linux 内核风格（`Documentation/process/coding-style.rst`）：
  制表符缩进（8 列）、花括号 K&R 风格、单行尽量 ≤ 100 字符。
- **用户态代码**遵循 POSIX 风格：4 空格缩进、`snake_case`、函数注释头说明用途/参数/返回值。
- 注释语言：代码内注释以中文为主，接口/协议文档中英对照。
- 头文件一律加 `#ifndef` 卫士与 SPDX 标识符：
  - `kernel/**` → `SPDX-License-Identifier: GPL-2.0`
  - 其余 → `SPDX-License-Identifier: MIT`

### Python（pkg/、tools/、fs/）

- 标准库优先，不引入第三方运行时依赖（测试可用 `pytest`，但 `unittest` 也必须能跑）。
- 公共函数必须有 docstring；遵循 PEP 8，行宽 100。

### 通用

- 每个可独立工作的功能一个 PR；大改动请先开 Issue 讨论。
- 禁止提交编译产物、镜像文件、`*.fos` 包与任何密钥。

## 提交规范（Git Commit）

采用 Conventional Commits 的简化版：

```
<type>(<scope>): <摘要（中文可，≤50 字）>

<正文：为什么改，怎么改>
```

| type | 用途 |
| --- | --- |
| `feat` | 新功能 |
| `fix` | 修 Bug |
| `docs` | 文档 |
| `test` | 测试 |
| `refactor` | 重构（不改行为） |
| `chore` | 构建/杂项 |

scope 使用目录名：`kernel` `super-ring` `ai-bus` `home-bridge` `voice` `virt-compat` `migration` `health` `fosfs` `pkg` `flash` `edu` `docs`。

示例：`feat(ai-bus): 为模型总线增加超时保护`

## 分支与 PR 流程

1. `main` 始终可编译、测试通过（受 CI 保护）。
2. 从 `main` 切出 `feat/xxx`、`fix/xxx` 分支。
3. PR 标题沿用提交规范；描述里写清动机、影响面、测试方式。
4. CI 通过 + 至少一位维护者 review 后合并（维护者保留最终决定权，但**不会因为「和你的想法不一样」而拒绝开源贡献**）。

## 测试要求

- 新服务接口必须附带至少一个冒烟测试（`services/<name>/tests/`）。
- Python 组件改动必须保证 `make test-python` 全绿。
- 内核模块至少通过 `make -C kernel` 编译 + CI 的 modpost 检查。

## CLA（贡献者许可协议）

为保持开轮版「可自由商用、永久开源」的双重可能性，首次被合并 PR 的作者需签署 CLA：

1. 维护者在首个 PR 下自动发送 CLA 链接（见仓库 Discussions 置顶帖）。
2. 你授予项目对你贡献代码的永久、免版税、可再许可的使用权；**代码版权仍归你本人**。
3. 你声明贡献为自己原创、有权授权。
4. 若不同意 CLA，可以不开 PR，改为在 Issue 里提供思路——同样欢迎。

## 问题反馈

- Bug / 功能建议：GitHub Issues（模板已配置）
- 使用讨论、玩法分享：仓库 Discussions
- 文档更新随版本发布同步，见 [CHANGELOG.md](CHANGELOG.md)

—— FamilyOS Lab
