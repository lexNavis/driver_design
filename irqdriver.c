#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/pci.h>


#define CLOCK_INTERVAL 100000000

/* Counters. Save information, how many
 * interrupts have entered top-half and bottom-half
 */
/* hrtimer irqs */
static atomic_t timer_count = ATOMIC_INIT(0);
/* tasklet irqs*/
static atomic_t tasklet_count = ATOMIC_INIT(0);
/* workqueue irqs */
static atomic_t workqueue_count = ATOMIC_INIT(0);
/* Ptr to PCI edu device */
static struct pci_dev *edu_dev = NULL; 
/* Pointer to file in /proc directory */
struct proc_dir_entry *proc;
/* Create a timer var */
static struct hrtimer gtimer;
/* Create global tasklet variable */
static struct tasklet_struct timer_tasklet;  
/* Workqueue structure */
struct work_struct gwork;
/* Stats function */
static int show_stats(struct seq_file *file, void *);
/* Callbacks definition */
static void timer_tasklet_callback(unsigned long);
static void timer_workqueue_callback(struct work_struct *work);
static enum hrtimer_restart timer_callback(struct hrtimer *timer);

static int __init irqdriver_init(void)
{
	printk(KERN_INFO "irqdriver: Module mounted!\n");
	/* Assign ptr to an opened file
	 * 0 stands for 0444 - read-only
	 * Create file in root dir, so parent is NULL */
	proc = proc_create_single("irqdriver", 0, NULL, show_stats);
	if (!proc) {
		printk(KERN_ERR "irqdriver: proc_create failed, returned NULL\n");
		return -ENOMEM;
	}
	/* Find PCI device and its IRQ number by
	 * Vendor ID and Device ID
	 * NULL says start from the beginning of dev list */
	edu_dev = pci_get_device(0x1234, 0x11e8, NULL);
	if (!edu_dev){
		printk(KERN_ERR "irqdriver: edu device not found, returned NULL\n");		
		return -ENODEV;
	}
	/* Turn it on */
	if (pci_enable_device(edu_dev)){
		printk(KERN_ERR "irqdriver: failed to enable PCI device\n");
		return -EIO;
	}
	/* If found, print its IRQ num */
	printk(KERN_INFO "Edu device IRQ is %d\n", edu_dev->irq);
	/* Initialize global variables */
	tasklet_init(&timer_tasklet, timer_tasklet_callback, 0);
	INIT_WORK(&gwork, timer_workqueue_callback);
	hrtimer_init(&gtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	/* Register top-half callback */
	gtimer.function = timer_callback;
	hrtimer_start(&gtimer, ktime_set(0, CLOCK_INTERVAL), HRTIMER_MODE_REL);
	return 0;
}
static void __exit irqdriver_exit(void)
{
	printk(KERN_INFO "irqdriver: Module unmounted!\n");
	/* Stop timer */
	hrtimer_cancel(&gtimer);
	/* Free tasklet */
	tasklet_kill(&timer_tasklet);
	/* Free workqueue */
	cancel_work_sync(&gwork);
	/* Delete /proc file */
	remove_proc_entry("irqdriver", NULL);
	/* Remove edu */
	pci_dev_put(edu_dev);
}
/* Prints statistics of handled soft/hardirqs */
int show_stats(struct seq_file *file, void *) {
	seq_printf(file, "------HARDIRQS------\n");
	seq_printf(file, "hrtimer handled %d irqs\n", atomic_read(&timer_count));
	seq_printf(file, "------SOFTIRQS------\n");
	seq_printf(file, "tasklet handled %d irqs\n", atomic_read(&tasklet_count));
	seq_printf(file, "workqueue handled %d irqs\n", atomic_read(&workqueue_count));
	return 0;
}

void timer_workqueue_callback(struct work_struct *work)
{
	/* We are not in irq context, so
	 * sleeping is allowed */
	/* Inc bottom-half workqueue counter */
	atomic_inc(&workqueue_count);
}


void timer_tasklet_callback(unsigned long)
{
	/* msleep test. tasklet works in irq context,
	 * so we mustnt sleep here. */
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
	/* Plan workqueue
	 * Add gwork to list of planned actions */
	queue_work(system_wq, &gwork);
	return HRTIMER_RESTART;
}

module_init(irqdriver_init);
module_exit(irqdriver_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("DENIS SHEIN");
MODULE_DESCRIPTION("Different implementations of top-bottom halfs of interrupt handlers");
