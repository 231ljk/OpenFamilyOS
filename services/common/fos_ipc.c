// SPDX-License-Identifier: MIT
/*
 * fos_ipc.c — 守护进程公共骨架实现（协议见 docs/ipc-protocol.md）。
 *
 * 事件循环：poll({quit_pipe, listen_fd} + 已用槽 clients[])
 *   - idx 0 可读     → accept4 新连接，放入空闲槽（满则直接 close）
 *   - idx 1 可读     → SIGTERM/SIGINT 触发，排空缓冲后优雅退出
 *   - clients[i] 可读 → 累积到行边界，逐行 dispatch + 写回
 * 请求处理器约定同步返回（参考实现的处理器均为内存操作，毫秒级）。
 *
 * 安全边界：
 *   - 每客户端读取累积超 FOS_LINE_MAX（行协议却拿不到换行）即断连；
 *   - 对端 uid 通过 SO_PEERCRED 获取，处理器可用 fos_ipc_peer_uid()
 *     做写命令鉴权（root/fos 服务账号才可改状态）。
 */
#include "fos_ipc.h"
#include "fos_log.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS 64

/* 当前正在处理的请求来自哪个 uid（仅 dispatch 期间有效） */
static uid_t g_peer_uid;

uid_t fos_ipc_peer_uid(void)
{
	return g_peer_uid;
}

struct client {
	int fd;
	char rbuf[FOS_LINE_MAX * 2];
	size_t rlen;
	uid_t peer_uid;      /* SO_PEERCRED 获取，dispatch 期间生效 */
};

static struct client *clients;

/* ────────────────────── socket 路径 ────────────────────── */

void fos_ipc_sockpath(const char *name, char *buf, size_t sz)
{
	const char *dir = getenv("FOS_RUN_DIR");

	if (dir && *dir)
		snprintf(buf, sz, "%s/%s.sock", dir, name);
	else
		snprintf(buf, sz, "%s/%s.sock", FOS_SOCK_DIR, name);
}

/* ────────────────────── 服务端 ────────────────────── */

int fos_ipc_listen(const char *path, int perm)
{
	struct sockaddr_un un;
	int fd, on = 1;

	if (strlen(path) >= sizeof(un.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);

	/* 陈旧 socket 文件（上次异常退出残留）直接替换 */
	unlink(path);
	if (bind(fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
		int e = errno;

		close(fd);
		errno = e;
		return -1;
	}
	if (chmod(path, perm) < 0) {
		int e = errno;

		close(fd);
		errno = e;
		return -1;
	}
	if (listen(fd, 16) < 0) {
		int e = errno;

		close(fd);
		unlink(path);
		errno = e;
		return -1;
	}
	return fd;
}

/* 把一行完整写出（忽略对端断开：那是 poll 要回收的事） */
static void write_all(int fd, const char *s, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t w = write(fd, s + off, n - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		off += (size_t)w;
	}
}

/*
 * 处理一行请求，生成一行响应。
 * 框架负责外壳 {"id":..,"ok":..,...}；处理器只填 result 对象的成员。
 */
static void dispatch(const char *line, size_t len,
		     const fos_cmd_table *table, void *user,
		     char *out, size_t outsz)
{
	fos_jobj req;
	fos_jbuf b;
	fos_jtok t;
	char cmd[64] = "";
	char idbuf[24];
	int i, id = 0;

	if (fos_json_parse(line, len, &req) != 0 ||
	    fos_json_get(&req, "id", &t) != 0 || !req.ok) {
		fos_ipc_err_raw(out, outsz, 0, -32600, "invalid request");
		return;
	}
	id = (int)fos_json_get_int(&req, "id", 0);
	fos_json_get_str(&req, "cmd", "", cmd, sizeof(cmd));

	fj_init(&b, out, outsz);
	snprintf(idbuf, sizeof(idbuf), "%d", id);
	fj_raw(&b, "{\"id\":");
	fj_raw(&b, idbuf);

	for (i = 0; table[i].cmd; i++) {
		fos_jtok argv;
		fos_jobj args;
		char errmsg[256] = "";
		int rc, have_args;

		if (strcmp(table[i].cmd, cmd) != 0)
			continue;

		/* 先写外壳：{"id":N,"ok":true,"result":{ */
		fj_kv_bool(&b, "ok", 1);
		fj_begin_obj(&b, "result");

		/* 处理器收到的是 args 对象（协议：参数在 args 里） */
		have_args = (fos_json_get(&req, "args", &argv) == 0 &&
			     argv.len >= 2 && argv.s[0] == '{' &&
			     fos_json_parse(argv.s, argv.len, &args) == 0);
		rc = table[i].fn(have_args ? &args : &req, &b,
				 errmsg, sizeof(errmsg), user);
		if (rc == 0 && !b.ovf) {
			fj_end_obj(&b);      /* 关 result */
			fj_raw(&b, "}");     /* 关顶层 */
			if (b.ovf)
				fos_ipc_err_raw(out, outsz, id, -32603,
						"response too large");
			return;
		}
		/* 失败：重置缓冲区写错误体 */
		fj_init(&b, out, outsz);
		fj_raw(&b, "{\"id\":");
		fj_raw(&b, idbuf);
		fj_kv_bool(&b, "ok", 0);
		fj_begin_obj(&b, "error");
		fj_kv_i64(&b, "code", rc ? rc : -32603);
		fj_kv_str(&b, "msg", errmsg[0] ? errmsg : "handler failed");
		fj_end_obj(&b);
		fj_raw(&b, "}");
		return;
	}
	fos_ipc_err_raw(out, outsz, id, -32601, "unknown cmd");
}

void fos_ipc_err_raw(char *buf, size_t sz, int id, int code, const char *msg)
{
	fos_jbuf b;
	char idbuf[24];

	fj_init(&b, buf, sz);
	snprintf(idbuf, sizeof(idbuf), "%d", id);
	fj_raw(&b, "{\"id\":");
	fj_raw(&b, idbuf);
	fj_kv_bool(&b, "ok", 0);
	fj_begin_obj(&b, "error");
	fj_kv_i64(&b, "code", code);
	fj_kv_str(&b, "msg", msg);
	fj_end_obj(&b);
	fj_raw(&b, "}");
}

/* 读入并处理某客户端缓冲区里的所有完整行；返回 0 继续，-1 应断连 */
static int pump_client(struct client *c, const fos_cmd_table *table, void *user)
{
	char out[FOS_LINE_MAX * 2];

	for (;;) {
		char *nl = memchr(c->rbuf, '\n', c->rlen);
		size_t line_len;

		if (!nl)
			return 0;
		line_len = (size_t)(nl - c->rbuf);
		if (line_len >= FOS_LINE_MAX)
			return -1;            /* 无换行的超长行：视为攻击 */

		dispatch(c->rbuf, line_len, table, user, out, sizeof(out));
		{
			size_t olen = strlen(out);

			if (olen + 1 < sizeof(out)) {
				out[olen] = '\n';
				write_all(c->fd, out, olen + 1);
			} else {
				LOGW("ipc", "响应溢出被截断");
			}
		}
		memmove(c->rbuf, nl + 1, c->rlen - line_len - 1);
		c->rlen -= line_len + 1;
	}
}

int fos_ipc_serve(int listen_fd, const fos_cmd_table *table, void *user,
		  int quit_fd)
{
	struct pollfd pfd[MAX_CLIENTS + 2];
	int i, rc;

	clients = calloc(MAX_CLIENTS, sizeof(*clients));
	if (!clients)
		return -ENOMEM;

	for (;;) {
		nfds_t n = 0;

		pfd[n].fd = quit_fd > 0 ? quit_fd : listen_fd;
		pfd[n].events = POLLIN;
		pfd[n].revents = 0;
		n++;
		if (quit_fd > 0) {
			pfd[n].fd = listen_fd;
			pfd[n].events = POLLIN;
			pfd[n].revents = 0;
			n++;
		}
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd <= 0)
				continue;
			pfd[n].fd = clients[i].fd;
			pfd[n].events = POLLIN;
			pfd[n].revents = 0;
			n++;
		}

		rc = poll(pfd, n, 1000);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			goto out;
		}
		if (rc == 0)
			continue;

		/* quit 信号 */
		if (quit_fd > 0 && pfd[0].revents & POLLIN) {
			rc = 0;
			goto out;
		}

		/* 新连接 */
		{
			int li = quit_fd > 0 ? 1 : 0;

			if (pfd[li].revents & POLLIN) {
				int cfd = accept4(listen_fd, NULL, NULL,
						  SOCK_CLOEXEC | SOCK_NONBLOCK);

				if (cfd >= 0) {
					for (i = 0; i < MAX_CLIENTS; i++) {
						struct ucred cred;
						socklen_t cl = sizeof(cred);

						if (clients[i].fd <= 0) {
							clients[i].fd = cfd;
							clients[i].rlen = 0;
							clients[i].peer_uid = 0;
							if (getsockopt(cfd, SOL_SOCKET,
								       SO_PEERCRED, &cred,
								       &cl) == 0)
								clients[i].peer_uid =
									cred.uid;
							break;
						}
					}
					if (i == MAX_CLIENTS)
						close(cfd);   /* 容量保护 */
				}
			}
		}

		/* 客户端数据 */
		{
			int base = (quit_fd > 0 ? 2 : 1);

			for (i = 0; i < MAX_CLIENTS &&
				base + i < (int)n; i++) {
				struct client *c = &clients[i];
				ssize_t r;

				if (c->fd <= 0 || !(pfd[base + i].revents & POLLIN))
					continue;
				r = read(c->fd, c->rbuf + c->rlen,
					 sizeof(c->rbuf) - c->rlen - 1);
				if (r == 0) {          /* EOF */
					close(c->fd);
					memset(c, 0, sizeof(*c));
					continue;
				}
				if (r < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK ||
					    errno == EINTR)
						continue;
					close(c->fd);
					memset(c, 0, sizeof(*c));
					continue;
				}
				c->rlen += (size_t)r;
				c->rbuf[c->rlen] = 0;
				g_peer_uid = c->peer_uid;
				if (pump_client(c, table, user) < 0) {
					close(c->fd);
					memset(c, 0, sizeof(*c));
				}
			}
		}

		/* poll 超时也回来一次：便于将来加周期任务 */
	}
out:
	for (i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd > 0)
			close(clients[i].fd);
	free(clients);
	clients = NULL;
	return rc;
}

/* ────────────────────── 客户端 ────────────────────── */

int fos_ipc_call(const char *path, const char *req_line,
		 char *resp, size_t resp_sz)
{
	struct sockaddr_un un;
	struct pollfd pf;
	char rbuf[FOS_LINE_MAX * 2];
	size_t rlen = 0;
	int fd, ret;

	if (resp && resp_sz)
		resp[0] = 0;
	if (strlen(path) >= sizeof(un.sun_path))
		return -ENAMETOOLONG;
	if (strchr(req_line, '\n') == NULL)
		return -EINVAL;      /* 请求必须是完整一行 */

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);

	ret = connect(fd, (struct sockaddr *)&un, sizeof(un));
	if (ret < 0) {
		ret = -errno;
		goto out;
	}
	if (write(fd, req_line, strlen(req_line)) < 0) {
		ret = -errno;
		goto out;
	}

	/* 等一行响应（5 秒超时） */
	for (;;) {
		pf.fd = fd;
		pf.events = POLLIN;
		ret = poll(&pf, 1, 5000);
		if (ret == 0) {
			ret = -ETIMEDOUT;
			goto out;
		}
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			goto out;
		}
		ret = (int)recv(fd, rbuf + rlen, sizeof(rbuf) - rlen - 1, 0);
		if (ret <= 0) {
			ret = ret == 0 ? -ECONNRESET : -errno;
			goto out;
		}
		rlen += (size_t)ret;
		rbuf[rlen] = 0;
		if (memchr(rbuf, '\n', rlen))
			break;
	}

	{
		char *nl = memchr(rbuf, '\n', rlen);
		size_t olen = (size_t)(nl - rbuf);

		if (resp && resp_sz) {
			if (olen >= resp_sz)
				olen = resp_sz - 1;
			memcpy(resp, rbuf, olen);
			resp[olen] = 0;
		}
	}
	ret = 0;
out:
	close(fd);
	return ret;
}
