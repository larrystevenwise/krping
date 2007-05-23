KSRC=/lib/modules/`uname -r`/source
CFLAGS += -DLINUX -D__KERNEL__ -DMODULE -O2 -pipe -Wall
CFLAGS += -I/usr/local/src/ofa_1_2_kernel-20070313-1000/include -I$(KSRC)/include -I.
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
	make -C $(KSRC) SUBDIRS=$(shell pwd) LINUXINCLUDE='-I/usr/local/src/ofa_1_2_kernel-20070310-1414/include -I$(KSRC)/include -I.  -include include/linux/autoconf.h ' modules

clean:
	rm -f *.o
	rm -f *.ko
	rm -f rdma_krping.mod.c
	rm -f Module.symvers
