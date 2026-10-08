obj-m += freesync_mccs_guard.o

KDIR ?= /usr/lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
