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
#include <linux/io.h>


#define CLOCK_INTERVAL 100000000

/* Offsets for edu registers */
#define EDU_REG_RAISE_IRQ	0x60
#define EDU_REG_ACK_IRQ		0x64
#define EDU_REG_IRQ_STATUS	0x24


/* Counters. Save information, how many
 * interrupts have entered top-half and bottom-half
 */
/* hrtimer irqs */
static atomic_t timer_count = ATOMIC_INIT(0);
/* edu irqs */
static atomic_t edu_count = ATOMIC_INIT(0);
/* tasklet irqs*/
static atomic_t tasklet_count = ATOMIC_INIT(0);
/* workqueue irqs */
static atomic_t workqueue_count = ATOMIC_INIT(0);
/* threaded_irqs */
static atomic_t threaded_irq_count = ATOMIC_INIT(0);
/* Pointer to start of edu register data */
static void __iomem *edu_base = NULL;
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
/* Dev irq handlers */
/* Top half */
static irqreturn_t edu_irq_top_half(int irq, void *dev_id);
/* Bottom half */
static irqreturn_t edu_irq_thread(int irq, void *dev_id);
/* Callbacks definition */
static void timer_tasklet_callback(unsigned long);
static void timer_workqueue_callback(struct work_struct *work);
static enum hrtimer_restart timer_callback(struct hrtimer *timer);
/* Driver operations */
static ssize_t irqdriver_proc_write(struct file *filp, const char __user *buf, size_t size, loff_t *);
static int irqdriver_proc_open(struct inode *inode, struct file *filp);
/* Create proc_ops */
static struct proc_ops ops = {
	.proc_open = irqdriver_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write	= irqdriver_proc_write,
};

static int __init irqdriver_init(void)
{
	printk(KERN_INFO "irqdriver: Module mounted!\n");
	/* Assign ptr to an opened file
	 * 0 stands for 0666 - read-write to everyone
	 * Create file in root dir, so parent is NULL */
	proc = proc_create("irqdriver", 0666, NULL, &ops);
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
	/* Get pointer to edu regs
	 * 0 - BAR0 (Base Address Register) 
	 * 0 - size by default */
	edu_base = pci_iomap(edu_dev, 0, 0);
	if (!edu_base) {
		printk(KERN_ERR "irqdriver: pci_iomap failed to get virtual address\n");		
		pci_dev_put(edu_dev);
		return -ENOMEM;
	}
	/* Register irq handler */
	int err = request_threaded_irq(edu_dev->irq, edu_irq_top_half, edu_irq_thread, IRQF_SHARED, "irqdriver", edu_dev);
	if (err){
		printk(KERN_ERR "irqdriver: failed to request irq\n");			
		pci_iounmap(edu_dev, edu_base);
		pci_dev_put(edu_dev);		
		return err;
	}
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
	free_irq(edu_dev->irq, edu_dev);
	/* Free virtual memory */
	pci_iounmap(edu_dev, edu_base);
	/* Remove edu */
	pci_dev_put(edu_dev);
}
/* Prints statistics of handled soft/hardirqs */
int show_stats(struct seq_file *file, void *) {
	seq_printf(file, "------HARDIRQS------\n");
	seq_printf(file, "hrtimer handled %d irqs\n", atomic_read(&timer_count));
	seq_printf(file, "edu handled %d irqs\n", atomic_read(&edu_count));
	seq_printf(file, "------SOFTIRQS------\n");
	seq_printf(file, "tasklet handled %d irqs\n", atomic_read(&tasklet_count));
	seq_printf(file, "workqueue handled %d irqs\n", atomic_read(&workqueue_count));
	seq_printf(file, "threaded_irq handled %d irqs\n", atomic_read(&threaded_irq_count));
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

irqreturn_t edu_irq_top_half(int irq, void *dev_id)
{
	/* Edu is a PCI device, so dev_id should be pci_dev */
		
	/* Check if irq has come from edu, because irq is shared */
	u32 status = ioread32(edu_base + EDU_REG_IRQ_STATUS);
	/* If its not edu, leave handler */
	if (status == 0){
		return IRQ_NONE;
	}
	/* Accept irq */
	iowrite32(status, edu_base + EDU_REG_ACK_IRQ);
	atomic_inc(&edu_count);
	/* Plan bottom half in other thread */
	return IRQ_WAKE_THREAD;	
}
irqreturn_t edu_irq_thread(int irq, void *dev_id)
{
	atomic_inc(&threaded_irq_count);
	/* Irq complete */
	return IRQ_HANDLED;
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
	printk(KERN_INFO "irqdriver: Write called. Val = %d\n", val);
	/* If user wants an irq */
	if (val == 1) {
		iowrite32(1, edu_base + EDU_REG_RAISE_IRQ);
	}
	return size;
}


module_init(irqdriver_init);
module_exit(irqdriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DENIS SHEIN");
MODULE_DESCRIPTION("Different implementations of top-bottom halfs of interrupt handlers");
