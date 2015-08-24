obj-m	+= lab1_mutex1.o lab1_mutex2.o lab1_mutex3.o

KDIR	:= /lib/modules/$(shell uname -r)/build
PWD	:= $(shell pwd)


default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
	rm -f modules.order


