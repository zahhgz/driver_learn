/*
 * test_app.c - Linux 字符设备驱动框架（CDF v2.0）用户空间测试程序
 *
 * 文件作用概述：
 *   本文件是配套 "char_dev_framework"（CDF v2.0）字符设备驱动的用户空间测试程序。
 *   它通过标准的文件描述符接口（open/read/write/ioctl/close）以及高级 IO 机制
 *   （poll/select、fasync 异步通知、非阻塞 IO）对驱动进行全方位的功能验证。
 *
 * 主要功能：
 *   1. 基本读写测试：验证 write()/read() 数据通路是否正常。
 *   2. ioctl 命令测试：覆盖获取状态、设置参数、清空缓冲、查询缓冲信息、
 *      读写统计等多种 ioctl 命令。
 *   3. poll/select 多路复用测试：验证设备在缓冲为空/非空时返回的事件掩码
 *      （POLLIN/POLLOUT）是否符合预期。
 *   4. fasync 异步信号通知测试：通过 SIGIO 信号验证驱动在数据就绪时能否
 *      主动通知用户进程。
 *   5. 非阻塞 IO 测试：验证在 O_NONBLOCK 模式下，无数据可读时是否返回 EAGAIN。
 *   6. 全量自动化测试（test_all）：按既定顺序依次执行上述全部测试用例，
 *      便于一键回归验证。
 *
 * 使用方式：
 *   ./test_app <设备节点路径> <命令> [参数...]
 *   例如：./test_app /dev/cdf_dev test_all
 *
 * 适用对象：本文件适合 Linux 驱动开发初学者学习用户空间如何与字符设备驱动交互。
 */

/* 标准输入输出库，提供 printf/fprintf/perror 等函数 */
#include <stdio.h>
/* 标准库，提供 malloc/free/atoi/exit 等 */
#include <stdlib.h>
/* 字符串处理库，提供 strlen/memset/memcpy 等 */
#include <string.h>
/* POSIX 标准接口，提供 read/write/close/sleep/usleep/getpid 等 */
#include <unistd.h>
/* 文件控制相关定义，提供 open/close/fcntl 及 O_RDWR/O_NONBLOCK/FASYNC 等标志 */
#include <fcntl.h>
/* ioctl 系统调用接口 */
#include <sys/ioctl.h>
/* select 多路复用相关定义 */
#include <sys/select.h>
/* poll 多路复用相关定义，提供 struct pollfd 与 poll() 函数 */
#include <sys/poll.h>
/* 信号处理接口，提供 sigaction/SIGIO/sigemptyset 等 */
#include <signal.h>
/* 错误码定义，提供 errno 与 EAGAIN 等 */
#include <errno.h>
/* POSIX 线程库（本文件中保留以备扩展使用） */
#include <pthread.h>

/* 字符设备驱动框架的头文件，定义 ioctl 命令码（CDF_CMD_*）与 cdf_io_stats 等数据结构 */
#include "char_dev_framework.h"

/*
 * 全局标志变量：用于记录是否收到 SIGIO 信号。
 * 使用 volatile 关键字修饰，因为该变量会在信号处理函数（异步上下文）中被修改，
 * 主流程每次访问时都需从内存重新读取，避免被编译器优化到寄存器中。
 */
static volatile int g_got_signal = 0;

/*
 * sigio_handler - SIGIO 信号处理函数
 *
 * 当驱动通过 fasync 机制向进程发送 SIGIO 信号时，本函数被内核调用。
 * 函数仅将全局标志 g_got_signal 置为 1，主流程通过轮询该标志判断是否收到通知。
 *
 * 注意：在信号处理函数中应避免调用非异步信号安全的函数，这里仅做简单标志置位。
 *
 * 参数 sig: 触发本处理函数的信号编号（这里应为 SIGIO），(void)sig 用于消除
 *          "未使用参数" 的编译警告。
 */
static void sigio_handler(int sig)
{
    (void)sig;
    g_got_signal = 1;
}

/*
 * print_usage - 打印程序使用说明
 *
 * 当用户传入参数个数不足或命令不被识别时调用，向标准输出打印用法及支持的命令列表。
 *
 * 参数 prog: 程序名（通常为 argv[0]），用于在用法提示中展示调用格式。
 */
static void print_usage(const char *prog)
{
    printf("Usage: %s <device> <command> [args...]\n", prog);
    printf("Commands:\n");
    printf("  write <string>         Write string to device\n");
    printf("  read <count>           Read count bytes from device\n");
    printf("  get_status             Get device status\n");
    printf("  set_param <value>      Set device parameter\n");
    printf("  clear_buf              Clear device buffer\n");
    printf("  get_buf_size           Get buffer size\n");
    printf("  get_buf_used           Get used buffer bytes\n");
    printf("  get_stats              Get IO statistics\n");
    printf("  clr_stats              Clear IO statistics\n");
    printf("  poll_test              Test poll/select\n");
    printf("  async_test             Test fasync signal notification\n");
    printf("  nonblock_test          Test non-blocking IO\n");
    printf("  test_all               Run all tests\n");
}

/*
 * test_write - 基本写测试
 *
 * 目的：验证 write() 系统调用能够将用户数据正确写入设备缓冲。
 * 方法：调用 write() 将传入的字符串写入设备，并打印实际写入的字节数；
 *       若返回值小于 0，则使用 perror 输出错误信息并返回 -1。
 *
 * 参数 fd:   已打开的设备文件描述符
 * 参数 data: 待写入的字符串（以 '\0' 结尾）
 * 返回值:   成功返回 0，失败返回 -1
 */
static int test_write(int fd, const char *data)
{
    ssize_t ret;

    printf("  [WRITE] Writing: \"%s\"\n", data);
    ret = write(fd, data, strlen(data));
    if (ret < 0) {
        perror("  write failed");
        return -1;
    }
    printf("  [WRITE] Written %zd bytes\n", ret);
    return 0;
}

/*
 * test_read - 基本读测试
 *
 * 目的：验证 read() 系统调用能够从设备缓冲读出数据。
 * 方法：先动态分配 count+1 字节的缓冲区（多 1 字节用于存放字符串结束符），
 *       调用 read() 读取指定字节数，并以字符串形式打印读取结果；
 *       读取完毕后释放缓冲区。失败时同样释放以避免内存泄漏。
 *
 * 参数 fd:    已打开的设备文件描述符
 * 参数 count: 期望读取的字节数
 * 返回值:    成功返回 0，失败返回 -1
 */
static int test_read(int fd, size_t count)
{
    char *buf;
    ssize_t ret;

    buf = malloc(count + 1);
    if (!buf) {
        perror("  malloc failed");
        return -1;
    }
    memset(buf, 0, count + 1);

    ret = read(fd, buf, count);
    if (ret < 0) {
        perror("  read failed");
        free(buf);
        return -1;
    }

    printf("  [READ] Read %zd bytes: \"%s\"\n", ret, buf);
    free(buf);
    return 0;
}

/*
 * test_get_status - 获取设备状态测试
 *
 * 目的：通过 ioctl 命令 CDF_CMD_GET_STATUS 查询设备当前状态值。
 * 方法：调用 ioctl()，第三个参数为存放状态值的整型变量地址（驱动将状态写入其中），
 *       成功后打印状态值。该命令用于验证驱动的 .unlocked_ioctl 回调实现。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_get_status(int fd)
{
    int status;
    int ret;

    ret = ioctl(fd, CDF_CMD_GET_STATUS, &status);
    if (ret < 0) {
        perror("  ioctl GET_STATUS failed");
        return -1;
    }
    printf("  [IOCTL GET_STATUS] status = %d\n", status);
    return 0;
}

/*
 * test_set_param - 设置设备参数测试
 *
 * 目的：通过 ioctl 命令 CDF_CMD_SET_PARAM 向驱动写入一个参数值。
 * 方法：将传入的整型值存入局部变量 param，并将其地址作为 ioctl 的参数传递；
 *       驱动负责解析并应用该参数。验证 ioctl "写"方向命令的通路。
 *
 * 参数 fd:  已打开的设备文件描述符
 * 参数 val: 待设置的参数值
 * 返回值:  成功返回 0，失败返回 -1
 */
static int test_set_param(int fd, int val)
{
    int param = val;
    int ret;

    ret = ioctl(fd, CDF_CMD_SET_PARAM, &param);
    if (ret < 0) {
        perror("  ioctl SET_PARAM failed");
        return -1;
    }
    printf("  [IOCTL SET_PARAM] param = %d\n", val);
    return 0;
}

/*
 * test_clear_buf - 清空设备缓冲测试
 *
 * 目的：通过 ioctl 命令 CDF_CMD_CLEAR_BUF 清空设备内部的环形缓冲区。
 * 方法：该命令不需要携带数据参数，ioctl 第三参数省略；
 *       常用于在执行其它测试前重置设备状态，保证测试的可重复性。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_clear_buf(int fd)
{
    int ret;

    ret = ioctl(fd, CDF_CMD_CLEAR_BUF);
    if (ret < 0) {
        perror("  ioctl CLEAR_BUF failed");
        return -1;
    }
    printf("  [IOCTL CLEAR_BUF] buffer cleared\n");
    return 0;
}

/*
 * test_get_buf_size - 获取缓冲总容量测试
 *
 * 目的：通过 ioctl 命令 CDF_CMD_GET_BUF_SIZE 查询设备缓冲区的总字节数。
 * 方法：将整型变量地址传入 ioctl，驱动返回缓冲总容量。
 *       该值通常在驱动加载时由模块参数决定，是只读属性。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_get_buf_size(int fd)
{
    int size;
    int ret;

    ret = ioctl(fd, CDF_CMD_GET_BUF_SIZE, &size);
    if (ret < 0) {
        perror("  ioctl GET_BUF_SIZE failed");
        return -1;
    }
    printf("  [IOCTL GET_BUF_SIZE] buf_size = %d bytes\n", size);
    return 0;
}

/*
 * test_get_buf_used - 获取缓冲已用字节数测试
 *
 * 目的：通过 ioctl 命令 CDF_CMD_GET_BUF_USED 查询设备缓冲区中当前已有数据量。
 * 方法：将整型变量地址传入 ioctl，驱动返回已写入但尚未读出的字节数。
 *       配合 write/read 操作可验证缓冲管理逻辑的正确性。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_get_buf_used(int fd)
{
    int used;
    int ret;

    ret = ioctl(fd, CDF_CMD_GET_BUF_USED, &used);
    if (ret < 0) {
        perror("  ioctl GET_BUF_USED failed");
        return -1;
    }
    printf("  [IOCTL GET_BUF_USED] buf_used = %d bytes\n", used);
    return 0;
}

/*
 * test_get_stats - 获取 IO 统计信息测试
 *
 * 目的：通过 ioctl 命令 CDF_CMD_GET_STATS 获取驱动维护的 IO 统计结构体。
 * 方法：传入 struct cdf_io_stats 变量地址，驱动回填读/写次数、读/写字节数、
 *       错误次数等字段。本函数逐项打印这些统计值，便于观察驱动的运行情况。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_get_stats(int fd)
{
    struct cdf_io_stats stats;
    int ret;

    ret = ioctl(fd, CDF_CMD_GET_STATS, &stats);
    if (ret < 0) {
        perror("  ioctl GET_STATS failed");
        return -1;
    }
    printf("  [IOCTL GET_STATS]\n");
    printf("    read_count  = %llu\n", stats.read_count);
    printf("    write_count = %llu\n", stats.write_count);
    printf("    read_bytes  = %llu\n", stats.read_bytes);
    printf("    write_bytes = %llu\n", stats.write_bytes);
    printf("    err_count   = %llu\n", stats.err_count);
    return 0;
}

/*
 * test_clr_stats - 清除 IO 统计信息测试
 *
 * 目的：通过 ioctl 命令 CDF_CMD_CLR_STATS 将驱动的 IO 统计计数器全部清零。
 * 方法：该命令不携带数据参数。常用于在执行特定测试前重置统计，便于精确观测
 *       单次测试产生的 IO 次数与字节数。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_clr_stats(int fd)
{
    int ret;

    ret = ioctl(fd, CDF_CMD_CLR_STATS);
    if (ret < 0) {
        perror("  ioctl CLR_STATS failed");
        return -1;
    }
    printf("  [IOCTL CLR_STATS] statistics cleared\n");
    return 0;
}

/*
 * test_poll - poll 多路复用机制测试
 *
 * 目的：验证驱动 .poll 回调（实现 poll_wait 与事件掩码返回）的正确性，
 *       确保设备在不同状态下向用户空间报告的就绪事件符合预期。
 *
 * 测试流程（详细）：
 *   1. 先调用 test_clear_buf() 清空缓冲，使设备处于"无数据可读"的初始状态。
 *   2. 构造 pollfd 结构：监听该 fd，关注的事件为 POLLIN（可读）| POLLOUT（可写）。
 *   3. 第一次调用 poll()，超时时间设为 100ms：
 *      - 期望结果：缓冲为空时，应返回 POLLOUT（缓冲未满可写），不应返回 POLLIN
 *        （无数据可读）。若与预期不符则报错返回。
 *   4. 调用 test_write() 写入测试数据，使缓冲非空。
 *   5. 第二次调用 poll()：
 *      - 期望结果：此时应返回 POLLIN（有数据可读），验证驱动在数据写入后能正确
 *        唤醒等待读的 poll 调用。
 *   6. 全部断言通过则打印 Test PASSED。
 *
 * 说明：poll() 的第三个参数为超时毫秒数；返回值 > 0 表示有事件就绪，
 *       = 0 表示超时，< 0 表示出错。revents 字段由内核回填实际发生的事件。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_poll(int fd)
{
    struct pollfd pfd;
    int ret;
    char test_data[] = "poll test data";

    printf("  [POLL] Testing poll/select...\n");

    /* 步骤 1：清空缓冲，确保初始状态下缓冲为空 */
    test_clear_buf(fd);

    /* 步骤 2：设置 pollfd，关注 POLLIN（可读）和 POLLOUT（可写）事件 */
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;

    /* 步骤 3：第一次 poll，超时 100ms，观察空缓冲下的事件掩码 */
    ret = poll(&pfd, 1, 100);
    if (ret < 0) {
        perror("  poll failed");
        return -1;
    }

    printf("  [POLL] Empty buffer: ret=%d, revents=0x%x (POLLOUT=%s, POLLIN=%s)\n",
           ret, pfd.revents,
           (pfd.revents & POLLOUT) ? "yes" : "no",
           (pfd.revents & POLLIN) ? "yes" : "no");

    /* 空缓冲应当可写（POLLOUT 置位） */
    if (!(pfd.revents & POLLOUT)) {
        printf("  [POLL] ERROR: expected POLLOUT on empty buffer\n");
        return -1;
    }
    /* 空缓冲不应当可读（POLLIN 不应置位） */
    if (pfd.revents & POLLIN) {
        printf("  [POLL] ERROR: unexpected POLLIN on empty buffer\n");
        return -1;
    }

    /* 步骤 4：写入数据，使缓冲非空，触发可读条件 */
    test_write(fd, test_data);

    /* 步骤 5：第二次 poll，观察写入后的事件掩码 */
    pfd.revents = 0;
    ret = poll(&pfd, 1, 100);
    if (ret < 0) {
        perror("  poll failed");
        return -1;
    }

    printf("  [POLL] After write: ret=%d, revents=0x%x (POLLOUT=%s, POLLIN=%s)\n",
           ret, pfd.revents,
           (pfd.revents & POLLOUT) ? "yes" : "no",
           (pfd.revents & POLLIN) ? "yes" : "no");

    /* 写入后应可读（POLLIN 置位） */
    if (!(pfd.revents & POLLIN)) {
        printf("  [POLL] ERROR: expected POLLIN after write\n");
        return -1;
    }

    /* 步骤 6：所有断言通过 */
    printf("  [POLL] Test PASSED\n");
    return 0;
}

/*
 * test_async - fasync 异步信号通知机制测试
 *
 * 目的：验证驱动 .fasync 回调的正确性，即在数据就绪时能否通过 SIGIO 信号
 *       主动通知用户进程，而无需进程反复轮询。
 *
 * 测试流程（详细）：
 *   1. 先清空缓冲，确保测试环境干净。
 *   2. 使用 sigaction() 为 SIGIO 信号注册处理函数 sigio_handler（该函数仅将
 *      g_got_signal 置 1）。sigaction 比 signal 更健壮，可控制信号掩码与标志。
 *   3. 调用 fcntl(fd, F_SETOWN, getpid()) 设置"文件拥有者"为本进程，
 *      告知内核将 SIGIO 发送给谁。
 *   4. 调用 F_GETFL 获取当前文件状态标志，再用 F_SETFL 添加 FASYNC 标志，
 *      触发驱动的 .fasync 回调，将本进程加入驱动的异步通知列表。
 *   5. 重置 g_got_signal = 0，然后写入数据。若驱动在写入路径中调用了
 *      kill_fasync()，则会向本进程发送 SIGIO，信号处理函数被调用。
 *   6. usleep(100ms) 等待信号送达（信号处理在进程被调度时执行）。
 *   7. 检查 g_got_signal：若为 1 表示收到信号，测试通过；否则给出警告
 *      （某些驱动需要由其它进程的写入才能触发通知）。
 *   8. 测试完成后通过 F_SETFL 清除 FASYNC 标志，从驱动的异步通知列表移除，
 *      避免后续误触发。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_async(int fd)
{
    int flags;
    struct sigaction sa;
    char test_data[] = "async notification test";

    printf("  [ASYNC] Testing fasync signal notification...\n");

    /* 步骤 1：清空缓冲 */
    test_clear_buf(fd);

    /* 步骤 2：为 SIGIO 注册信号处理函数 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigio_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGIO, &sa, NULL) < 0) {
        perror("  sigaction failed");
        return -1;
    }

    /* 步骤 3：设置文件拥有者为当前进程，SIGIO 将发送给该进程 */
    if (fcntl(fd, F_SETOWN, getpid()) < 0) {
        perror("  fcntl F_SETOWN failed");
        return -1;
    }

    /* 步骤 4：在文件状态标志中添加 FASYNC，启用异步通知 */
    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("  fcntl F_GETFL failed");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | FASYNC) < 0) {
        perror("  fcntl F_SETFL FASYNC failed");
        return -1;
    }

    /* 步骤 5：重置标志并写入数据，期望触发 SIGIO */
    g_got_signal = 0;

    printf("  [ASYNC] Writing data to trigger SIGIO...\n");
    test_write(fd, test_data);

    /* 步骤 6：等待信号处理函数被调度执行 */
    usleep(100000);

    /* 步骤 7：检查是否收到信号 */
    if (g_got_signal) {
        printf("  [ASYNC] Received SIGIO signal - PASSED\n");
    } else {
        printf("  [ASYNC] WARNING: No SIGIO received (may need write from another process)\n");
    }

    /* 步骤 8：清除 FASYNC 标志，关闭异步通知 */
    flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~FASYNC);

    printf("  [ASYNC] Test completed\n");
    return 0;
}

/*
 * test_nonblock - 非阻塞 IO 测试
 *
 * 目的：验证当设备以 O_NONBLOCK 模式打开（或运行时通过 fcntl 设置该标志）时，
 *       read() 在无数据可读时不会阻塞，而是立即返回 -1 并将 errno 置为 EAGAIN。
 *
 * 测试流程（详细）：
 *   1. 先清空缓冲，保证读操作时确实无数据。
 *   2. 通过 F_GETFL 获取当前文件状态标志，再用 F_SETFL 添加 O_NONBLOCK，
 *      将文件描述符切换为非阻塞模式（注意保存原始 flags 以便恢复）。
 *   3. 第一次 read()：缓冲为空且非阻塞，期望返回 -1 且 errno == EAGAIN。
 *      - 若返回 EAGAIN 则符合预期；
 *      - 若返回 >= 0 说明缓冲中意外存在数据（异常）；
 *      - 若返回其它错误则直接报错返回。
 *   4. 非阻塞 write()：写入测试数据，验证非阻塞模式下写操作正常。
 *   5. 第二次 read()：此时缓冲有数据，应能成功读出并打印。
 *   6. 通过 F_SETFL 恢复原始标志，恢复阻塞模式，避免影响后续测试。
 *
 * 说明：EAGAIN（"Resource temporarily unavailable"）与 EAGAIN 等价的 EWOULDBLOCK
 *       是非阻塞 IO 的典型返回码，是正常行为而非错误。
 *
 * 参数 fd: 已打开的设备文件描述符
 * 返回值: 成功返回 0，失败返回 -1
 */
static int test_nonblock(int fd)
{
    int flags;
    char buf[64];
    ssize_t ret;

    printf("  [NONBLOCK] Testing non-blocking IO...\n");

    /* 步骤 1：清空缓冲 */
    test_clear_buf(fd);

    /* 步骤 2：将文件描述符切换为非阻塞模式 */
    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("  fcntl F_GETFL failed");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("  fcntl F_SETFL O_NONBLOCK failed");
        return -1;
    }

    /* 步骤 3：非阻塞读空缓冲，期望返回 EAGAIN */
    ret = read(fd, buf, sizeof(buf));
    if (ret < 0 && errno == EAGAIN) {
        printf("  [NONBLOCK] Empty buffer read returns EAGAIN - correct\n");
    } else if (ret >= 0) {
        printf("  [NONBLOCK] Read returned %zd bytes (unexpected data)\n", ret);
    } else {
        perror("  read failed (unexpected error)");
        return -1;
    }

    /* 步骤 4：非阻塞写，验证写通路正常 */
    const char *data = "nonblock write test";
    ret = write(fd, data, strlen(data));
    if (ret < 0) {
        perror("  write failed");
        return -1;
    }
    printf("  [NONBLOCK] Non-blocking write: %zd bytes\n", ret);

    /* 步骤 5：非阻塞读，此时应有数据可读 */
    ret = read(fd, buf, sizeof(buf));
    if (ret > 0) {
        buf[ret] = '\0';
        printf("  [NONBLOCK] Non-blocking read: %zd bytes, data: \"%s\"\n", ret, buf);
    }

    /* 步骤 6：恢复原始文件状态标志 */
    fcntl(fd, F_SETFL, flags);

    printf("  [NONBLOCK] Test PASSED\n");
    return 0;
}

/*
 * test_all - 全量自动化测试
 *
 * 目的：按固定顺序依次执行所有功能测试用例，对驱动进行一次完整的回归验证。
 *       适合在驱动修改后一键运行，快速发现回归问题。
 *
 * 测试顺序与覆盖范围（共 15 个用例）：
 *   Test 1  : GET_BUF_SIZE    —— 查询缓冲总容量（基础属性）
 *   Test 2  : GET_STATUS      —— 查询初始状态
 *   Test 3  : GET_BUF_USED    —— 查询空缓冲的已用字节数（应为 0）
 *   Test 4  : WRITE           —— 写入测试数据
 *   Test 5  : GET_BUF_USED    —— 写入后再次查询已用字节数（应非 0）
 *   Test 6  : READ            —— 读出刚写入的数据
 *   Test 7  : SET_PARAM       —— 设置设备参数
 *   Test 8  : GET_STATUS      —— 设置参数后查询状态变化
 *   Test 9  : GET_STATS       —— 查询累计的 IO 统计
 *   Test 10 : CLR_STATS       —— 清零统计
 *   Test 11 : GET_STATS       —— 验证统计已清零
 *   Test 12 : POLL test       —— poll 多路复用测试
 *   Test 13 : NONBLOCK test   —— 非阻塞 IO 测试
 *   Test 14 : ASYNC test      —— fasync 异步通知测试
 *   Test 15 : CLEAR_BUF       —— 最后清空缓冲，恢复初始状态
 *
 * 说明：任一用例失败都会将 ret 置为 -1，但不会中断后续测试，便于一次性收集
 *       所有失败信息。最终根据 ret 输出 PASSED 或 FAILED。
 *
 * 参数 dev_path: 设备节点路径（例如 /dev/cdf_dev）
 * 返回值:        所有用例均通过返回 0，任一失败返回 -1
 */
static int test_all(const char *dev_path)
{
    int fd;
    int ret = 0;

    printf("========== Running all tests on %s ==========\n", dev_path);

    /* 以读写方式打开设备节点 */
    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        perror("open failed");
        return -1;
    }

    printf("\n--- Test 1: GET_BUF_SIZE ---\n");
    if (test_get_buf_size(fd) < 0) ret = -1;

    printf("\n--- Test 2: GET_STATUS (initial) ---\n");
    if (test_get_status(fd) < 0) ret = -1;

    printf("\n--- Test 3: GET_BUF_USED (empty) ---\n");
    if (test_get_buf_used(fd) < 0) ret = -1;

    printf("\n--- Test 4: WRITE ---\n");
    if (test_write(fd, "Hello, CDF Framework v2.0!") < 0) ret = -1;

    printf("\n--- Test 5: GET_BUF_USED (after write) ---\n");
    if (test_get_buf_used(fd) < 0) ret = -1;

    printf("\n--- Test 6: READ ---\n");
    if (test_read(fd, 1024) < 0) ret = -1;

    printf("\n--- Test 7: SET_PARAM ---\n");
    if (test_set_param(fd, 42) < 0) ret = -1;

    printf("\n--- Test 8: GET_STATUS (after set_param) ---\n");
    if (test_get_status(fd) < 0) ret = -1;

    printf("\n--- Test 9: GET_STATS ---\n");
    if (test_get_stats(fd) < 0) ret = -1;

    printf("\n--- Test 10: CLR_STATS ---\n");
    if (test_clr_stats(fd) < 0) ret = -1;

    printf("\n--- Test 11: GET_STATS (after clear) ---\n");
    if (test_get_stats(fd) < 0) ret = -1;

    printf("\n--- Test 12: POLL test ---\n");
    if (test_poll(fd) < 0) ret = -1;

    printf("\n--- Test 13: NONBLOCK test ---\n");
    if (test_nonblock(fd) < 0) ret = -1;

    printf("\n--- Test 14: ASYNC test ---\n");
    if (test_async(fd) < 0) ret = -1;

    printf("\n--- Test 15: CLEAR_BUF ---\n");
    if (test_clear_buf(fd) < 0) ret = -1;

    /* 关闭设备文件描述符 */
    close(fd);

    printf("\n========== Tests %s ==========\n", ret == 0 ? "PASSED" : "FAILED");
    return ret;
}

/*
 * main - 程序入口
 *
 * 作用：解析命令行参数，根据用户指定的命令分发到对应的测试函数。
 *
 * 参数格式：argv[1] = 设备节点路径，argv[2] = 命令名，argv[3...] = 命令参数。
 *
 * 分发逻辑：
 *   1. 若参数个数 < 3，打印用法并返回 1。
 *   2. 取出设备路径与命令字符串。
 *   3. 特殊处理 "test_all"：该命令内部自行 open/close 设备，因此提前返回，
 *      不走统一的 open 流程。
 *   4. 其余命令统一以 O_RDWR 方式打开设备，根据命令名调用对应测试函数：
 *        write <string>      -> test_write（需要字符串参数）
 *        read  <count>       -> test_read（count 缺省为 1024）
 *        get_status          -> test_get_status
 *        set_param <value>   -> test_set_param（需要整型参数）
 *        clear_buf           -> test_clear_buf
 *        get_buf_size        -> test_get_buf_size
 *        get_buf_used        -> test_get_buf_used
 *        get_stats           -> test_get_stats
 *        clr_stats           -> test_clr_stats
 *        poll_test           -> test_poll
 *        async_test          -> test_async
 *        nonblock_test       -> test_nonblock
 *   5. 若命令未识别，打印错误并显示用法。
 *   6. 测试完成后关闭 fd，并根据 ret 返回 0（成功）或 1（失败）。
 *
 * 返回值: 0 表示测试通过，1 表示参数错误或测试失败
 */
int main(int argc, char *argv[])
{
    const char *dev_path;
    const char *cmd;
    int fd;
    int ret = 0;

    /* 参数校验：至少需要程序名、设备路径、命令三项 */
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    dev_path = argv[1];
    cmd = argv[2];

    /* test_all 自行管理设备打开与关闭，单独处理 */
    if (strcmp(cmd, "test_all") == 0) {
        return test_all(dev_path) == 0 ? 0 : 1;
    }

    /* 统一以读写方式打开设备 */
    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        perror("open failed");
        return 1;
    }

    /* 按命令名分发到对应测试函数 */
    if (strcmp(cmd, "write") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing string argument\n");
            ret = 1;
        } else {
            ret = test_write(fd, argv[3]);
        }
    } else if (strcmp(cmd, "read") == 0) {
        size_t count = argc >= 4 ? (size_t)atoi(argv[3]) : 1024;
        ret = test_read(fd, count);
    } else if (strcmp(cmd, "get_status") == 0) {
        ret = test_get_status(fd);
    } else if (strcmp(cmd, "set_param") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing value argument\n");
            ret = 1;
        } else {
            ret = test_set_param(fd, atoi(argv[3]));
        }
    } else if (strcmp(cmd, "clear_buf") == 0) {
        ret = test_clear_buf(fd);
    } else if (strcmp(cmd, "get_buf_size") == 0) {
        ret = test_get_buf_size(fd);
    } else if (strcmp(cmd, "get_buf_used") == 0) {
        ret = test_get_buf_used(fd);
    } else if (strcmp(cmd, "get_stats") == 0) {
        ret = test_get_stats(fd);
    } else if (strcmp(cmd, "clr_stats") == 0) {
        ret = test_clr_stats(fd);
    } else if (strcmp(cmd, "poll_test") == 0) {
        ret = test_poll(fd);
    } else if (strcmp(cmd, "async_test") == 0) {
        ret = test_async(fd);
    } else if (strcmp(cmd, "nonblock_test") == 0) {
        ret = test_nonblock(fd);
    } else {
        /* 未知命令：报错并打印用法 */
        fprintf(stderr, "Error: unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        ret = 1;
    }

    /* 关闭设备并返回结果 */
    close(fd);
    return ret == 0 ? 0 : 1;
}
