#######################################################
# Copyright (C) 1999-2021 Parallels International GmbH.
# All Rights Reserved.
# http://www.parallels.com
#######################################################

KVER ?= $(shell uname -r)
KERNEL_DIR ?= /lib/modules/$(KVER)/build
DRIVER_DIR ?= $(PWD)
DRIVER_MAJOR ?= 1
DRIVER_MINOR ?= 0
DRIVER_PATCH ?= 0
DRIVER_VERSION ?= $(DRIVER_MAJOR).$(DRIVER_MINOR).$(DRIVER_PATCH)
DRIVER := prl_notifier

CFILES = ./prlnotifier.c

ccflags-y += -DDRV_VERSION=\"$(DRIVER_VERSION)\"

obj-m += $(DRIVER).o
$(DRIVER)-objs += prlnotifier.o

prl_notifier:	$(CFILES)
	$(info Start compile $(DRIVER)...)
	$(MAKE) -C $(KERNEL_DIR) M=$(DRIVER_DIR) SRCROOT=$(DRIVER_DIR) CC=$(CC) modules

clean:
	$(info Start cleaning $(DRIVER)...)
	rm -rf *.o* *.ko *.mod* *symvers modules.order
