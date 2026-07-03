#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/errno.h>

#include "char_dev_framework.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CDF Developer");
MODULE_DESCRIPTION("Linux Character Device Driver Framework");
MODULE_VERSION("1.0.0");

struct cdf_dev {
    struct cdev cdev;
    struct device *dev;
    char buf[CDF_BUF_SIZE];
    size_t buf_len;
    int status;
    int param;
    struct mutex lock;
};

static dev_t g_devt;
static struct class *g_class;
static struct cdf_dev g_devices[CDF_DEV_NUM];

static int cdf_open(struct inode *inode, struct file *filp)
{
    struct cdf_dev *dev = container_of(inode->i_cdev, struct cdf_dev, cdev);
    filp->private_data = dev;
    return 0;
}

static int cdf_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t cdf_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct cdf_dev *dev = filp->private_data;
    ssize_t ret = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (*f_pos >= dev->buf_len)
        goto out;

    if (*f_pos + count > dev->buf_len)
        count = dev->buf_len - *f_pos;

    if (copy_to_user(buf, dev->buf + *f_pos, count)) {
        ret = -EFAULT;
        goto out;
    }

    *f_pos += count;
    ret = count;

out:
    mutex_unlock(&dev->lock);
    return ret;
}

static ssize_t cdf_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct cdf_dev *dev = filp->private_data;
    ssize_t ret = 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (*f_pos >= CDF_BUF_SIZE) {
        ret = -ENOSPC;
        goto out;
    }

    if (*f_pos + count > CDF_BUF_SIZE)
        count = CDF_BUF_SIZE - *f_pos;

    if (copy_from_user(dev->buf + *f_pos, buf, count)) {
        ret = -EFAULT;
        goto out;
    }

    *f_pos += count;
    if (*f_pos > dev->buf_len)
        dev->buf_len = *f_pos;
    ret = count;

out:
    mutex_unlock(&dev->lock);
    return ret;
}

static loff_t cdf_llseek(struct file *filp, loff_t off, int whence)
{
    struct cdf_dev *dev = filp->private_data;
    loff_t new_pos;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    switch (whence) {
    case 0:
        new_pos = off;
        break;
    case 1:
        new_pos = filp->f_pos + off;
        break;
    case 2:
        new_pos = dev->buf_len + off;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    if (new_pos < 0 || new_pos > CDF_BUF_SIZE) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = new_pos;
    mutex_unlock(&dev->lock);
    return new_pos;
}

static long cdf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct cdf_dev *dev = filp->private_data;
    int val;
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
        memset(dev->buf, 0, CDF_BUF_SIZE);
        dev->buf_len = 0;
        filp->f_pos = 0;
        break;

    case CDF_CMD_GET_BUF_SIZE:
        val = CDF_BUF_SIZE;
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
    .llseek         = cdf_llseek,
    .unlocked_ioctl = cdf_ioctl,
    .compat_ioctl   = cdf_ioctl,
};

static void cdf_cleanup_devices(int from)
{
    int i;

    for (i = from - 1; i >= 0; i--) {
        if (g_devices[i].dev)
            device_destroy(g_class, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i));
        cdev_del(&g_devices[i].cdev);
        mutex_destroy(&g_devices[i].lock);
    }

    if (g_class)
        class_destroy(g_class);

    if (g_devt)
        unregister_chrdev_region(g_devt, CDF_DEV_NUM);
}

static int __init cdf_init(void)
{
    int ret, i;

    ret = alloc_chrdev_region(&g_devt, 0, CDF_DEV_NUM, CDF_DEV_NAME);
    if (ret < 0) {
        pr_err("%s: alloc_chrdev_region failed, ret=%d\n", __func__, ret);
        return ret;
    }

    g_class = class_create(CDF_CLASS_NAME);
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("%s: class_create failed, ret=%d\n", __func__, ret);
        g_class = NULL;
        goto err_class;
    }

    for (i = 0; i < CDF_DEV_NUM; i++) {
        mutex_init(&g_devices[i].lock);
        g_devices[i].buf_len = 0;
        g_devices[i].status = 0;
        g_devices[i].param = 0;
        g_devices[i].dev = NULL;

        cdev_init(&g_devices[i].cdev, &cdf_fops);
        g_devices[i].cdev.owner = THIS_MODULE;

        ret = cdev_add(&g_devices[i].cdev, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i), 1);
        if (ret < 0) {
            pr_err("%s: cdev_add failed for dev %d, ret=%d\n", __func__, i, ret);
            goto err_cdev_add;
        }

        g_devices[i].dev = device_create(g_class, NULL,
                                          MKDEV(MAJOR(g_devt), MINOR(g_devt) + i),
                                          NULL, "%s%d", CDF_DEV_NAME, i);
        if (IS_ERR(g_devices[i].dev)) {
            ret = PTR_ERR(g_devices[i].dev);
            pr_err("%s: device_create failed for dev %d, ret=%d\n", __func__, i, ret);
            g_devices[i].dev = NULL;
            cdev_del(&g_devices[i].cdev);
            mutex_destroy(&g_devices[i].lock);
            goto err_cdev_add;
        }
    }

    pr_info("%s: cdf driver loaded, major=%d, devices=%d\n",
            __func__, MAJOR(g_devt), CDF_DEV_NUM);
    return 0;

err_cdev_add:
    cdf_cleanup_devices(i);
    return ret;

err_class:
    unregister_chrdev_region(g_devt, CDF_DEV_NUM);
    g_devt = 0;
    return ret;
}

static void __exit cdf_exit(void)
{
    int i;

    for (i = CDF_DEV_NUM - 1; i >= 0; i--) {
        if (g_devices[i].dev)
            device_destroy(g_class, MKDEV(MAJOR(g_devt), MINOR(g_devt) + i));
        cdev_del(&g_devices[i].cdev);
        mutex_destroy(&g_devices[i].lock);
    }

    class_destroy(g_class);
    unregister_chrdev_region(g_devt, CDF_DEV_NUM);

    pr_info("%s: cdf driver unloaded\n", __func__);
}

module_init(cdf_init);
module_exit(cdf_exit);
