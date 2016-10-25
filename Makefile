ccflags-y := -Iinclude/drm
netvdrm-y :=	simpledrm_drv.o simpledrm_kms.o simpledrm_gem.o \
		simpledrm_damage.o netv_hw.o netv_kms_helper.o
netvdrm-$(CONFIG_FB) += simpledrm_fbdev.o

obj-m := netvdrm.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) modules
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) clean

depend .depend dep:
	$(CC) $(EXTRA_CFLAGS) -M *.c > .depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif
