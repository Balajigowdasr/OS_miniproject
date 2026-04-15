#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ================= STRUCT ================= */
struct monitor_entry {
    pid_t pid;
    char container_id[32];
    unsigned long soft;
    unsigned long hard;
    int warned;
    struct list_head list;
};

/* ================= GLOBALS ================= */
static LIST_HEAD(entry_list);
static DEFINE_MUTEX(entry_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ================= RSS ================= */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }

    put_task_struct(task);
    return rss_pages * PAGE_SIZE;
}

/* ================= TIMER ================= */
static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *e, *tmp;

    mutex_lock(&entry_lock);

    list_for_each_entry_safe(e, tmp, &entry_list, list) {

        long rss = get_rss_bytes(e->pid);

        if (rss < 0) {
            list_del(&e->list);
            kfree(e);
            continue;
        }

        if (rss > e->soft && !e->warned) {
            printk(KERN_WARNING
                   "[monitor] SOFT LIMIT %s pid=%d rss=%ld\n",
                   e->container_id, e->pid, rss);
            e->warned = 1;
        }

        if (rss > e->hard) {
            struct task_struct *task;

            rcu_read_lock();
            task = pid_task(find_vpid(e->pid), PIDTYPE_PID);
            if (task)
                send_sig(SIGKILL, task, 1);
            rcu_read_unlock();

            printk(KERN_WARNING
                   "[monitor] HARD LIMIT %s pid=%d killed\n",
                   e->container_id, e->pid);

            list_del(&e->list);
            kfree(e);
        }
    }

    mutex_unlock(&entry_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ================= IOCTL ================= */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitor_entry *e, *tmp;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        e = kmalloc(sizeof(*e), GFP_KERNEL);
        if (!e)
            return -ENOMEM;

        e->pid = req.pid;
        strncpy(e->container_id, req.container_id, 31);
        e->soft = req.soft_limit_bytes;
        e->hard = req.hard_limit_bytes;
        e->warned = 0;

        mutex_lock(&entry_lock);
        list_add(&e->list, &entry_list);
        mutex_unlock(&entry_lock);

        printk(KERN_INFO "[monitor] Registered %s pid=%d\n",
               e->container_id, e->pid);

        return 0;
    }

    if (cmd == MONITOR_UNREGISTER) {
        mutex_lock(&entry_lock);

        list_for_each_entry_safe(e, tmp, &entry_list, list) {
            if (e->pid == req.pid) {
                list_del(&e->list);
                kfree(e);
                break;
            }
        }

        mutex_unlock(&entry_lock);

        printk(KERN_INFO "[monitor] Unregistered pid=%d\n", req.pid);
        return 0;
    }

    return -EINVAL;
}

/* ================= FILE OPS ================= */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ================= INIT ================= */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    if (IS_ERR(cl))
        return PTR_ERR(cl);

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME)))
        return -1;

    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + HZ);

    printk(KERN_INFO "[monitor] Module loaded\n");
    return 0;
}

/* ================= EXIT ================= */
static void __exit monitor_exit(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    timer_delete_sync(&monitor_timer);
#else
    del_timer_sync(&monitor_timer);
#endif

    struct monitor_entry *e, *tmp;

    mutex_lock(&entry_lock);
    list_for_each_entry_safe(e, tmp, &entry_list, list) {
        list_del(&e->list);
        kfree(e);
    }
    mutex_unlock(&entry_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[monitor] Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container memory monitor");
