MODULE_NAME = asgn2
#EXTRA_CFLAGS += -w


obj-m   := $(MODULE_NAME).o
asgn2-objs := asgn_2.o gpio.o

KDIR    := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)



all: module

module:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) M=$(PWD)  modules

#gpio:
#	gcc -g -W -Wall gpio.c -o gpio

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

help:
	$(MAKE) -C $(KDIR) M=$(PWD) help

install:
	sudo dmesg -C
	-sudo rmmod asgn2
	sudo insmod ./asgn2.ko
	sudo chmod 777 /dev/asgn2
