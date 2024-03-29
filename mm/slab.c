/*
 * linux/mm/slab.c
 * Written by Mark Hemment, 1996/97.
 * (markhe@nextd.demon.co.uk)
 *
 * kmem_cache_destroy() + some cleanup - 1999 Andrea Arcangeli
 *
 * Major cleanup, different bufctl logic, per-cpu arrays
 *	(c) 2000 Manfred Spraul
 *
 * An implementation of the Slab Allocator as described in outline in;
 *	UNIX Internals: The New Frontiers by Uresh Vahalia
 *	Pub: Prentice Hall	ISBN 0-13-101908-2
 * or with a little more detail in;
 *	The Slab Allocator: An Object-Caching Kernel Memory Allocator
 *	Jeff Bonwick (Sun Microsystems).
 *	Presented at: USENIX Summer 1994 Technical Conference
 *
 *
 * The memory is organized in caches, one cache for each object type.
 * (e.g. inode_cache, dentry_cache, buffer_head, vm_area_struct)
 * Each cache consists out of many slabs (they are small (usually one
 * page long) and always contiguous), and each slab contains multiple
 * initialized objects.
 *
 * Each cache can only support one memory type (GFP_DMA, GFP_HIGHMEM,
 * normal). If you need a special memory type, then must create a new
 * cache for that memory type.
 *
 * In order to reduce fragmentation, the slabs are sorted in 3 groups:
 *   full slabs with 0 free objects
 *   partial slabs
 *   empty slabs with no allocated objects
 *
 * If partial slabs exist, then new allocations come from these slabs,
 * otherwise from empty slabs or new slabs are allocated.
 *
 * kmem_cache_destroy() CAN CRASH if you try to allocate from the cache
 * during kmem_cache_destroy(). The caller must prevent concurrent allocs.
 *
 * On SMP systems, each cache has a short per-cpu head array, most allocs
 * and frees go into that array, and if that array overflows, then 1/2
 * of the entries in the array are given back into the global cache.
 * This reduces the number of spinlock operations.
 *
 * The c_cpuarray may not be read with enabled local interrupts.
 *
 * SMP synchronization:
 *  constructors and destructors are called without any locking.
 *  Several members in kmem_cache_t and slab_t never change, they
 *	are accessed without any locking.
 *  The per-cpu arrays are never accessed from the wrong cpu, no locking.
 *  The non-constant members are protected with a per-cache irq spinlock.
 *
 * Further notes from the original documentation:
 *
 * 11 April '97.  Started multi-threading - markhe
 *	The global cache-chain is protected by the semaphore 'cache_chain_sem'.
 *	The sem is only needed when accessing/extending the cache-chain, which
 *	can never happen inside an interrupt (kmem_cache_create(),
 *	kmem_cache_shrink() and kmem_cache_reap()).
 *
 *	To prevent kmem_cache_shrink() trying to shrink a 'growing' cache (which
 *	maybe be sleeping and therefore not holding the semaphore/lock), the
 *	growing field is used.  This also prevents reaping from a cache.
 *
 *	At present, each engine can be growing a cache.  This should be blocked.
 *
 */

#include	<linux/config.h>
#include	<linux/slab.h>
#include	<linux/interrupt.h>
#include	<linux/init.h>
#include	<asm/uaccess.h>

/*
 * DEBUG	- 1 for kmem_cache_create() to honour; SLAB_DEBUG_INITIAL,
 *		  SLAB_RED_ZONE & SLAB_POISON.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * STATS	- 1 to collect stats for /proc/slabinfo.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * FORCED_DEBUG	- 1 enables SLAB_RED_ZONE and SLAB_POISON (if possible)
 */

#define	DEBUG		0
#define	STATS		0
#define	FORCED_DEBUG	0

/*
 * Parameters for kmem_cache_reap
 */
#define REAP_SCANLEN	10
#define REAP_PERFECT	10

/* Shouldn't this be in a header file somewhere? */
#define	BYTES_PER_WORD		sizeof(void *)

/* Legal flag mask for kmem_cache_create(). */
#if DEBUG
# define CREATE_MASK	(SLAB_DEBUG_INITIAL | SLAB_RED_ZONE | \
			 SLAB_POISON | SLAB_HWCACHE_ALIGN | \
			 SLAB_NO_REAP | SLAB_CACHE_DMA)
#else
# define CREATE_MASK	(SLAB_HWCACHE_ALIGN | SLAB_NO_REAP | SLAB_CACHE_DMA)
#endif

/*
 * kmem_bufctl_t:
 *
 * Bufctl's are used for linking objs within a slab
 * linked offsets.
 *
 * This implementaion relies on "struct page" for locating the cache &
 * slab an object belongs to.
 * This allows the bufctl structure to be small (one int), but limits
 * the number of objects a slab (not a cache) can contain when off-slab
 * bufctls are used. The limit is the size of the largest general cache
 * that does not use off-slab slabs.
 * For 32bit archs with 4 kB pages, is this 56.
 * This is not serious, as it is only for large objects, when it is unwise
 * to have too many per slab.
 * Note: This limit can be raised by introducing a general cache whose size
 * is less than 512 (PAGE_SIZE<<3), but greater than 256.
 */

#define BUFCTL_END 0xffffFFFF
#define	SLAB_LIMIT 0xffffFFFE
typedef unsigned int kmem_bufctl_t;

/* Max number of objs-per-slab for caches which use off-slab slabs.
 * Needed to avoid a possible looping condition in kmem_cache_grow().
 */
static unsigned long offslab_limit;

/*
 * slab_t
 *
 * Manages the objs in a slab. Placed either at the beginning of mem allocated
 * for a slab, or allocated from an general cache.
 * Slabs are chained into one ordered list: fully used, partial, then fully
 * free slabs.
 * Slabs 串联在固定顺序的链表中: 全部用完了，用了部分，全部空闲。
 */
typedef struct slab_s {
	struct list_head	list;
	unsigned long		colouroff;
	void			*s_mem;		/* including colour offset */
	unsigned int		inuse;		/* num of objs active in slab */
	kmem_bufctl_t		free;
} slab_t;

#define slab_bufctl(slabp) \
	((kmem_bufctl_t *)(((slab_t*)slabp)+1))

/*
 * cpucache_t
 *
 * Per cpu structures
 * The limit is stored in the per-cpu structure to reduce the data cache
 * footprint.
 */
/*
 * SMP 情况下，使能cpucache。
 * 执行的时候会在 kmem_cache_s{} 中的cpudata 后申请limit 个指针。
 * 释放的obj 会首先放入到cpudata 的指针中；申请obj 的时候也会首先在该结构体
 * 中查询。
 */
typedef struct cpucache_s {
	unsigned int avail;
	unsigned int limit;
} cpucache_t;

/* cpucache_t{} 结构体后的指针数组 */
#define cc_entry(cpucache) \
	((void **)(((cpucache_t*)cpucache)+1))
/* kmem_cache_t{} 中的cpudata 数组，每个CPU 占一个元素 */
#define cc_data(cachep) \
	((cachep)->cpudata[smp_processor_id()])
/*
 * kmem_cache_t
 *
 * manages a cache.
 */

#define CACHE_NAMELEN	20	/* max name length for a slab cache */

struct kmem_cache_s {
/* 1) each alloc & free */
	/* full, partial first, then free */
	struct list_head	slabs;		/* 所有slabs 的链表 */
	struct list_head	*firstnotfull;	/* 链表头指针，指向地一个不为空的slab */
	unsigned int		objsize;	/* obj 的大小 */
	unsigned int	 	flags;		/* constant flags */
	unsigned int		num;		/* # of objs per slab */
	spinlock_t		spinlock;
#ifdef CONFIG_SMP
	unsigned int		batchcount;
#endif

/* 2) slab additions /removals */
	/* order of pgs per slab (2^n) */
	unsigned int		gfporder;

	/* force GFP flags, e.g. GFP_DMA */
	unsigned int		gfpflags;

	size_t			colour;		/* cache colouring range */
	unsigned int		colour_off;	/* colour offset */
	unsigned int		colour_next;	/* cache colouring */
	kmem_cache_t		*slabp_cache;	/* slab 与obj 分开管理时，slab 也是kmem_cache_t{} */
	unsigned int		growing;	/* 标识位，不会收割设置了该标识位的kmem_cache_t{} */
	unsigned int		dflags;		/* dynamic flags */

	/* constructor func */
	void (*ctor)(void *, kmem_cache_t *, unsigned long);

	/* de-constructor func */
	void (*dtor)(void *, kmem_cache_t *, unsigned long);

	unsigned long		failures;

/* 3) cache creation/removal */
	char			name[CACHE_NAMELEN];	/* 名称 */
	struct list_head	next;			/* 所有kmem_cache_t串到一个链表中 */
#ifdef CONFIG_SMP
/* 4) per-cpu data */
	cpucache_t		*cpudata[NR_CPUS];
#endif
#if STATS
	unsigned long		num_active;
	unsigned long		num_allocations;
	unsigned long		high_mark;
	unsigned long		grown;
	unsigned long		reaped;
	unsigned long 		errors;
#ifdef CONFIG_SMP
	atomic_t		allochit;
	atomic_t		allocmiss;
	atomic_t		freehit;
	atomic_t		freemiss;
#endif
#endif
};

/* internal c_flags */
#define	CFLGS_OFF_SLAB	0x010000UL	/* slab management in own cache */
#define	CFLGS_OPTIMIZE	0x020000UL	/* optimized slab lookup */

/* c_dflags (dynamic flags). Need to hold the spinlock to access this member */
#define	DFLGS_GROWN	0x000001UL	/* don't reap a recently grown */
					/* 不要回收最近增长的缓存 */

//slab跟内存不在一起管理
#define	OFF_SLAB(x)	((x)->flags & CFLGS_OFF_SLAB)
#define	OPTIMIZE(x)	((x)->flags & CFLGS_OPTIMIZE)
#define	GROWN(x)	((x)->dlags & DFLGS_GROWN)

#if STATS
#define	STATS_INC_ACTIVE(x)	((x)->num_active++)
#define	STATS_DEC_ACTIVE(x)	((x)->num_active--)
#define	STATS_INC_ALLOCED(x)	((x)->num_allocations++)
#define	STATS_INC_GROWN(x)	((x)->grown++)
#define	STATS_INC_REAPED(x)	((x)->reaped++)
#define	STATS_SET_HIGH(x)	do { if ((x)->num_active > (x)->high_mark) \
					(x)->high_mark = (x)->num_active; \
				} while (0)
#define	STATS_INC_ERR(x)	((x)->errors++)
#else
#define	STATS_INC_ACTIVE(x)	do { } while (0)
#define	STATS_DEC_ACTIVE(x)	do { } while (0)
#define	STATS_INC_ALLOCED(x)	do { } while (0)
#define	STATS_INC_GROWN(x)	do { } while (0)
#define	STATS_INC_REAPED(x)	do { } while (0)
#define	STATS_SET_HIGH(x)	do { } while (0)
#define	STATS_INC_ERR(x)	do { } while (0)
#endif

#if STATS && defined(CONFIG_SMP)
#define STATS_INC_ALLOCHIT(x)	atomic_inc(&(x)->allochit)
#define STATS_INC_ALLOCMISS(x)	atomic_inc(&(x)->allocmiss)
#define STATS_INC_FREEHIT(x)	atomic_inc(&(x)->freehit)
#define STATS_INC_FREEMISS(x)	atomic_inc(&(x)->freemiss)
#else
#define STATS_INC_ALLOCHIT(x)	do { } while (0)
#define STATS_INC_ALLOCMISS(x)	do { } while (0)
#define STATS_INC_FREEHIT(x)	do { } while (0)
#define STATS_INC_FREEMISS(x)	do { } while (0)
#endif

#if DEBUG
/* Magic nums for obj red zoning.
 * Placed in the first word before and the first word after an obj.
 */
/*
 * 设置在obj 内存的起始位置和结束位置，发现魔数被修改则报异常
 */
#define	RED_MAGIC1	0x5A2CF071UL	/* when obj is active */
#define	RED_MAGIC2	0x170FC2A5UL	/* when obj is inactive */

/* ...and for poisoning */
#define	POISON_BYTE	0x5a		/* byte value for poisoning */
#define	POISON_END	0xa5		/* end-byte of poisoning */

#endif

/* maximum size of an obj (in 2^order pages) */
#define	MAX_OBJ_ORDER	5	/* 32 pages */

/*
 * Do not go above this order unless 0 objects fit into the slab.
 */
#define	BREAK_GFP_ORDER_HI	2
#define	BREAK_GFP_ORDER_LO	1
static int slab_break_gfp_order = BREAK_GFP_ORDER_LO;

/*
 * Absolute limit for the gfp order
 */
#define	MAX_GFP_ORDER	5	/* 32 pages */


/* Macros for storing/retrieving the cachep and or slab from the
 * global 'mem_map'. These are used to find the slab an obj belongs to.
 * With kfree(), these are used to find the cache which an obj belongs to.
 */
#define	SET_PAGE_CACHE(pg,x)  ((pg)->list.next = (struct list_head *)(x))
#define	GET_PAGE_CACHE(pg)    ((kmem_cache_t *)(pg)->list.next)
#define	SET_PAGE_SLAB(pg,x)   ((pg)->list.prev = (struct list_head *)(x))
#define	GET_PAGE_SLAB(pg)     ((slab_t *)(pg)->list.prev)

/* Size description struct for general caches. */
typedef struct cache_sizes {
	size_t		 cs_size;
	kmem_cache_t	*cs_cachep;
	kmem_cache_t	*cs_dmacachep;
} cache_sizes_t;

//仅仅初始化了大小，没有初始化缓存指针
static cache_sizes_t cache_sizes[] = {
#if PAGE_SIZE == 4096
	{    32,	NULL, NULL},
#endif
	{    64,	NULL, NULL},
	{   128,	NULL, NULL},
	{   256,	NULL, NULL},
	{   512,	NULL, NULL},
	{  1024,	NULL, NULL},
	{  2048,	NULL, NULL},
	{  4096,	NULL, NULL},
	{  8192,	NULL, NULL},
	{ 16384,	NULL, NULL},
	{ 32768,	NULL, NULL},
	{ 65536,	NULL, NULL},
	{131072,	NULL, NULL},
	{     0,	NULL, NULL}
};

/* internal cache of cache description objs */
/* 缓存描述符结构体的内部缓存 */
static kmem_cache_t cache_cache = {
	slabs:		LIST_HEAD_INIT(cache_cache.slabs),
	firstnotfull:	&cache_cache.slabs,
	objsize:	sizeof(kmem_cache_t),
	flags:		SLAB_NO_REAP,
	spinlock:	SPIN_LOCK_UNLOCKED,
	colour_off:	L1_CACHE_BYTES,
	name:		"kmem_cache",
};

/* Guard access to the cache-chain. */
static struct semaphore	cache_chain_sem;

/* Place maintainer for reaping. */
static kmem_cache_t *clock_searchp = &cache_cache;

#define cache_chain (cache_cache.next)

#ifdef CONFIG_SMP
/*
 * chicken and egg problem: delay the per-cpu array allocation
 * until the general caches are up.
 */
static int g_cpucache_up;

static void enable_cpucache (kmem_cache_t *cachep);
static void enable_all_cpucaches (void);
#endif

/* 计算一个slab_t{} 中可以创建obj 的个数，创建之后剩余的内存大小 */
/* Cal the num objs, wastage, and bytes left over for a given slab size. */
static void kmem_cache_estimate (unsigned long gfporder, size_t size,
		 int flags, size_t *left_over, unsigned int *num)
{
	int i;
	//cache中的slab占用多少个页面
	size_t wastage = PAGE_SIZE<<gfporder;
	size_t extra = 0;
	size_t base = 0;

	if (!(flags & CFLGS_OFF_SLAB)) {
		base = sizeof(slab_t);
		extra = sizeof(kmem_bufctl_t);
	}
	i = 0;

	/*
	 * base 为 slab_t 结构体大小; extra 为 kmem_bufctl_t 大小;
	 * size 为 obj 大小; wastage 为 slab 占用内存大小.
	 *
	 * 这里的意思是计算该 slab 中能容纳多少个 obj.
	 */
	while (i*size + L1_CACHE_ALIGN(base+i*extra) <= wastage)
		i++;
	if (i > 0)
		i--;

	//不能超过最大限度，在数组链表中标识结束
	if (i > SLAB_LIMIT)
		i = SLAB_LIMIT;

	//返回值，能创建多少个obj，会剩余多少内存
	*num = i;
	wastage -= i*size;
	wastage -= L1_CACHE_ALIGN(base+i*extra);
	*left_over = wastage;
}

/* Initialisation - setup the `cache' cache. */
void __init kmem_cache_init(void)
{
	size_t left_over;

	/* 初始化锁和链表 */
	init_MUTEX(&cache_chain_sem);
	INIT_LIST_HEAD(&cache_chain);

	kmem_cache_estimate(0, cache_cache.objsize, 0, &left_over, &cache_cache.num);
	if (!cache_cache.num)
		BUG();

	cache_cache.colour = left_over/cache_cache.colour_off;
	cache_cache.colour_next = 0;
}


/* Initialisation - setup remaining internal and general caches.
 * Called after the gfp() functions have been enabled, and before smp_init().
 */
void __init kmem_cache_sizes_init(void)
{
	cache_sizes_t *sizes = cache_sizes;
	char name[20];
	/*
	 * Fragmentation resistance on low memory - only use bigger
	 * page orders on machines with more than 32MB of memory.
	 */
	if (num_physpages > (32 << 20) >> PAGE_SHIFT)
		slab_break_gfp_order = BREAK_GFP_ORDER_HI;
	do {
		/*
		 * For performance, all the general caches are L1 aligned.
		 * This should be particularly beneficial on SMP boxes, as it
		 * eliminates "false sharing".
		 * Note for systems short on memory removing the alignment will
		 * allow tighter packing of the smaller caches.
		 *
		 * 为了优化性能，在所有的缓存中都开启了L1对齐，但是在内存量比较少
		 * 的系统上，应该将该对其去掉，可以使内存更紧凑。
		 */
		sprintf(name,"size-%Zd",sizes->cs_size);
		if (!(sizes->cs_cachep =
			kmem_cache_create(name, sizes->cs_size,
					0, SLAB_HWCACHE_ALIGN, NULL, NULL))) {
			BUG();
		}

		/* Inc off-slab bufctl limit until the ceiling is hit. */
		if (!(OFF_SLAB(sizes->cs_cachep))) {
			offslab_limit = sizes->cs_size-sizeof(slab_t);
			offslab_limit /= 2;
		}
		sprintf(name, "size-%Zd(DMA)",sizes->cs_size);
		sizes->cs_dmacachep = kmem_cache_create(name, sizes->cs_size, 0,
			      SLAB_CACHE_DMA|SLAB_HWCACHE_ALIGN, NULL, NULL);
		if (!sizes->cs_dmacachep)
			BUG();
		sizes++;
	} while (sizes->cs_size);
}

int __init kmem_cpucache_init(void)
{
#ifdef CONFIG_SMP
	g_cpucache_up = 1;
	enable_all_cpucaches();
#endif
	return 0;
}

__initcall(kmem_cpucache_init);

/* Interface to system's page allocator. No need to hold the cache-lock. */
static inline void * kmem_getpages (kmem_cache_t *cachep, unsigned long flags)
{
	void	*addr;

	/*
	 * If we requested dmaable memory, we will get it. Even if we
	 * did not request dmaable memory, we might get it, but that
	 * would be relatively rare and ignorable.
	 */
	flags |= cachep->gfpflags;
	addr = (void*) __get_free_pages(flags, cachep->gfporder);
	/* Assume that now we have the pages no one else can legally
	 * messes with the 'struct page's.
	 * However vm_scan() might try to test the structure to see if
	 * it is a named-page or buffer-page.  The members it tests are
	 * of no interest here.....
	 */
	return addr;
}

/* Interface to system's page release. */
static inline void kmem_freepages (kmem_cache_t *cachep, void *addr)
{
	unsigned long i = (1<<cachep->gfporder);
	struct page *page = virt_to_page(addr);

	/* free_pages() does not clear the type bit - we do that.
	 * The pages have been unlinked from their cache-slab,
	 * but their 'struct page's might be accessed in
	 * vm_scan(). Shouldn't be a worry.
	 */
	while (i--) {
		PageClearSlab(page);	//清空标识位
		page++;
	}
	//释放页面
	free_pages((unsigned long)addr, cachep->gfporder);
}

#if DEBUG
/* 设置cachep 中该obj 的所有字节为 POISON_BYTE，最后一字节为 POISON_END */
static inline void kmem_poison_obj (kmem_cache_t *cachep, void *addr)
{
	int size = cachep->objsize;
	if (cachep->flags & SLAB_RED_ZONE) {
		addr += BYTES_PER_WORD;
		size -= 2*BYTES_PER_WORD;
	}
	memset(addr, POISON_BYTE, size);
	*(unsigned char *)(addr+size-1) = POISON_END;
}

/* 检查cachep 中该obj 的所有字节是否为 POISON_BYTE */
static inline int kmem_check_poison_obj (kmem_cache_t *cachep, void *addr)
{
	int size = cachep->objsize;
	void *end;
	if (cachep->flags & SLAB_RED_ZONE) {
		addr += BYTES_PER_WORD;
		size -= 2*BYTES_PER_WORD;
	}
	end = memchr(addr, POISON_END, size);
	if (end != (addr+size-1))
		return 1;
	return 0;
}
#endif

/* Destroy all the objs in a slab, and release the mem back to the system.
 * Before calling the slab must have been unlinked from the cache.
 * The cache-lock is not held/needed.
 */
static void kmem_slab_destroy (kmem_cache_t *cachep, slab_t *slabp)
{
	if (cachep->dtor
#if DEBUG
		|| cachep->flags & (SLAB_POISON | SLAB_RED_ZONE)
#endif
	) {
		int i;
		for (i = 0; i < cachep->num; i++) {
			void* objp = slabp->s_mem+cachep->objsize*i;
#if DEBUG
			if (cachep->flags & SLAB_RED_ZONE) {
				if (*((unsigned long*)(objp)) != RED_MAGIC1)
					BUG();
				if (*((unsigned long*)(objp + cachep->objsize
						- BYTES_PER_WORD)) != RED_MAGIC1)
					BUG();
				objp += BYTES_PER_WORD;
			}
#endif
			//调用析构函数
			if (cachep->dtor)
				(cachep->dtor)(objp, cachep, 0);
#if DEBUG
			if (cachep->flags & SLAB_RED_ZONE) {
				objp -= BYTES_PER_WORD;
			}
			if ((cachep->flags & SLAB_POISON)  &&
				kmem_check_poison_obj(cachep, objp))
				BUG();
#endif
		}
	}

	//释放管理的内存
	kmem_freepages(cachep, slabp->s_mem-slabp->colouroff);
	if (OFF_SLAB(cachep))
		//释放对应的slab内存，此时slabp 也是一个kmem_cache
		kmem_cache_free(cachep->slabp_cache, slabp);
}

/**
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @offset: The offset to use within the page.
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 * @dtor: A destructor for the objects.
 *
 * Returns a ptr to the cache on success, NULL on failure.
 * Cannot be called within a int, but can be interrupted.
 * The @ctor is run when new pages are allocated by the cache
 * and the @dtor is run before the pages are handed back.
 * The flags are
 *
 * %SLAB_POISON - Poison the slab with a known test pattern (a5a5a5a5)
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check
 * for buffer overruns.
 *
 * %SLAB_NO_REAP - Don't automatically reap this cache when we're under
 * memory pressure.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
kmem_cache_t *
kmem_cache_create (const char *name, size_t size, size_t offset,
	unsigned long flags, void (*ctor)(void*, kmem_cache_t *, unsigned long),
	void (*dtor)(void*, kmem_cache_t *, unsigned long))
{
	const char *func_nm = KERN_ERR "kmem_create: ";
	size_t left_over, align, slab_size;
	kmem_cache_t *cachep = NULL;

	/*
	 * Sanity checks... these are all serious usage bugs.
	 */
	if ((!name) ||
		((strlen(name) >= CACHE_NAMELEN - 1)) ||
		in_interrupt() ||
		(size < BYTES_PER_WORD) ||
		(size > (1<<MAX_OBJ_ORDER)*PAGE_SIZE) ||
		(dtor && !ctor) ||
		(offset < 0 || offset > size))
			BUG();

#if DEBUG
	if ((flags & SLAB_DEBUG_INITIAL) && !ctor) {
		/* No constructor, but inital state check requested */
		printk("%sNo con, but init state check requested - %s\n", func_nm, name);
		flags &= ~SLAB_DEBUG_INITIAL;
	}

	if ((flags & SLAB_POISON) && ctor) {
		/* request for poisoning, but we can't do that with a constructor */
		printk("%sPoisoning requested, but con given - %s\n", func_nm, name);
		flags &= ~SLAB_POISON;
	}
#if FORCED_DEBUG
	if (size < (PAGE_SIZE>>3))
		/*
		 * do not red zone large object, causes severe
		 * fragmentation.
		 */
		flags |= SLAB_RED_ZONE;
	if (!ctor)
		flags |= SLAB_POISON;
#endif
#endif

	/*
	 * Always checks flags, a caller might be expecting debug
	 * support which isn't available.
	 */
	if (flags & ~CREATE_MASK)
		BUG();

	/* Get cache's description obj. */
	cachep = (kmem_cache_t *) kmem_cache_alloc(&cache_cache, SLAB_KERNEL);
	if (!cachep)
		goto opps;
	memset(cachep, 0, sizeof(kmem_cache_t));

	/* Check that size is in terms of words.  This is needed to avoid
	 * unaligned accesses for some archs when redzoning is used, and makes
	 * sure any on-slab bufctl's are also correctly aligned.
	 */
	if (size & (BYTES_PER_WORD-1)) {
		size += (BYTES_PER_WORD-1);
		size &= ~(BYTES_PER_WORD-1);
		printk("%sForcing size word alignment - %s\n", func_nm, name);
	}

#if DEBUG
	if (flags & SLAB_RED_ZONE) {
		/*
		 * There is no point trying to honour cache alignment
		 * when redzoning.
		 */
		flags &= ~SLAB_HWCACHE_ALIGN;
		size += 2*BYTES_PER_WORD;	/* words for redzone */
	}
#endif
	align = BYTES_PER_WORD;
	if (flags & SLAB_HWCACHE_ALIGN)
		align = L1_CACHE_BYTES;

	/* Determine if the slab management is 'on' or 'off' slab. */
	if (size >= (PAGE_SIZE>>3))
		/*
		 * Size is large, assume best to place the slab management obj
		 * off-slab (should allow better packing of objs).
		 */
		flags |= CFLGS_OFF_SLAB;

	if (flags & SLAB_HWCACHE_ALIGN) {
		/* Need to adjust size so that objs are cache aligned. */
		/* Small obj size, can get at least two per cache line. */
		/* FIXME: only power of 2 supported, was better */
		while (size < align/2)
			align /= 2;
		size = (size+align-1)&(~(align-1));
	}

	/* Cal size (in pages) of slabs, and the num of objs per slab.
	 * This could be made much more intelligent.  For now, try to avoid
	 * using high page-orders for slabs.  When the gfp() funcs are more
	 * friendly towards high-order requests, this should be changed.
	 */
	/*
	 * 计算slabs 占用的页面数和每个slab 中的obj 个数.
	 * 现在应该尽量避免给slab 分配大的页面，当gfp() 函数对大的页面请求更
	 * 友善的时候，这部分可以修改.
	 */
	do {
		unsigned int break_flag = 0;
cal_wastage:
		kmem_cache_estimate(cachep->gfporder, size, flags,
						&left_over, &cachep->num);
		if (break_flag)
			break;
		if (cachep->gfporder >= MAX_GFP_ORDER)
			break;
		if (!cachep->num)
			goto next;
		if (flags & CFLGS_OFF_SLAB && cachep->num > offslab_limit) {
			/* Oops, this num of objs will cause problems. */
			cachep->gfporder--;
			break_flag++;
			goto cal_wastage;
		}

		/*
		 * Large num of objs is good, but v. large slabs are currently
		 * bad for the gfp()s.
		 */
		if (cachep->gfporder >= slab_break_gfp_order)
			break;

		if ((left_over*8) <= (PAGE_SIZE<<cachep->gfporder))
			break;	/* Acceptable internal fragmentation. */
next:
		cachep->gfporder++;
	} while (1);

	if (!cachep->num) {
		printk("kmem_cache_create: couldn't create cache %s.\n", name);
		kmem_cache_free(&cache_cache, cachep);
		cachep = NULL;
		goto opps;
	}
	slab_size = L1_CACHE_ALIGN(cachep->num*sizeof(kmem_bufctl_t)+sizeof(slab_t));

	/*
	 * If the slab has been placed off-slab, and we have enough space then
	 * move it on-slab. This is at the expense of any extra colouring.
	 */
	/*
	 * 设置了off-slab时，此时如果有足够空间则使用on-slab。
	 */
	if (flags & CFLGS_OFF_SLAB && left_over >= slab_size) {
		flags &= ~CFLGS_OFF_SLAB;
		left_over -= slab_size;
	}

	/* Offset must be a multiple of the alignment. */
	offset += (align-1);
	offset &= ~(align-1);
	if (!offset)
		offset = L1_CACHE_BYTES;
	cachep->colour_off = offset;
	cachep->colour = left_over/offset;

	/* init remaining fields */
	if (!cachep->gfporder && !(flags & CFLGS_OFF_SLAB))
		flags |= CFLGS_OPTIMIZE;

	cachep->flags = flags;
	cachep->gfpflags = 0;
	if (flags & SLAB_CACHE_DMA)
		cachep->gfpflags |= GFP_DMA;
	spin_lock_init(&cachep->spinlock);
	cachep->objsize = size;
	INIT_LIST_HEAD(&cachep->slabs);
	cachep->firstnotfull = &cachep->slabs;

	if (flags & CFLGS_OFF_SLAB)
		cachep->slabp_cache = kmem_find_general_cachep(slab_size,0);
	cachep->ctor = ctor;
	cachep->dtor = dtor;
	/* Copy name over so we don't have problems with unloaded modules */
	strcpy(cachep->name, name);

#ifdef CONFIG_SMP
	if (g_cpucache_up)
		enable_cpucache(cachep);
#endif
	/* 将新创建的mem cache 添加到链表中，需要信号量保护 */
	/* Need the semaphore to access the chain. */
	down(&cache_chain_sem);
	{
		struct list_head *p;

		list_for_each(p, &cache_chain) {
			kmem_cache_t *pc = list_entry(p, kmem_cache_t, next);

			/* The name field is constant - no lock needed. */
			if (!strcmp(pc->name, name))
				BUG();
		}
	}

	/* There is no reason to lock our new cache before we
	 * link it in - no one knows about it yet...
	 */
	list_add(&cachep->next, &cache_chain);
	up(&cache_chain_sem);
opps:
	return cachep;
}

/*
 * This check if the kmem_cache_t pointer is chained in the cache_cache
 * list. -arca
 */
/*
 * 检查kmem_cache_t 指针是否在cache_cache 链表中.
 */
static int is_chained_kmem_cache(kmem_cache_t * cachep)
{
	struct list_head *p;
	int ret = 0;

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	list_for_each(p, &cache_chain) {
		if (p == &cachep->next) {
			ret = 1;
			break;
		}
	}
	up(&cache_chain_sem);

	return ret;
}

#ifdef CONFIG_SMP
/*
 * Waits for all CPUs to execute func().
 */
static void smp_call_function_all_cpus(void (*func) (void *arg), void *arg)
{
	local_irq_disable();
	func(arg);
	local_irq_enable();

	if (smp_call_function(func, arg, 1, 1))
		BUG();
}
typedef struct ccupdate_struct_s
{
	kmem_cache_t *cachep;
	cpucache_t *new[NR_CPUS];
} ccupdate_struct_t;

static void do_ccupdate_local(void *info)
{
	//交换ccupdate_struct_t{}、kmem_cache_t{} 中的cpucache_t{}
	ccupdate_struct_t *new = (ccupdate_struct_t *)info;
	cpucache_t *old = cc_data(new->cachep);

	cc_data(new->cachep) = new->new[smp_processor_id()];
	new->new[smp_processor_id()] = old;
}

static void free_block (kmem_cache_t* cachep, void** objpp, int len);

/*
 * 释放所有的CPU 缓存
 */
static void drain_cpu_caches(kmem_cache_t *cachep)
{
	ccupdate_struct_t new;
	int i;

	memset(&new.new,0,sizeof(new.new));

	new.cachep = cachep;

	down(&cache_chain_sem);	//获取信号量
	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);

	//替换之后清空
	for (i = 0; i < smp_num_cpus; i++) {
		cpucache_t* ccold = new.new[cpu_logical_map(i)];
		if (!ccold || (ccold->avail == 0))
			continue;
		local_irq_disable();
		//释放
		free_block(cachep, cc_entry(ccold), ccold->avail);
		local_irq_enable();
		ccold->avail = 0;
	}
	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);
	up(&cache_chain_sem);	//释放信号量
}

#else
#define drain_cpu_caches(cachep)	do { } while (0)
#endif

static int __kmem_cache_shrink(kmem_cache_t *cachep)
{
	slab_t *slabp;
	int ret;

	drain_cpu_caches(cachep);

	spin_lock_irq(&cachep->spinlock);

	/* If the cache is growing, stop shrinking. */
	while (!cachep->growing) {
		struct list_head *p;

		p = cachep->slabs.prev;
		if (p == &cachep->slabs)
			break;

		slabp = list_entry(cachep->slabs.prev, slab_t, list);
		if (slabp->inuse)
			break;

		/*
		 * 因为此时删除的时候是从后往前删，所以如果遇到firstnotfull，
		 * 前边的slab 一定就是被全部占用了。
		 */
		list_del(&slabp->list);
		if (cachep->firstnotfull == &slabp->list)
			cachep->firstnotfull = &cachep->slabs;

		spin_unlock_irq(&cachep->spinlock);
		kmem_slab_destroy(cachep, slabp);
		spin_lock_irq(&cachep->spinlock);
	}
	ret = !list_empty(&cachep->slabs);
	spin_unlock_irq(&cachep->spinlock);
	return ret;
}

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 */
int kmem_cache_shrink(kmem_cache_t *cachep)
{
	if (!cachep || in_interrupt() || !is_chained_kmem_cache(cachep))
		BUG();

	return __kmem_cache_shrink(cachep);
}

/**
 * kmem_cache_destroy - delete a cache
 * @cachep: the cache to destroy
 *
 * Remove a kmem_cache_t object from the slab cache.
 * Returns 0 on success.
 *
 * It is expected this function will be called by a module when it is
 * unloaded.  This will remove the cache completely, and avoid a duplicate
 * cache being allocated each time a module is loaded and unloaded, if the
 * module doesn't have persistent in-kernel storage across loads and unloads.
 *
 * The caller must guarantee that noone will allocate memory from the cache
 * during the kmem_cache_destroy().
 */
int kmem_cache_destroy (kmem_cache_t * cachep)
{
	//安全检查
	if (!cachep || in_interrupt() || cachep->growing)
		BUG();

	/* 从链表中删除该缓存 */
	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	/* the chain is never empty, cache_cache is never destroyed */
	if (clock_searchp == cachep)
		clock_searchp = list_entry(cachep->next.next,
						kmem_cache_t, next);
	list_del(&cachep->next);
	up(&cache_chain_sem);

	if (__kmem_cache_shrink(cachep)) {
		printk(KERN_ERR "kmem_cache_destroy: Can't free all objects %p\n",
		       cachep);
		down(&cache_chain_sem);
		list_add(&cachep->next,&cache_chain);	//没法删除，添加回来
		up(&cache_chain_sem);
		return 1;
	}
#ifdef CONFIG_SMP
	{
		int i;
		for (i = 0; i < NR_CPUS; i++)
			kfree(cachep->cpudata[i]);
	}
#endif
	kmem_cache_free(&cache_cache, cachep);

	return 0;
}

/* Get the memory for a slab management obj. */
static inline slab_t * kmem_cache_slabmgmt (kmem_cache_t *cachep,
			void *objp, int colour_off, int local_flags)
{
	slab_t *slabp;
	
	if (OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		/*
		 * slab与对应的objs是分开管理的，给对应的内存分配一个
		 * 对应的slab 。
		 */
		slabp = kmem_cache_alloc(cachep->slabp_cache, local_flags);
		if (!slabp)
			return NULL;
	} else {
		/* FIXME: change to
			slabp = objp
		 * if you enable OPTIMIZE
		 */
		/*
		 * slab与对应的objs是在一起管理的
		 */
		slabp = objp+colour_off;
		colour_off += L1_CACHE_ALIGN(cachep->num *
				sizeof(kmem_bufctl_t) + sizeof(slab_t));
	}
	slabp->inuse = 0;
	slabp->colouroff = colour_off;		//申请内存起始位置到objects 位置的偏移量
	slabp->s_mem = objp+colour_off;		//实际可用的内存地址，objects起始内存地址

	return slabp;
}

static inline void kmem_cache_init_objs (kmem_cache_t * cachep,
			slab_t * slabp, unsigned long ctor_flags)
{
	int i;

	/* num 是每个slab 中包含的obj 数量 */
	for (i = 0; i < cachep->num; i++) {
		void* objp = slabp->s_mem+cachep->objsize*i;
#if DEBUG
		if (cachep->flags & SLAB_RED_ZONE) {
			*((unsigned long*)(objp)) = RED_MAGIC1;
			*((unsigned long*)(objp + cachep->objsize - BYTES_PER_WORD)) = RED_MAGIC1;
			objp += BYTES_PER_WORD;
		}
#endif

		/*
		 * Constructors are not allowed to allocate memory from
		 * the same cache which they are a constructor for.
		 * Otherwise, deadlock. They must also be threaded.
		 */
		if (cachep->ctor)
			cachep->ctor(objp, cachep, ctor_flags);
#if DEBUG
		if (cachep->flags & SLAB_RED_ZONE)
			objp -= BYTES_PER_WORD;
		if (cachep->flags & SLAB_POISON)
			/* need to poison the objs */
			kmem_poison_obj(cachep, objp);
		if (cachep->flags & SLAB_RED_ZONE) {
			if (*((unsigned long*)(objp)) != RED_MAGIC1)
				BUG();
			if (*((unsigned long*)(objp + cachep->objsize -
					BYTES_PER_WORD)) != RED_MAGIC1)
				BUG();
		}
#endif
		slab_bufctl(slabp)[i] = i+1;
	}
	slab_bufctl(slabp)[i-1] = BUFCTL_END;	//最后一个指向BUFCTL_END
	slabp->free = 0;			//slab 中地一个空先的obj 编号
}

/*
 * Grow (by 1) the number of slabs within a cache.  This is called by
 * kmem_cache_alloc() when there are no active objs left in a cache.
 */
/*
 * 结构体的包含关系为cache 包含slab，slab包含obj.
 * 当通过kmem_cache_alloc() 申请内存的时候，如果此时没有可用的obj，调用
 * 该函数在cache 中增加一个新的slab.
 */
static int kmem_cache_grow (kmem_cache_t * cachep, int flags)
{
	slab_t	*slabp;
	struct page	*page;
	void		*objp;
	size_t		 offset;
	unsigned int	 i, local_flags;
	unsigned long	 ctor_flags;
	unsigned long	 save_flags;

	/* Be lazy and only check for valid flags here,
	 * keeping it out of the critical path in kmem_cache_alloc().
	 */
	if (flags & ~(SLAB_DMA|SLAB_LEVEL_MASK|SLAB_NO_GROW))
		BUG();
	if (flags & SLAB_NO_GROW)
		return 0;

	/*
	 * The test for missing atomic flag is performed here, rather than
	 * the more obvious place, simply to reduce the critical path length
	 * in kmem_cache_alloc(). If a caller is seriously mis-behaving they
	 * will eventually be caught here (where it matters).
	 */
	/* 中断中必须要保持原子操作 */
	if (in_interrupt() && (flags & SLAB_LEVEL_MASK) != SLAB_ATOMIC)
		BUG();

	ctor_flags = SLAB_CTOR_CONSTRUCTOR;
	local_flags = (flags & SLAB_LEVEL_MASK);
	if (local_flags == SLAB_ATOMIC)
		/*
		 * Not allowed to sleep.  Need to tell a constructor about
		 * this - it might need to know...
		 * 需要通知构造器不能够引起休眠.
		 */
		ctor_flags |= SLAB_CTOR_ATOMIC;

	/* About to mess with non-constant members - lock. */
	/* 关闭本地中断，加锁 */
	spin_lock_irqsave(&cachep->spinlock, save_flags);

	/* Get colour for the slab, and cal the next value. */
	offset = cachep->colour_next;
	cachep->colour_next++;
	if (cachep->colour_next >= cachep->colour)
		cachep->colour_next = 0;
	offset *= cachep->colour_off;
	cachep->dflags |= DFLGS_GROWN;

	cachep->growing++;
	/* 解锁，开启本地中断 */
	spin_unlock_irqrestore(&cachep->spinlock, save_flags);

	/* A series of memory allocations for a new slab.
	 * Neither the cache-chain semaphore, or cache-lock, are
	 * held, but the incrementing c_growing prevents this
	 * cache from being reaped or shrunk.
	 * Note: The cache could be selected in for reaping in
	 * kmem_cache_reap(), but when the final test is made the
	 * growing value will be seen.
	 */

	/* Get mem for the objs(获取内存). */
	if (!(objp = kmem_getpages(cachep, flags)))
		goto failed;

	/* Get slab management. */
	if (!(slabp = kmem_cache_slabmgmt(cachep, objp, offset, local_flags)))
		goto opps1;

	/* Nasty!!!!!! I hope this is OK. */
	i = 1 << cachep->gfporder;
	page = virt_to_page(objp);
	do {
		SET_PAGE_CACHE(page, cachep);
		SET_PAGE_SLAB(page, slabp);
		PageSetSlab(page);		//设置标识位
		page++;
	} while (--i);

	kmem_cache_init_objs(cachep, slabp, ctor_flags);

	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;

	/* Make slab active. */
	list_add_tail(&slabp->list, &cachep->slabs);	//新申请的是空闲的，加到末尾
	if (cachep->firstnotfull == &cachep->slabs)
		cachep->firstnotfull = &slabp->list;
	STATS_INC_GROWN(cachep);
	cachep->failures = 0;

	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	return 1;
opps1:
	kmem_freepages(cachep, objp);
failed:
	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;
	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	return 0;
}

/*
 * Perform extra freeing checks:
 * - detect double free
 * - detect bad pointers.
 * Called with the cache-lock held.
 */

#if DEBUG
static int kmem_extra_free_checks (kmem_cache_t * cachep,
			slab_t *slabp, void * objp)
{
	int i;
	unsigned int objnr = (objp-slabp->s_mem)/cachep->objsize;

	/* 是否超过最大个数 */
	if (objnr >= cachep->num)
		BUG();
	/* 地址是否一致，除法会省略余数 */
	if (objp != slabp->s_mem + objnr*cachep->objsize)
		BUG();

	/* 检查当前是否是空闲的(重复释放) */
	/* Check slab's freelist to see if this obj is there. */
	for (i = slabp->free; i != BUFCTL_END; i = slab_bufctl(slabp)[i]) {
		if (i == objnr)
			BUG();
	}
	return 0;
}
#endif

static inline void kmem_cache_alloc_head(kmem_cache_t *cachep, int flags)
{
#if DEBUG
	if (flags & SLAB_DMA) {
		if (!(cachep->gfpflags & GFP_DMA))
			BUG();
	} else {
		if (cachep->gfpflags & GFP_DMA)
			BUG();
	}
#endif
}

static inline void * kmem_cache_alloc_one_tail (kmem_cache_t *cachep,
							 slab_t *slabp)
{
	void *objp;

	/* 更新统计信息 */
	STATS_INC_ALLOCED(cachep);
	STATS_INC_ACTIVE(cachep);
	STATS_SET_HIGH(cachep);

	/* get obj pointer(slab中申请一个obj) */
	/*
	 * 每一个slab_t 结构体后边紧跟着的是一个数组链表，通过
	 * slab_bufctl(slabp)[slabp->free] 可以查找下一个free的下标，
	 * slabp->s_mem + slabp->free*cachep->objsize 为空闲的obj，当
	 * 该slab 已经不存在空闲obj的时候，此时的slabp->free 为 BUFCTL_END.
	 */
	slabp->inuse++;
	objp = slabp->s_mem + slabp->free*cachep->objsize;
	slabp->free = slab_bufctl(slabp)[slabp->free];

	/* 如果该slab 被全部使用了，指向下一个slab */
	if (slabp->free == BUFCTL_END)
		/* slab now full: move to next slab for next alloc */
		cachep->firstnotfull = slabp->list.next;
#if DEBUG
	if (cachep->flags & SLAB_POISON)
		if (kmem_check_poison_obj(cachep, objp))
			BUG();
	if (cachep->flags & SLAB_RED_ZONE) {
		/* Set alloc red-zone, and check old one. */
		if (xchg((unsigned long *)objp, RED_MAGIC2) !=
							 RED_MAGIC1)
			BUG();
		if (xchg((unsigned long *)(objp+cachep->objsize -
			  BYTES_PER_WORD), RED_MAGIC2) != RED_MAGIC1)
			BUG();
		objp += BYTES_PER_WORD;
	}
#endif
	return objp;
}

/*
 * Returns a ptr to an obj in the given cache.
 * caller must guarantee synchronization
 * #define for the goto optimization 8-)
 */
#define kmem_cache_alloc_one(cachep)				\
({								\
	slab_t	*slabp;					\
								\
	/* Get slab alloc is to come from. */			\
	{							\
		struct list_head* p = cachep->firstnotfull;	\
		if (p == &cachep->slabs)			\
			goto alloc_new_slab;			\
		slabp = list_entry(p,slab_t, list);	\
	}							\
	kmem_cache_alloc_one_tail(cachep, slabp);		\
})

#ifdef CONFIG_SMP
/* 一次性申请多个，放到缓存中，只在smp 情况下生效 */
void* kmem_cache_alloc_batch(kmem_cache_t* cachep, int flags)
{
	int batchcount = cachep->batchcount;
	cpucache_t* cc = cc_data(cachep);

	spin_lock(&cachep->spinlock);
	while (batchcount--) {
		/* Get slab alloc is to come from. */
		struct list_head *p = cachep->firstnotfull;
		slab_t *slabp;

		//cache中不存在空闲的slab了
		if (p == &cachep->slabs)
			break;
		slabp = list_entry(p, slab_t, list);
		cc_entry(cc)[cc->avail++] =
				kmem_cache_alloc_one_tail(cachep, slabp);
	}
	spin_unlock(&cachep->spinlock);

	if (cc->avail)
		return cc_entry(cc)[--cc->avail];
	return NULL;
}
#endif

static inline void * __kmem_cache_alloc (kmem_cache_t *cachep, int flags)
{
	unsigned long save_flags;
	void* objp;

	kmem_cache_alloc_head(cachep, flags);
try_again:
	local_irq_save(save_flags);
#ifdef CONFIG_SMP
	{
		cpucache_t *cc = cc_data(cachep);

		if (cc) {
			if (cc->avail) {
				STATS_INC_ALLOCHIT(cachep);	//申请命中
				objp = cc_entry(cc)[--cc->avail];
			} else {
				STATS_INC_ALLOCMISS(cachep);	//申请miss
				objp = kmem_cache_alloc_batch(cachep,flags);
				if (!objp)
					goto alloc_new_slab_nolock;
			}
		} else {
			spin_lock(&cachep->spinlock);
			objp = kmem_cache_alloc_one(cachep);
			spin_unlock(&cachep->spinlock);
		}
	}
#else
	objp = kmem_cache_alloc_one(cachep);
#endif
	local_irq_restore(save_flags);
	return objp;
alloc_new_slab:
#ifdef CONFIG_SMP
	spin_unlock(&cachep->spinlock);
alloc_new_slab_nolock:
#endif
	local_irq_restore(save_flags);
	if (kmem_cache_grow(cachep, flags))
		/* Someone may have stolen our objs.  Doesn't matter, we'll
		 * just come back here again.
		 */
		goto try_again;
	return NULL;
}

/*
 * Release an obj back to its cache. If the obj has a constructed
 * state, it should be in this state _before_ it is released.
 * - caller is responsible for the synchronization
 */

#if DEBUG
# define CHECK_NR(pg)						\
	do {							\
		if (!VALID_PAGE(pg)) {				\
			printk(KERN_ERR "kfree: out of range ptr %lxh.\n", \
				(unsigned long)objp);		\
			BUG();					\
		} \
	} while (0)
# define CHECK_PAGE(page)					\
	do {							\
		CHECK_NR(page);					\
		if (!PageSlab(page)) {				\
			printk(KERN_ERR "kfree: bad ptr %lxh.\n", \
				(unsigned long)objp);		\
			BUG();					\
		}						\
	} while (0)

#else
# define CHECK_PAGE(pg)	do { } while (0)
#endif

static inline void kmem_cache_free_one(kmem_cache_t *cachep, void *objp)
{
	slab_t* slabp;

	CHECK_PAGE(virt_to_page(objp));
	/* reduces memory footprint
	 *
	if (OPTIMIZE(cachep))
		slabp = (void*)((unsigned long)objp&(~(PAGE_SIZE-1)));
	 else
	 */
	slabp = GET_PAGE_SLAB(virt_to_page(objp));

#if DEBUG
	if (cachep->flags & SLAB_DEBUG_INITIAL)
		/* Need to call the slab's constructor so the
		 * caller can perform a verify of its state (debugging).
		 * Called without the cache-lock held.
		 */
		cachep->ctor(objp, cachep, SLAB_CTOR_CONSTRUCTOR|SLAB_CTOR_VERIFY);

	if (cachep->flags & SLAB_RED_ZONE) {
		objp -= BYTES_PER_WORD;
		if (xchg((unsigned long *)objp, RED_MAGIC1) != RED_MAGIC2)
			/* Either write before start, or a double free. */
			BUG();
		if (xchg((unsigned long *)(objp+cachep->objsize -
				BYTES_PER_WORD), RED_MAGIC1) != RED_MAGIC2)
			/* Either write past end, or a double free. */
			BUG();
	}
	if (cachep->flags & SLAB_POISON)
		kmem_poison_obj(cachep, objp);
	if (kmem_extra_free_checks(cachep, slabp, objp))
		return;
#endif
	{
		unsigned int objnr = (objp-slabp->s_mem)/cachep->objsize;
		//释放之后添加到空闲链表中
		slab_bufctl(slabp)[objnr] = slabp->free;
		slabp->free = objnr;
	}
	STATS_DEC_ACTIVE(cachep);
	
	/* fixup slab chain */
	if (slabp->inuse-- == cachep->num)	//删除之前是满的
		goto moveslab_partial;
	if (!slabp->inuse)			//删除之后是空的
		goto moveslab_free;
	return;
	/*
	 * 由于slab在cache中按照: 全部满、部分空闲、全部空闲.
	 * 的顺序排列，当一个slab删除一个obj的时候，如果状态变
	 * 化了，需要调整slab 在cache 中的位置.
	 */
moveslab_partial:
    	/* was full.
	 * Even if the page is now empty, we can set c_firstnotfull to
	 * slabp: there are no partial slabs in this case
	 */
	{
		struct list_head *t = cachep->firstnotfull;

		cachep->firstnotfull = &slabp->list;
		if (slabp->list.next == t)
			return;
		list_del(&slabp->list);
		list_add_tail(&slabp->list, t);
		return;
	}
moveslab_free:
	/*
	 * was partial, now empty.
	 * c_firstnotfull might point to slabp
	 * FIXME: optimize
	 */
	{
		struct list_head *t = cachep->firstnotfull->prev;

		list_del(&slabp->list);
		//添加到cachep 最后
		list_add_tail(&slabp->list, &cachep->slabs);
		//如果此时firstnotfull 指向的是该slab 需要调整
		if (cachep->firstnotfull == &slabp->list)
			cachep->firstnotfull = t->next;
		return;
	}
}

#ifdef CONFIG_SMP
static inline void __free_block (kmem_cache_t* cachep, void** objpp, int len)
{
	//释放所有的节点
	for ( ; len > 0; len--, objpp++)
		kmem_cache_free_one(cachep, *objpp);
}

static void free_block (kmem_cache_t* cachep, void** objpp, int len)
{
	//由自旋锁保护
	spin_lock(&cachep->spinlock);
	__free_block(cachep, objpp, len);
	spin_unlock(&cachep->spinlock);
}
#endif

/*
 * __kmem_cache_free
 * called with disabled ints (调用该函数时候需要禁止中断)
 */
static inline void __kmem_cache_free (kmem_cache_t *cachep, void* objp)
{
#ifdef CONFIG_SMP
	cpucache_t *cc = cc_data(cachep);

	CHECK_PAGE(virt_to_page(objp));
	if (cc) {
		int batchcount;
		//缓存小于limit时，不立即释放，放入到缓存中；
		//否则释放batchcount，将最近要释放的放入到缓存中。
		if (cc->avail < cc->limit) {
			STATS_INC_FREEHIT(cachep);
			cc_entry(cc)[cc->avail++] = objp;
			return;
		}
		STATS_INC_FREEMISS(cachep);
		batchcount = cachep->batchcount;
		cc->avail -= batchcount;
		free_block(cachep, &cc_entry(cc)[cc->avail], batchcount);
		cc_entry(cc)[cc->avail++] = objp;
		return;
	} else {
		free_block(cachep, &objp, 1);
	}
#else
	kmem_cache_free_one(cachep, objp);
#endif
}

/**
 * kmem_cache_alloc - Allocate an object
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache.  The flags are only relevant
 * if the cache has no available objects.
 */
void * kmem_cache_alloc (kmem_cache_t *cachep, int flags)
{
	return __kmem_cache_alloc(cachep, flags);
}

/**
 * kmalloc - allocate memory
 *         - 申请内存
 * @size: how many bytes of memory are required.
 *      : 需要申请的内存大小
 * @flags: the type of memory to allocate.
 *
 * kmalloc is the normal method of allocating memory
 * in the kernel.  The @flags argument may be one of:
 * kmalloc() 是内核中标准的内存申请方式，flag参数可以
 * 是下边的几种:
 *
 * %GFP_BUFFER - XXX
 *
 * %GFP_ATOMIC - allocation will not sleep.  Use inside interrupt handlers.
 *
 * %GFP_USER - allocate memory on behalf of user.  May sleep.
 *
 * %GFP_KERNEL - allocate normal kernel ram.  May sleep.
 *
 * %GFP_NFS - has a slightly lower probability of sleeping than %GFP_KERNEL.
 * Don't use unless you're in the NFS code.
 *
 * %GFP_KSWAPD - Don't use unless you're modifying kswapd.
 */
void * kmalloc (size_t size, int flags)
{
	cache_sizes_t *csizep = cache_sizes;

	//遍历，找到满足条件的大小，申请
	for (; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		return __kmem_cache_alloc(flags & GFP_DMA ?
			 csizep->cs_dmacachep : csizep->cs_cachep, flags);
	}
	BUG(); // too big size
	return NULL;
}

/**
 * kmem_cache_free - Deallocate an object
 * @cachep: The cache the allocation was from.
 * @objp: The previously allocated object.
 *
 * Free an object which was previously allocated from this
 * cache.
 */
void kmem_cache_free (kmem_cache_t *cachep, void *objp)
{
	unsigned long flags;
#if DEBUG
	CHECK_PAGE(virt_to_page(objp));
	if (cachep != GET_PAGE_CACHE(virt_to_page(objp)))
		BUG();
#endif

	local_irq_save(flags);
	__kmem_cache_free(cachep, objp);
	local_irq_restore(flags);
}

/**
 * kfree - free previously allocated memory
 *
 * @objp: pointer returned by kmalloc.
 *
 * Don't free memory not originally allocated by kmalloc()
 * or you will run into trouble.
 */
/* 只能释放通过kmalloc() 申请的内存，否则会导致异常 */
void kfree (const void *objp)
{
	kmem_cache_t *c;
	unsigned long flags;

	if (!objp)
		return;
	local_irq_save(flags);
	CHECK_PAGE(virt_to_page(objp));
	c = GET_PAGE_CACHE(virt_to_page(objp));
	__kmem_cache_free(c, (void*)objp);
	local_irq_restore(flags);
}

kmem_cache_t * kmem_find_general_cachep (size_t size, int gfpflags)
{
	cache_sizes_t *csizep = cache_sizes;

	/* This function could be moved to the header file, and
	 * made inline so consumers can quickly determine what
	 * cache pointer they require.
	 */
	for ( ; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		break;
	}
	return (gfpflags & GFP_DMA) ? csizep->cs_dmacachep : csizep->cs_cachep;
}

#ifdef CONFIG_SMP

/* called with cache_chain_sem acquired.  */
/*
 * 调用的时候需要获取cache_chain_sem 信号量
 * limit: 最大被缓存的数量
 * batchcount: On SMP systems, this specifies the number of objects to
 * 	transfer at one time when refilling the available object list.
 */
static int kmem_tune_cpucache (kmem_cache_t* cachep, int limit, int batchcount)
{
	ccupdate_struct_t new;
	int i;

	/*
	 * These are admin-provided, so we are more graceful.
	 */
	if (limit < 0)
		return -EINVAL;
	if (batchcount < 0)
		return -EINVAL;
	if (batchcount > limit)
		return -EINVAL;
	if (limit != 0 && !batchcount)
		return -EINVAL;

	memset(&new.new,0,sizeof(new.new));
	if (limit) {
		for (i = 0; i< smp_num_cpus; i++) {
			cpucache_t* ccnew;
			//此处申请了limit 个空指针
			ccnew = kmalloc(sizeof(void*)*limit+
					sizeof(cpucache_t), GFP_KERNEL);
			if (!ccnew)
				goto oom;
			ccnew->limit = limit;
			ccnew->avail = 0;
			new.new[cpu_logical_map(i)] = ccnew;
		}
	}
	new.cachep = cachep;
	spin_lock_irq(&cachep->spinlock);	//获取自旋锁
	cachep->batchcount = batchcount;
	spin_unlock_irq(&cachep->spinlock);	//释放自旋锁

	//所有的cpu 都调用该函数
	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);

	for (i = 0; i < smp_num_cpus; i++) {
		cpucache_t* ccold = new.new[cpu_logical_map(i)];
		if (!ccold)
			continue;
		local_irq_disable();	//关闭本地中断
		free_block(cachep, cc_entry(ccold), ccold->avail);
		local_irq_enable();
		kfree(ccold);
	}
	return 0;
oom:
	for (i--; i >= 0; i--)
		kfree(new.new[cpu_logical_map(i)]);
	return -ENOMEM;
}

static void enable_cpucache (kmem_cache_t *cachep)
{
	int err;
	int limit;

	/* FIXME: optimize */
	if (cachep->objsize > PAGE_SIZE)
		return;
	if (cachep->objsize > 1024)
		limit = 60;
	else if (cachep->objsize > 256)
		limit = 124;
	else
		limit = 252;

	err = kmem_tune_cpucache(cachep, limit, limit/2);
	if (err)
		printk(KERN_ERR "enable_cpucache failed for %s, error %d.\n",
					cachep->name, -err);
}

static void enable_all_cpucaches (void)
{
	struct list_head* p;

	//获取信号量并遍历所有cache
	down(&cache_chain_sem);

	p = &cache_cache.next;
	do {
		kmem_cache_t* cachep = list_entry(p, kmem_cache_t, next);

		enable_cpucache(cachep);
		p = cachep->next.next;
	} while (p != &cache_cache.next);

	up(&cache_chain_sem);
}
#endif

/**
 * kmem_cache_reap - Reclaim memory from caches.
 *
 * @gfp_mask: the type of memory required.
 *
 * Called from try_to_free_page().
 */
void kmem_cache_reap (int gfp_mask)
{
	slab_t *slabp;
	kmem_cache_t *searchp;
	kmem_cache_t *best_cachep;
	unsigned int best_pages;
	unsigned int best_len;
	unsigned int scan;

	if (gfp_mask & __GFP_WAIT)
		down(&cache_chain_sem);			//获取信号量
	else
		if (down_trylock(&cache_chain_sem))	//尝试获取信号量，获取不到则返回
			return;

	scan = REAP_SCANLEN;
	best_len = 0;
	best_pages = 0;
	best_cachep = NULL;
	searchp = clock_searchp;
	do {
		unsigned int pages;
		struct list_head* p;
		unsigned int full_free;

		/* It's safe to test this without holding the cache-lock. */
		if (searchp->flags & SLAB_NO_REAP)
			goto next;
		spin_lock_irq(&searchp->spinlock);	//加锁
		if (searchp->growing)
			goto next_unlock;
		/* 如果设置了DFLGS_GROWN 标识位，取消标识位，跳转到下一个查找 */
		if (searchp->dflags & DFLGS_GROWN) {
			searchp->dflags &= ~DFLGS_GROWN;
			goto next_unlock;
		}
#ifdef CONFIG_SMP
		{
			/* 将缓存中的全部释放 */
			cpucache_t *cc = cc_data(searchp);
			if (cc && cc->avail) {
				__free_block(searchp, cc_entry(cc), cc->avail);
				cc->avail = 0;
			}
		}
#endif

		full_free = 0;
		p = searchp->slabs.prev;
		while (p != &searchp->slabs) {
			slabp = list_entry(p, slab_t, list);
			if (slabp->inuse)
				break;
			full_free++;
			p = p->prev;
		}

		/*
		 * Try to avoid slabs with constructors and/or
		 * more than one page per slab (as it can be difficult
		 * to get high orders from gfp()).
		 */
		/*
		 * 这里的pages 就是一个比较标准，通过 (pages*4 + 1)/5 的方式
		 * 减小被选中的几率.
		 */
		pages = full_free * (1<<searchp->gfporder);
		if (searchp->ctor)
			pages = (pages*4+1)/5;
		if (searchp->gfporder)
			pages = (pages*4+1)/5;
		if (pages > best_pages) {
			best_cachep = searchp;
			best_len = full_free;
			best_pages = pages;
			if (full_free >= REAP_PERFECT) {
				clock_searchp = list_entry(searchp->next.next,
							kmem_cache_t,next);
				goto perfect;
			}
		}
next_unlock:
		spin_unlock_irq(&searchp->spinlock);	//解锁
next:
		/* 跳转到下一个链表查找 */
		searchp = list_entry(searchp->next.next, kmem_cache_t, next);
	} while (--scan && searchp != clock_searchp);

	clock_searchp = searchp;

	/* 没找到可以释放的缓存，直接退出 */
	if (!best_cachep)
		/* couldn't find anything to reap */
		goto out;

	spin_lock_irq(&best_cachep->spinlock);
perfect:
	/* free only 80% of the free slabs */
	best_len = (best_len*4 + 1)/5;
	for (scan = 0; scan < best_len; scan++) {
		struct list_head *p;

		if (best_cachep->growing)
			break;
		p = best_cachep->slabs.prev;
		if (p == &best_cachep->slabs)
			break;
		slabp = list_entry(p, slab_t, list);
		if (slabp->inuse)
			break;
		list_del(&slabp->list);
		if (best_cachep->firstnotfull == &slabp->list)
			best_cachep->firstnotfull = &best_cachep->slabs;
		STATS_INC_REAPED(best_cachep);

		/* Safe to drop the lock. The slab is no longer linked to the
		 * cache.
		 */
		spin_unlock_irq(&best_cachep->spinlock);
		kmem_slab_destroy(best_cachep, slabp);
		spin_lock_irq(&best_cachep->spinlock);
	}
	spin_unlock_irq(&best_cachep->spinlock);
out:
	up(&cache_chain_sem);
	return;
}

#ifdef CONFIG_PROC_FS
/* /proc/slabinfo
 *	cache-name num-active-objs total-objs
 *	obj-size num-active-slabs total-slabs
 *	num-pages-per-slab
 */
#define FIXUP(t)				\
	do {					\
		if (len <= off) {		\
			off -= len;		\
			len = 0;		\
		} else {			\
			if (len-off > count)	\
				goto t;		\
		}				\
	} while (0)

static int proc_getdata (char*page, char**start, off_t off, int count)
{
	struct list_head *p;
	int len = 0;

	/* Output format version, so at least we can change it without _too_
	 * many complaints.
	 */
	len += sprintf(page+len, "slabinfo - version: 1.1"
#if STATS
				" (statistics)"
#endif
#ifdef CONFIG_SMP
				" (SMP)"
#endif
				"\n");
	FIXUP(got_data);

	down(&cache_chain_sem);
	p = &cache_cache.next;
	do {
		kmem_cache_t	*cachep;
		struct list_head *q;
		slab_t		*slabp;
		unsigned long	active_objs;
		unsigned long	num_objs;
		unsigned long	active_slabs = 0;
		unsigned long	num_slabs;
		cachep = list_entry(p, kmem_cache_t, next);

		spin_lock_irq(&cachep->spinlock);
		active_objs = 0;
		num_slabs = 0;
		list_for_each(q,&cachep->slabs) {
			slabp = list_entry(q, slab_t, list);
			active_objs += slabp->inuse;
			num_objs += cachep->num;
			if (slabp->inuse)
				active_slabs++;
			else
				num_slabs++;
		}
		num_slabs+=active_slabs;
		num_objs = num_slabs*cachep->num;

		len += sprintf(page+len, "%-17s %6lu %6lu %6u %4lu %4lu %4u",
			cachep->name, active_objs, num_objs, cachep->objsize,
			active_slabs, num_slabs, (1<<cachep->gfporder));

#if STATS
		/* 输出统计信息 */
		{
			unsigned long errors = cachep->errors;
			unsigned long high = cachep->high_mark;
			unsigned long grown = cachep->grown;
			unsigned long reaped = cachep->reaped;
			unsigned long allocs = cachep->num_allocations;

			len += sprintf(page+len, " : %6lu %7lu %5lu %4lu %4lu",
					high, allocs, grown, reaped, errors);
		}
#endif
#ifdef CONFIG_SMP
		/* 输出缓存信息 */
		{
			unsigned int batchcount = cachep->batchcount;
			unsigned int limit;

			if (cc_data(cachep))
				limit = cc_data(cachep)->limit;
			 else
				limit = 0;
			len += sprintf(page+len, " : %4u %4u",
					limit, batchcount);
		}
#endif
#if STATS && defined(CONFIG_SMP)
		/* 输出统计信息 */
		{
			unsigned long allochit = atomic_read(&cachep->allochit);
			unsigned long allocmiss = atomic_read(&cachep->allocmiss);
			unsigned long freehit = atomic_read(&cachep->freehit);
			unsigned long freemiss = atomic_read(&cachep->freemiss);
			len += sprintf(page+len, " : %6lu %6lu %6lu %6lu",
					allochit, allocmiss, freehit, freemiss);
		}
#endif
		len += sprintf(page+len,"\n");
		spin_unlock_irq(&cachep->spinlock);
		FIXUP(got_data_up);
		p = cachep->next.next;
	} while (p != &cache_cache.next);
got_data_up:
	up(&cache_chain_sem);

got_data:
	*start = page+off;
	return len;
}

/**
 * slabinfo_read_proc - generates /proc/slabinfo
 * @page: scratch area, one page long
 * @start: pointer to the pointer to the output buffer
 * @off: offset within /proc/slabinfo the caller is interested in
 * @count: requested len in bytes
 * @eof: eof marker
 * @data: unused
 *
 * The contents of the buffer are
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */
int slabinfo_read_proc (char *page, char **start, off_t off,
				 int count, int *eof, void *data)
{
	int len = proc_getdata(page, start, off, count);
	len -= (*start-page);
	if (len <= count)
		*eof = 1;
	if (len>count) len = count;
	if (len<0) len = 0;
	return len;
}

#define MAX_SLABINFO_WRITE 128
/**
 * slabinfo_write_proc - SMP tuning for the slab allocator
 * @file: unused
 * @buffer: user buffer
 * @count: data len
 * @data: unused
 */
int slabinfo_write_proc (struct file *file, const char *buffer,
				unsigned long count, void *data)
{
#ifdef CONFIG_SMP
	char kbuf[MAX_SLABINFO_WRITE], *tmp;
	int limit, batchcount, res;
	struct list_head *p;
	
	if (count > MAX_SLABINFO_WRITE)
		return -EINVAL;
	if (copy_from_user(&kbuf, buffer, count))
		return -EFAULT;

	tmp = strchr(kbuf, ' ');
	if (!tmp)
		return -EINVAL;
	*tmp = '\0';
	tmp++;
	limit = simple_strtol(tmp, &tmp, 10);
	while (*tmp == ' ')
		tmp++;
	batchcount = simple_strtol(tmp, &tmp, 10);

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);		//获取信号量
	res = -EINVAL;
	list_for_each(p,&cache_chain) {
		kmem_cache_t *cachep = list_entry(p, kmem_cache_t, next);

		if (!strcmp(cachep->name, kbuf)) {
			res = kmem_tune_cpucache(cachep, limit, batchcount);
			break;
		}
	}
	up(&cache_chain_sem);		//释放信号量
	if (res >= 0)
		res = count;
	return res;
#else
	return -EINVAL;
#endif
}
#endif
