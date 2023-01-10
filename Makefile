KDIR ?= /lib/modules/$(shell uname -r)/build
SRC_DIR := drivers/hwmon

modules:  # default target

%:
	$(MAKE) W=1 -C $(KDIR) M=$(abspath $(SRC_DIR)) $@

$(SRC_DIR)/%:
	$(MAKE) W=1 -C $(KDIR) M=$(abspath $(SRC_DIR)) $*

include $(SRC_DIR)/Makefile

SOURCES := $(patsubst %.o,%.c,$(obj-m))
SOURCES := $(addprefix $(SRC_DIR)/,$(SOURCES))

checkpatch:
	$(KDIR)/scripts/checkpatch.pl $(SOURCES) Documentation/hwmon/*.rst

PKGVER ?= $(shell ./gitversion.sh)

$(SRC_DIR)/dkms.conf: $(SRC_DIR)/dkms.conf.in
	sed -e "s/@PKGVER@/$(PKGVER)/" $< >$@

# Generate dkms.conf again every time to avoid stale version numbers
.PHONY: $(SRC_DIR)/dkms.conf

# https://www.gnu.org/software/make/manual/html_node/Command-Variables.html
INSTALL := install
INSTALL_PROGRAM := $(INSTALL)
INSTALL_DATA := $(INSTALL) -m 644

# https://www.gnu.org/software/make/manual/html_node/Directory-Variables.html
prefix := /usr

dkms_install: DKMS_INSTALL_BASE_DIR = $(DESTDIR)$(prefix)/src/liquidtux-$(PKGVER)

dkms_install: $(SRC_DIR)/dkms.conf $(SRC_DIR)/Makefile $(SOURCES)
	mkdir -p $(DKMS_INSTALL_BASE_DIR)
	$(INSTALL_DATA) $^ $(DKMS_INSTALL_BASE_DIR)/
