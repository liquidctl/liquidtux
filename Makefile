KDIR ?= /lib/modules/`uname -r`/build

modules modules_install clean:
	make -C $(KDIR) M=$$PWD $@

__pull:
	cp ~/Code/linux/drivers/hwmon/nzxt*.c ./
	cp ~/Code/linux/Documentation/hwmon/nzxt*.rst ./docs/
