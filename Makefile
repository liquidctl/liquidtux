include Kbuild

KDIR ?= /lib/modules/`uname -r`/build

modules modules_install clean:
	make -C $(KDIR) M=$$PWD $@

SOURCES := $(patsubst %.o,%.c,$(obj-m))

checkpatch:
	$(KDIR)/scripts/checkpatch.pl $(SOURCES)

PKGVER ?= $(shell ./gitversion.sh)

dkms.conf: dkms.conf.in
	sed -e "s/@PKGVER@/$(PKGVER)/" $< >$@

# https://www.gnu.org/software/make/manual/html_node/Command-Variables.html
INSTALL := install
INSTALL_PROGRAM := $(INSTALL)
INSTALL_DATA := $(INSTALL) -m 644

# https://www.gnu.org/software/make/manual/html_node/Directory-Variables.html
prefix := /usr

# PACKAGE_NAME and PACKAGE_VERSION should come from dkms.conf.
# Source dkms.conf before using INSTALL_BASE_DIR.
dkms_install: INSTALL_BASE_DIR = $(DESTDIR)$(prefix)/src/$$PACKAGE_NAME-$$PACKAGE_VERSION

dkms_install: dkms.conf
	. ./dkms.conf && mkdir -p $(INSTALL_BASE_DIR)
	. ./dkms.conf && $(INSTALL_DATA) dkms.conf Makefile Kbuild $(SOURCES) $(INSTALL_BASE_DIR)/
