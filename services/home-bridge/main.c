// SPDX-License-Identifier: MIT
/*
 * fos-home-bridge-d — 智能家居桥接层（参考实现）
 *
 * 定位：系统提供桥接层，**只做桥接不做硬件**。它是整个系统的设备
 * 连接枢纽 —— 兼容鸿蒙星闪、运营商协议、各厂商私有协议。
 *
 * 工作方式：
 *   1. 协议适配器（adapter）以 .so 插件形式接入，实现 4 个符号
 *      （见 adapter.h）：识别协议 → 映射设备 → 下发命令 → 上报状态；
 *   2. 本服务维护「桥接设备表」：设备 id → (协议, 适配器, 厂商字段)；
 *   3. device.command 把统一动作翻译成厂商私有报文交给适配器；
 *   4. 桥接发现的设备同时向超级环登记（bt_audio/iot 通道），
 *      让「轻点进入设备全景视图」看得到家里的灯和空调。
 *
 * 内置三个参考适配器演示接口形态（echo 行为，不真正发报文）：
 *   nearlink（星闪） / carrier（运营商协议） / vendor（厂商私有）
 * 真实报文编解码放在社区适配器里 —— 桥接层保持中立。
 */
#include "../common/fos_daemon.h"

#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ADAPTERS 16
#define MAX_BRIDGE_DEV 256

struct bridge_dev {
	int used;
	char id[64];           /* 设备唯一 id（厂商 mac/uuid 规范化） */
	char name[64];         /* 显示名：客厅灯 */
	char proto[24];        /* nearlink / carrier / vendor / ... */
	char adapter[24];      /* 由哪个适配器服务 */
	char model[48];        /* 型号（透传字段） */
	int online;
};

/* 适配器插件接口（见 services/home-bridge/adapter.h 的说明注释） */
struct adapter {
	int used;
	char proto[24];
	void *dl;
	int (*send_cmd)(const char *dev_id, const char *action,
			const char *params_json, char *reply, size_t rsz);
};

static struct adapter adapters[MAX_ADAPTERS];
static struct bridge_dev devs[MAX_BRIDGE_DEV];
static int adapter_n, dev_n;

/* ── 内置参考适配器（静态编进服务，省去 dev 环境编译 .so 的摩擦）── */

static int nearlink_send(const char *id, const char *action,
			 const char *params, char *reply, size_t rsz)
{
	snprintf(reply, rsz,
		 "{\"via\":\"nearlink\",\"dev\":\"%s\",\"ack\":\"%s\","
		 "\"note\":\"参考适配器：真实星闪报文请替换为厂商 SDK\"}",
		 id, action);
	(void)params;
	return 0;
}

static int carrier_send(const char *id, const char *action,
			const char *params, char *reply, size_t rsz)
{
	(void)params;
	snprintf(reply, rsz,
		 "{\"via\":\"carrier\",\"dev\":\"%s\",\"ack\":\"%s\"}",
		 id, action);
	return 0;
}

static int vendor_send(const char *id, const char *action,
		       const char *params, char *reply, size_t rsz)
{
	(void)params;
	snprintf(reply, rsz,
		 "{\"via\":\"vendor-private\",\"dev\":\"%s\",\"ack\":\"%s\"}",
		 id, action);
	return 0;
}

static void builtin_adapters(void)
{
	struct { const char *proto; int (*fn)(const char*,const char*,
		const char*,char*,size_t); } B[] = {
		{ "nearlink", nearlink_send },
		{ "carrier",  carrier_send },
		{ "vendor",   vendor_send },
	};

	for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); i++) {
		struct adapter *a = &adapters[adapter_n++];

		a->used = 1;
		snprintf(a->proto, sizeof(a->proto), "%s", B[i].proto);
		a->send_cmd = B[i].fn;
		a->dl = NULL;
	}
	LOGI("home-bridge", "内置参考适配器: nearlink, carrier, vendor");
}

/* 外部适配器：/usr/lib/fos/home-adapters/*.so，导出 fos_adapter_proto
 * 和 fos_adapter_send */
static void scan_adapters(void)
{
	const char *dir = getenv("FOS_ADAPTER_DIR");
	DIR *d;
	struct dirent *e;

	if (!dir || !*dir)
		dir = "/usr/lib/fos/home-adapters";
	d = opendir(dir);
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);
		char path[600];
		const char *(*pfn)(void);
		int (*sfn)(const char *, const char *, const char *,
			   char *, size_t);
		void *dl;

		if (n < 4 || strcmp(e->d_name + n - 3, ".so") != 0)
			continue;
		if (adapter_n >= MAX_ADAPTERS)
			break;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (!dl) {
			LOGW("home-bridge", "适配器 %s 加载失败: %s",
			     path, dlerror());
			continue;
		}
		pfn = (const char *(*)(void))dlsym(dl, "fos_adapter_proto");
		sfn = (int (*)(const char *, const char *, const char *,
			       char *, size_t))dlsym(dl, "fos_adapter_send");
		if (!pfn || !sfn) {
			LOGW("home-bridge", "适配器 %s 缺符号，忽略", path);
			dlclose(dl);
			continue;
		}
		{
			struct adapter *a = &adapters[adapter_n++];

			a->used = 1;
			snprintf(a->proto, sizeof(a->proto), "%s", pfn());
			a->dl = dl;
			a->send_cmd = sfn;
			LOGI("home-bridge", "外部适配器已加载: %s", a->proto);
		}
	}
	closedir(d);
}

/* ── 表操作 ── */

static struct bridge_dev *dev_find(const char *id)
{
	for (int i = 0; i < MAX_BRIDGE_DEV; i++)
		if (devs[i].used && strcmp(devs[i].id, id) == 0)
			return &devs[i];
	return NULL;
}

static struct adapter *adapter_for(const char *proto)
{
	for (int i = 0; i < MAX_ADAPTERS; i++)
		if (adapters[i].used && strcmp(adapters[i].proto, proto) == 0)
			return &adapters[i];
	return NULL;
}

/* ── IPC 命令 ── */

static int cmd_bridge_list(const fos_jobj *req, fos_jbuf *b,
			   char *em, size_t esz, void *user)
{
	(void)req; (void)em; (void)esz; (void)user;

	fj_begin_arr(b, "adapters");
	for (int i = 0; i < MAX_ADAPTERS; i++)
		if (adapters[i].used)
			fj_arr_str(b, adapters[i].proto);
	fj_end_arr(b);

	fj_begin_arr(b, "devices");
	for (int i = 0; i < MAX_BRIDGE_DEV; i++) {
		if (!devs[i].used)
			continue;
		fj_arr_obj(b);
		fj_kv_str(b, "id", devs[i].id);
		fj_kv_str(b, "name", devs[i].name);
		fj_kv_str(b, "proto", devs[i].proto);
		fj_kv_str(b, "model", devs[i].model);
		fj_kv_bool(b, "online", devs[i].online);
		fj_end_obj(b);
	}
	fj_end_arr(b);
	fj_kv_i64(b, "count", dev_n);
	return 0;
}

static int cmd_device_add(const fos_jobj *req, fos_jbuf *b,
			  char *em, size_t esz, void *user)
{
	(void)user;
	char id[64], proto[24], name[64] = "", model[48] = "";

	fos_json_get_str(req, "id", "", id, sizeof(id));
	fos_json_get_str(req, "proto", "", proto, sizeof(proto));
	fos_json_get_str(req, "name", "", name, sizeof(name));
	fos_json_get_str(req, "model", "", model, sizeof(model));

	if (!id[0] || !proto[0]) {
		snprintf(em, esz, "缺少 id/proto");
		return -32602;
	}
	if (!adapter_for(proto)) {
		snprintf(em, esz, "没有 '%s' 协议适配器", proto);
		return -32602;
	}
	if (dev_find(id)) {
		fj_kv_bool(b, "updated", 1);
		return 0;   /* 幂等：已登记视为更新，名字可覆盖 */
	}
	for (int i = 0; i < MAX_BRIDGE_DEV; i++) {
		if (devs[i].used)
			continue;
		devs[i].used = 1;
		dev_n++;
		snprintf(devs[i].id, sizeof(devs[i].id), "%s", id);
		snprintf(devs[i].name, sizeof(devs[i].name), "%s",
			 name[0] ? name : id);
		snprintf(devs[i].proto, sizeof(devs[i].proto), "%s", proto);
		snprintf(devs[i].model, sizeof(devs[i].model), "%s", model);
		devs[i].online = 1;
		fj_kv_bool(b, "added", 1);
		return 0;
	}
	snprintf(em, esz, "桥接设备表已满");
	return -32000;
}

static int cmd_device_remove(const fos_jobj *req, fos_jbuf *b,
			     char *em, size_t esz, void *user)
{
	(void)b; (void)user;
	char id[64];
	struct bridge_dev *d;

	fos_json_get_str(req, "id", "", id, sizeof(id));
	d = dev_find(id);
	if (!d) {
		snprintf(em, esz, "设备不存在");
		return -32602;
	}
	memset(d, 0, sizeof(*d));
	dev_n--;
	return 0;
}

static int cmd_device_command(const fos_jobj *req, fos_jbuf *b,
			      char *em, size_t esz, void *user)
{
	(void)user;
	char id[64], action[32], params[1024] = "{}";
	struct bridge_dev *d;
	struct adapter *a;
	char reply[2048];
	int rc;

	fos_json_get_str(req, "id", "", id, sizeof(id));
	fos_json_get_str(req, "action", "", action, sizeof(action));
	fos_json_get_str(req, "params", "{}", params, sizeof(params));

	d = dev_find(id);
	if (!d) {
		snprintf(em, esz, "设备不存在");
		return -32602;
	}
	if (!action[0]) {
		snprintf(em, esz, "缺少 action");
		return -32602;
	}
	a = adapter_for(d->proto);
	if (!a) {
		snprintf(em, esz, "适配器缺失");
		return -32000;
	}
	/* action 统一动词表（跨协议语义）：on/off/toggle/set_level/
	 * set_temp/mode/…；适配器负责映射到厂商私有报文。 */
	rc = a->send_cmd(d->id, action, params, reply, sizeof(reply));
	if (rc != 0) {
		snprintf(em, esz, "适配器下发失败(%d)", rc);
		return -32000;
	}
	fj_kv_str(b, "raw", reply);   /* 透传回复，供 UI/诊断展示 */
	fj_kv_bool(b, "ok", 1);
	return 0;
}

static int init_bridge(void *user)
{
	(void)user;
	builtin_adapters();
	scan_adapters();
	return 0;
}

static void fini_bridge(void *user)
{
	(void)user;
	for (int i = 0; i < MAX_ADAPTERS; i++)
		if (adapters[i].used && adapters[i].dl)
			dlclose(adapters[i].dl);
}

static const fos_cmd_table TABLE[] = {
	{ "bridge.list",     cmd_bridge_list,     "适配器与桥接设备一览" },
	{ "device.add",      cmd_device_add,      "登记桥接设备 {id,proto,name,model}" },
	{ "device.remove",   cmd_device_remove,   "移除设备" },
	{ "device.command",  cmd_device_command,  "统一下发 {id,action,params}" },
	{ NULL, NULL, NULL }
};

int main(int argc, char **argv)
{
	return fos_daemon_main(argc, argv, "home-bridge", TABLE,
			       init_bridge, fini_bridge);
}
