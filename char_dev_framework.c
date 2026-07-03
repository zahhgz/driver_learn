#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/atomic.h>

#include "char_dev_framework.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CDF Developer");
MODULE_DESCRIPTION("Linux Character Device Driver Framework - Enhanced Edition");
MODULE_VERSION("2.0.0");

static int dev_num = CDF_DEV_NUM_DEF;
module_param(dev_num, int, 0444);
MODULE_PARM_DESC(dev_num, "Number of device instances (default: 3)");

static int buf_size = CDF_BUF_SIZE_DEF;
module_param(buf_size, int, 0444);
MODULE_PARM_DESC(buf_size, "Buffer size per device in bytes (default: 4096)");

struct cdf_dev {
    struct cdev cdev;
    struct device *dev;
    struct kfifo fifo;
    unsigned char *fifo_buf;
    int dev_id;
    int status;
    int param;
    struct mutex lock;
    wait_queue_head_t read_queue;
    wait_queue_head_t write_queue;
    struct fasync_struct *async_queue;
    struct cdf_io_stats stats;
    atomic_t open_count;
};

static dev_t g_devt;
static struct class *g_class;
static struct cdf_dev *g_devices;
static int g_dev_count;
static struct proc_dir_entry *g_proc_dir;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define CDF_CLASS_CREATE(name)  class_create(name)
#else
#define CDF_CLASS_CREATE(name)  class_create(THIS_MODULE, name)
#endif

static void cdf_notify_readers(struct cdf_dev *dev)
{
    if (kfifo_len(&dev->fifo) > 0) {
        wake_up_interruptible(&dev->read_queue);
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
    }
}

static void cdf_notify_writers(struct cdf_dev *dev)
{
    if (kfifo_avail(&dev->fifo) > 0) {
        wake_up_interruptible(&dev->write_queue);
        kill_fasync(&dev->async_queue, SIGIO, POLL_OUT);
    }
}

static int cdf_open(struct inode *inode, struct file *filp)
{
    struct cdf_dev *dev = container_of(inode->i_cdev, struct cdf_dev, cdev);
    filp->private_data = dev;
    atomic_inc(&dev->open_count);
    return 0;
}

static int cdf_release(struct inode *inode, struct file *filp)
{
    struct cdf_dev *dev = filp->private_data;
    fasync_helper(-1, filp, 0, &dev->async_queue);
    atomic_dec(&dev->open_count);
    return 0;
}

static ssize_t cdf_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct cdf_dev *dev = filp->private_data;
    unsigned int copied;
    int ret;
    bool nonblock = filp->f_flags & O_NONBLOCK;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    while (kfifo_is_empty(&dev->fifo)) {
        mutex_unlock(&dev->lock);
        if (nonblock)
            return -EAGAIN;
        if (wait_event_interruptible(dev->read_queue, kfifo_len(&dev->fifo) > 0))
            return -ERESTARTSYS;
        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;
    }

    ret = kfifo_to_user(&dev->fifo, buf, count, &copied);
    if (ret) {
        dev->stats.err_count++;
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    dev->stats.read_count++;
    dev->stats.read_bytes += copied;

    mutex_unlock(&dev->lock);
    cdf_notify_writers(dev);
    return copied;
}

static ssize_t cdf_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct cdf_dev *dev = filp->private_data;
    unsigned int copied;
    int ret;
    bool nonblock = filp->f_flags & O_NONBLOCK;
    size_t total_copied = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    while (total_copied < count) {
        size_t avail = kfifo_avail(&dev->fifo);
        if (avail == 0) {
            mutex_unlock(&dev->lock);
            if (nonblock) {
                if (total_copied > 0)
                    break;
                return -EAGAIN;
            }
            if (wait_event_interruptible(dev->write_queue, kfifo_avail(&dev->fifo) > 0))
                return -ERESTARTSYS;
            if (mutex_lock_interruptible(&dev->lock))
                return -ERESTARTSYS;
            continue;
        }

        ret = kfifo_from_user(&dev->fifo, buf + total_copied,
                              count - total_copied, &copied);
        if (ret) {
            dev->stats.err_count++;
            mutex_unlock(&dev->lock);
            return -EFAULT;
        }

        total_copied += copied;
        if (copied == 0)
            break;
    }

    dev->stats.write_count++;
    dev->stats.write_bytes += total_copied;
    dev->status = 1;

    mutex_unlock(&dev->lock);
    cdf_notify_readers(dev);
    return total_copied;
}

static unsigned int cdf_poll(struct file *filp, poll_table *wait)
{
    struct cdf_dev *dev = filp->private_data;
    unsigned int mask = 0;

    poll_wait(filp, &dev->read_queue, wait);
    poll_wait(filp, &dev->write_queue, wait);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (kfifo_len(&dev->fifo) > 0)
        mask |= POLLIN | POLLRDNORM;
    if (kfifo_avail(&dev->fifo) > 0)
        mask |= POLLOUT | POLLWRNORM;

    mutex_unlock(&dev->lock);
    return mask;
}

static int cdf_fasync(int fd, struct file *filp, int on)
{
    struct cdf_dev *dev = filp->private_data;
    return fasync_helper(fd, filp, on, &dev->async_queue);
}

static long cdf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct cdf_dev *dev = filp->private_data;
    int val;
    struct cdf_io_stats kstats;
    long ret = 0;

    if (_IOC_TYPE(cmd) != CDF_MAGIC)
        return -ENOTTY;
    if (_IOC_NR(cmd) >= CDF_CMD_MAX)
        return -ENOTTY;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    switch (cmd) {
    case CDF_CMD_GET_STATUS:
        val = dev->status;
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            ret = -EFAULT;
        break;

    case CDF_CMD_SET_PARAM:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int))) {
            ret = -EFAULT;
            break;
        }
        dev->param = val;
        dev->status = 1;
        break;

    case CDF_CMD_CLEAR_BUF:
        kfifo_reset(&dev->fifo);
        dev->status = 0;
        break;

    case CDF_CMD_GET_BUF_SIZE:
        val = kfifo_size(&dev->fifo);
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            ret = -EFAULT;
        break;

    case CDF_CMD_GET_STATS:
        kstats = dev->stats;
        if (copy_to_user((struct cdf_io_stats __user *)arg, &kstats, sizeof(kstats)))
            ret = -EFAULT;
        break;

    case CDF_CMD_CLR_STATS:
        memset(&dev->stats, 0, sizeof(dev->stats));
        break;

    case CDF_CMD_SET_NONBLOCK:
        if (copy_from_user(&val, (int __user *)arg, sizeof(int))) {
            ret = -EFAULT;
            break;
        }
        if (val)
            filp->f_flags |= O_NONBLOCK;
        else
            filp->f_flags &= ~O_NONBLOCK;
        break;

    case CDF_CMD_TRIGGER_ASYNC:
        mutex_unlock(&dev->lock);
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
        return 0;

    case CDF_CMD_GET_BUF_USED:
        val = kfifo_len(&dev->fifo);
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            ret = -EFAULT;
        break;

    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&dev->lock);
    return ret;
}

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

static int cdf_proc_show(struct seq_file *s, void *v)
{
    int i;

    seq_puts(s, "=== CDF Driver Statistics ===\n");
    seq_printf(s, "Device count: %d\n", g_dev_count);
    seq_printf(s, "Buffer size per device: %d bytes\n", buf_size);
    seq_puts(s, "\n");

    for (i = 0; i < g_dev_count; i++) {
        struct cdf_dev *dev = &g_devices[i];
        struct cdf_io_stats stats;
        int fifo_len, fifo_avail, open_cnt;

        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;

        stats = dev->stats;
        fifo_len = kfifo_len(&dev->fifo);
        fifo_avail = kfifo_avail(&dev->fifo);
        open_cnt = atomic_read(&dev->open_count);

        mutex_unlock(&dev->lock);

        seq_printf(s, "--- Device %d (%s%d) ---\n", i, CDF_DEV_NAME, i);
        seq_printf(s, "  status:      %d\n", dev->status);
        seq_printf(s, "  param:       %d\n", dev->param);
        seq_printf(s, "  open_count:  %d\n", open_cnt);
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

static int cdf_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, cdf_proc_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops cdf_proc_fops = {
    .proc_open    = cdf_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations cdf_proc_fops = {
    .owner   = THIS_MODULE,
    .open    = cdf_proc_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};
#endif

static void cdf_cleanup_devices(int from)
{
    int i;

    for (i = from - 1; i >= 0; i--) {
        if (g_devices[i].dev)
            device_destroy(g_class, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i));
        cdev_del(&g_devices[i].cdev);
        if (g_devices[i].fifo_buf) {
            kfifo_free(&g_devices[i].fifo);
            kfree(g_devices[i].fifo_buf);
        }
        mutex_destroy(&g_devices[i].lock);
    }

    kfree(g_devices);
    g_devices = NULL;

    if (g_class) {
        class_destroy(g_class);
        g_class = NULL;
    }

    if (g_devt) {
        unregister_chrdev_region(g_devt, g_dev_count);
        g_devt = 0;
    }
}

static int __init cdf_init(void)
{
    int ret, i;

    if (dev_num <= 0 || dev_num > 32) {
        pr_err("%s: invalid dev_num=%d, must be 1-32\n", __func__, dev_num);
        return -EINVAL;
    }
    if (buf_size < 256 || buf_size > (1 << 20)) {
        pr_err("%s: invalid buf_size=%d, must be 256-1MB\n", __func__, buf_size);
        return -EINVAL;
    }

    buf_size = roundup_pow_of_two(buf_size);
    g_dev_count = dev_num;

    ret = alloc_chrdev_region(&g_devt, 0, g_dev_count, CDF_DEV_NAME);
    if (ret < 0) {
        pr_err("%s: alloc_chrdev_region failed, ret=%d\n", __func__, ret);
        return ret;
    }

    g_class = CDF_CLASS_CREATE(CDF_CLASS_NAME);
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("%s: class_create failed, ret=%d\n", __func__, ret);
        g_class = NULL;
        goto err_class;
    }

    g_devices = kcalloc(g_dev_count, sizeof(struct cdf_dev), GFP_KERNEL);
    if (!g_devices) {
        ret = -ENOMEM;
        goto err_alloc;
    }

    for (i = 0; i < g_dev_count; i++) {
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

        g_devices[i].fifo_buf = kmalloc(buf_size, GFP_KERNEL);
        if (!g_devices[i].fifo_buf) {
            ret = -ENOMEM;
            goto err_dev_init;
        }

        ret = kfifo_init(&g_devices[i].fifo, g_devices[i].fifo_buf, buf_size);
        if (ret) {
            pr_err("%s: kfifo_init failed for dev %d\n", __func__, i);
            kfree(g_devices[i].fifo_buf);
            g_devices[i].fifo_buf = NULL;
            goto err_dev_init;
        }

        cdev_init(&g_devices[i].cdev, &cdf_fops);
        g_devices[i].cdev.owner = THIS_MODULE;

        ret = cdev_add(&g_devices[i].cdev, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i), 1);
        if (ret < 0) {
            pr_err("%s: cdev_add failed for dev %d, ret=%d\n", __func__, i, ret);
            goto err_dev_init;
        }

        g_devices[i].dev = device_create(g_class, NULL,
                                          MKDEV(MAJOR(g_devt), MINOR(g_devt) + i),
                                          NULL, "%s%d", CDF_DEV_NAME, i);
        if (IS_ERR(g_devices[i].dev)) {
            ret = PTR_ERR(g_devices[i].dev);
            pr_err("%s: device_create failed for dev %d, ret=%d\n", __func__, i, ret);
            g_devices[i].dev = NULL;
            cdev_del(&g_devices[i].cdev);
            goto err_dev_init;
        }
    }

    g_proc_dir = proc_create(CDF_PROC_NAME, 0444, NULL, &cdf_proc_fops);
    if (!g_proc_dir) {
        pr_warn("%s: proc_create failed, continuing without proc interface\n", __func__);
    }

    pr_info("%s: cdf driver loaded, major=%d, devices=%d, buf_size=%d\n",
            __func__, MAJOR(g_devt), g_dev_count, buf_size);
    return 0;

err_dev_init:
    cdf_cleanup_devices(i);
    return ret;

err_alloc:
    class_destroy(g_class);
    g_class = NULL;
err_class:
    unregister_chrdev_region(g_devt, g_dev_count);
    g_devt = 0;
    return ret;
}

static void __exit cdf_exit(void)
{
    int i;

    if (g_proc_dir)
        proc_remove(g_proc_dir);

    for (i = g_dev_count - 1; i >= 0; i--) {
        if (g_devices[i].dev)
            device_destroy(g_class, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i));
        cdev_del(&g_devices[i].cdev);
        fasync_helper(-1, NULL, 0, &g_devices[i].async_queue);
        if (g_devices[i].fifo_buf) {
            kfifo_free(&g_devices[i].fifo);
            kfree(g_devices[i].fifo_buf);
        }
        mutex_destroy(&g_devices[i].lock);
    }

    kfree(g_devices);
    class_destroy(g_class);
    unregister_chrdev_region(g_devt, g_dev_count);

    pr_info("%s: cdf driver unloaded\n", __func__);
}

module_init(cdf_init);
module_exit(cdf_exit);
