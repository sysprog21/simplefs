obj-m += simplefs.o
simplefs-objs := fs.o super.o inode.o file.o dir.o extent.o

KDIR ?= /lib/modules/$(shell uname -r)/build

MKFS = mkfs.simplefs

all: $(MKFS)
	make -C $(KDIR) M=$(PWD) modules

IMAGE ?= test.img
IMAGESIZE ?= 200
JOURNAL ?= journal.img
JOURNALSIZE ?= 8

# To test max files(40920) in directory, the image size should be at least 159.85 MiB
# 40920 * 4096(block size) ~= 159.85 MiB

$(MKFS): mkfs.c
	$(CC) -std=gnu99 -Wall -o $@ $<

$(IMAGE): $(MKFS)
	dd if=/dev/zero of=${IMAGE} bs=1M count=${IMAGESIZE}
	./$< $(IMAGE)

journal: $(JOURNAL)

$(JOURNAL):
	dd if=/dev/zero of=$(JOURNAL) bs=1M count=$(JOURNALSIZE)
	mke2fs -b 4096 -O journal_dev $(JOURNAL)

check: all
	script/test.sh $(IMAGE) $(IMAGESIZE) $(MKFS)

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f *~ $(PWD)/*.ur-safe
	rm -f $(MKFS) $(IMAGE) $(JOURNAL)

.PHONY: all clean journal
