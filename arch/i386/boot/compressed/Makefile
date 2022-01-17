#
# linux/arch/i386/boot/compressed/Makefile
#
# create a compressed vmlinux image from the original vmlinux
#

HEAD = head.o
SYSTEM = $(TOPDIR)/vmlinux

OBJECTS = $(HEAD) misc.o

CFLAGS = $(CPPFLAGS) -O2 -DSTDC_HEADERS
ZLDFLAGS = -e startup_32

#
# ZIMAGE_OFFSET is the load offset of the compression loader
# BZIMAGE_OFFSET is the load offset of the high loaded compression loader
#
# 此处的偏移量表示的是system加载到内存时text起始位置所在的段内偏移地址，
# 只有移动到了正确的位置，代码才能够正确执行.
#
ZIMAGE_OFFSET = 0x1000		# 4KB
BZIMAGE_OFFSET = 0x100000	# 1MB

ZLINKFLAGS = -Ttext $(ZIMAGE_OFFSET) $(ZLDFLAGS)
BZLINKFLAGS = -Ttext $(BZIMAGE_OFFSET) $(ZLDFLAGS)

all: vmlinux

vmlinux: piggy.o $(OBJECTS)
	$(LD) $(ZLINKFLAGS) -o vmlinux $(OBJECTS) piggy.o

bvmlinux: piggy.o $(OBJECTS)
	$(LD) $(BZLINKFLAGS) -o bvmlinux $(OBJECTS) piggy.o

head.o: head.S
	$(CC) $(AFLAGS) -traditional -c head.S

misc.o: misc.c
	$(CC) $(CFLAGS) -c misc.c

# $$ 会被展开，变成当前进程的pid
piggy.o:	$(SYSTEM)
	tmppiggy=_tmp_$$$$piggy; \
	rm -f $$tmppiggy $$tmppiggy.gz $$tmppiggy.lnk; \
	$(OBJCOPY) $(SYSTEM) $$tmppiggy; \
	gzip -f -9 < $$tmppiggy > $$tmppiggy.gz; \
	echo "SECTIONS { .data : { input_len = .; LONG(input_data_end - input_data) input_data = .; *(.data) input_data_end = .; }}" > $$tmppiggy.lnk; \
	$(LD) -r -o piggy.o -b binary $$tmppiggy.gz -b elf32-i386 -T $$tmppiggy.lnk; \
	rm -f $$tmppiggy $$tmppiggy.gz $$tmppiggy.lnk

clean:
	rm -f vmlinux bvmlinux _tmp_*
