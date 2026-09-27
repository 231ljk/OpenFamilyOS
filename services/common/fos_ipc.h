/* SPDX-License-Identifier: MulanPSL-2.0 */
/*
 * fos_ipc.h — FamilyOS 守护进程公共骨架（协议见 docs/ipc-protocol.md）
 *
 * 服务端：单线程 poll，行分隔 JSON；每客户端一个读缓冲一个写队列，
 *   断连即清理；崩溃的客户端不会留残（fd 关闭由内核回收）。
 * 客户端：一次调用一条请求（connect→send→recv→close），简单可靠；
 *   守护进程间互相调用也走同一入口。
 */
#ifndef FOS_IPC_H
#define FOS_IPC_H

#include "fos_json.h"

#include <sys/types.h>
#include <unistd.h>

#define FOS_LINE_MAX   8192   /* 单报文上限 */
#define FOS_SOCK_DIR   "/run/fos"

/*
 * 请求处理器。req 已解析；resp 为输出缓冲区，**约定**：
 *   - 成功：返回 0，并用 fj_* 向 resp 写入 result 对象的内部字段
 *     （框架已写好 {"id":..,"ok":true,"result":{ 前缀，你只填 kv，
 *     最后调用处自动补 }}）。
 *   - 失败：返回 -EXXX 负 errno，并可选在 errmsg 里给出说明
 *     （非 NULL 时写进 error.msg）。
 */
typedef int (*fos_cmd_fn)(const fos_jobj *req, fos_jbuf *resp,
			  char *errmsg, size_t errmsg_sz, void *user);

typedef struct {
	const char *cmd;      /* 如 "devices.list" */
	fos_cmd_fn fn;
	const char *help;
} fos_cmd_table;

/* 创建并绑定监听 socket；已存在的 socket 文件会被替换。返回 fd 或 -1 */
int fos_ipc_listen(const char *path, int perm);

/* 运行服务主循环（阻塞）。quit_fd 若 >0，被写入任意字节时优雅退出。 */
int fos_ipc_serve(int listen_fd, const fos_cmd_table *table, void *user,
		  int quit_fd);

/* 客户端：向 socket 发一行请求，读一行响应。返回 0 成功；
 * resp 内为原始响应文本（可为 NULL）。超时 5 秒。 */
int fos_ipc_call(const char *path, const char *req_line,
		 char *resp, size_t resp_sz);

/* 便捷：构造错误响应行（含 id/code/msg） */
void fos_ipc_err_raw(char *buf, size_t sz, int id, int code, const char *msg);

/* 当前正在处理请求的对端 uid（服务端鉴权用，进程内全局） */
uid_t fos_ipc_peer_uid(void);

/* socket 路径助手：FOS_RUN_DIR 环境变量优先（测试用） */
void fos_ipc_sockpath(const char *name, char *buf, size_t sz);

#endif /* FOS_IPC_H */
