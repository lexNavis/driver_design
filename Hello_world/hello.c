#include <linux/module.h>
#include <linux/init.h>


MODULE_LICENSE("GPL"); ///< Critical for driver implementation
MODULE_AUTHOR("lex navis");
MODULE_DESCRIPTION("Hello world driver");

/**
 * @brief Function, which greets the console with message
 * @return 0 if no errors occured
 */
static int hello_init(void) {
    /*
     * printk is the kernel logging function 
     * "\n" is essential to see the message. It 
     * signals printk buffer that line is complete triggering a flush of data into the 
     * ring buffer of the kernel, so we can see the message
     */
    printk(KERN_ALERT "Hello, I am lexNavis\n");
    return 0;
}

static void hello_exit(void) {
    printk(KERN_ALERT "Goodbye, cruel world\n");
}


module_init(hello_init); ///< define hello_init as entry (after insmod) function

module_exit(hello_exit); ///< define hello_exit as exit (after rmmod) function