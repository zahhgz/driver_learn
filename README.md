# CDF - Linux 字符设备驱动框架 v2.0

一个功能完整的 Linux 字符设备驱动学习框架，涵盖阻塞/非阻塞 IO、poll 多路复用、fasync 异步通知、ioctl 控制接口、proc 统计接口等核心机制，配套用户空间测试程序。

## 项目结构

```
.
├── char_dev_framework.c    # 内核模块核心实现
├── char_dev_framework.h    # 公共头文件（ioctl 命令码、数据结构、默认常量）
├── test_app.c              # 用户空间测试程序
├── Makefile                # 构建脚本
├── LICENSE                 # Apache 2.0 许可证
└── README.md
```

## 功能特性

- **动态设备创建** - 通过 `alloc_chrdev_region` / `cdev_add` / `device_create` 动态注册字符设备，自动生成 `/dev/cdf_dev0`、`/dev/cdf_dev1` 等设备节点
- **kfifo 环形缓冲区** - 基于内核 kfifo 实现高效的数据读写，支持用户空间与内核空间之间的数据搬运
- **阻塞与非阻塞 IO** - 通过等待队列实现进程阻塞/唤醒，支持 `O_NONBLOCK` 非阻塞模式
- **poll/select 多路复用** - 允许应用层同时监控多个设备的可读/可写状态
- **fasync 异步通知** - 当设备可读/可写时通过 `SIGIO` 信号主动通知应用层
- **ioctl 控制接口** - 提供获取状态、设置参数、清空缓冲、统计信息等 9 个控制命令
- **proc 统计接口** - 通过 `/proc/cdf_stats` 输出每个设备的运行统计信息
- **内核版本兼容** - 兼容内核 5.6+（proc_ops）及 6.4+（class_create 接口变更）

## 模块参数

| 参数 | 说明 | 默认值 | 范围 |
|------|------|--------|------|
| `dev_num` | 设备实例数量 | 3 | 1-32 |
| `buf_size` | 每设备缓冲区大小（字节） | 4096 | 256-1MB |

缓冲区大小会自动向上取整为 2 的幂（kfifo 要求）。

## ioctl 命令

| 命令 | 方向 | 说明 |
|------|------|------|
| `CDF_CMD_GET_STATUS` | 读 | 获取设备状态 |
| `CDF_CMD_SET_PARAM` | 写 | 设置设备参数 |
| `CDF_CMD_CLEAR_BUF` | 无 | 清空缓冲区 |
| `CDF_CMD_GET_BUF_SIZE` | 读 | 获取缓冲区总大小 |
| `CDF_CMD_GET_STATS` | 读 | 获取 IO 统计信息 |
| `CDF_CMD_CLR_STATS` | 无 | 清零统计信息 |
| `CDF_CMD_SET_NONBLOCK` | 写 | 动态切换阻塞/非阻塞模式 |
| `CDF_CMD_TRIGGER_ASYNC` | 无 | 主动触发异步通知 |
| `CDF_CMD_GET_BUF_USED` | 读 | 获取缓冲区已用字节数 |

## 编译构建

**前提条件：** 需要安装当前内核版本对应的头文件。

```bash
# Ubuntu/Debian
sudo apt install linux-headers-$(uname -r) build-essential

# 构建全部（内核模块 + 测试程序）
make

# 仅构建内核模块
make modules

# 仅构建测试程序
make app

# 清理构建产物
make clean
```

## 加载与使用

```bash
# 加载内核模块（使用默认参数）
sudo insmod char_dev_framework.ko

# 加载内核模块（自定义参数）
sudo insmod char_dev_framework.ko dev_num=5 buf_size=8192

# 查看内核日志
dmesg | tail

# 查看 proc 统计信息
cat /proc/cdf_stats
```

## 测试程序

`test_app` 是配套的用户空间测试程序，支持单项测试和全自动回归测试。

```bash
# 运行全量自动测试（15 个用例，覆盖所有功能）
sudo ./test_app /dev/cdf_dev0 test_all

# 基本读写测试
sudo ./test_app /dev/cdf_dev0 write "Hello, CDF!"
sudo ./test_app /dev/cdf_dev0 read 1024

# ioctl 测试
sudo ./test_app /dev/cdf_dev0 get_status
sudo ./test_app /dev/cdf_dev0 set_param 42
sudo ./test_app /dev/cdf_dev0 get_buf_size
sudo ./test_app /dev/cdf_dev0 get_buf_used
sudo ./test_app /dev/cdf_dev0 get_stats
sudo ./test_app /dev/cdf_dev0 clr_stats
sudo ./test_app /dev/cdf_dev0 clear_buf

# 高级 IO 机制测试
sudo ./test_app /dev/cdf_dev0 poll_test       # poll/select 多路复用
sudo ./test_app /dev/cdf_dev0 async_test      # fasync 异步信号通知
sudo ./test_app /dev/cdf_dev0 nonblock_test   # 非阻塞 IO
```

## 卸载模块

```bash
sudo rmmod char_dev_framework
```

## 许可证

Apache License 2.0 - 详见 [LICENSE](LICENSE) 文件。
