#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define CLOCK_INTERVAL 100000000

/* Counters. Save information, how many
 * interrupts have entered top-half and bottom-half
 */
static atomic_t timer_count = ATOMIC_INIT(0);
static atomic_t tasklet_count = ATOMIC_INIT(0);
/* Pointer to file in /proc directory */
struct proc_dir_entry *proc;
/* Create a timer var */
static struct hrtimer timer;
/* Create global tasklet variable */
static struct tasklet_struct timer_tasklet;  
/* Stats function */
static int show_stats(struct seq_file *file, void *);
/* Callbacks definition */
static void timer_tasklet_callback(unsigned long);
enum hrtimer_restart timer_callback(struct hrtimer *timer);

static int __init irqdriver_init(void)
{
	printk(KERN_INFO "irqdriver: Module mounted!\n");
	/* Assign ptr to an opened file
	 * 0 stands for 0444 - read-only
	 * Create file in root dir, so parent is NULL */
	proc = proc_create_single("irqdriver", 0, NULL, show_stats);
	if (!proc) {
		printk(KERN_ERR "proc_create failed, returned NULL\n");
		return -ENOMEM;
	}
	/* Initialize global variables */
	tasklet_init(&timer_tasklet, timer_tasklet_callback, 0);
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	/* Register top-half callback */
	timer.function = timer_callback;
	hrtimer_start(&timer, ktime_set(0, CLOCK_INTERVAL), HRTIMER_MODE_REL);
	return 0;
}
static void __exit irqdriver_exit(void)
{
	printk(KERN_INFO "irqdriver: Module unmounted!\n");
	/* Stop timer */
	hrtimer_cancel(&timer);
	/* Free tasklet */
	tasklet_kill(&timer_tasklet);
	/* Delete /proc file */
	remove_proc_entry("irqdriver", NULL);
}
/* Prints statistics of handled soft/hardirqs */
int show_stats(struct seq_file *file, void *) {
	seq_printf(file, "------HARDIRQS------\n");
	seq_printf(file, "hrtimer handled %d irqs\n", atomic_read(&timer_count));
	seq_printf(file, "------SOFTIRQS------\n");
	seq_printf(file, "tasklet handled %d irqs\n", atomic_read(&tasklet_count));
	return 0;
}

void timer_tasklet_callback(unsigned long)
{
	/* Inc bottom-half tasklet counter */
	atomic_inc(&tasklet_count);	
}

enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
	/* Irqs are already forbidden, so
	 * no need to use local_irq_save() */
	/* New irq from timer has come */
	atomic_inc(&timer_count);
	/* Replan the timer */
	hrtimer_forward(timer, ktime_get(), ktime_set(0, CLOCK_INTERVAL));
	/* Plan softirq */
	tasklet_schedule(&timer_tasklet);
	return HRTIMER_RESTART;
}

module_init(irqdriver_init);
module_exit(irqdriver_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("DENIS SHEIN");
MODULE_DESCRIPTION("Different implementations of top-bottom halfs of interrupt handlers");
