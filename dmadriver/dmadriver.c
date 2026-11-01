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
/* Define offsets */
#define DMADRIVER_PCI_STATUS	0x0008
#define DMADRIVER_TCTL		0x0400
#define DMADRIVER_TIPG		0x0410
/* Transmit descriptor base address low */
#define DMADRIVER_TDBAL		0x3800
/* Transmit descriptor base address high */
#define DMADRIVER_TDBAH		0x3804
/* Size of descriptor buffer */
#define DMADRIVER_TDLEN		0x3808
/* Enable transmitter */
#define ENABLE_EN(reg) ((reg)|= (1 << 1));
/* Enable padding for short packets */
#define ENABLE_PSP(reg) ((reg)|= (1 << 3));
/* TIPG default magic number for gigabit ethernet */
#define TIPG_DEFAULT 		0x0060200A
/* Ring buffer size */
#define TX_RING_SIZE 		8
/* Transmitter description. 
 * Copied from e1000.h */
struct e1000_tx_desc {
    __le64 buffer_addr;      /* Data buffer address */
    union {
        __le32 data;
        struct {
            __le16 length;
            u8 cso;           /* Checksum */
            u8 cmd;           /* Command type */
        } flags;
    } lower;
    union {
        __le32 data;
        struct {
            u8 status;        /* Command status (e.g. done) */
            u8 css;
            __le16 special;
        } fields;
    } upper;
};
/* target slot defines, which network card 
 * driver will be...replaced */
static char 				*target_slot = "0000:00:06.0";
static struct pci_dev 		*nc_dev = NULL;
static void __iomem			*nc_base = NULL;
/* Ring buffer virtual address */
struct e1000_tx_desc *tx_ring = NULL;
/* Physical memory address of ring buffer */
dma_addr_t tx_ring_dma = 0;
/* Parameters
 * 0444 - read-only */
module_param(target_slot, charp, 0444);
MODULE_PARM_DESC(target_slot, "PCI slot of the target network card (e.g. 0000:00:06.0)");

static int ring_buffer_memory_allocate(void)
{
	tx_ring = dma_alloc_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), &tx_ring_dma, GFP_KERNEL);
	if (!tx_ring) {
		printk(KERN_ERR "dmadriver: dma_alloc_coherent failed\n");
		return -ENOMEM;
	}
	printk(KERN_INFO "dmadriver: Physical address for dma  = %pad", &tx_ring_dma);
	return 0;
}

static int dmadriver_configure_netcard(void) 
{
	/* Enable transmitter and configure padding */
	u32 tctl = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
	printk(KERN_INFO "dmadriver: old tctl is 0x%x\n", tctl);
	ENABLE_EN(tctl);
	ENABLE_PSP(tctl);
	printk(KERN_INFO "dmadriver: new tctl is 0x%x\n", tctl);
	iowrite32(tctl, DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
	/* Configure inter packet gap
	 * with default linux value */
	iowrite32(TIPG_DEFAULT, DMADRIVER_PCI_FIELD(DMADRIVER_TIPG));
	int err = ring_buffer_memory_allocate();
	if (err) {
		return err;
	}
	/* Configure tx buffer parameters */
	
	/* Copy lower part of physical address */
	iowrite32((u32)tx_ring_dma, DMADRIVER_PCI_FIELD(DMADRIVER_TDBAL));
	u32 data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDBAL));
	printk(KERN_INFO "dmadriver: new tdbal is 0x%x\n", data);
	/* Copy higher part of physical address */
	iowrite32((u32)(tx_ring_dma >> 32), DMADRIVER_PCI_FIELD(DMADRIVER_TDBAH));		
	data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDBAH));
	printk(KERN_INFO "dmadriver: new tdbah is 0x%x\n", data);
	/* Set ring buffer size. 
	 * It should be muliple of 128 bytes,
	 * so with TX_RING_SIZE = 8 and
	 * sizeof(e1000_tx_desc) = 16 its ok */
	iowrite32(TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMADRIVER_PCI_FIELD(DMADRIVER_TDLEN));		
	data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDLEN));
	printk(KERN_INFO "dmadriver: new tdlen is 0x%x\n", data);
	return 0;
}

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
	/* Read netcard inital status */
	u32 status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
	printk(KERN_INFO "dmadriver: old PCI netcard status is 0x%x\n", status);
	/* Configure netcard */
	dmadriver_configure_netcard();	
	status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
	printk(KERN_INFO "dmadriver: new PCI netcard status is 0x%x\n", status);	
	printk(KERN_INFO "dmadriver: Driver mounted\n");
	return 0;
}

static void __exit dmadriver_exit(void)
{
	printk(KERN_INFO "dmadriver: Driver unmounting\n");
	/* Free virtual mem */
	pci_iounmap(nc_dev, nc_base);
	/* Disoccupy netcard memory */
	pci_release_regions(nc_dev);
	/* Deactivate netcard */
	pci_dev_put(nc_dev);	
	printk(KERN_INFO "dmadriver: Driver unmounted\n");
}

module_init(dmadriver_init);
module_exit(dmadriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Shein");
MODULE_DESCRIPTION("DMA driver for Intel e1000 PCIe Ethernet controller");
