KERNELRELEASE   ?= `uname -r`
KERNEL_DIR      ?= /lib/modules/$(KERNELRELEASE)/build
PWD             := $(shell pwd)

obj-m	+= ch341-core.o
obj-m	+= gpio-ch341.o
obj-m	+= i2c-ch341.o

PREFIX ?= /usr/local

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD)

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

