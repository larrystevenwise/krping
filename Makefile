KSRC=/lib/modules/`uname -r`/source
KOBJ=/lib/modules/`uname -r`/build
OFA=/usr/src/ofa_kernel-1.2.5
CFLAGS += -DLINUX -D__KERNEL__ -DMODULE -O2 -pipe -Wall
CFLAGS += -I$(OFA)/include -I$(KSRC)/include -I.
CFLAGS += $(shell [ -f $(KSRC)/include/linux/modversions.h ] && \
            echo "-DMODVERSIONS -DEXPORT_SYMTAB \
                  -include $(KSRC)/include/linux/modversions.h")
CFLAGS += $(shell [ -f $(KSRC)/include/config/modversions.h ] && \
            echo "-DMODVERSIONS -DEXPORT_SYMTAB \
                  -include $(KSRC)/include/config/modversions.h")

CFLAGS += $(CFLAGS_EXTRA)

obj-m += rdma_krping.o
rdma_krping-y			:= getopt.o krping.o

default:
	make -C $(KSRC) O=$(KOBJ) SUBDIRS=$(shell pwd) LINUXINCLUDE=' -I$(OFA)/include -Iinclude -include include/linux/autoconf.h -include $(OFA)/include/linux/autoconf.h' modules
install:
	make -C $(KSRC) O=$(KOBJ) SUBDIRS=$(shell pwd) LINUXINCLUDE=' -I$(OFA)/include -Iinclude -include include/linux/autoconf.h -include $(OFA)/include/linux/autoconf.h' modules_install
	depmod -a

clean:
	rm -f *.o
	rm -f *.ko
	rm -f rdma_krping.mod.c
	rm -f Module.symvers
