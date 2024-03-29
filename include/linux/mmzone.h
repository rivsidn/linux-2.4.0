#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#ifdef __KERNEL__
#ifndef __ASSEMBLY__

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/list.h>

/*
 * Free memory management - zoned buddy allocator.
 */

#define MAX_ORDER 10

typedef struct free_area_struct {
	struct list_head	free_list;
	unsigned int		*map;		//位图
} free_area_t;

struct pglist_data;

/* 管理区 */
typedef struct zone_struct {
	/*
	 * Commonly accessed fields:
	 */
	spinlock_t		lock;
	unsigned long		offset;			//起始页面号在mem_map中下标
	unsigned long		free_pages;		//空闲页面数
	unsigned long		inactive_clean_pages;
	unsigned long		inactive_dirty_pages;
	unsigned long		pages_min, pages_low, pages_high;

	/*
	 * free areas of different sizes
	 */
	struct list_head	inactive_clean_list;
	free_area_t		free_area[MAX_ORDER];	//空闲页面队列

	/*
	 * rarely used fields:
	 */
	char			*name;
	unsigned long		size;
	/*
	 * Discontig memory support fields.
	 */
	struct pglist_data	*zone_pgdat;
	unsigned long		zone_start_paddr;
	unsigned long		zone_start_mapnr;
	struct page		*zone_mem_map;
} zone_t;

/* 管理区类型 */
#define ZONE_DMA		0
#define ZONE_NORMAL		1
#define ZONE_HIGHMEM		2
#define MAX_NR_ZONES		3

/*
 * One allocation request operates on a zonelist. A zonelist
 * is a list of zones, the first one is the 'goal' of the
 * allocation, the other zones are fallback zones, in decreasing
 * priority.
 *
 * Right now a zonelist takes up less than a cacheline. We never
 * modify it apart from boot-up, and only a few indices are used,
 * so despite the zonelist table being relatively big, the cache
 * footprint of this construct is very small.
 */
typedef struct zonelist_struct {
	zone_t * zones [MAX_NR_ZONES+1];	//NULL delimited
	int gfp_mask;
} zonelist_t;

#define NR_GFPINDEX		0x100

struct bootmem_data;
/*
 * NUMA
 * 代表着存储节点的结构体，每个存储节点至少有两个管理区
 */
typedef struct pglist_data {
	zone_t node_zones[MAX_NR_ZONES];	//管理区，最多三个
	zonelist_t node_zonelists[NR_GFPINDEX];	//规定不同的分配策略
	struct page *node_mem_map;		//节点的page{}结构数组
	unsigned long *valid_addr_bitmap;	//位图
	struct bootmem_data *bdata;
	unsigned long node_start_paddr;		//起始物理地址
	unsigned long node_start_mapnr;		//起始页面号
	unsigned long node_size;		//node 内页面数
	int node_id;				//节点id
	struct pglist_data *node_next;		//存储节点的单链表
} pg_data_t;

extern int numnodes;
extern pg_data_t *pgdat_list;

#define memclass(pgzone, tzone)	(((pgzone)->zone_pgdat == (tzone)->zone_pgdat) \
			&& (((pgzone) - (pgzone)->zone_pgdat->node_zones) <= \
			((tzone) - (pgzone)->zone_pgdat->node_zones)))

/*
 * The following two are not meant for general usage. They are here as
 * prototypes for the discontig memory code.
 */
struct page;
extern void show_free_areas_core(pg_data_t *pgdat);
extern void free_area_init_core(int nid, pg_data_t *pgdat, struct page **gmap,
  unsigned long *zones_size, unsigned long paddr, unsigned long *zholes_size,
  struct page *pmap);

extern pg_data_t contig_page_data;

#ifndef CONFIG_DISCONTIGMEM

#define NODE_DATA(nid)		(&contig_page_data)
#define NODE_MEM_MAP(nid)	mem_map

#else /* !CONFIG_DISCONTIGMEM */

#include <asm/mmzone.h>

#endif /* !CONFIG_DISCONTIGMEM */

#define MAP_ALIGN(x)	((((x) % sizeof(mem_map_t)) == 0) ? (x) : ((x) + \
		sizeof(mem_map_t) - ((x) % sizeof(mem_map_t))))

#endif /* !__ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* _LINUX_MMZONE_H */
