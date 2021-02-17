#ifndef __SHMEM_FS_H
#define __SHMEM_FS_H

/* inode in-kernel data */

#define SHMEM_NR_DIRECT 16

/*
 * A swap entry has to fit into a "unsigned long", as
 * the entry is hidden in the "index" field of the
 * swapper address space.
 *
 * We have to move it here, since not every user of fs.h is including
 * mm.h, but m.h is including fs.h via sched .h :-/
 *
 * 32位无符号整数，分为3部分：
 * [offset(24-bit)]:[type(7-bit)]:[0(1-bit)] 可以通过SWP_TYPE，SWP_OFFSET 来访问。
 * 当页面在内存中时，也表项最后一位为 1，表示页面在内存中，当页表项不在内存中
 * 时，页表项内容变成 swp_entry_t，指示这个页面的去处。
 */
typedef struct {
	unsigned long val;
} swp_entry_t;

struct shmem_inode_info {
	spinlock_t	lock;
	swp_entry_t	i_direct[SHMEM_NR_DIRECT]; /* for the first blocks */
	swp_entry_t   **i_indirect; /* doubly indirect blocks */
	unsigned long	swapped;
	int		locked;     /* into memory */
	struct list_head	list;
};

struct shmem_sb_info {
	unsigned long max_blocks;   /* How many blocks are allowed */
	unsigned long free_blocks;  /* How many are left for allocation */
	unsigned long max_inodes;   /* How many inodes are allowed */
	unsigned long free_inodes;  /* How many are left for allocation */
	spinlock_t    stat_lock;
};

#endif
