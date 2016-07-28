#*.ko

#DEBUG = y

ifeq ($(DEBUG),y)

DEBFLAGS = -O -g -DSCULL_DEBUG -D__KERNEL__
else
DEBFLAGS = -O2
endif
	
	
ccflags-y	+=	$(DEBFLAGS)
	
ifneq ($(KERNELRELEASE),)

#pipe-objs := pipe.o
obj-m		:= pipe.o

else
KERN_DIR ?= /home/jiang/linux-socfpga
PWD := $(shell pwd)

modules:
	@echo	'building...'
	make -C $(KERN_DIR) M=$(PWD) modules 
	@echo	'building end'
endif


clean:
	rm -rf *.o  *.ko *.mod.c *.symvers *.order
	
	
depend .depend dep:
	$(CC) $(CFLAGS) -M *.c > .depend
	
ifeq (.depend,$(wildcard .depend))
include .depend
endif
	
