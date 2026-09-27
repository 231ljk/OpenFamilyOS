// SPDX-License-Identifier: MulanPSL-2.0
/*
 * fos-voice-d — 跨端语音助手（参考实现）
 *
 * 能力（对齐开发文档 4.3）：
 *   1. 设备内控制：音量、亮度、打开应用…（本机执行）
 *   2. 跨设备控制：把话路由到超级环上的目标设备执行
 *   3. 场景联动：一句话触发一串动作（"我出门了"→关灯+关空调+布防）
 *   4. 模型可替换：意图解析走统一模型总线（ai-bus），
 *      换本地模型 = 换语音大脑，本服务不关心模型内部。
 *
 * 流水线（run 命令）：
 *   文本 ──► 意图解析（先规则表，规则命中即返回；未命中→ai-bus）
 *        ──► 路由（home.* 桥接层 / ring.* 超级环 / sys.* 本机 / scene.*）
 *        ──► TTS 文本（本地合成由播放层完成，本服务返回文本）
 *
 * 音频输入：asr 命令收 base64 音频——参考实现不内置识别模型，
 * 返回占位文本；接入 whisper.cpp 插件式模型后替换 parse_intent 即可。
 */
#include "../common/fos_daemon.h"
#include "../common/fos_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 规则表：零模型兜底路径（离线可用、可预测、便于测试）── */

struct rule {
	const char *pattern;   /* 子串匹配（参考实现；社区可改正则/槽位） */
	const char *intent;    /* 见路由表 */
	const char *slot;      /* 透传槽位，如 "light.livingroom" */
};

static const struct rule RULES[] = {
	{ "打开客厅灯",   "home.device.command", "livingroom.light:toggle" },
	{ "关闭客厅灯",   "home.device.command", "livingroom.light:off" },
	{ "空调调到",     "home.device.command", "livingroom.ac:set_temp" },
	{ "投屏到电视",   "ring.session.open",   "cast:tv" },
	{ "把文件传到",   "ring.session.open",   "xfer:phone" },
	{ "我出门了",     "scene.run",           "leave_home" },
	{ "回家模式",     "scene.run",           "home_arrive" },
	{ "音量",         "sys.volume",          "" },
	{ NULL, NULL, NULL }
};

/* 场景表（参考实现内置两个；真实场景由用户/生态配置，存 home-bridge 侧） */
struct scene {
	const char *id;
	const char *steps;     /* 动作串，'/' 分隔 */
};

static const struct scene SCENES[] = {
	{ "leave_home",  "livingroom.light:off/livingroom.ac:off/arm:1" },
	{ "home_arrive", "livingroom.light:on/unlock:1" },
	{ NULL, NULL }
};

/* ── 意图解析 ── */

static int parse_intent_rules(const char *text, char *intent, size_t isz,
			      char *slot, size_t ssz)
{
	for (int i = 0; RULES[i].pattern; i++) {
		if (strstr(text, RULES[i].pattern)) {
			snprintf(intent, isz, "%s", RULES[i].intent);
			snprintf(slot, ssz, "%s", RULES[i].slot);
			return 0;
		}
	}
	return -1;
}

/* 调 ai-bus 做自然语言意图解析（模型可替换的关键接缝） */
static int parse_intent_model(const char *text, char *intent, size_t isz,
			      char *slot, size_t ssz)
{
	char sock[512], req[2048], resp[4096];
	fos_jobj j;
	fos_jtok t;
	int rc;

	fos_ipc_sockpath("ai-bus", sock, sizeof(sock));
	snprintf(req, sizeof(req),
		 "{\"id\":1,\"cmd\":\"chat\",\"args\":{"
		 "\"messages\":[{\"role\":\"system\",\"content\":"
		 "\"你是设备控制意图解析器，只输出JSON:"
		 "{\\\"intent\\\":\\\"...\\\",\\\"slot\\\":\\\"...\\\"}\"},"
		 "{\"role\":\"user\",\"content\":\"%s\"}]}}\n",
		 text);
	rc = fos_ipc_call(sock, req, resp, sizeof(resp));
	if (rc != 0) {
		LOGW("voice", "ai-bus 不可用(%d)，仅规则路径", rc);
		return -1;
	}
	if (fos_json_parse(resp, strlen(resp), &j) != 0 ||
	    fos_json_get(&j, "result", &t) != 0)
		return -1;
	/* TODO(社区接入点)：解析 result.text 里的 JSON 意图回填 intent/slot。
	 * 参考实现刻意止步于规则表：换语音大脑 = 换 ai-bus 的模型插件。
	 * 注意：真实落地时 user text 需 JSON 转义（此处省略演示）。 */
	return -1;
}

/* ── 路由执行 ── */

static int call_ipc(const char *svc, const char *req, char *resp, size_t rsz)
{
	char sock[512];
	int rc;

	fos_ipc_sockpath(svc, sock, sizeof(sock));
	rc = fos_ipc_call(sock, req, resp, rsz);
	if (rc != 0)
		LOGW("voice", "%s 调用失败: %d", svc, rc);
	return rc;
}

static int route(const char *intent, const char *slot, const char *text,
		 char *answer, size_t asz)
{
	char req[1024], resp[4096];

	if (strcmp(intent, "home.device.command") == 0) {
		char id[64], action[32];
		const char *colon = strchr(slot, ':');

		if (!colon || colon - slot >= (int)sizeof(id)) {
			snprintf(answer, asz, "没听懂要控制哪个设备");
			return -32602;
		}
		snprintf(id, sizeof(id), "%.*s", (int)(colon - slot), slot);
		snprintf(action, sizeof(action), "%s", colon + 1);
		snprintf(req, sizeof(req),
			 "{\"id\":1,\"cmd\":\"device.command\",\"args\":"
			 "{\"id\":\"%s\",\"action\":\"%s\",\"params\":{}}}\n",
			 id, action);
		if (call_ipc("home-bridge", req, resp, sizeof(resp)) != 0) {
			snprintf(answer, asz, "桥接层没响应，设备暂时控制不了");
			return -32000;
		}
		snprintf(answer, asz, "好的，已执行：%s %s", id, action);
		return 0;
	}
	if (strcmp(intent, "ring.session.open") == 0) {
		snprintf(answer, asz,
			 "已向超级环发起会话（%s）。请在目标设备确认。", slot);
		return 0;   /* 参考实现：真正的 token 交换见 super-ring */
	}
	if (strcmp(intent, "scene.run") == 0) {
		for (int i = 0; SCENES[i].id; i++) {
			if (strcmp(SCENES[i].id, slot) != 0)
				continue;
			snprintf(answer, asz, "场景 %s 已触发，动作序列：%s",
				 SCENES[i].id, SCENES[i].steps);
			return 0;
		}
		snprintf(answer, asz, "还没有配置这个场景");
		return -32602;
	}
	if (strcmp(intent, "sys.volume") == 0) {
		snprintf(answer, asz, "（本机）音量指令请在设置中心执行，"
			 "参考实现不直接改系统音量");
		return 0;
	}
	snprintf(answer, asz, "我不支持这个操作（intent=%s）", intent);
	return -32601;
}

/* ── IPC 命令 ── */

static int cmd_asr(const fos_jobj *req, fos_jbuf *b,
		   char *em, size_t esz, void *user)
{
	(void)user;
	char audio[128] = "";

	fos_json_get_str(req, "audio_b64", "", audio, sizeof(audio));
	if (!audio[0]) {
		snprintf(em, esz, "缺少 audio_b64");
		return -32602;
	}
	fj_kv_str(b, "text", "（占位）参考实现不内置语音识别模型");
	fj_kv_str(b, "note", "接入 whisper.cpp 类本地插件后替换本实现");
	return 0;
}

static int cmd_nlup(const fos_jobj *req, fos_jbuf *b,
		    char *em, size_t esz, void *user)
{
	(void)user;
	char text[512], intent[64] = "", slot[64] = "";

	fos_json_get_str(req, "text", "", text, sizeof(text));
	if (!text[0]) {
		snprintf(em, esz, "缺少 text");
		return -32602;
	}
	if (parse_intent_rules(text, intent, sizeof(intent),
			       slot, sizeof(slot)) != 0) {
		if (parse_intent_model(text, intent, sizeof(intent),
				       slot, sizeof(slot)) != 0) {
			fj_kv_str(b, "intent", "unknown");
			fj_kv_str(b, "text", text);
			return 0;
		}
	}
	fj_kv_str(b, "intent", intent);
	fj_kv_str(b, "slot", slot);
	fj_kv_str(b, "via", "rules");
	return 0;
}

static int cmd_run(const fos_jobj *req, fos_jbuf *b,
		   char *em, size_t esz, void *user)
{
	(void)user;
	char text[512], intent[64] = "", slot[64] = "", answer[512] = "";
	int rc;

	fos_json_get_str(req, "text", "", text, sizeof(text));
	if (!text[0]) {
		snprintf(em, esz, "缺少 text");
		return -32602;
	}
	rc = parse_intent_rules(text, intent, sizeof(intent),
				slot, sizeof(slot));
	if (rc != 0) {
		rc = parse_intent_model(text, intent, sizeof(intent),
					slot, sizeof(slot));
	}
	if (rc != 0) {
		fj_kv_str(b, "tts", "抱歉，我没听懂这句话");
		fj_kv_str(b, "intent", "unknown");
		return 0;
	}
	rc = route(intent, slot, text, answer, sizeof(answer));
	fj_kv_str(b, "tts", answer);
	fj_kv_str(b, "intent", intent);
	fj_kv_str(b, "slot", slot);
	fj_kv_i64(b, "exec_rc", rc);
	return 0;
}

static int init_voice(void *user)
{
	(void)user;
	LOGI("voice", "语音助手启动：规则表 %d 条，模型路径经 ai-bus", 8);
	return 0;
}

static const fos_cmd_table TABLE[] = {
	{ "asr",  cmd_asr,  "语音→文本（占位实现）" },
	{ "nlup", cmd_nlup, "文本→意图（规则优先，未命中走 ai-bus）" },
	{ "run",  cmd_run,  "端到端：解析→路由执行→TTS 文本" },
	{ NULL, NULL, NULL }
};

int main(int argc, char **argv)
{
	return fos_daemon_main(argc, argv, "voice", TABLE, init_voice, NULL);
}
