#
# Makefile for the linux kernel.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...

# Rules.make 中使用，将该文件作为目标文件
O_TARGET := kernel.o

# 此处的 export-objs 意思是有导出符号表的文件
export-objs = signal.o sys.o kmod.o context.o ksyms.o pm.o

obj-y     = sched.o dma.o fork.o exec_domain.o panic.o printk.o \
	    module.o exit.o itimer.o info.o time.o softirq.o resource.o \
	    sysctl.o acct.o capability.o ptrace.o timer.o user.o \
	    signal.o sys.o kmod.o context.o

obj-$(CONFIG_UID16) += uid16.o
obj-$(CONFIG_MODULES) += ksyms.o
obj-$(CONFIG_PM) += pm.o

ifneq ($(CONFIG_IA64),y)
# According to Alan Modra <alan@linuxcare.com.au>, the -fno-omit-frame-pointer is
# needed for x86 only.  Why this used to be enabled for all architectures is beyond
# me.  I suspect most platforms don't need this, but until we know that for sure
# I turn this off for IA-64 only.  Andreas Schwab says it's also needed on m68k
# to get a correct value for the wait-channel (WCHAN in ps). --davidm
#
# 编译sched.o 的时候添加独有的编译选项，通过在Rules.make 中重新定义隐式规则来实现该功能
CFLAGS_sched.o := $(PROFILING) -fno-omit-frame-pointer
endif

include $(TOPDIR)/Rules.make
