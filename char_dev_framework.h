#ifndef __CHAR_DEV_FRAMEWORK_H__
#define __CHAR_DEV_FRAMEWORK_H__

#include <linux/ioctl.h>
#include <linux/types.h>

/* ===== 名称与默认参数 ===== */
#define CDF_DEV_NAME        "cdf_dev"
#define CDF_CLASS_NAME      "cdf_class"
#define CDF_DEV_NUM_DEF     3
#define CDF_BUF_SIZE_DEF   4096
#define CDF_PROC_NAME       "cdf_stats"
#define CDF_MMAP_SIZE      4096     /* mmap 缓冲区大小，必须 >= PAGE_SIZE */

#define CDF_MAGIC           'c'

/* ===== IOCTL 数据结构 ===== */
struct cdf_io_stats {
    __u64 read_count;
    __u64 write_count;
    __u64 read_bytes;
    __u64 write_bytes;
    __u64 err_count;
    __u64 irq_count;        /* 累计触发的"硬件中断"次数 */
};

/* 供 mmap 路径使用的元数据：用户态通过该结构定位数据长度 */
struct cdf_mmap_header {
    __u32 magic;            /* 'CDFM' */
    __u32 size;             /* 数据区有效字节 */
    __u32 seq;              /* 数据序号，每次更新递增 */
    __u32 reserved;
};

/* ===== IOCTL 命令 ===== */
/* v2.0 既有命令 */
#define CDF_CMD_GET_STATUS       _IOR(CDF_MAGIC, 0x01, int)
#define CDF_CMD_SET_PARAM      _IOW(CDF_MAGIC, 0x02, int)
#define CDF_CMD_CLEAR_BUF      _IO(CDF_MAGIC, 0x03)
#define CDF_CMD_GET_BUF_SIZE   _IOR(CDF_MAGIC, 0x04, int)
#define CDF_CMD_GET_STATS      _IOR(CDF_MAGIC, 0x05, struct cdf_io_stats)
#define CDF_CMD_CLR_STATS      _IO(CDF_MAGIC, 0x06)
#define CDF_CMD_SET_NONBLOCK     _IOW(CDF_MAGIC, 0x07, int)
#define CDF_CMD_TRIGGER_ASYNC   _IO(CDF_MAGIC, 0x08)
#define CDF_CMD_GET_BUF_USED    _IOR(CDF_MAGIC, 0x09, int)

/* v3.0 新增命令 */
#define CDF_CMD_IRQ_ENABLE     _IOW(CDF_MAGIC, 0x0A, int)   /* 启/停模拟中断源 */
#define CDF_CMD_GET_MMAP_INFO   _IOR(CDF_MAGIC, 0x0B, struct cdf_mmap_header)
#define CDF_CMD_FLUSH         _IO(CDF_MAGIC, 0x0C)          /* flush workqueue */

#define CDF_CMD_MAX            0x0D

#endif
