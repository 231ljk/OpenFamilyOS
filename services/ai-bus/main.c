// SPDX-License-Identifier: MIT
/*
 * fos-ai-bus-d — 统一模型总线（Unified Model Bus）
 *
 * 开轮版 AI 能力的唯一入口：
 *   - 扫描模型目录（/usr/lib/fos/models，可用 FOS_MODEL_DIR 覆盖），
 *     dlopen 加载所有符合 ai_plugin.h 规范的 .so；
 *   - 校验 ABI 版本，不兼容一律拒绝加载并记录原因；
 *   - 对上层提供 chat 接口：模型按名字选，缺省用默认模型；
 *   - 全部本地运行，本进程不创建任何对外网络连接（开轮版硬约束，
 *     systemd 单元里 PrivateNetwork/无出网权限进一步兜底）。
 *
 * 并发策略：模型插件不保证线程安全，总线用单线程 poll 串行调用，
 * 一条请求处理完再收下一条。真实系统可换「每模型一 worker」，
 * 接口不变。
 */
#include "../common/fos_daemon.h"
#include "ai_plugin.h"

#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODELS 16
#define MAX_MSGS   16

struct model {
	int used;
	char path[512];
	char name[64];
	char version[32];
	char desc[192];
	void *dl;
	void *handle;                       /* 插件私有上下文 */
	int (*fn_chat)(void *, const fos_msg *, int, int, char **);
	int (*fn_unload)(void *);
	int is_default;
};

static struct model models[MAX_MODELS];
static int loaded;

static const char *model_dir(void)
{
	const char *d = getenv("FOS_MODEL_DIR");

	return d && *d ? d : "/usr/lib/fos/models";
}

/* 加载单个 .so；返回 0 成功。错误信息进 errbuf。 */
static int load_one(const char *path, char *errbuf, size_t esz)
{
	int (*abi)(void);
	int (*info)(const char **, const char **, const char **);
	int (*mload)(void **);
	int (*chat)(void *, const fos_msg *, int, int, char **);
	void (*unload)(void *);
	void *dl, *h = NULL;
	struct model *m = NULL;
	int i;

	dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!dl) {
		snprintf(errbuf, esz, "dlopen: %s", dlerror());
		return -1;
	}
	abi = (int (*)(void))dlsym(dl, "fos_model_abi");
	info = (int (*)(const char **, const char **, const char **))
		dlsym(dl, "fos_model_info");
	chat = (int (*)(void *, const fos_msg *, int, int, char **))
		dlsym(dl, "fos_model_chat");
	if (!abi || !info || !chat) {
		snprintf(errbuf, esz, "缺少必需符号(abi/info/chat)");
		dlclose(dl);
		return -1;
	}
	if (abi() != FOS_MODEL_ABI_VERSION) {
		snprintf(errbuf, esz, "ABI 不兼容: got %d want %d",
			 abi(), FOS_MODEL_ABI_VERSION);
		dlclose(dl);
		return -1;
	}

	mload = (int (*)(void **))dlsym(dl, "fos_model_load");
	unload = (void (*)(void *))dlsym(dl, "fos_model_unload");
	if (mload && mload(&h) != 0) {
		snprintf(errbuf, esz, "模型初始化失败");
		dlclose(dl);
		return -1;
	}

	for (i = 0; i < MAX_MODELS; i++)
		if (!models[i].used) {
			m = &models[i];
			break;
		}
	if (!m) {
		snprintf(errbuf, esz, "模型槽位已满");
		if (unload)
			unload(h);
		dlclose(dl);
		return -1;
	}

	memset(m, 0, sizeof(*m));
	m->used = 1;
	m->dl = dl;
	m->handle = h;
	m->fn_chat = chat;
	m->fn_unload = unload;
	snprintf(m->path, sizeof(m->path), "%s", path);
	{
		const char *pn = "", *pv = "", *pd = "";

		info(&pn, &pv, &pd);
		snprintf(m->name, sizeof(m->name), "%s", pn);
		snprintf(m->version, sizeof(m->version), "%s", pv);
		snprintf(m->desc, sizeof(m->desc), "%s", pd);
	}
	if (!m->name[0])
		snprintf(m->name, sizeof(m->name), "model-%d", loaded);
	loaded++;
	LOGI("ai-bus", "已加载模型 '%s' v%s (%s)", m->name, m->version, path);
	return 0;
}

static void scan_dir(void)
{
	DIR *d = opendir(model_dir());
	struct dirent *e;
	char err[256];

	if (!d) {
		LOGW("ai-bus", "模型目录不可读: %s（总线先以空模型启动）",
		     model_dir());
		return;
	}
	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);

		if (n < 4 || strcmp(e->d_name + n - 3, ".so") != 0)
			continue;
		{
			char path[600];

			snprintf(path, sizeof(path), "%s/%s", model_dir(),
				 e->d_name);
			if (load_one(path, err, sizeof(err)) != 0)
				LOGW("ai-bus", "跳过 %s: %s", path, err);
		}
	}
	closedir(d);
}

static struct model *find_model(const char *name)
{
	int i, first_used = -1;

	if (!name || !name[0]) {
		for (i = 0; i < MAX_MODELS; i++) {
			if (!models[i].used)
				continue;
			if (models[i].is_default)
				return &models[i];
			if (first_used < 0)
				first_used = i;
		}
		return first_used >= 0 ? &models[first_used] : NULL;
	}
	for (i = 0; i < MAX_MODELS; i++)
		if (models[i].used && strcmp(models[i].name, name) == 0)
			return &models[i];
	return NULL;
}

/* 跳过 p 指向的字符串（含闭合引号），返回其后继；失败返回 end */
static const char *skip_str_or(const char *p, const char *end, char c)
{
	if (*p == '"') {
		p++;
		while (p < end) {
			if (*p == '\\') {
				p += 2;
				continue;
			}
			if (*p == '"')
				return p + 1;
			p++;
		}
		return end;
	}
	(void)c;
	return p + 1;
}

/* 从请求里抽取 messages 数组：字符串感知地扫描每个顶层 {...} 对象 */
static int extract_messages(const fos_jobj *req, fos_msg *msgs, int cap)
{
	static char roles[MAX_MSGS][16];
	static char texts[MAX_MSGS][1024];
	fos_jtok arr;
	const char *p, *end;
	int n = 0;

	if (fos_json_get(req, "messages", &arr))
		return 0;
	p = arr.s;
	end = arr.s + arr.len;
	while (p < end && n < cap) {
		const char *obj;
		int depth = 0;

		/* 找下一个对象起点 */
		obj = NULL;
		for (; p < end; p++) {
			if (*p == '{') {
				obj = p;
				break;
			}
			if (*p == '"') {
				p = skip_str_or(p, end, 0) - 1;
				continue;
			}
		}
		if (!obj)
			break;
		/* 字符串感知地找对象终点 */
		p = obj;
		while (p < end) {
			if (*p == '"') {
				p = skip_str_or(p, end, 0) - 1;
			} else if (*p == '{') {
				depth++;
			} else if (*p == '}') {
				depth--;
				if (!depth) {
					p++;
					break;
				}
			}
			p++;
		}
		if (depth)
			break;

		fos_jobj one;

		if (fos_json_parse(obj, (size_t)(p - obj), &one) == 0) {
			fos_json_get_str(&one, "role", "user",
					 roles[n], sizeof(roles[n]));
			fos_json_get_str(&one, "content", "",
					 texts[n], sizeof(texts[n]));
			msgs[n].role = roles[n];
			msgs[n].content = texts[n];
			n++;
		}
	}
	return n;
}

/* ── 命令实现 ── */

static int cmd_models_list(const fos_jobj *req, fos_jbuf *b,
			   char *em, size_t esz, void *user)
{
	(void)req; (void)em; (void)esz; (void)user;

	fj_begin_arr(b, "models");
	for (int i = 0; i < MAX_MODELS; i++) {
		if (!models[i].used)
			continue;
		fj_arr_obj(b);
		fj_kv_str(b, "name", models[i].name);
		fj_kv_str(b, "version", models[i].version);
		fj_kv_str(b, "desc", models[i].desc);
		fj_kv_str(b, "path", models[i].path);
		fj_end_obj(b);
	}
	fj_end_arr(b);
	fj_kv_i64(b, "count", loaded);
	fj_kv_str(b, "privacy", "all-local: 不联网、不烧 Token、不上传用户数据");
	return 0;
}

static int cmd_chat(const fos_jobj *req, fos_jbuf *b,
		    char *em, size_t esz, void *user)
{
	(void)user;
	struct model *m;
	fos_msg msgs[MAX_MSGS];
	char model_name[64] = "";
	int n, rc;
	char *out = NULL;

	fos_json_get_str(req, "model", "", model_name, sizeof(model_name));
	m = find_model(model_name[0] ? model_name : NULL);
	if (!m) {
		snprintf(em, esz, "没有可用模型（把符合规范的 .so 放进 %s）",
			 model_dir());
		return -32000;
	}
	n = extract_messages(req, msgs, MAX_MSGS);
	if (n <= 0) {
		snprintf(em, esz, "messages 为空或格式不对");
		return -32602;
	}
	rc = m->fn_chat(m->handle, msgs, n,
		       (int)fos_json_get_int(req, "max_tokens", 0), &out);
	if (rc != 0 || !out) {
		snprintf(em, esz, "模型推理失败(%d)", rc);
		free(out);
		return -32000;
	}
	fj_kv_str(b, "model", m->name);
	fj_kv_str(b, "text", out);
	free(out);   /* 契约：总线负责释放插件 malloc 的文本 */
	return 0;
}

static int cmd_model_load(const fos_jobj *req, fos_jbuf *b,
			  char *em, size_t esz, void *user)
{
	(void)b; (void)user;
	char path[512];
	char err[256];

	fos_json_get_str(req, "path", "", path, sizeof(path));
	if (!path[0] || strstr(path, "..")) {
		snprintf(em, esz, "缺少 path 或含 '..'");
		return -32602;
	}
	if (load_one(path, err, sizeof(err)) != 0) {
		snprintf(em, esz, "%s", err);
		return -32000;
	}
	return 0;
}

static int init_bus(void *user)
{
	(void)user;
	scan_dir();
	if (!loaded)
		LOGW("ai-bus", "未发现任何模型插件；chat 将返回错误直到有模型接入");
	return 0;
}

static void fini_bus(void *user)
{
	(void)user;
	for (int i = 0; i < MAX_MODELS; i++) {
		if (!models[i].used)
			continue;
		if (models[i].fn_unload)
			models[i].fn_unload(models[i].handle);
		dlclose(models[i].dl);
	}
}

static const fos_cmd_table TABLE[] = {
	{ "models.list", cmd_models_list, "列出已加载模型" },
	{ "chat",        cmd_chat,        "对话：{model?,messages,max_tokens?}" },
	{ "model.load",  cmd_model_load,  "热插一个模型 .so" },
	{ NULL, NULL, NULL }
};

int main(int argc, char **argv)
{
	return fos_daemon_main(argc, argv, "ai-bus", TABLE,
			       init_bus, fini_bus);
}
