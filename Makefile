# ==============================================================================
# Makefile - Linux 字符设备驱动框架 (CDF v2.0) 构建脚本
# ==============================================================================
# 本文件用于构建一个 Linux 字符设备驱动框架，包含两部分构建任务：
#   1. 内核模块 (char_dev_framework.ko)：运行在内核空间的驱动程序
#   2. 用户态测试程序 (test_app)：运行在用户空间的测试应用程序
#
# 工作原理：
#   - 内核模块通过调用内核构建系统 (Kbuild) 来编译，需要内核头文件支持
#   - 用户态测试程序通过 gcc 直接编译，链接 pthread 库
#   - 执行 `make` 时默认会同时构建内核模块和测试程序
#
# 使用方法：
#   make              # 构建全部（内核模块 + 测试程序）
#   make modules      # 仅构建内核模块
#   make app          # 仅构建测试程序
#   make clean        # 清理所有构建产物
#   make help         # 查看帮助信息
# ==============================================================================

# 告诉内核构建系统 (Kbuild) 要编译生成名为 char_dev_framework.ko 的内核模块
# obj-m 表示将该目标编译为模块 (module)，而非编译进内核 (obj-y)
# 文件名对应源文件 char_dev_framework.c
obj-m := char_dev_framework.o

# 内核源码树 (构建目录) 的路径，编译内核模块时需要
# ?= 表示仅在变量未被定义时才赋值，允许外部覆盖
# $(shell uname -r) 调用 shell 命令获取当前内核版本号
# 默认指向当前运行内核对应的头文件目录
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# 当前工作目录的绝对路径，作为模块源码所在目录传给内核构建系统
# $(shell pwd) 调用 shell 的 pwd 命令获取当前目录
PWD := $(shell pwd)

# 用户态程序使用的 C 编译器，gcc 是 Linux 下标准的 C 编译器
CC := gcc

# 用户态测试程序的可执行文件名（构建产物）
TARGET := test_app

# 用户态测试程序的源文件
SRC := test_app.c

# 用户态程序编译选项：
#   -Wall  开启所有常用警告信息，有助于发现潜在问题
#   -O2    开启二级优化，提升程序运行效率
CFLAGS := -Wall -O2

# 用户态程序链接选项：
#   -lpthread 链接 pthread (POSIX 线程) 库，支持多线程编程
LDFLAGS := -lpthread

# 声明伪目标 (phony targets)
# 这些目标不对应实际文件名，避免与同名文件冲突导致 make 误判目标已是最新的
# 即使目录下存在同名文件，make 也会重新执行这些目标的命令
.PHONY: all clean modules modules_install app help

# 默认目标：同时构建内核模块和用户态测试程序
# 依赖 modules 和 app 两个目标，会依次执行
all: modules app

# 构建内核模块的目标
# 通过调用内核构建系统 (Kbuild) 来完成内核模块的编译
modules:
	# 首先检查内核头文件目录是否存在，不存在则给出错误提示并退出
	# @ 表示不回显该命令本身，只显示命令输出
	# $$ 转义为单个 $，用于在 shell 中执行子命令 uname -r
	@if [ ! -d "$(KERNELDIR)" ]; then \
		echo "ERROR: Kernel headers not found at $(KERNELDIR)"; \
		echo "Please install: sudo apt install linux-headers-$$(uname -r)"; \
		exit 1; \
	fi
	# 调用内核构建系统编译模块
	# -C $(KERNELDIR)  先切换到内核源码目录读取顶层 Makefile
	# M=$(PWD)         指定外部模块源码所在目录（即本目录）
	# modules          执行 Kbuild 的 modules 目标，编译外部模块
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

# 构建用户态测试程序的目标
# 依赖于具体的可执行文件 $(TARGET)，会触发其构建规则
app: $(TARGET)

# 构建用户态可执行文件 test_app 的具体规则
# 依赖源文件 test_app.c 和头文件 char_dev_framework.h
# 当源文件或头文件更新时，会重新编译生成可执行文件
# $@ 表示目标文件名 (test_app)，$< 表示第一个依赖文件 (test_app.c)
$(TARGET): $(SRC) char_dev_framework.h
	# 使用 gcc 编译源文件，应用编译选项生成可执行文件，并链接所需库
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# 安装内核模块的目标
# 将编译生成的 .ko 模块安装到系统的内核模块目录 (通常为 /lib/modules/$(uname -r)/extra/)
# 安装后通常需要执行 depmod 更新模块依赖，再用 modprobe 或 insmod 加载
# 注意：此命令需要 root 权限 (sudo make modules_install)
modules_install:
	# 调用内核构建系统的 modules_install 目标完成模块安装
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

# 清理所有构建产物的目标
# 同时清理内核模块和用户态程序生成的中间文件与可执行文件
clean:
	# 判断内核头文件目录是否存在，存在则使用 Kbuild 的 clean 目标清理
	# 不存在则手动删除内核模块构建过程中产生的中间文件
	# 文件类型说明：
	#   *.o        目标文件
	#   *.ko       编译生成的内核模块
	#   *.mod.c    模块相关生成的 C 文件
	#   *.mod.o    模块相关生成的目标文件
	#   *.order    模块构建顺序文件
	#   *.symvers  模块符号版本文件
	#   *.mod      模块信息文件
	@if [ -d "$(KERNELDIR)" ]; then \
		$(MAKE) -C $(KERNELDIR) M=$(PWD) clean; \
	else \
		rm -f *.o *.ko *.mod.c *.mod.o *.order *.symvers *.mod; \
	fi
	# 删除用户态测试程序的可执行文件
	rm -f $(TARGET)
	# 删除可能残留的内核构建辅助文件
	rm -f Module.symvers modules.order

# 帮助目标：显示 Makefile 的使用说明
# 所有命令前加 @，避免在屏幕上回显命令本身，只显示 echo 输出的内容
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
