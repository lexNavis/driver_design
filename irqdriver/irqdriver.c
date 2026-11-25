#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/ktime.h> /* For ktime_t */

/* Default interval: 100 ms */
static unsigned int interval_ms = 100;
module_param(interval_ms, uint, 0644);
MODULE_PARM_DESC(interval_ms, "Default timer interval in milliseconds");

/* Structure to hold tasklet data and timing */
static struct irq_tasklet_data {
    struct tasklet_struct tasklet;
    ktime_t start_time;
    u64 total_lag;
} tasklet_data;

/* Structure to hold workqueue data and timing */
static struct irq_workqueue_data {
    struct work_struct work;
    ktime_t start_time;
    u64 total_lag;
} workqueue_data;

/* 
 * Timing metrics for the timer itself.
 * timer_total_lag accumulates the jitter (difference between 
 * scheduled and actual firing time).
 */
static u64 timer_total_lag = 0;
static ktime_t last_actual_time;

/* Counters for bottom-halves and timer */
static atomic_t timer_count = ATOMIC_INIT(0);
static atomic_t timer_tasklet_count = ATOMIC_INIT(0);
static atomic_t timer_workqueue_count = ATOMIC_INIT(0);
static atomic_t log_count = ATOMIC_INIT(0); /* To limit dmesg spam */

/* Pointer to /proc entry */
static struct proc_dir_entry *proc;

/* High-resolution timer */
static struct hrtimer gtimer;

/* Expected time of the next timer expiry */
static ktime_t next_expiry;

/* Stats function */
static int show_stats(struct seq_file *file, void *);

/* Callbacks */
static void timer_tasklet_callback(unsigned long);
static void timer_workqueue_callback(struct work_struct *work);
static enum hrtimer_restart timer_callback(struct hrtimer *timer);

/* Driver operations */
static ssize_t irqdriver_proc_write(struct file *filp, const char __user *buf, size_t size, loff_t *);
static int irqdriver_proc_open(struct inode *inode, struct file *filp);

static struct proc_ops ops = {
    .proc_open = irqdriver_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
    .proc_write = irqdriver_proc_write,
};

static int __init irqdriver_init(void)
{
    ktime_t interval;

    printk(KERN_INFO "irqdriver: Module mounted with interval %u ms\n", interval_ms);

    proc = proc_create("irqdriver", 0666, NULL, &ops);
    if (!proc) {
        printk(KERN_ERR "irqdriver: proc_create failed, returned NULL\n");
        return -ENOMEM;
    }

    /* Initialize tasklet and workqueue */
    tasklet_init(&tasklet_data.tasklet, timer_tasklet_callback, (unsigned long)&tasklet_data);
    INIT_WORK(&workqueue_data.work, timer_workqueue_callback);

    /* Setup hrtimer using the new API (hrtimer_setup) */
    hrtimer_setup(&gtimer, timer_callback, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

    interval = ktime_set(0, interval_ms * 1000000ULL); /* Convert ms to ns */
    next_expiry = ktime_add(ktime_get(), interval);
    hrtimer_start(&gtimer, interval, HRTIMER_MODE_REL);

    return 0;
}

static void __exit irqdriver_exit(void)
{
    printk(KERN_INFO "irqdriver: Module unmounted!\n");

    hrtimer_cancel(&gtimer);
    tasklet_kill(&tasklet_data.tasklet);
    cancel_work_sync(&workqueue_data.work);

    remove_proc_entry("irqdriver", NULL);
}

/* Prints statistics of handled soft/hardirqs */
int show_stats(struct seq_file *file, void *)
{
    int t_count = atomic_read(&timer_tasklet_count);
    int w_count = atomic_read(&timer_workqueue_count);
    int timer_c = atomic_read(&timer_count);

    u64 avg_t_lag = t_count ? (tasklet_data.total_lag / t_count) : 0;
    u64 avg_w_lag = w_count ? (workqueue_data.total_lag / w_count) : 0;
    u64 avg_timer_lag = timer_c ? (timer_total_lag / timer_c) : 0;

    seq_printf(file, "------ CONFIGURATION ------\n");
    seq_printf(file, "Configured interval: %u ms\n", interval_ms);

    seq_printf(file, "------ TIMER (HARDIRQ) ------\n");
    seq_printf(file, "Timer fired: %d times\n", timer_c);
    seq_printf(file, "Timer avg jitter: %llu ns\n", avg_timer_lag);
    seq_printf(file, "Last actual time: %lld ns\n", ktime_to_ns(last_actual_time));

    seq_printf(file, "------ TASKLET (SOFTIRQ) ------\n");
    seq_printf(file, "Tasklet handled: %d times\n", t_count);
    seq_printf(file, "Tasklet avg lag: %llu ns\n", avg_t_lag);

    seq_printf(file, "------ WORKQUEUE (PROCESS CONTEXT) ------\n");
    seq_printf(file, "Workqueue handled: %d times\n", w_count);
    seq_printf(file, "Workqueue avg lag: %llu ns\n", avg_w_lag);

    return 0;
}

void timer_workqueue_callback(struct work_struct *work)
{
    /* We are not in irq context, so sleeping is allowed */
    msleep(100);
    struct irq_workqueue_data *w_data = container_of(work, struct irq_workqueue_data, work);
    if (!w_data) {
        return;
    }

    /* Calculate lag from the moment the bottom-half was scheduled */
    u64 lag = ktime_to_ns(ktime_sub(ktime_get(), w_data->start_time));
    w_data->total_lag += lag;

    atomic_inc(&timer_workqueue_count);
}

void timer_tasklet_callback(unsigned long data)
{
    /* Tasklet works in irq context, so we mustn't sleep here. */
    struct irq_tasklet_data *t_data = (struct irq_tasklet_data *)data;
    if (!t_data) {
        return;
    }

    /* Calculate lag from the moment the bottom-half was scheduled */
    u64 lag = ktime_to_ns(ktime_sub(ktime_get(), t_data->start_time));
    t_data->total_lag += lag;

    atomic_inc(&timer_tasklet_count);
}

enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
    ktime_t now = ktime_get();
    ktime_t interval = ktime_set(0, interval_ms * 1000000ULL);
    s64 timer_jitter;

    /* Save actual time for the /proc stats */
    last_actual_time = now;

    /* Increment hardirq counter */
    atomic_inc(&timer_count);

    /* Calculate jitter for the timer itself (how late it fired) */
    timer_jitter = ktime_to_ns(ktime_sub(now, next_expiry));
    if (timer_jitter > 0) {
        timer_total_lag += timer_jitter;
    }

    /* Log the first 5 firings to dmesg (to avoid spam) */
    if (atomic_read(&log_count) < 5) {
        printk(KERN_INFO "irqdriver: Timer fired, jitter = %lld ns\n", timer_jitter);
        atomic_inc(&log_count);
    }

    /* Schedule the next expiry */
    next_expiry = ktime_add(next_expiry, interval);
    hrtimer_forward(timer, now, interval);

    /* Save start time before scheduling bottom-halves */
    tasklet_data.start_time = now;
    workqueue_data.start_time = now;

    /* Schedule bottom halves */
    tasklet_schedule(&tasklet_data.tasklet);
    queue_work(system_wq, &workqueue_data.work);

    return HRTIMER_RESTART;
}

int irqdriver_proc_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, show_stats, NULL);
}

ssize_t irqdriver_proc_write(struct file *filp, const char __user *buf, size_t size, loff_t *)
{
    int val;
    int err = kstrtoint_from_user(buf, size, 10, &val);
    
    if (err) {
        printk(KERN_ERR "irqdriver: kstrtoint_from_user failed\n");
        return err;
    }

    /* If user writes a valid interval, update it on the fly */
    if (val > 0 && val <= 5000) {
        interval_ms = val;
        printk(KERN_INFO "irqdriver: Interval changed to %u ms\n", interval_ms);
        
        /* Re-schedule the timer immediately with new interval */
        hrtimer_cancel(&gtimer);
        next_expiry = ktime_add(ktime_get(), ktime_set(0, interval_ms * 1000000ULL));
        hrtimer_start(&gtimer, ktime_set(0, interval_ms * 1000000ULL), HRTIMER_MODE_REL);
    }
    return size;
}

module_init(irqdriver_init);
module_exit(irqdriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DENIS SHEIN");
MODULE_DESCRIPTION("Adapted timer-only driver for bottom-half comparison");
