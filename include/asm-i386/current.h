#ifndef _I386_CURRENT_H
#define _I386_CURRENT_H

struct task_struct;

static inline struct task_struct * get_current(void)
{
	struct task_struct *current;
	/*
	 * 将当前栈取8KB对齐，获得当前的进程的task_struct{} 结构体.
	 */
	__asm__("andl %%esp,%0; ":"=r" (current) : "0" (~8191UL));
	return current;
}
 
#define current get_current()

#endif /* !(_I386_CURRENT_H) */
