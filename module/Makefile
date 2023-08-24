KERNEL ?= $(shell uname -r)
obj-m += pteditor.o
ccflags-y += -Wno-unused-result -I/usr/src/linux-headers-${KERNEL}/include/linux/ -DCC_USING_FENTRY
all:
	make -C /lib/modules/${KERNEL}/build M=$(PWD) modules
clean:
	make -C /lib/modules/${KERNEL}/build M=$(PWD) clean
