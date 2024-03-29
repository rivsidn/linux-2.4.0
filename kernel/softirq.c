/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 * Fixed a disable_bh()/enable_bh() race (was causing a console lockup)
 * due bh_mask_count not atomic handling. Copyright (C) 1998  Andrea Arcangeli
 *
 * Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/tqueue.h>

/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.
   - These softirqs are not masked by global cli() and start_bh_atomic()
     (by clear reasons). Hence, old parts of code still using global locks
     MUST NOT use softirqs, but insert interfacing routines acquiring
     global locks. F.e. look at BHs implementation.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
   - Bottom halves: globally serialized, grr...
 */

/* No separate irq_stat for s390, it is part of PSA */
#if !defined(CONFIG_ARCH_S390)
irq_cpustat_t irq_stat[NR_CPUS];
#endif	/* CONFIG_ARCH_S390 */

static struct softirq_action softirq_vec[32] __cacheline_aligned;

asmlinkage void do_softirq()
{
	int cpu = smp_processor_id();
	__u32 active, mask;

	if (in_interrupt())
		return;

	/* 递增local_bh_count()，递增之后会在in_interrupt() 中直接返回 */
	local_bh_disable();

	local_irq_disable();
	mask = softirq_mask(cpu);
	active = softirq_active(cpu) & mask;

	if (active) {
		struct softirq_action *h;

restart:
		/* Reset active bitmask before enabling irqs */
		softirq_active(cpu) &= ~active;

		/*
		 * 中断开启。
		 * 软中断处理过程中，对应CPU的中断是开启的，软中断是关闭的。
		 */
		local_irq_enable();

		h = softirq_vec;
		mask &= ~active;

		/*
		 * 通过active 掩码执行对应软中断
		 */
		do {
			if (active & 1)
				h->action(h);
			h++;
			active >>= 1;
		} while (active);

		local_irq_disable();

		active = softirq_active(cpu);
		if ((active &= mask) != 0)
			goto retry;
	}

	/* 递减local_bh_count() */
	local_bh_enable();

	/* Leave with locally disabled hard irqs. It is critical to close
	 * window for infinite recursion, while we help local bh count,
	 * it protected us. Now we are defenceless.
	 */
	return;

retry:
	goto restart;
}


static spinlock_t softirq_mask_lock = SPIN_LOCK_UNLOCKED;

void open_softirq(int nr, void (*action)(struct softirq_action*), void *data)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&softirq_mask_lock, flags);
	softirq_vec[nr].data = data;
	softirq_vec[nr].action = action;

	for (i=0; i<NR_CPUS; i++)
		softirq_mask(i) |= (1<<nr);
	spin_unlock_irqrestore(&softirq_mask_lock, flags);
}


/* Tasklets */

struct tasklet_head tasklet_vec[NR_CPUS] __cacheline_aligned;

/*
 * 参见 tasklet_hi_action() 注释.
 */
static void tasklet_action(struct softirq_action *a)
{
	int cpu = smp_processor_id();
	struct tasklet_struct *list;

	local_irq_disable();
	list = tasklet_vec[cpu].list;
	tasklet_vec[cpu].list = NULL;
	local_irq_enable();

	while (list != NULL) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (atomic_read(&t->count) == 0) {
				clear_bit(TASKLET_STATE_SCHED, &t->state);

				t->func(t->data);
				/*
				 * talklet_trylock() uses test_and_set_bit that imply
				 * an mb when it returns zero, thus we need the explicit
				 * mb only here: while closing the critical section.
				 */
#ifdef CONFIG_SMP
				smp_mb__before_clear_bit();
#endif
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}
		local_irq_disable();
		t->next = tasklet_vec[cpu].list;
		tasklet_vec[cpu].list = t;
		__cpu_raise_softirq(cpu, TASKLET_SOFTIRQ);
		local_irq_enable();
	}
}



struct tasklet_head tasklet_hi_vec[NR_CPUS] __cacheline_aligned;

/*
 * 在函数tasklet_hi_schedule() 中将需要处理的tasklet_struct{} 挂载
 * 在tasklet_hi_vec[] 中，在该函数中将tasklet_struct{} 结构体取出，
 * 执行.
 */
static void tasklet_hi_action(struct softirq_action *a)
{
	int cpu = smp_processor_id();
	struct tasklet_struct *list;

	local_irq_disable();
	list = tasklet_hi_vec[cpu].list;
	tasklet_hi_vec[cpu].list = NULL;
	local_irq_enable();

	while (list != NULL) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (atomic_read(&t->count) == 0) {
				clear_bit(TASKLET_STATE_SCHED, &t->state);

				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}
		local_irq_disable();
		t->next = tasklet_hi_vec[cpu].list;
		tasklet_hi_vec[cpu].list = t;
		__cpu_raise_softirq(cpu, HI_SOFTIRQ);
		local_irq_enable();
	}
}

void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->func = func;
	t->data = data;
	t->state = 0;
	atomic_set(&t->count, 0);
}

void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())
		printk("Attempt to kill tasklet from interrupt\n");

	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		current->state = TASK_RUNNING;
		do {
			current->policy |= SCHED_YIELD;
			schedule();
		} while (test_bit(TASKLET_STATE_SCHED, &t->state));
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}



/* Old style BHs */

static void (*bh_base[32])(void);
struct tasklet_struct bh_task_vec[32];

/* BHs are serialized by spinlock global_bh_lock.

   It is still possible to make synchronize_bh() as
   spin_unlock_wait(&global_bh_lock). This operation is not used
   by kernel now, so that this lock is not made private only
   due to wait_on_irq().

   It can be removed only after auditing all the BHs.
 */
spinlock_t global_bh_lock = SPIN_LOCK_UNLOCKED;

static void bh_action(unsigned long nr)
{
	int cpu = smp_processor_id();

	//处理时候必须要获取global_bh_lock，该操作确保后半部串行化处理.
	//由于此处是一个自旋锁，所以此处的串行化是全局串行化.
	if (!spin_trylock(&global_bh_lock))
		goto resched;

	if (!hardirq_trylock(cpu))
		goto resched_unlock;

	if (bh_base[nr])
		bh_base[nr]();

	hardirq_endlock(cpu);
	spin_unlock(&global_bh_lock);
	return;

resched_unlock:
	spin_unlock(&global_bh_lock);
resched:
	mark_bh(nr);
}

void init_bh(int nr, void (*routine)(void))
{
	bh_base[nr] = routine;
	mb();
}

void remove_bh(int nr)
{
	tasklet_kill(bh_task_vec+nr);
	bh_base[nr] = NULL;
}

/*
 * bottom half(后半段) 是老版本内核的处理机制。
 * 老版本内核中后半段处理是串行的，某一时间段只能有一个CPU处理后半段函数，
 * 该机制在SMP下会限制系统性能。
 * 当前版本引入了软中断机制，后半段作为软中断机制的特例处理。
 * 后半段函数的调用顺序：
 * tasklet_hi_action()--> bh_action()-->后半段处理函数
 * 正常的软中断函数调用顺序：
 * tasklet_action()-->tasklet_struct{}->func() 软中断处理函数
 *
 * 所以软中断中有三个层次结构体：
 * softirq_action{}	软中断，do_softirq() 中调用其中的action()函数
 * tasklet_struct{}	软中断处理函数中调用其中的func()函数
 * 后半段处理函数	bh_task_vec[] 是tasklet_struct{} 结构体，其中的func()
 * 			都是bh_action()，在该函数中加上各种串行化各种限制，而
 * 			后调用后半段处理函数.
 * 			后半段处理函数是一类特殊的 HI_SOFTIRQ.
 */
void __init softirq_init()
{
	int i;

	/*
	 * 初始化了32个tasklet_struct{} 结构体，每个结构体的函数都是bh_action。
	 * 后半段是特殊的 HI_SOFTIRQ 软中断。
	 */
	for (i=0; i<32; i++)
		tasklet_init(bh_task_vec+i, bh_action, i);

	open_softirq(TASKLET_SOFTIRQ, tasklet_action, NULL);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action, NULL);
}

void __run_task_queue(task_queue *list)
{
	struct list_head head, *next;
	unsigned long flags;

	spin_lock_irqsave(&tqueue_lock, flags);
	list_add(&head, list);
	list_del_init(list);
	spin_unlock_irqrestore(&tqueue_lock, flags);

	next = head.next;
	while (next != &head) {
		void (*f) (void *);
		struct tq_struct *p;
		void *data;

		p = list_entry(next, struct tq_struct, list);
		next = next->next;
		f = p->routine;
		data = p->data;
		wmb();
		p->sync = 0;
		if (f)
			f(data);
	}
}
