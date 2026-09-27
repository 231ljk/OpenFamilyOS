# OpenFamilyOS 系统文档

## 一、这是什么

OpenFamilyOS 是 FamilyOS 的开源开轮版。

它不是发行版套壳，不是别的系统的换皮，是基于 Linux 内核二次开发、从零搭建的开轮版操作系统底座。

覆盖五端：电脑、平板、手机、电视、手表。

整系统采用"通用标准 + 自研模块"组合架构：
- 通用部分：兼容行业标准，保证外部互通（文件系统 ext4/exFAT/NTFS、网络协议、硬件接口）
- 自研部分：内核改动、系统服务、缓存层，全部官方自研

## 二、定位

开轮版 = 毛坯地基。

功能少、玩法少、代码清晰有注释。不预设壁纸，不绑架用户，免费，不管控。开发者拿到后可以随便改、随便用、随便发。

好用的东西在闭源版里，开轮版只负责两件事：
1. 让开发者能看懂、能改
2. 让社区能长出来

## 三、目录结构

kernel/         内核（基于 Linux，二次开发，带注释）
system/         系统服务
superring/      超级环（开轮版简化能力：发现设备、传文件、投屏）
familyhome/     Family Home 智能家居桥接（通用协议兼容）
aibus/          AI Bus 统一模型总线（本地开源模型接口）
storage/        存储模块（通用格式 + 自研格式）
education/      教育系统框架（四类课程结构定义）
health/         健康使用设备（全设备通用）
tools/          刷机工具（全设备统一）
build/          构建脚本
docs/           文档

## 四、构建

详见 build.md。

简要步骤：
1. 准备 Linux 编译环境
2. 执行 build/init.sh 初始化工具链
3. 执行 build/build.sh 编译
4. 输出镜像在 build/output/ 目录

## 五、刷机

详见 tools/README.md。

支持全设备统一刷机：镜像写入、数据备份恢复、环境切换、救砖模式。

## 六、同步到其他仓库

本仓库是主仓，通过以下方式同步到子仓：

- kernel/ 同步到 familyos-lab/OpenFamilyOS-kernel
- system/ 同步到 familyos-lab/OpenFamilyOS-system
- superring/ 同步到 familyos-lab/OpenFamilyOS-superring
- familyhome/ 同步到 familyos-lab/OpenFamilyOS-familyhome
- aibus/ 同步到 familyos-lab/OpenFamilyOS-aibus
- education/ 同步到 familyos-lab/OpenFamilyOS-education
- health/ 同步到 familyos-lab/OpenFamilyOS-health
- tools/ 同步到 familyos-lab/OpenFamilyOS-tools

同步脚本：sync.sh（在 build/ 目录下）

执行方式：
  ./build/sync.sh all        # 同步所有子仓
  ./build/sync.sh kernel     # 只同步内核子仓

## 七、贡献

详见 CONTRIBUTING.md。

核心原则：
- 代码清晰有注释
- 不引入闭源二进制
- 不自称"完整系统"，开轮版就是开轮版
- 提交时附带 Signed-off-by

## 八、协议

MulanPSL-2.0。

## 九、与闭源版的关系

OpenFamilyOS 是开轮版，闭源 FamilyOS 是完整版。
闭源版包含：游戏中心、音乐制作软件、完整超级环、深度连接机制等。
这些不在本仓库中，也不会以开源形式发布。

## 十、理念

"科技不是高高在上的，而是服务每一个人。"
OpenFamilyOS 让每一个开发者都能从零理解和修改一个真正的操作系统。

---

有问题？提 Issue。想改？提 PR。想骂？也行。
