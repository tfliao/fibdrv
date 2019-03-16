// vim: ts=4:sw=4:expandtab

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#define ENABLE_STAT

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 92

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);

#ifdef ENABLE_STAT
static struct kobject *stat_kobj;
static uint64_t *stat_ns = NULL;
static uint32_t *stat_cnt = NULL;
#endif /* ENABLE_STAT */

#ifdef ENABLE_STAT
static ssize_t result_show(struct kobject *kobj,
                           struct kobj_attribute *attr,
                           char *buf)
{
    size_t limit = PAGE_SIZE - 32;
    size_t len = 0;
    int i;
    for (i = 0; i <= MAX_LENGTH && len < limit; ++i) {
        if (stat_cnt[i] == 0) {
            continue;
        }
        len += snprintf(buf + len, limit - len, "%d: %llu / %lu\n", i,
                        (unsigned long long) stat_ns[i],
                        (unsigned long) stat_cnt[i]);
    }
    if (len >= limit) {
        len +=
            snprintf(buf + len, PAGE_SIZE - len, "... more lines truncated\n");
    }
    return len;
}

static struct kobj_attribute result_attribute =
    __ATTR(result, 0444, result_show, NULL);

static ssize_t reset_show(struct kobject *kobj,
                          struct kobj_attribute *attr,
                          char *buf)
{
    return sprintf(buf, "store 1 to trigger stat data reset\n");
}

static ssize_t reset_store(struct kobject *kobj,
                           struct kobj_attribute *attr,
                           const char *buf,
                           size_t count)
{
    int ret, val, i;
    ret = kstrtoint(buf, 10, &val);
    if (ret == 0) {
        for (i = 0; i <= MAX_LENGTH; ++i) {
            stat_ns[i] = 0;
            stat_cnt[i] = 0;
        }
    }
    return count;
}

static struct kobj_attribute reset_attribute =
    __ATTR(reset, 0644, reset_show, reset_store);

static struct attribute *attrs[] = {
    &result_attribute.attr,
    &reset_attribute.attr,
    NULL, /* need to NULL terminate the list of attributes */
};

static struct attribute_group stat_group = {
    .attrs = attrs,
};
#endif /* ENABLE_STAT */

static long long fib_sequence(long long k)
{
    /* FIXME: use clz/ctz and fast algorithms to speed up */
    long long f[k + 2];

    f[0] = 0;
    f[1] = 1;

    for (int i = 2; i <= k; i++) {
        f[i] = f[i - 1] + f[i - 2];
    }

    return f[k];
}

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    ssize_t r;
#ifdef ENABLE_STAT
    ktime_t begin = ktime_get();
#endif /* ENABLE_STAT */
    r = fib_sequence(*offset);
#ifdef ENABLE_STAT
    uint64_t ns = ktime_to_ns(ktime_get() - begin);
    stat_ns[*offset] += ns;
    stat_cnt[*offset]++;
#endif /* ENABLE_STAT */
    return r;
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return 1;
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

#ifdef ENABLE_STAT
static int init_stat(void)
{
    stat_kobj = kobject_create_and_add("fibonacci", kernel_kobj);
    if (!stat_kobj) {
        printk(KERN_ERR "Failed to create sysfs object");
        return -ENOMEM;
    }

    int rc = sysfs_create_group(stat_kobj, &stat_group);
    if (rc) {
        kobject_put(stat_kobj);
    }

    stat_ns = kcalloc(MAX_LENGTH + 1, sizeof(uint64_t), GFP_KERNEL);
    stat_cnt = kcalloc(MAX_LENGTH + 1, sizeof(uint32_t), GFP_KERNEL);

    if (!stat_ns || !stat_ns) {
        kfree(stat_cnt);
        kfree(stat_ns);
        kobject_put(stat_kobj);
        return -ENOMEM;
    }

    return rc;
}
#endif /* ENABLE_STAT */

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    cdev_init(fib_cdev, &fib_fops);
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
#ifdef ENABLE_STAT
    rc = init_stat();
    if (rc < 0) {
        goto failed_stat_create;
    }
#endif /* ENABLE_STAT */
    return rc;
#ifdef ENABLE_STAT
failed_stat_create:
    device_destroy(fib_class, fib_dev);
#endif /* ENABLE_STAT */
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
#ifdef ENABLE_STAT
    kfree(stat_cnt);
    kfree(stat_ns);
    kobject_put(stat_kobj);
#endif /* ENABLE_STAT */
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
