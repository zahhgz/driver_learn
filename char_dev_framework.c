/*
 * char_dev_framework.c - Linux 字符设备驱动框架 (CDF v2.0)
 *
 * 本文件是字符设备驱动框架 (Character Device Framework, CDF v2.0) 的内核模块核心实现。
 * 该框架为学习 Linux 字符设备驱动开发提供了一个功能完整的参考实现，涵盖以下主要内容：
 *
 *   1. 字符设备的动态创建、注册和销毁（alloc_chrdev_region / cdev_add / device_create）
 *   2. 基于 kfifo 环形缓冲区的数据读写，支持用户空间与内核空间之间的高效数据搬运
 *   3. 阻塞 IO 与非阻塞 IO (O_NONBLOCK) 支持，通过等待队列实现进程阻塞/唤醒
 *   4. poll/select 多路复用支持，允许应用层高效地同时监控多个设备
 *   5. fasync 异步信号通知机制，当设备可读/可写时通过 SIGIO 主动通知应用层
 *   6. ioctl 控制接口，提供获取状态、设置参数、清空缓冲、统计信息等控制命令
 *   7. proc 文件系统统计接口，通过 /proc/cdf_stats 输出每个设备的运行统计信息
 *
 * 设计要点：
 *   - 每个设备实例独立持有自己的 kfifo 缓冲区、互斥锁、读写等待队列
 *   - 使用 mutex 保护共享数据，避免并发访问导致的数据竞争
 *   - 通过 module_param 提供可调参数：设备数量、缓冲区大小
 *
 * 依赖头文件：char_dev_framework.h（定义命令码、统计结构、默认常量等）
 */

/* 模块头文件：提供 MODULE_LICENSE、module_param、module_init/exit 等宏 */
#include <linux/module.h>
/* 文件系统头文件：提供 file_operations、inode 等文件操作相关结构 */
#include <linux/fs.h>
/* 字符设备头文件：提供 cdev 结构体及 cdev_init/cdev_add/cdev_del 等操作函数 */
#include <linux/cdev.h>
/* 设备模型头文件：提供 class、device 创建/销毁接口，用于自动生成 /dev 节点 */
#include <linux/device.h>
/* 用户空间访问头文件：提供 copy_to_user / copy_from_user 等内核↔用户数据拷贝函数 */
#include <linux/uaccess.h>
/* 内核内存分配头文件：提供 kmalloc / kfree / kcalloc 等内存分配函数 */
#include <linux/slab.h>
/* 互斥锁头文件：提供 mutex 结构体及加锁/解锁接口 */
#include <linux/mutex.h>
/* 错误码头文件：提供 -EFAULT、-EAGAIN、-ERESTARTSYS 等错误码定义 */
#include <linux/errno.h>
/* kfifo 环形缓冲区头文件：提供 kfifo_init / kfifo_to_user / kfifo_from_user 等接口 */
#include <linux/kfifo.h>
/* 等待队列头文件：提供 wait_queue_head_t 及 wait_event_interruptible 等接口 */
#include <linux/wait.h>
/* poll/select 头文件：提供 poll_table、poll_wait 以及 POLLIN/POLLOUT 等事件掩码 */
#include <linux/poll.h>
/* proc 文件系统头文件：提供 proc_create / proc_remove 等接口 */
#include <linux/proc_fs.h>
/* seq_file 头文件：提供 seq_printf / single_open 等大文件顺序输出辅助接口 */
#include <linux/seq_file.h>
/* 文件系统头文件（重复包含，无副作用）：file_operations 等结构 */
#include <linux/fs.h>
/* 内核版本头文件：提供 LINUX_VERSION_CODE、KERNEL_VERSION 宏，用于版本兼容判断 */
#include <linux/version.h>
/* 原子操作头文件：提供 atomic_t 及 atomic_inc/dec/read 等原子操作接口 */
#include <linux/atomic.h>

/* 包含本驱动对应的头文件：定义命令码、统计结构体、默认常量等 */
#include "char_dev_framework.h"

/* 模块许可证声明：GPL 许可证，使用 GPL 协议的内核符号需声明此许可证 */
MODULE_LICENSE("GPL");
/* 模块作者信息 */
MODULE_AUTHOR("CDF Developer");
/* 模块功能描述 */
MODULE_DESCRIPTION("Linux Character Device Driver Framework - Enhanced Edition");
/* 模块版本号 */
MODULE_VERSION("2.0.0");

/* 设备实例数量，可通过模块参数指定，默认值在头文件中定义为 3 */
static int dev_num = CDF_DEV_NUM_DEF;
/* 注册模块参数 dev_num：类型为 int，权限 0444 表示只读，加载后通过 sysfs 可见 */
module_param(dev_num, int, 0444);
/* 模块参数描述：在 modinfo 中显示，说明该参数用途 */
MODULE_PARM_DESC(dev_num, "Number of device instances (default: 3)");

/* 每个设备的缓冲区大小（字节），默认 4096 字节 */
static int buf_size = CDF_BUF_SIZE_DEF;
/* 注册模块参数 buf_size：类型 int，权限 0444 只读 */
module_param(buf_size, int, 0444);
/* 模块参数描述：说明缓冲区大小参数的用途与默认值 */
MODULE_PARM_DESC(buf_size, "Buffer size per device in bytes (default: 4096)");

/*
 * struct cdf_dev - 单个字符设备实例的私有数据结构
 *
 * 每个设备节点（/dev/cdf_dev0、/dev/cdf_dev1 ...）都对应一个 cdf_dev 实例，
 * 在 open 时通过 container_of 取出并保存到 filp->private_data，后续 read/write
 * 等回调即可直接访问该结构。
 */
struct cdf_dev {
    struct cdev cdev;                /* 内核字符设备结构，注册到 cdev_map 中 */
    struct device *dev;              /* 指向 sysfs 设备对象，由 device_create 创建 */
    struct kfifo fifo;               /* kfifo 环形缓冲区对象，存放读写数据 */
    unsigned char *fifo_buf;         /* kfifo 底层使用的真实数据缓冲区指针 */
    int dev_id;                      /* 设备编号（0/1/2...），用于区分不同实例 */
    int status;                      /* 设备状态标志，写入数据或设置参数后置 1，清空时置 0 */
    int param;                       /* 用户可设置的参数值，通过 ioctl 设置 */
    struct mutex lock;               /* 互斥锁，保护本设备所有共享数据的并发访问 */
    wait_queue_head_t read_queue;    /* 读等待队列，缓冲区为空时阻塞读进程 */
    wait_queue_head_t write_queue;   /* 写等待队列，缓冲区满时阻塞写进程 */
    struct fasync_struct *async_queue; /* 异步通知队列，向注册了 fasync 的进程发送 SIGIO */
    struct cdf_io_stats stats;       /* IO 统计信息：读写次数、字节数、错误次数 */
    atomic_t open_count;             /* 当前打开该设备的引用计数（原子变量） */
};

/* 全局设备号（主设备号 + 起始次设备号），由 alloc_chrdev_region 动态分配 */
static dev_t g_devt;
/* 全局设备类指针，用于在 /sys/class 下创建类并在 /dev 下自动生成节点 */
static struct class *g_class;
/* 指向所有设备实例数组的指针，按 dev_num 个连续存放 */
static struct cdf_dev *g_devices;
/* 实际创建的设备实例数量（= dev_num） */
static int g_dev_count;
/* proc 目录项指针，对应 /proc/cdf_stats 文件 */
static struct proc_dir_entry *g_proc_dir;

/*
 * 版本兼容宏：class_create() 在内核 6.4.0 之后不再需要 THIS_MODULE 参数
 * 通过宏定义实现新老内核接口的统一调用，避免使用 #if 污染调用处代码
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define CDF_CLASS_CREATE(name)  class_create(name)
#else
#define CDF_CLASS_CREATE(name)  class_create(THIS_MODULE, name)
#endif

/*
 * cdf_notify_readers - 唤醒等待读取的进程并触发异步通知
 * @dev: 指向目标设备的 cdf_dev 结构
 *
 * 当数据被写入到缓冲区后调用：若缓冲区非空，则唤醒所有阻塞在读等待队列上的
 * 进程，并通过 kill_fasync 向注册了异步通知的应用进程发送 SIGIO 信号
 * （事件类型 POLL_IN 表示有数据可读）。
 */
static void cdf_notify_readers(struct cdf_dev *dev)
{
    /* 仅在缓冲区确实有数据时才唤醒，避免虚假唤醒 */
    if (kfifo_len(&dev->fifo) > 0) {
        /* 唤醒所有等待 read_queue 的可中断睡眠进程 */
        wake_up_interruptible(&dev->read_queue);
        /* 向异步队列中的进程发送 SIGIO，附带 POLL_IN 事件 */
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
    }
}

/*
 * cdf_notify_writers - 唤醒等待写入的进程并触发异步通知
 * @dev: 指向目标设备的 cdf_dev 结构
 *
 * 当数据被读出后缓冲区出现空闲空间时调用：唤醒所有阻塞在写等待队列上的
 * 进程，并通过 kill_fasync 向注册了异步通知的进程发送 SIGIO 信号
 * （事件类型 POLL_OUT 表示有空间可写）。
 */
static void cdf_notify_writers(struct cdf_dev *dev)
{
    /* 仅在缓冲区有空闲空间时才唤醒 */
    if (kfifo_avail(&dev->fifo) > 0) {
        /* 唤醒所有等待 write_queue 的可中断睡眠进程 */
        wake_up_interruptible(&dev->write_queue);
        /* 向异步队列中的进程发送 SIGIO，附带 POLL_OUT 事件 */
        kill_fasync(&dev->async_queue, SIGIO, POLL_OUT);
    }
}

/*
 * cdf_open - 设备打开回调
 * @inode: 内核 inode 结构，包含指向 cdev 的指针
 * @filp:  用户进程对应的 file 结构
 *
 * 通过 container_of 从 inode->i_cdev 反推出所属的 cdf_dev 实例，并保存到
 * filp->private_data 中，方便后续 read/write 等回调直接获取设备上下文。
 * 同时递增打开计数。
 *
 * 返回值：0 表示成功
 */
static int cdf_open(struct inode *inode, struct file *filp)
{
    /* 由 cdev 成员反推得到外层 cdf_dev 结构指针 */
    struct cdf_dev *dev = container_of(inode->i_cdev, struct cdf_dev, cdev);
    /* 将设备上下文保存到 file 的私有数据指针，供其他回调使用 */
    filp->private_data = dev;
    /* 原子递增打开计数，反映当前持有该设备的文件描述符数 */
    atomic_inc(&dev->open_count);
    return 0;
}

/*
 * cdf_release - 设备关闭回调
 * @inode: 内核 inode 结构
 * @filp:  用户进程对应的 file 结构
 *
 * 在文件描述符被关闭时调用：从异步队列中移除当前 file（避免悬挂指针），
 * 并递减打开计数。
 *
 * 返回值：0 表示成功
 */
static int cdf_release(struct inode *inode, struct file *filp)
{
    /* 取回在 open 时保存的设备上下文 */
    struct cdf_dev *dev = filp->private_data;
    /* 将当前 file 从异步队列移除：第三参数 0 表示删除模式 */
    fasync_helper(-1, filp, 0, &dev->async_queue);
    /* 原子递减打开计数 */
    atomic_dec(&dev->open_count);
    return 0;
}

/*
 * cdf_read - 设备读取回调
 * @filp:  用户进程对应的 file 结构
 * @buf:   用户空间缓冲区指针（目标地址）
 * @count: 请求读取的字节数
 * @f_pos: 文件偏移指针（字符设备通常不使用）
 *
 * 从 kfifo 缓冲区向用户空间拷贝数据，支持阻塞与非阻塞模式：
 *   - 非阻塞模式下缓冲区为空时立即返回 -EAGAIN
 *   - 阻塞模式下进程会进入睡眠，直到有数据或被信号中断
 * 读取成功后唤醒可能在等待写入的进程。
 *
 * 返回值：>0 实际读取的字节数；-EAGAIN 非阻塞且无数据；
 *         -ERESTARTSYS 被信号中断；-EFAULT 用户空间拷贝失败
 */
static ssize_t cdf_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    /* 取回设备上下文 */
    struct cdf_dev *dev = filp->private_data;
    /* kfifo_to_user 实际拷贝出的字节数 */
    unsigned int copied;
    int ret;
    /* 检查是否以非阻塞方式打开（O_NONBLOCK 标志位） */
    bool nonblock = filp->f_flags & O_NONBLOCK;

    /* 获取互斥锁，可被信号中断；若被中断直接返回 -ERESTARTSYS 让系统调用重启 */
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* 缓冲区为空时需要等待数据到达 */
    while (kfifo_is_empty(&dev->fifo)) {
        /* 释放锁后再睡眠，避免长时间持锁阻塞写进程 */
        mutex_unlock(&dev->lock);
        /* 非阻塞模式：直接返回 -EAGAIN 提示应用稍后重试 */
        if (nonblock)
            return -EAGAIN;
        /* 阻塞模式：在 read_queue 上等待，直到 kfifo_len > 0 或被信号中断 */
        if (wait_event_interruptible(dev->read_queue, kfifo_len(&dev->fifo) > 0))
            return -ERESTARTSYS;  /* 被信号中断，返回让系统调用重启 */
        /* 被唤醒后需要重新获取锁，可能再次被信号中断 */
        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;
    }

    /* 从 kfifo 直接拷贝数据到用户空间，copied 输出实际拷贝字节数 */
    ret = kfifo_to_user(&dev->fifo, buf, count, &copied);
    if (ret) {
        /* 拷贝失败，统计错误次数后返回 -EFAULT */
        dev->stats.err_count++;
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    /* 更新读取次数与字节数统计 */
    dev->stats.read_count++;
    dev->stats.read_bytes += copied;

    /* 释放锁后通知等待写入的进程：现在缓冲区有空闲空间了 */
    mutex_unlock(&dev->lock);
    cdf_notify_writers(dev);
    /* 返回实际读取的字节数 */
    return copied;
}

/*
 * cdf_write - 设备写入回调
 * @filp:  用户进程对应的 file 结构
 * @buf:   用户空间缓冲区指针（源地址）
 * @count: 请求写入的字节数
 * @f_pos: 文件偏移指针（字符设备通常不使用）
 *
 * 将用户空间数据写入 kfifo 缓冲区，支持阻塞与非阻塞模式：
 *   - 非阻塞模式且缓冲区满时：若已写入部分数据则返回已写字节数，否则返回 -EAGAIN
 *   - 阻塞模式下进程会进入睡眠，直到缓冲区有空闲空间或被信号中断
 * 支持分批写入：当用户请求字节数大于缓冲区空闲空间时循环写入。
 * 写入成功后唤醒等待读取的进程。
 *
 * 返回值：>0 实际写入的字节数；-EAGAIN 非阻塞且无空间；
 *         -ERESTARTSYS 被信号中断；-EFAULT 用户空间拷贝失败
 */
static ssize_t cdf_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    /* 取回设备上下文 */
    struct cdf_dev *dev = filp->private_data;
    /* 单次 kfifo_from_user 拷贝的字节数 */
    unsigned int copied;
    int ret;
    /* 检查是否以非阻塞方式打开 */
    bool nonblock = filp->f_flags & O_NONBLOCK;
    /* 累计已成功写入的字节数（支持分批写入） */
    size_t total_copied = 0;

    /* 获取互斥锁 */
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* 循环写入直到满足 count 字节或无法继续写入 */
    while (total_copied < count) {
        /* 查询当前缓冲区可用空闲空间 */
        size_t avail = kfifo_avail(&dev->fifo);
        if (avail == 0) {
            /* 缓冲区已满，需要先释放锁再睡眠 */
            mutex_unlock(&dev->lock);
            /* 非阻塞模式处理 */
            if (nonblock) {
                /* 若此前已写入部分数据，则返回这部分字节数（短写） */
                if (total_copied > 0)
                    break;
                /* 完全没写入则返回 -EAGAIN */
                return -EAGAIN;
            }
            /* 阻塞模式：在 write_queue 上等待空闲空间 */
            if (wait_event_interruptible(dev->write_queue, kfifo_avail(&dev->fifo) > 0))
                return -ERESTARTSYS;
            /* 被唤醒后重新获取锁 */
            if (mutex_lock_interruptible(&dev->lock))
                return -ERESTARTSYS;
            /* 重新循环判断可用空间（不直接 continue 也可，下面逻辑会重新计算） */
            continue;
        }

        /* 从用户空间拷贝数据到 kfifo，注意 buf 偏移已写入部分 */
        ret = kfifo_from_user(&dev->fifo, buf + total_copied,
                              count - total_copied, &copied);
        if (ret) {
            /* 拷贝失败：记录错误并返回 -EFAULT（已写入数据不回滚） */
            dev->stats.err_count++;
            mutex_unlock(&dev->lock);
            return -EFAULT;
        }

        /* 累加本次拷贝字节数 */
        total_copied += copied;
        /* copied 为 0 表示无任何进展，跳出避免死循环 */
        if (copied == 0)
            break;
    }

    /* 更新写入次数与字节数统计 */
    dev->stats.write_count++;
    dev->stats.write_bytes += total_copied;
    /* 标记设备状态为“有数据” */
    dev->status = 1;

    /* 释放锁后通知等待读取的进程：现在缓冲区有数据了 */
    mutex_unlock(&dev->lock);
    cdf_notify_readers(dev);
    /* 返回实际写入的字节数（可能是短写） */
    return total_copied;
}

/*
 * cdf_poll - poll/select 多路复用回调
 * @filp: 用户进程对应的 file 结构
 * @wait: poll_table 结构，用于注册等待队列
 *
 * 应用通过 select/poll/epoll 监控设备时调用本函数：
 *   1. 通过 poll_wait 将读/写等待队列注册到 poll_table 中
 *   2. 返回当前设备可立即执行的事件掩码
 *
 * 返回值：事件掩码位图，POLLIN/POLLRDNORM 表示可读，
 *         POLLOUT/POLLWRNORM 表示可写；返回 -ERESTARTSYS 表示被信号中断
 */
static unsigned int cdf_poll(struct file *filp, poll_table *wait)
{
    /* 取回设备上下文 */
    struct cdf_dev *dev = filp->private_data;
    /* 事件掩码，初始为 0 */
    unsigned int mask = 0;

    /* 将读、写等待队列加入 poll_table，使 vfs poll 机制能在条件不满足时挂起进程 */
    poll_wait(filp, &dev->read_queue, wait);
    poll_wait(filp, &dev->write_queue, wait);

    /* 加锁后查询实际缓冲区状态 */
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* 缓冲区非空：可读 */
    if (kfifo_len(&dev->fifo) > 0)
        mask |= POLLIN | POLLRDNORM;
    /* 缓冲区非满：可写 */
    if (kfifo_avail(&dev->fifo) > 0)
        mask |= POLLOUT | POLLWRNORM;

    mutex_unlock(&dev->lock);
    return mask;
}

/*
 * cdf_fasync - 异步通知注册/注销回调
 * @fd:  文件描述符
 * @filp: 对应的 file 结构
 * @on:  1 表示注册，0 表示注销
 *
 * 应用通过 fcntl(F_SETFL, FASYNC) 启用异步通知时调用，由 fasync_helper
 * 维护内核内部的异步通知队列。之后缓冲区状态变化时驱动会通过 kill_fasync
 * 向队列中的进程发送 SIGIO。
 *
 * 返回值：fasync_helper 的返回值（通常为 0 或负错误码）
 */
static int cdf_fasync(int fd, struct file *filp, int on)
{
    struct cdf_dev *dev = filp->private_data;
    /* 委托给内核通用函数维护异步队列 */
    return fasync_helper(fd, filp, on, &dev->async_queue);
}

/*
 * cdf_ioctl - 设备控制接口
 * @filp: 用户进程对应的 file 结构
 * @cmd:  用户传入的 ioctl 命令码（已编码方向、类型、序号、参数大小）
 * @arg:  用户传入的参数，可能是值或用户空间地址
 *
 * 支持的控制命令（定义在头文件中）：
 *   CDF_CMD_GET_STATUS     - 获取设备状态
 *   CDF_CMD_SET_PARAM      - 设置用户参数
 *   CDF_CMD_CLEAR_BUF      - 清空缓冲区
 *   CDF_CMD_GET_BUF_SIZE   - 获取缓冲区总大小
 *   CDF_CMD_GET_STATS      - 获取 IO 统计信息
 *   CDF_CMD_CLR_STATS      - 清零统计信息
 *   CDF_CMD_SET_NONBLOCK   - 动态切换阻塞/非阻塞模式
 *   CDF_CMD_TRIGGER_ASYNC  - 主动触发一次异步通知（测试用）
 *   CDF_CMD_GET_BUF_USED   - 获取缓冲区已用字节数
 *
 * 返回值：0 表示成功；-ENOTTY 命令非法；-EFAULT 用户空间拷贝失败；
 *         -ERESTARTSYS 被信号中断
 */
static long cdf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    /* 取回设备上下文 */
    struct cdf_dev *dev = filp->private_data;
    /* 临时变量，用于与用户空间交换单个 int 值 */
    int val;
    /* 临时变量，用于拷贝统计结构体到用户空间 */
    struct cdf_io_stats kstats;
    /* 返回值，默认 0 表示成功 */
    long ret = 0;

    /* 校验命令码的 magic number，防止误调用其他驱动的命令 */
    if (_IOC_TYPE(cmd) != CDF_MAGIC)
        return -ENOTTY;
    /* 校验命令序号是否超出本驱动支持的范围 */
    if (_IOC_NR(cmd) >= CDF_CMD_MAX)
        return -ENOTTY;

    /* 获取互斥锁保护设备数据 */
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    switch (cmd) {
    /* 获取设备状态：将 status 字段返回给用户空间 */
    case CDF_CMD_GET_STATUS:
        val = dev->status;
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            ret = -EFAULT;
        break;

    /* 设置用户参数：从用户空间读取 int 值并保存到 param，同时置位 status */
    case CDF_CMD_SET_PARAM:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int))) {
            ret = -EFAULT;
            break;
        }
        dev->param = val;
        dev->status = 1;
        break;

    /* 清空缓冲区：重置 kfifo 读写指针，同时清零状态 */
    case CDF_CMD_CLEAR_BUF:
        kfifo_reset(&dev->fifo);
        dev->status = 0;
        break;

    /* 获取缓冲区总容量（字节） */
    case CDF_CMD_GET_BUF_SIZE:
        val = kfifo_size(&dev->fifo);
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            ret = -EFAULT;
        break;

    /* 获取 IO 统计信息：将整个 stats 结构体返回给用户空间 */
    case CDF_CMD_GET_STATS:
        kstats = dev->stats;
        if (copy_to_user((struct cdf_io_stats __user *)arg, &kstats, sizeof(kstats)))
            ret = -EFAULT;
        break;

    /* 清零统计信息：将所有计数器归零 */
    case CDF_CMD_CLR_STATS:
        memset(&dev->stats, 0, sizeof(dev->stats));
        break;

    /* 动态切换阻塞/非阻塞模式：通过修改 f_flags 的 O_NONBLOCK 位实现 */
    case CDF_CMD_SET_NONBLOCK:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int))) {
            ret = -EFAULT;
            break;
        }
        if (val)
            filp->f_flags |= O_NONBLOCK;    /* 设置为非阻塞 */
        else
            filp->f_flags &= ~O_NONBLOCK;   /* 设置为阻塞 */
        break;

    /* 主动触发异步通知：用于测试 SIGIO 通知链路是否正常 */
    case CDF_CMD_TRIGGER_ASYNC:
        /* 注意：本分支提前释放锁并直接 return，不进入函数末尾统一解锁路径 */
        mutex_unlock(&dev->lock);
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
        return 0;

    /* 获取缓冲区当前已用字节数（即待读数据量） */
    case CDF_CMD_GET_BUF_USED:
        val = kfifo_len(&dev->fifo);
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            ret = -EFAULT;
        break;

    /* 未知命令：返回 -ENOTTY 表示不支持的 ioctl 操作 */
    default:
        ret = -ENOTTY;
        break;
    }

    /* 统一释放锁 */
    mutex_unlock(&dev->lock);
    return ret;
}

/*
 * cdf_fops - 字符设备文件操作集合
 *
 * 将驱动实现的各回调函数注册到 file_operations 结构体中，内核在
 * cdev_init 时绑定到 cdev。各字段对应关系：
 *   owner          - 模块所有权，防止模块在使用中被卸载
 *   open           - open() 系统调用对应 cdf_open
 *   release        - close() 系统调用对应 cdf_release
 *   read           - read() 系统调用对应 cdf_read
 *   write          - write() 系统调用对应 cdf_write
 *   poll           - poll/select/epoll 对应 cdf_poll
 *   fasync         - fcntl(F_SETFL, FASYNC) 对应 cdf_fasync
 *   unlocked_ioctl - 64 位用户空间的 ioctl() 对应 cdf_ioctl
 *   compat_ioctl   - 32 位用户空间在 64 位内核上的 ioctl()，同样指向 cdf_ioctl
 *   llseek         - 禁用 seek，字符设备流式访问无需定位
 */
static const struct file_operations cdf_fops = {
    .owner          = THIS_MODULE,
    .open           = cdf_open,
    .release        = cdf_release,
    .read           = cdf_read,
    .write          = cdf_write,
    .poll           = cdf_poll,
    .fasync         = cdf_fasync,
    .unlocked_ioctl = cdf_ioctl,
    .compat_ioctl   = cdf_ioctl,
    .llseek         = no_llseek,
};

/*
 * cdf_proc_show - proc 文件输出回调
 * @s: seq_file 输出缓冲区
 * @v: 当前迭代位置（single_open 模式下未使用）
 *
 * 当用户读取 /proc/cdf_stats 时由 seq_file 机制调用。依次输出全局信息
 * （设备总数、缓冲区大小）以及每个设备的运行统计（状态、参数、打开计数、
 * 缓冲区使用情况、读写次数与字节数、错误次数）。
 *
 * 返回值：0 表示成功；-ERESTARTSYS 表示加锁时被信号中断
 */
static int cdf_proc_show(struct seq_file *s, void *v)
{
    int i;

    /* 输出标题与全局信息 */
    seq_puts(s, "=== CDF Driver Statistics ===\n");
    seq_printf(s, "Device count: %d\n", g_dev_count);
    seq_printf(s, "Buffer size per device: %d bytes\n", buf_size);
    seq_puts(s, "\n");

    /* 逐个设备输出统计信息 */
    for (i = 0; i < g_dev_count; i++) {
        struct cdf_dev *dev = &g_devices[i];
        struct cdf_io_stats stats;
        int fifo_len, fifo_avail, open_cnt;

        /* 加锁后采集一致性快照数据 */
        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;

        /* 拷贝统计信息到本地变量，避免长时间持锁输出 */
        stats = dev->stats;
        fifo_len = kfifo_len(&dev->fifo);
        fifo_avail = kfifo_avail(&dev->fifo);
        open_cnt = atomic_read(&dev->open_count);

        /* 采集完毕立即释放锁 */
        mutex_unlock(&dev->lock);

        /* 输出当前设备的所有统计字段 */
        seq_printf(s, "--- Device %d (%s%d) ---\n", i, CDF_DEV_NAME, i);
        seq_printf(s, "  status:      %d\n", dev->status);
        seq_printf(s, "  param:       %d\n", dev->param);
        seq_printf(s, "  open_count:  %d\n", open_cnt);
        /* 输出已用/总容量；总容量 = 已用 + 空闲 */
        seq_printf(s, "  buf_used:    %d / %zu bytes\n", fifo_len,
                   (size_t)fifo_len + fifo_avail);
        seq_printf(s, "  read_count:  %llu\n", stats.read_count);
        seq_printf(s, "  write_count: %llu\n", stats.write_count);
        seq_printf(s, "  read_bytes:  %llu\n", stats.read_bytes);
        seq_printf(s, "  write_bytes: %llu\n", stats.write_bytes);
        seq_printf(s, "  err_count:   %llu\n", stats.err_count);
        seq_puts(s, "\n");
    }

    return 0;
}

/*
 * cdf_proc_open - proc 文件打开回调
 * @inode: proc 文件对应的 inode
 * @file:  即将打开的 file 结构
 *
 * 使用 single_open 简化 seq_file 的使用：整个文件内容一次性由
 * cdf_proc_show 输出，无需维护迭代器状态。
 *
 * 返回值：single_open 的返回值
 */
static int cdf_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, cdf_proc_show, NULL);
}

/*
 * proc 文件操作结构体的版本兼容处理：
 * 内核 5.6.0 起引入独立的 struct proc_ops 专用于 proc 文件，
 * 老版本仍使用 struct file_operations。
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
/* 新内核：使用 proc_ops 结构体 */
static const struct proc_ops cdf_proc_fops = {
    .proc_open    = cdf_proc_open,   /* 打开回调 */
    .proc_read    = seq_read,        /* 读取回调，使用 seq_file 通用实现 */
    .proc_lseek   = seq_lseek,       /* 定位回调，使用 seq_file 通用实现 */
    .proc_release = single_release,  /* 释放回调，对应 single_open */
};
#else
/* 老内核：使用 file_operations 结构体 */
static const struct file_operations cdf_proc_fops = {
    .owner   = THIS_MODULE,
    .open    = cdf_proc_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};
#endif

/*
 * cdf_cleanup_devices - 设备资源清理（部分回滚用）
 * @from: 已成功初始化的设备数量，清理范围为 [0, from)
 *
 * 在 cdf_init 中途某设备初始化失败时调用，逆序释放已创建的资源，
 * 包括：device、cdev、kfifo、内存、互斥锁；最后释放设备数组、
 * 销毁 class、注销设备号区域。确保不会产生资源泄漏。
 */
static void cdf_cleanup_devices(int from)
{
    int i;

    /* 逆序清理，符合“后创建先销毁”的原则 */
    for (i = from - 1; i >= 0; i--) {
        /* 销毁 /dev 节点对应的 device 对象 */
        if (g_devices[i].dev)
            device_destroy(g_class, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i));
        /* 从系统中移除字符设备 */
        cdev_del(&g_devices[i].cdev);
        /* 释放 kfifo 及其底层缓冲区 */
        if (g_devices[i].fifo_buf) {
            kfifo_free(&g_devices[i].fifo);
            kfree(g_devices[i].fifo_buf);
        }
        /* 销毁互斥锁（用于调试器检查锁状态） */
        mutex_destroy(&g_devices[i].lock);
    }

    /* 释放设备数组并置空，避免悬空指针 */
    kfree(g_devices);
    g_devices = NULL;

    /* 销毁设备类 */
    if (g_class) {
        class_destroy(g_class);
        g_class = NULL;
    }

    /* 注销字符设备号区域 */
    if (g_devt) {
        unregister_chrdev_region(g_devt, g_dev_count);
        g_devt = 0;
    }
}

/*
 * cdf_init - 模块加载入口函数
 *
 * 执行以下初始化流程：
 *   1. 校验模块参数（设备数量、缓冲区大小）
 *   2. 将缓冲区大小向上取整为 2 的幂（kfifo 要求）
 *   3. 动态分配字符设备号区域
 *   4. 创建设备类（用于自动生成 /dev 节点）
 *   5. 分配并初始化所有设备实例：kfifo、互斥锁、等待队列、cdev、device
 *   6. 创建 proc 文件接口
 * 任何步骤失败时通过 goto 跳转到对应的清理标签进行回滚。
 *
 * 返回值：0 成功；负数错误码失败
 */
static int __init cdf_init(void)
{
    int ret, i;

    /* 参数校验：设备数量必须在 1~32 之间 */
    if (dev_num <= 0 || dev_num > 32) {
        pr_err("%s: invalid dev_num=%d, must be 1-32\n", __func__, dev_num);
        return -EINVAL;
    }
    /* 参数校验：缓冲区大小必须在 256 字节 ~ 1MB 之间 */
    if (buf_size < 256 || buf_size > (1 << 20)) {
        pr_err("%s: invalid buf_size=%d, must be 256-1MB\n", __func__, buf_size);
        return -EINVAL;
    }

    /* kfifo 要求缓冲区大小为 2 的幂，向上取整 */
    buf_size = roundup_pow_of_two(buf_size);
    /* 记录实际创建的设备数量 */
    g_dev_count = dev_num;

    /* 动态分配主设备号 + 连续 g_dev_count 个次设备号 */
    ret = alloc_chrdev_region(&g_devt, 0, g_dev_count, CDF_DEV_NAME);
    if (ret < 0) {
        pr_err("%s: alloc_chrdev_region failed, ret=%d\n", __func__, ret);
        return ret;
    }

    /* 创建设备类（兼容新老内核通过宏统一调用） */
    g_class = CDF_CLASS_CREATE(CDF_CLASS_NAME);
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("%s: class_create failed, ret=%d\n", __func__, ret);
        g_class = NULL;
        goto err_class;
    }

    /* 分配所有设备实例的内存，kcalloc 会将内存清零 */
    g_devices = kcalloc(g_dev_count, sizeof(struct cdf_dev), GFP_KERNEL);
    if (!g_devices) {
        ret = -ENOMEM;
        goto err_alloc;
    }

    /* 逐个初始化设备实例 */
    for (i = 0; i < g_dev_count; i++) {
        /* 初始化各字段的初值 */
        g_devices[i].dev_id = i;
        g_devices[i].status = 0;
        g_devices[i].param = 0;
        g_devices[i].dev = NULL;
        g_devices[i].fifo_buf = NULL;
        atomic_set(&g_devices[i].open_count, 0);
        mutex_init(&g_devices[i].lock);
        init_waitqueue_head(&g_devices[i].read_queue);
        init_waitqueue_head(&g_devices[i].write_queue);
        g_devices[i].async_queue = NULL;
        memset(&g_devices[i].stats, 0, sizeof(g_devices[i].stats));

        /* 单独分配 kfifo 底层数据缓冲区，便于显式管理生命周期 */
        g_devices[i].fifo_buf = kmalloc(buf_size, GFP_KERNEL);
        if (!g_devices[i].fifo_buf) {
            ret = -ENOMEM;
            goto err_dev_init;
        }

        /* 用预分配的缓冲区初始化 kfifo 对象 */
        ret = kfifo_init(&g_devices[i].fifo, g_devices[i].fifo_buf, buf_size);
        if (ret) {
            pr_err("%s: kfifo_init failed for dev %d\n", __func__, i);
            kfree(g_devices[i].fifo_buf);
            g_devices[i].fifo_buf = NULL;
            goto err_dev_init;
        }

        /* 初始化 cdev 并绑定文件操作集合 */
        cdev_init(&g_devices[i].cdev, &cdf_fops);
        g_devices[i].cdev.owner = THIS_MODULE;

        /* 将 cdev 添加到系统，关联设备号（主设备号 + 偏移 i 的次设备号） */
        ret = cdev_add(&g_devices[i].cdev, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i), 1);
        if (ret < 0) {
            pr_err("%s: cdev_add failed for dev %d, ret=%d\n", __func__, i, ret);
            goto err_dev_init;
        }

        /* 在设备类下创建 device 对象，udev 会据此自动创建 /dev/cdf_devN 节点 */
        g_devices[i].dev = device_create(g_class, NULL,
                                          MKDEV(MAJOR(g_devt), MINOR(g_devt) + i),
                                          NULL, "%s%d", CDF_DEV_NAME, i);
        if (IS_ERR(g_devices[i].dev)) {
            ret = PTR_ERR(g_devices[i].dev);
            pr_err("%s: device_create failed for dev %d, ret=%d\n", __func__, i, ret);
            g_devices[i].dev = NULL;
            /* device_create 失败前 cdev 已添加，需要先删除 cdev 再回滚 */
            cdev_del(&g_devices[i].cdev);
            goto err_dev_init;
        }
    }

    /* 创建 proc 文件 /proc/cdf_stats，权限 0444 全局只读 */
    g_proc_dir = proc_create(CDF_PROC_NAME, 0444, NULL, &cdf_proc_fops);
    if (!g_proc_dir) {
        /* proc 创建失败不影响主功能，仅打印警告继续运行 */
        pr_warn("%s: proc_create failed, continuing without proc interface\n", __func__);
    }

    /* 打印加载成功信息，便于调试确认主设备号与配置 */
    pr_info("%s: cdf driver loaded, major=%d, devices=%d, buf_size=%d\n",
            __func__, MAJOR(g_devt), g_dev_count, buf_size);
    return 0;

err_dev_init:
    /* 设备初始化中途失败：清理已初始化的设备并回滚 class、设备号 */
    cdf_cleanup_devices(i);
    return ret;

err_alloc:
    /* 设备数组分配失败：销毁 class */
    class_destroy(g_class);
    g_class = NULL;
err_class:
    /* class 创建失败：注销已分配的设备号 */
    unregister_chrdev_region(g_devt, g_dev_count);
    g_devt = 0;
    return ret;
}

/*
 * cdf_exit - 模块卸载入口函数
 *
 * 模块卸载（rmmod）时调用，按与初始化相反的顺序释放所有资源：
 *   1. 移除 proc 文件
 *   2. 逆序销毁每个设备的 device、cdev、异步队列、kfifo、互斥锁
 *   3. 释放设备数组
 *   4. 销毁设备类
 *   5. 注销字符设备号区域
 */
static void __exit cdf_exit(void)
{
    int i;

    /* 移除 proc 文件 */
    if (g_proc_dir)
        proc_remove(g_proc_dir);

    /* 逆序清理所有设备资源 */
    for (i = g_dev_count - 1; i >= 0; i--) {
        /* 销毁 /dev 节点 */
        if (g_devices[i].dev)
            device_destroy(g_class, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i));
        /* 移除字符设备 */
        cdev_del(&g_devices[i].cdev);
        /* 清空异步队列，避免悬挂的 file 指针（传入 NULL 表示移除所有） */
        fasync_helper(-1, NULL, 0, &g_devices[i].async_queue);
        /* 释放 kfifo 及其底层缓冲区 */
        if (g_devices[i].fifo_buf) {
            kfifo_free(&g_devices[i].fifo);
            kfree(g_devices[i].fifo_buf);
        }
        /* 销毁互斥锁 */
        mutex_destroy(&g_devices[i].lock);
    }

    /* 释放设备数组 */
    kfree(g_devices);
    /* 销毁设备类 */
    class_destroy(g_class);
    /* 注销字符设备号区域 */
    unregister_chrdev_region(g_devt, g_dev_count);

    /* 打印卸载信息 */
    pr_info("%s: cdf driver unloaded\n", __func__);
}

/* 声明模块的初始化与清理入口，内核在 insmod/rmmod 时调用 */
module_init(cdf_init);
module_exit(cdf_exit);
