/* SPDX-License-Identifier: MIT */
/*
 * ai_plugin.h — 统一模型总线接口规范（模型侧 ABI）
 *
 * FamilyOS 开轮版不绑定任何云端大模型。所有模型以「插件 .so」形式
 * 接入模型总线（fos-ai-bus-d），全部本地运行：不联网、不烧 Token、
 * 不上传用户数据。社区可自行接入任意符合本规范的模型。
 *
 * ── 如何接入一个模型 ──
 * 1. 用 C/C++/Rust（cdylib）实现下面 5 个导出符号；
 * 2. 编译为共享库：<name>.so，放进 /usr/lib/fos/models/ 或
 *    $FOS_MODEL_DIR；
 * 3. 总线扫描目录自动加载，或运行时 model.load 热插。
 *
 * ── 生命周期 ──
 *   fos_model_info   总线启动时读取元数据（不加载权重）
 *   fos_model_load   真正初始化（读权重/建上下文），可返回 NULL 实现
 *                    「无状态代理型」插件（如本地 llama.cpp 包装）
 *   fos_model_chat   一次对话；并发由总线串行化，插件无需加锁
 *   fos_model_unload 释放
 *
 * 字符串约定：全部 UTF-8；插件返回的文本由总线负责 free，
 * 故插件必须用 malloc/strdup 家族分配。
 */
#ifndef FOS_AI_PLUGIN_H
#define FOS_AI_PLUGIN_H

#include <stddef.h>

#define FOS_MODEL_ABI_VERSION 1

/* 单条消息：role = "system" | "user" | "assistant" */
typedef struct {
	const char *role;
	const char *content;
} fos_msg;

typedef struct {
	/* 必填：模型元数据（用于总线注册） */
	int (*fos_model_info)(const char **name, const char **version,
			      const char **desc);

	/* 可选：初始化（返回不透明 handle 存入 h） */
	int (*fos_model_load)(void **h);

	/* 必填：一次对话。out_text 由插件 malloc，总线负责 free。
	 * max_tokens <= 0 表示插件自选默认值。返回 0 成功。 */
	int (*fos_model_chat)(void *h, const fos_msg *msgs, int n_msgs,
			      int max_tokens, char **out_text);

	/* 可选：反初始化 */
	void (*fos_model_unload)(void *h);

	/* 必填：ABI 版本，总线据此拒绝不兼容插件 */
	int (*fos_model_abi)(void);
} fos_model_vtable;

/*
 * 插件只需实现各函数并导出下面 5 个同名符号，总线 dlsym 逐个解析。
 * （不做统一 vtable 导出，图的是一个函数一个符号、最小依赖。）
 */
#endif /* FOS_AI_PLUGIN_H */
