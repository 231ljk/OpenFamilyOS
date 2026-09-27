// SPDX-License-Identifier: MulanPSL-2.0
/*
 * fos_json.c — 见 fos_json.h 设计说明。
 * 解析采用「记录原文偏移」策略，取值时再解转义；构造采用固定缓冲
 * + 溢出标记，保证守护进程不会因为超长响应越界写。
 */
#include "fos_json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ───────────────────────── 解析 ───────────────────────── */

static const char *j_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
		p++;
	return p;
}

static const char *j_str(const char *p, const char *end)
{
	if (p >= end || *p != '"')
		return NULL;
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
	return NULL;
}

/* 跳过一个任意 JSON 值，返回其后继位置（不含外层引号） */
static const char *j_skip(const char *p, const char *end)
{
	const char *q;
	int depth;

	p = j_ws(p, end);
	if (p >= end)
		return NULL;
	switch (*p) {
	case '"':
		return j_str(p, end);
	case '{':
	case '[': {
		char close = (*p == '{') ? '}' : ']';

		depth = 1;
		p++;
		while (p < end && depth) {
			if (*p == '"') {
				p = j_str(p, end);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[')
				depth++;
			else if (*p == '}' || *p == ']')
				depth--;
			p++;
		}
		if (depth)
			return NULL;
		(void)close;
		return p;
	}
	default:
		q = p;
		while (q < end && *q != ',' && *q != '}' && *q != ']' &&
		       *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n')
			q++;
		return (q > p) ? q : NULL;
	}
}

/* 解析一个对象体（p 指向 '{'），写入 out；返回 '}' 后继位置 */
static const char *j_parse_obj(const char *p, const char *end, fos_jobj *out)
{
	out->n = 0;
	out->ok = 0;
	p = j_ws(p + 1, end);
	if (p >= end)
		return NULL;
	if (*p == '}') {
		out->ok = 1;
		return p + 1;
	}
	for (;;) {
		const char *ks, *ke, *vs, *ve;

		p = j_ws(p, end);
		if (p >= end || *p != '"')
			return NULL;
		ks = p + 1;
		p = j_str(p, end);
		if (!p)
			return NULL;
		ke = p - 1;                       /* 闭合引号前 */

		p = j_ws(p, end);
		if (p >= end || *p != ':')
			return NULL;
		p = j_ws(p + 1, end);
		vs = p;
		p = j_skip(p, end);
		if (!p)
			return NULL;
		ve = p;

		if (out->n < FOS_JSON_MAX_KEYS) {
			out->keys[out->n].s = ks;
			out->keys[out->n].len = (size_t)(ke - ks);
			out->vals[out->n].s = vs;
			out->vals[out->n].len = (size_t)(ve - vs);
			out->n++;
		}
		p = j_ws(p, end);
		if (p >= end)
			return NULL;
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '}') {
			out->ok = 1;
			return p + 1;
		}
		return NULL;
	}
}

int fos_json_parse(const char *line, size_t len, fos_jobj *out)
{
	const char *p = j_ws(line, line + len);

	if (p >= line + len || *p != '{')
		return -1;
	memset(out, 0, sizeof(*out));
	return j_parse_obj(p, line + len, out) ? 0 : -1;
}

int fos_json_get(const fos_jobj *o, const char *key, fos_jtok *out)
{
	size_t klen = strlen(key);
	int i;

	for (i = 0; i < o->n; i++)
		if (o->keys[i].len == klen &&
		    strncmp(o->keys[i].s, key, klen) == 0) {
			if (out)
				*out = o->vals[i];
			return 0;
		}
	return -1;
}

/* token 是否等于裸字面量（true/false/null/数字） */
static int tok_is(const fos_jtok *t, const char *lit)
{
	return t->len == strlen(lit) && strncmp(t->s, lit, t->len) == 0;
}

int fos_json_get_bool(const fos_jobj *o, const char *key, int def)
{
	fos_jtok t;

	if (fos_json_get(o, key, &t))
		return def;
	if (tok_is(&t, "true"))
		return 1;
	if (tok_is(&t, "false"))
		return 0;
	return def;
}

int64_t fos_json_get_int(const fos_jobj *o, const char *key, int64_t def)
{
	char tmp[32];
	fos_jtok t;

	if (fos_json_get(o, key, &t))
		return def;
	if (t.len >= sizeof(tmp))
		return def;
	memcpy(tmp, t.s, t.len);
	tmp[t.len] = 0;
	return strtoll(tmp, NULL, 10);
}

int fos_jtok_unescape(const fos_jtok *t, char *buf, size_t bufsz)
{
	const char *p = t->s, *end = t->s + t->len;
	size_t n = 0;

	if (p >= end || *p != '"') {            /* 非字符串 token */
		if (bufsz)
			buf[0] = 0;
		return -1;
	}
	p++;
	while (p < end) {
		char c = *p++;

		if (c == '\\' && p < end) {
			switch (*p++) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'u':
				/* 参考实现：非 BMP 一律 '?'，社区替换时再完善 */
				p += 4;
				c = '?';
				break;
			default:
				/* 未知转义（\b \f 等）：保留字符本身 */
				c = p[-1];
				break;
			}
		}
		if (n + 1 >= bufsz)
			break;
		buf[n++] = c;
	}
	buf[n] = 0;
	return (int)n;
}

int fos_json_get_str(const fos_jobj *o, const char *key, const char *def,
		     char *buf, size_t bufsz)
{
	fos_jtok t;

	if (fos_json_get(o, key, &t)) {
		snprintf(buf, bufsz, "%s", def ? def : "");
		return -1;
	}
	return fos_jtok_unescape(&t, buf, bufsz);
}

/* ───────────────────────── 构造 ───────────────────────── */

void fj_init(fos_jbuf *b, char *buf, size_t cap)
{
	b->buf = buf;
	b->cap = cap;
	b->len = 0;
	b->ovf = 0;
	b->need_comma = 0;
	b->in_arr = 0;
	b->saved_need[0] = b->saved_need[1] = 0;
	b->saved_in_arr[0] = b->saved_in_arr[1] = 0;
	b->depth = 0;
	buf[0] = 0;
}

static void fjp_add(fos_jbuf *b, const char *s, size_t n)
{
	if (b->ovf || b->len + n + 1 > b->cap) {
		b->ovf = 1;
		return;
	}
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = 0;
}

/* 成员/元素分隔：需要时补逗号，然后置位标志 */
static void fjp_sep(fos_jbuf *b)
{
	if (b->need_comma)
		fjp_add(b, ",", 1);
	b->need_comma = 1;
}

void fj_raw(fos_jbuf *b, const char *s)
{
	fjp_add(b, s, strlen(s));
}

static void fj_escaped(fos_jbuf *b, const char *v)
{
	fjp_add(b, "\"", 1);
	while (*v) {
		unsigned char c = (unsigned char)*v++;

		switch (c) {
		case '"':  fjp_add(b, "\\\"", 2); break;
		case '\\': fjp_add(b, "\\\\", 2); break;
		case '\n': fjp_add(b, "\\n", 2); break;
		case '\r': fjp_add(b, "\\r", 2); break;
		case '\t': fjp_add(b, "\\t", 2); break;
		default:
			if (c < 0x20) {
				char u[8];

				snprintf(u, sizeof(u), "\\u%04x", c);
				fjp_add(b, u, 6);
			} else {
				fjp_add(b, (const char *)&c, 1);
			}
		}
	}
	fjp_add(b, "\"", 1);
}

/* 在「当前层级」写入一个具名成员的前缀："key": */
static void fjp_member(fos_jbuf *b, const char *k)
{
	fjp_sep(b);
	fj_escaped(b, k);
	fjp_add(b, ":", 1);
}

void fj_kv_str(fos_jbuf *b, const char *k, const char *v)
{
	fjp_member(b, k);
	fj_escaped(b, v ? v : "");
}

void fj_kv_i64(fos_jbuf *b, const char *k, int64_t v)
{
	char tmp[32];

	fjp_member(b, k);
	snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
	fjp_add(b, tmp, strlen(tmp));
}

void fj_kv_bool(fos_jbuf *b, const char *k, int v)
{
	fjp_member(b, k);
	fjp_add(b, v ? "true" : "false", v ? 4 : 5);
}

/* 进入嵌套容器（对象/数组）：保存外层状态 */
static void fjp_push(fos_jbuf *b, int in_arr)
{
	if (b->depth >= 4)
		return;   /* 超出支持深度：后续追加会因状态混乱被 cap 拦住 */
	b->saved_need[b->depth] = b->need_comma;
	b->saved_in_arr[b->depth] = b->in_arr;
	b->depth++;
	b->need_comma = 0;
	b->in_arr = in_arr;
}

static void fjp_pop(fos_jbuf *b)
{
	if (b->depth == 0)
		return;
	b->depth--;
	/* 恢复外层状态：saved_need 在容器作为成员/元素写入后保存，
	 * 本就应为 1，pop 后外层下一个成员前需要逗号。 */
	b->need_comma = b->saved_need[b->depth];
	b->in_arr = b->saved_in_arr[b->depth];
}

void fj_begin_obj(fos_jbuf *b, const char *k)
{
	if (k)
		fjp_member(b, k);
	else if (b->in_arr)
		fjp_sep(b);            /* 数组内直接开对象 */
	fjp_add(b, "{", 1);
	fjp_push(b, 0);
}

void fj_end_obj(fos_jbuf *b)
{
	fjp_add(b, "}", 1);
	fjp_pop(b);
}

void fj_begin_arr(fos_jbuf *b, const char *k)
{
	if (k)
		fjp_member(b, k);
	else if (b->in_arr)
		fjp_sep(b);
	fjp_add(b, "[", 1);
	fjp_push(b, 1);
}

void fj_end_arr(fos_jbuf *b)
{
	fjp_add(b, "]", 1);
	fjp_pop(b);
}

void fj_arr_str(fos_jbuf *b, const char *v)
{
	fjp_sep(b);
	fj_escaped(b, v ? v : "");
}

/* 数组里开一个对象：等价于 fj_begin_obj(b, NULL) */
void fj_arr_obj(fos_jbuf *b)
{
	fj_begin_obj(b, NULL);
}
