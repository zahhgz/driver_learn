obj-m := char_dev_framework.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

CC := gcc
TARGET := test_app
SRC := test_app.c

CFLAGS := -Wall -O2

.PHONY: all clean modules modules_install

all: modules $(TARGET)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

$(TARGET): $(SRC) char_dev_framework.h
	$(CC) $(CFLAGS) -o $@ $<

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(TARGET)
	rm -f Module.symvers modules.order
