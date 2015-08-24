obj-m	+= semaphore1.o semaphore2.o semaphore3.o

KDIR	:= /lib/modules/$(shell uname -r)/build
PWD	:= $(shell pwd)


default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
	rm -f modules.order


