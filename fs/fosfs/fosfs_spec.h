/* SPDX-License-Identifier: MIT */
/*
 * fosfs_spec.h — fosfs 磁盘格式字节布局契约（与 SPEC.md 同步维护）
 *
 * 开轮版内部存储格式。C 侧未来（内核态或用户态驱动）按本头文件
 * 解析；Python 参考实现（fosfs.py）必须与本布局逐字节一致，
 * CI 中用同一份测试卷互验。
 *
 * 全部字段小端（little-endian）。
 */
#ifndef FOSFS_SPEC_H
#define FOSFS_SPEC_H

#include <stdint.h>

/* ── superblock：512 字节 @ 偏移 0 ─────────────────────────── */

#define FOSFS_SB_MAGIC     "FOSFS001"
#define FOSFS_SB_SIZE      512
#define FOSFS_SB_VERSION   1u

#define FOSFS_FLAG_ENCRYPT_ALL 0x1u   /* 全盘加密（密钥槽派生不同） */

struct fosfs_superblock {
	uint8_t  magic[8];        /* +0  "FOSFS001" */
	uint32_t version;         /* +8  = FOSFS_SB_VERSION */
	uint32_t flags;           /* +12 FOSFS_FLAG_* */
	uint64_t created_ms;      /* +16 */
	uint8_t  uuid[16];        /* +24 */
	uint32_t hash_algo;       /* +40 0=sha256 */
	uint32_t comp_algo;       /* +44 0=zlib */
	uint64_t chunk_count;     /* +48 尽力维护值，verify 不信任 */
	uint64_t next_off;        /* +56 追加写偏移 */
	uint8_t  reserved[448];   /* +64 TLV 扩展从 reserved 尾部追加 */
} __attribute__((packed));

_Static_assert(sizeof(struct fosfs_superblock) == FOSFS_SB_SIZE,
	       "superblock must be 512B");

/* ── chunk header ─────────────────────────────────────────── */

#define FOSFS_CH_MAGIC     "FOSCHK01"
#define FOSFS_CH_HDR_SIZE  64

enum fosfs_chunk_type {
	FOSFS_T_DATA    = 1,   /* 数据块 */
	FOSFS_T_MANIFEST= 2,   /* 清单块（payload=JSON） */
	FOSFS_T_SNAPROOT= 3,   /* 快照根块（name=快照名） */
	FOSFS_T_TOMB    = 4,   /* 删除墓碑 */
};

#define FOSFS_COMP_RAW   0
#define FOSFS_COMP_ZLIB  1
#define FOSFS_ENC_NONE   0
#define FOSFS_ENC_SLOT1  1   /* 密钥槽 id */

/*
 * 布局（紧凑，64 字节定长头 + 变长体）：
 *   +0  magic[8]
 *   +8  type(1) comp(1) enc(1) rsv(1)
 *   +12 name_len(2) payload_len(4)
 *   +18 ext_len(2)  seq(4)
 *   +24 raw_len(8)
 *   +32 sha256[32]
 *   +64 name… ext… payload…
 *
 * sha256 覆盖**解压后**的 payload（内容寻址）。
 */
struct fosfs_chunk_header {
	uint8_t  magic[8];
	uint8_t  type, comp, enc, rsv;
	uint16_t name_len;
	uint32_t payload_len;
	uint16_t ext_len;
	uint32_t seq;
	uint64_t raw_len;
	uint8_t  sha256[32];
} __attribute__((packed));

_Static_assert(sizeof(struct fosfs_chunk_header) == 32 + 32,
	       "chunk header must be 64B");

/* ── TLV 扩展（ext 区，0 起）：[2B tag][2B len][value] ─────── */

#define FOSFS_EXT_TTL_MS   0x0001u   /* 缓存条目生存期（8B LE，毫秒） */
#define FOSFS_EXT_ORIGIN   0x0002u   /* 来源标记（字符串，如 "apk-conv"） */

#endif /* FOSFS_SPEC_H */
