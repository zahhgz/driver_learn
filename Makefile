obj-m := char_dev_framework.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

CC := gcc
TARGET := test_app
SRC := test_app.c

CFLAGS := -Wall -O2
LDFLAGS := -lpthread

.PHONY: all clean modules modules_install app help

all: modules app

modules:
	@if [ ! -d "$(KERNELDIR)" ]; then \
		echo "ERROR: Kernel headers not found at $(KERNELDIR)"; \
		echo "Please install: sudo apt install linux-headers-$$(uname -r)"; \
		exit 1; \
	fi
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

app: $(TARGET)

$(TARGET): $(SRC) char_dev_framework.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

clean:
	@if [ -d "$(KERNELDIR)" ]; then \
		$(MAKE) -C $(KERNELDIR) M=$(PWD) clean; \
	else \
		rm -f *.o *.ko *.mod.c *.mod.o *.order *.symvers *.mod; \
	fi
	rm -f $(TARGET)
	rm -f Module.symvers modules.order

help:
	@echo "CDF Driver Framework Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all            - Build kernel module and test app (default)"
	@echo "  modules        - Build kernel module only"
	@echo "  app            - Build test application only"
	@echo "  modules_install- Install kernel module"
	@echo "  clean          - Clean all build artifacts"
	@echo "  help           - Show this help"
	@echo ""
	@echo "Module parameters (pass via insmod):"
	@echo "  dev_num=N      - Number of devices (1-32, default 3)"
	@echo "  buf_size=N     - Buffer size per device (256-1MB, default 4096)"
