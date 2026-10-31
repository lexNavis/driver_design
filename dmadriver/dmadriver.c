#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/string.h>

#define VENDOR_ID 0x8086
#define DEVICE_ID 0x100E
#define DMADRIVER_PCI_FIELD(offset) (nc_base + (offset))
#define DMADRIVER_PCI_STATUS 0x0008
/* target slot defines, which network card 
 * driver will be...replaced */
static char 				*target_slot = "0000:00:06.0";
static struct pci_dev 		*nc_dev = NULL;
static void __iomem			*nc_base = NULL;
/* Parameters
 * 0444 - read-only */
module_param(target_slot, charp, 0444);
MODULE_PARM_DESC(target_slot, "PCI slot of the target network card (e.g. 0000:00:06.0)");
static int __init dmadriver_init(void)
{
	printk(KERN_INFO "dmadriver: Driver mounting\n");
	/* Find network card 
	 * We look through the network card list 
	 * and compare their pci numbers with the
	 * target */
	struct pci_dev *dev =NULL;
	while ((dev = pci_get_device(VENDOR_ID, DEVICE_ID, dev))) {
		if (strcmp(pci_name(dev), target_slot) == 0) {
			printk(KERN_INFO "dmadriver: Found netcard with number %s\n", target_slot);	
			nc_dev = dev;
			break;
		}
		/* If its not our card, release it */
		pci_dev_put(dev);
		
	}
	if (pci_enable_device(nc_dev)){
		printk(KERN_ERR "dmadriver: Failed to enable PCI device \n");
		return -EIO;		
	}
	/* Occupy network card memory */
	int	err = pci_request_regions(nc_dev, "dmadriver");
	if (err) {
		printk(KERN_ERR "dmadriver: pci_request_regions failed. Regions are occupied\n");
		pci_dev_put(nc_dev);
		return err;		
	}
	/* Get base register address */
	nc_base = pci_iomap(nc_dev, 0, 0);
	if (!nc_base) {
		printk(KERN_ERR "dmadriver: Failed to obtain virtual address\n");
		pci_release_regions(nc_dev);
		pci_dev_put(nc_dev);
		return -ENOMEM;		
	}
	/* Read netcard status */
	u32 status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
	printk(KERN_INFO "dmadriver: PCI netcard status is 0x%x\n", status);	
	printk(KERN_INFO "dmadriver: Driver mounted\n");
	return 0;
}

static void __exit dmadriver_exit(void)
{
	/* Free virtual mem */
	pci_iounmap(nc_dev, nc_base);
	/* Disoccupy netcard memory */
	pci_release_regions(nc_dev);
	/* Deactivate netcard */
	pci_dev_put(nc_dev);	
}

module_init(dmadriver_init);
module_exit(dmadriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Shein");
MODULE_DESCRIPTION("DMA driver for Intel e1000 PCIe Ethernet controller");
