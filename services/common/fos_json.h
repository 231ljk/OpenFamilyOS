/* SPDX-License-Identifier: MIT */
/*
 * fos_json.h — FamilyOS 极简 JSON 库（解析 + 构造）
 *
 * 只覆盖 IPC 协议需要的最小集：对象、字符串、数字、布尔、null。
 * 不支持数组内嵌对象的深度遍历（提供逐项提取的辅助函数）。
 * 设计目标：单文件 ~400 行、无依赖、可读、可被社区放心替换
 * （替换为 cJSON 等时只需保持同等函数签名，或直接改调用点）。
 */
#ifndef FOS_JSON_H
#define FOS_JSON_H

#include <stddef.h>
#include <stdint.h>

#define FOS_JSON_MAX_KEYS 24

/* 解析后的对象：键值对以「起点/长度」引用原文，避免复制 */
typedef struct {
	const char *s;   /* 原文指针 */
	size_t len;      /* 原文长度 */
} fos_jtok;

typedef struct {
	fos_jtok keys[FOS_JSON_MAX_KEYS];
	fos_jtok vals[FOS_JSON_MAX_KEYS];
	int n;
	int ok;
} fos_jobj;

/* 解析一行 JSON 对象。返回 0 成功，-1 失败（宽松失败：找不到对象即失败）。 */
int fos_json_parse(const char *line, size_t len, fos_jobj *out);

/* 按键取值：返回 0 命中；值写进 out（原始 token）。 */
int fos_json_get(const fos_jobj *o, const char *key, fos_jtok *out);

/* 便捷取值 */
int fos_json_get_str(const fos_jobj *o, const char *key, const char *def,
		     char *buf, size_t bufsz);
int64_t fos_json_get_int(const fos_jobj *o, const char *key, int64_t def);
int fos_json_get_bool(const fos_jobj *o, const char *key, int def);

/* 把字符串 token 解转义后写入 buf；返回实际长度或 -1 */
int fos_jtok_unescape(const fos_jtok *t, char *buf, size_t bufsz);

/* ── 构造（往固定缓冲区追加，溢出安全）── */
typedef struct {
	char *buf;
	size_t cap;
	size_t len;
	int ovf;           /* 一旦置位后续追加静默丢弃 */
	int need_comma;    /* 当前层级是否已写入过成员/元素 */
	int in_arr;        /* 当前层级是否为数组 */
	int depth;         /* 嵌套深度 */
	int saved_need[4]; /* 每层的"已写过成员"标志（支持 resp→result→arr→obj） */
	int saved_in_arr[4];
} fos_jbuf;

void fj_init(fos_jbuf *b, char *buf, size_t cap);
void fj_raw(fos_jbuf *b, const char *s);            /* 追加字面量 */
void fj_kv_str(fos_jbuf *b, const char *k, const char *v); /* 自动转义 */
void fj_kv_i64(fos_jbuf *b, const char *k, int64_t v);
void fj_kv_bool(fos_jbuf *b, const char *k, int v);
void fj_begin_obj(fos_jbuf *b, const char *k);      /* 嵌套对象 */
void fj_end_obj(fos_jbuf *b);
void fj_begin_arr(fos_jbuf *b, const char *k);
void fj_arr_str(fos_jbuf *b, const char *v);
void fj_arr_obj(fos_jbuf *b);                       /* 数组里开一个对象 */
void fj_end_arr(fos_jbuf *b);

#endif /* FOS_JSON_H */
