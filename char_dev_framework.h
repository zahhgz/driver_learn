/*
 * char_dev_framework.h - Linux 字符设备驱动框架 (CDF v2.0) 头文件
 *
 * 本文件定义了字符设备驱动框架 (Character Device Framework, 简称 CDF) 的
 * 公共接口，包括设备名称、缓冲区大小、ioctl 命令码以及 I/O 统计信息结构体等。
 *
 * 作用：
 *   1. 提供驱动模块与用户空间程序之间共享的常量定义，保证双方使用一致的
 *      设备名、类名、缓冲区大小等参数。
 *   2. 定义一组 ioctl 命令码，用于用户空间对设备进行状态查询、参数配置、
 *      缓冲区管理以及统计信息获取等控制操作。
 *   3. 定义 I/O 统计信息结构体，便于对设备的读写行为进行监控和性能分析。
 *
 * 使用说明：
 *   - 内核驱动模块在实现文件中 #include 本头文件以获取相关定义。
 *   - 用户空间应用程序也应 #include 本头文件，以便使用相同的 ioctl 命令码
 *     与驱动进行交互。
 */

#ifndef __CHAR_DEV_FRAMEWORK_H__
#define __CHAR_DEV_FRAMEWORK_H__

#include <linux/ioctl.h>   /* 提供 _IO/_IOR/_IOW 等宏，用于构造 ioctl 命令码 */
#include <linux/types.h>   /* 提供 __u64 等固定宽度整数类型定义 */

/* 设备名称：在 /dev 目录下创建的设备节点名称 */
#define CDF_DEV_NAME        "cdf_dev"

/* 设备类名称：用于在 sysfs 中创建设备类 (/sys/class/cdf_class) */
#define CDF_CLASS_NAME      "cdf_class"

/* 默认创建的设备数量：驱动加载时默认注册的次设备实例个数 */
#define CDF_DEV_NUM_DEF     3

/* 默认缓冲区大小（字节）：设备内部数据缓冲区的默认容量，此处为 4KB */
#define CDF_BUF_SIZE_DEF   4096

/* proc 文件名称：在 /proc 目录下创建的统计信息文件 (/proc/cdf_stats) */
#define CDF_PROC_NAME       "cdf_stats"

/*
 * ioctl 魔数（magic number）
 * 用于标识本驱动的 ioctl 命令族，防止与其他驱动的命令码冲突。
 * 这里使用字符 'c' 作为魔数。
 */
#define CDF_MAGIC           'c'

/*
 * struct cdf_io_stats - I/O 统计信息结构体
 *
 * 用于记录设备自加载以来的读写活动统计信息，可通过
 * CDF_CMD_GET_STATS 命令获取，便于监控设备使用情况和进行性能分析。
 */
struct cdf_io_stats {
    __u64 read_count;    /* 读操作完成的次数 */
    __u64 write_count;   /* 写操作完成的次数 */
    __u64 read_bytes;    /* 累计读取的字节数 */
    __u64 write_bytes;   /* 累计写入的字节数 */
    __u64 err_count;     /* 发生错误的次数（如读写下溢/溢出等） */
};

/*
 * 以下为 ioctl 命令码定义，使用 <linux/ioctl.h> 中的宏构造：
 *   _IO(magic, nr)        : 无数据传输的命令
 *   _IOR(magic, nr, type) : 从内核读取数据到用户空间（read 方向）
 *   _IOW(magic, nr, type) : 从用户空间写入数据到内核（write 方向）
 * 其中 magic 为魔数，nr 为命令编号，type 为传输的数据类型。
 */

/* 获取设备当前状态（如是否打开、是否阻塞等），返回 int 类型状态值 */
#define CDF_CMD_GET_STATUS       _IOR(CDF_MAGIC, 0x01, int)

/* 设置设备参数（如工作模式等），传入 int 类型参数 */
#define CDF_CMD_SET_PARAM      _IOW(CDF_MAGIC, 0x02, int)

/* 清空设备内部数据缓冲区，不传输数据 */
#define CDF_CMD_CLEAR_BUF      _IO(CDF_MAGIC, 0x03)

/* 获取当前缓冲区大小（字节），返回 int 类型 */
#define CDF_CMD_GET_BUF_SIZE   _IOR(CDF_MAGIC, 0x04, int)

/* 获取 I/O 统计信息，返回 struct cdf_io_stats 结构体 */
#define CDF_CMD_GET_STATS      _IOR(CDF_MAGIC, 0x05, struct cdf_io_stats)

/* 清零 I/O 统计信息计数器，不传输数据 */
#define CDF_CMD_CLR_STATS      _IO(CDF_MAGIC, 0x06)

/* 设置是否为非阻塞模式（O_NONBLOCK），传入 int 类型参数（0 为阻塞，非 0 为非阻塞） */
#define CDF_CMD_SET_NONBLOCK     _IOW(CDF_MAGIC, 0x07, int)

/* 触发一次异步操作（如异步通知或异步任务），不传输数据 */
#define CDF_CMD_TRIGGER_ASYNC   _IO(CDF_MAGIC, 0x08)

/* 获取缓冲区当前已使用字节数，返回 int 类型 */
#define CDF_CMD_GET_BUF_USED    _IOR(CDF_MAGIC, 0x09, int)

/*
 * 命令码上限值：用于在驱动中校验传入的 ioctl 命令编号是否合法，
 * 任何 nr >= CDF_CMD_MAX 的命令都会被拒绝。
 */
#define CDF_CMD_MAX            0x0A

#endif
