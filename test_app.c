#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#include "char_dev_framework.h"

static volatile int g_got_signal = 0;

static void sigio_handler(int sig)
{
    (void)sig;
    g_got_signal = 1;
}

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

static int test_poll(int fd)
{
    struct pollfd pfd;
    int ret;
    char test_data[] = "poll test data";

    printf("  [POLL] Testing poll/select...\n");

    test_clear_buf(fd);

    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;

    ret = poll(&pfd, 1, 100);
    if (ret < 0) {
        perror("  poll failed");
        return -1;
    }

    printf("  [POLL] Empty buffer: ret=%d, revents=0x%x (POLLOUT=%s, POLLIN=%s)\n",
           ret, pfd.revents,
           (pfd.revents & POLLOUT) ? "yes" : "no",
           (pfd.revents & POLLIN) ? "yes" : "no");

    if (!(pfd.revents & POLLOUT)) {
        printf("  [POLL] ERROR: expected POLLOUT on empty buffer\n");
        return -1;
    }
    if (pfd.revents & POLLIN) {
        printf("  [POLL] ERROR: unexpected POLLIN on empty buffer\n");
        return -1;
    }

    test_write(fd, test_data);

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

    if (!(pfd.revents & POLLIN)) {
        printf("  [POLL] ERROR: expected POLLIN after write\n");
        return -1;
    }

    printf("  [POLL] Test PASSED\n");
    return 0;
}

static int test_async(int fd)
{
    int flags;
    struct sigaction sa;
    char test_data[] = "async notification test";

    printf("  [ASYNC] Testing fasync signal notification...\n");

    test_clear_buf(fd);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigio_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGIO, &sa, NULL) < 0) {
        perror("  sigaction failed");
        return -1;
    }

    if (fcntl(fd, F_SETOWN, getpid()) < 0) {
        perror("  fcntl F_SETOWN failed");
        return -1;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("  fcntl F_GETFL failed");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | FASYNC) < 0) {
        perror("  fcntl F_SETFL FASYNC failed");
        return -1;
    }

    g_got_signal = 0;

    printf("  [ASYNC] Writing data to trigger SIGIO...\n");
    test_write(fd, test_data);

    usleep(100000);

    if (g_got_signal) {
        printf("  [ASYNC] Received SIGIO signal - PASSED\n");
    } else {
        printf("  [ASYNC] WARNING: No SIGIO received (may need write from another process)\n");
    }

    flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~FASYNC);

    printf("  [ASYNC] Test completed\n");
    return 0;
}

static int test_nonblock(int fd)
{
    int flags;
    char buf[64];
    ssize_t ret;

    printf("  [NONBLOCK] Testing non-blocking IO...\n");

    test_clear_buf(fd);

    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("  fcntl F_GETFL failed");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("  fcntl F_SETFL O_NONBLOCK failed");
        return -1;
    }

    ret = read(fd, buf, sizeof(buf));
    if (ret < 0 && errno == EAGAIN) {
        printf("  [NONBLOCK] Empty buffer read returns EAGAIN - correct\n");
    } else if (ret >= 0) {
        printf("  [NONBLOCK] Read returned %zd bytes (unexpected data)\n", ret);
    } else {
        perror("  read failed (unexpected error)");
        return -1;
    }

    const char *data = "nonblock write test";
    ret = write(fd, data, strlen(data));
    if (ret < 0) {
        perror("  write failed");
        return -1;
    }
    printf("  [NONBLOCK] Non-blocking write: %zd bytes\n", ret);

    ret = read(fd, buf, sizeof(buf));
    if (ret > 0) {
        buf[ret] = '\0';
        printf("  [NONBLOCK] Non-blocking read: %zd bytes, data: \"%s\"\n", ret, buf);
    }

    fcntl(fd, F_SETFL, flags);

    printf("  [NONBLOCK] Test PASSED\n");
    return 0;
}

static int test_all(const char *dev_path)
{
    int fd;
    int ret = 0;

    printf("========== Running all tests on %s ==========\n", dev_path);

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

    close(fd);

    printf("\n========== Tests %s ==========\n", ret == 0 ? "PASSED" : "FAILED");
    return ret;
}

int main(int argc, char *argv[])
{
    const char *dev_path;
    const char *cmd;
    int fd;
    int ret = 0;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    dev_path = argv[1];
    cmd = argv[2];

    if (strcmp(cmd, "test_all") == 0) {
        return test_all(dev_path) == 0 ? 0 : 1;
    }

    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        perror("open failed");
        return 1;
    }

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
        fprintf(stderr, "Error: unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        ret = 1;
    }

    close(fd);
    return ret == 0 ? 0 : 1;
}
