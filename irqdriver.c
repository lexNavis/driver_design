#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static int __init irqdriver_init(void)
{
	printk(KERN_INFO "irqdriver: Module mounted!\n");
	return 0;
}
static void __exit irqdriver_exit(void)
{
	printk(KERN_INFO "irqdriver: Module unmounted!\n");	
}

module_init(irqdriver_init);
module_exit(irqdriver_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("DENIS SHEIN");
MODULE_DESCRIPTION("Different implementations of top-bottom halfs of interrupt handlers");
