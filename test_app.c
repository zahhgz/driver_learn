#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "char_dev_framework.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s <device> <command> [args...]\n", prog);
    printf("Commands:\n");
    printf("  write <string>        Write string to device\n");
    printf("  read <count>          Read count bytes from device\n");
    printf("  get_status            Get device status\n");
    printf("  set_param <value>     Set device parameter\n");
    printf("  clear_buf             Clear device buffer\n");
    printf("  get_buf_size          Get buffer size\n");
    printf("  test_all              Run all tests\n");
}

static int test_write(int fd, const char *data)
{
    ssize_t ret;

    printf("[WRITE] Writing: \"%s\"\n", data);
    ret = write(fd, data, strlen(data));
    if (ret < 0) {
        perror("write failed");
        return -1;
    }
    printf("[WRITE] Written %zd bytes\n", ret);
    return 0;
}

static int test_read(int fd, size_t count)
{
    char *buf;
    ssize_t ret;

    buf = malloc(count + 1);
    if (!buf) {
        perror("malloc failed");
        return -1;
    }
    memset(buf, 0, count + 1);

    lseek(fd, 0, SEEK_SET);
    ret = read(fd, buf, count);
    if (ret < 0) {
        perror("read failed");
        free(buf);
        return -1;
    }

    printf("[READ] Read %zd bytes: \"%s\"\n", ret, buf);
    free(buf);
    return 0;
}

static int test_get_status(int fd)
{
    int status;
    int ret;

    ret = ioctl(fd, CDF_CMD_GET_STATUS, &status);
    if (ret < 0) {
        perror("ioctl GET_STATUS failed");
        return -1;
    }
    printf("[IOCTL GET_STATUS] status = %d\n", status);
    return 0;
}

static int test_set_param(int fd, int val)
{
    int param = val;
    int ret;

    ret = ioctl(fd, CDF_CMD_SET_PARAM, &param);
    if (ret < 0) {
        perror("ioctl SET_PARAM failed");
        return -1;
    }
    printf("[IOCTL SET_PARAM] param = %d\n", val);
    return 0;
}

static int test_clear_buf(int fd)
{
    int ret;

    ret = ioctl(fd, CDF_CMD_CLEAR_BUF);
    if (ret < 0) {
        perror("ioctl CLEAR_BUF failed");
        return -1;
    }
    printf("[IOCTL CLEAR_BUF] buffer cleared\n");
    return 0;
}

static int test_get_buf_size(int fd)
{
    int size;
    int ret;

    ret = ioctl(fd, CDF_CMD_GET_BUF_SIZE, &size);
    if (ret < 0) {
        perror("ioctl GET_BUF_SIZE failed");
        return -1;
    }
    printf("[IOCTL GET_BUF_SIZE] buf_size = %d\n", size);
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

    printf("\n--- Test 3: WRITE ---\n");
    if (test_write(fd, "Hello, CDF Framework!") < 0) ret = -1;

    printf("\n--- Test 4: READ ---\n");
    if (test_read(fd, 1024) < 0) ret = -1;

    printf("\n--- Test 5: SET_PARAM ---\n");
    if (test_set_param(fd, 42) < 0) ret = -1;

    printf("\n--- Test 6: GET_STATUS (after set_param) ---\n");
    if (test_get_status(fd) < 0) ret = -1;

    printf("\n--- Test 7: CLEAR_BUF ---\n");
    if (test_clear_buf(fd) < 0) ret = -1;

    printf("\n--- Test 8: READ (after clear) ---\n");
    if (test_read(fd, 1024) < 0) ret = -1;

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
    } else {
        fprintf(stderr, "Error: unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        ret = 1;
    }

    close(fd);
    return ret == 0 ? 0 : 1;
}
