// SPDX-License-Identifier: MulanPSL-2.0
/*
 * model_default.c — 默认搭载模型：本地规则模型（参考实现）
 *
 * 这是开轮版「默认搭载可替换开源模型」的占位参考实现：不联网、
 * 不需要权重文件，用关键词规则演示插件接口的正确用法。
 * 社区替换为真实本地模型（llama.cpp / whisper.cpp / 自研推理）时，
 * 保持同样的 5 个导出符号即可，总线与上层零改动。
 */
#include "../ai_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int default_info(const char **name, const char **version,
			const char **desc)
{
	*name = "default-rule";
	*version = "0.1";
	*desc = "本地规则模型（参考实现，可被任意符合规范的模型替换）";
	return 0;
}

static int default_abi(void)
{
	return FOS_MODEL_ABI_VERSION;
}

/* 演示「系统提示词 + 多轮上下文」的读取方式：只看最后一条 user */
static int default_chat(void *h, const fos_msg *msgs, int n,
			int max_tokens, char **out)
{
	const fos_msg *last_user = NULL;
	char *reply;
	int i;

	(void)h; (void)max_tokens;

	for (i = n - 1; i >= 0; i--)
		if (msgs[i].role && strcmp(msgs[i].role, "user") == 0) {
			last_user = &msgs[i];
			break;
		}
	if (!last_user) {
		*out = strdup("（没有用户输入）");
		return *out ? 0 : -1;
	}

	reply = calloc(512, 1);
	if (!reply)
		return -1;

	/* 极简规则：体现接口，不做真智能 */
	if (strstr(last_user->content, "你好") ||
	    strcasestr(last_user->content, "hello"))
		snprintf(reply, 512, "你好，我是 FamilyOS 本地默认模型（规则版）。"
			 "换上真正的本地模型后我就退休。");
	else if (strstr(last_user->content, "时间") ||
		 strstr(last_user->content, "几点"))
		snprintf(reply, 512, "本模型不读取系统时钟，建议调用系统时钟服务。");
	else
		snprintf(reply, 512, "（规则模型）收到：%s。"
			 "要获得真实对话能力，请在 %s 放入兼容模型插件。",
			 last_user->content, "/usr/lib/fos/models/");
	*out = reply;
	return 0;
}

/* 导出总线约定的 5 个符号 */
int fos_model_info(const char **n, const char **v, const char **d)
{
	return default_info(n, v, d);
}
int fos_model_abi(void)
{
	return default_abi();
}
int fos_model_load(void **h)
{
	*h = NULL;     /* 无状态 */
	return 0;
}
int fos_model_chat(void *h, const fos_msg *m, int n, int t, char **o)
{
	return default_chat(h, m, n, t, o);
}
void fos_model_unload(void *h)
{
	(void)h;
}
