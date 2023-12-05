include Kbuild

KDIR ?= /lib/modules/`uname -r`/build

modules modules_install clean:
	make W=1 -C $(KDIR) M=$$PWD $@

SOURCES := $(patsubst %.o,%.c,$(obj-m))

checkpatch:
	$(KDIR)/scripts/checkpatch.pl $(SOURCES) docs/*.rst

PKGVER ?= $(shell ./gitversion.sh)

dkms.conf: dkms.conf.in
	sed -e "s/@PKGVER@/$(PKGVER)/" $< >$@

# Generate dkms.conf again every time to avoid stale version numbers
.PHONY: dkms.conf

# https://www.gnu.org/software/make/manual/html_node/Command-Variables.html
INSTALL := install
INSTALL_PROGRAM := $(INSTALL)
INSTALL_DATA := $(INSTALL) -m 644

# https://www.gnu.org/software/make/manual/html_node/Directory-Variables.html
prefix := /usr

dkms_install: INSTALL_BASE_DIR = $(DESTDIR)$(prefix)/src/liquidtux-$(PKGVER)

dkms_install: dkms.conf
	mkdir -p $(INSTALL_BASE_DIR)
	$(INSTALL_DATA) dkms.conf Makefile Kbuild $(SOURCES) $(INSTALL_BASE_DIR)/
