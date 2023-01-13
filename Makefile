include Kbuild

KDIR ?= /lib/modules/`uname -r`/build

modules modules_install clean:
	make -C $(KDIR) M=$$PWD $@

SOURCES := $(patsubst %.o,%.c,$(obj-m))

checkpatch:
	$(KDIR)/scripts/checkpatch.pl $(SOURCES)
