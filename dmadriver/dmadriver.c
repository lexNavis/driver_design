#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/string.h>
#include <linux/processor.h>
#include <linux/minmax.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>

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
/* Ring head */
#define DMADRIVER_TDH		0x3810
/* Ring tail */
#define DMADRIVER_TDT		0x3818
/* Enable transmitter */
#define ENABLE_EN(reg) ((reg)|= (1 << 1));
/* Enable padding for short packets */
#define ENABLE_PSP(reg) ((reg)|= (1 << 3));
/* Set end of packet */
#define DMADRIVER_EOP		0x01
#define DMADRIVER_IFCS		0x02
/* Ask netcard to fill status register */
#define DMADRIVER_RS		0x08
/* TIPG default magic number for gigabit ethernet */
#define TIPG_DEFAULT 		0x0060200A
/* Ring buffer size */
#define TX_RING_SIZE 		8
#define TX_BUFFER_SIZE		((1UL << 20) + 4096)	// 1 MB + 4096
#define TX_PACKET_SIZE		1500
#define TIMEOUT_SIZE		1000000
/* Check whether tailed reached head or not */
#define TAIL_REACHED_HEAD(tail)	((((tail) + 1) % TX_RING_SIZE) == ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH)))	
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
/* Descriptor ring virtual address */
struct e1000_tx_desc *tx_ring = NULL;
/* Physical memory address of descriptor ring */
dma_addr_t tx_ring_dma = 0;
/* Ring buffer virtual address */
static u8 *tx_buffer = NULL;
/* Physical memory address of ring buffer */
dma_addr_t tx_buffer_dma = 0;
/* Tail index in ring buffer */
int tx_tail = 0;
/* Number of bytes sent to netcard */
size_t bytes_sent = 0;
/* Wait timeout */
int timeout = TIMEOUT_SIZE;
/* Parameters
 * 0444 - read-only */
module_param(target_slot, charp, 0444);
MODULE_PARM_DESC(target_slot, "PCI slot of the target network card (e.g. 0000:00:06.0)");
static unsigned int buffer_size = (1UL << 20);
module_param(buffer_size, uint, 0644);
MODULE_PARM_DESC(buffer_size, "Size of DMA buffer in bytes");
static unsigned int test_size = 0;
module_param(test_size, uint, 0644);
MODULE_PARM_DESC(test_size, "Size of data in bytes to test (0 = skip)");

static int ring_buffer_store(dma_addr_t phys_addr, size_t len)
{
	/* First we should find out, if
	 * tail has not reaches head 
	 * from behind */
	while (TAIL_REACHED_HEAD(tx_tail)){
		if ((--timeout) == 0) {
			printk(KERN_ERR "dmadriver: Timeout waiting for free space\n");
			return -EIO;				
		}
		cpu_relax();
	}
	/* Clear desc status */
	tx_ring[tx_tail].upper.fields.status = 0;
	
	/* Fill descriptor with physical address */
	tx_ring[tx_tail].buffer_addr = cpu_to_le64(phys_addr);
	tx_ring[tx_tail].lower.flags.length = len;
	tx_ring[tx_tail].lower.flags.cmd = (DMADRIVER_EOP | DMADRIVER_IFCS | DMADRIVER_RS);
	
	/* Ensure descriptor is written to memory before notifying device */
	dma_wmb(); 
	tx_tail = (tx_tail + 1) % TX_RING_SIZE;
	/* Move real tail */
	iowrite32(tx_tail, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));
	return 0;
}

static int ring_buffer_send_data(size_t block_len)
{
	size_t offset = 0;
	while (block_len > 0) {
		/* We can copy not more than 1500 bytes at once */
		size_t chunk = min(block_len, (size_t)TX_PACKET_SIZE);
		
		/* Calculate physical address of the chunk inside tx_buffer */
		dma_addr_t chunk_dma = tx_buffer_dma + offset;
		
		int err = ring_buffer_store(chunk_dma, chunk);
		if (err) {
			printk(KERN_ERR "dmadriver: ring_buffer_store failed\n");
			return err;
		}
		offset += chunk;
		block_len -= chunk;
	}
	return 0;
}
static int ring_buffer_transfer_data(size_t total_len)
{
	int err;
	size_t offset = 0;
	while (total_len > 0) {
		size_t block = min(total_len, (size_t)buffer_size);
		memset(tx_buffer, 0xAA, block);
		err = ring_buffer_send_data(block);
		if (err) {
			printk(KERN_ERR "dmadriver: send_data failed\n");
			return err;
		}
		/* Reset timeout for each block */
		timeout = TIMEOUT_SIZE;
		while (ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH)) != tx_tail) {
			if ((--timeout) == 0) {
				printk(KERN_ERR "dmadriver: Timeout waiting for netcard\n");
				return -EIO;
			}
			cpu_relax();
		}
		offset += block;
		total_len -= block;
	}
	return 0;
}

static int ring_buffer_memory_allocate(void)
{
	/* Allocate memory for descriptor ring */
	tx_ring = dma_alloc_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), &tx_ring_dma, GFP_KERNEL);
	if (!tx_ring) {
		printk(KERN_ERR "dmadriver: dma_alloc_coherent failed\n");
		return -ENOMEM;
	}
	/* Allocate memory for ring buffer */
	tx_buffer = dma_alloc_coherent(&nc_dev->dev, buffer_size, &tx_buffer_dma, GFP_KERNEL);
	if (!tx_buffer) {
		printk(KERN_ERR "dmadriver: dma_alloc_coherent failed for tx_buffer\n");
		/* Откатываем выделение кольца, чтобы не было утечки */
		dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), tx_ring, tx_ring_dma);
		return -ENOMEM;		
	}
	printk(KERN_INFO "dmadriver: Physical address for dma  = %pad", &tx_ring_dma);
	return 0;
}

static int dmadriver_configure_netcard(void) 
{
	/* Reset the netcard to clear any previous state */
	u32 ctrl = ioread32(DMADRIVER_PCI_FIELD(0x0000));
	iowrite32(ctrl | (1 << 26), DMADRIVER_PCI_FIELD(0x0000));
	/* Wait for reset to complete (usually takes a few microseconds) */
	mdelay(1); 
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

static void dmadriver_benchmark(size_t size)
{
	ktime_t start, end;
	s64 time_ns;
	int err;

	printk(KERN_INFO "dmadriver: --- DMA Benchmark for %zu bytes ---\n", size);
	bytes_sent = 0;
	timeout = TIMEOUT_SIZE;
	start = ktime_get();

	err = ring_buffer_transfer_data(size);
	if (err) {
		printk(KERN_ERR "dmadriver: DMA benchmark failed with error %d\n", err);
		return;
	}

	end = ktime_get();
	time_ns = ktime_to_ns(ktime_sub(end, start));
	printk(KERN_INFO "dmadriver: DMA transfer of %zu bytes took %lld ns\n", size, time_ns);
	if (time_ns > 0) {
		u64 mbps = div_u64((u64)size * 1000, time_ns);
		printk(KERN_INFO "dmadriver: DMA Throughput: ~%llu MB/s\n", mbps);
	}
}

static void memcpy_benchmark(size_t size)
{
	ktime_t start, end;
	s64 time_ns;
	void *dst;
	size_t offset = 0;

	printk(KERN_INFO "dmadriver: --- memcpy Benchmark for %zu bytes ---\n", size);

	/* Allocate destination buffer. 
	 * If kmalloc fails (e.g. not enough memory for 16MB), 
	 * just skip the test to avoid panic */
	dst = vmalloc(size);
	if (!dst) {
		printk(KERN_ERR "dmadriver: memcpy_benchmark: kmalloc failed for %zu bytes\n", size);
		return;
	}

	/* Start timer */
	start = ktime_get();

	/* We must copy block by block, because tx_buffer 
	 * is only buffer_size bytes long. If we try to 
	 * memcpy the whole size, we will read out of bounds */
	while (offset < size) {
		size_t block = min(size - offset, (size_t)buffer_size);
		/* Copy from our tx_buffer to the new dst buffer */
		memcpy(dst + offset, tx_buffer, block); 
		offset += block;
	}

	end = ktime_get();
	time_ns = ktime_to_ns(ktime_sub(end, start));

	printk(KERN_INFO "dmadriver: memcpy of %zu bytes took %lld ns\n", size, time_ns);
	if (time_ns > 0) {
		u64 mbps = div_u64((u64)size * 1000, time_ns);
		printk(KERN_INFO "dmadriver: memcpy Throughput: ~%llu MB/s\n", mbps);
	}

	/* Free destination buffer */
	vfree(dst);
}

static void dmadriver_run_benchmarks(size_t size)
{
	printk(KERN_INFO "dmadriver: ========================================\n");
	printk(KERN_INFO "dmadriver: Starting benchmarks for %zu bytes\n", size);
	printk(KERN_INFO "dmadriver: ========================================\n");

	/* 1. Test DMA transfer */
	dmadriver_benchmark(size);

	/* 2. Test memcpy */
	memcpy_benchmark(size);

	printk(KERN_INFO "dmadriver: ========================================\n");
	printk(KERN_INFO "dmadriver: Benchmarks finished\n");
	printk(KERN_INFO "dmadriver: ========================================\n");
}

static int __init dmadriver_init(void)
{
	printk(KERN_INFO "dmadriver: Driver mounting\n");
	/* Check if buffer size is appropriate */
	if (buffer_size > (64 * 1024 * 1024)) { // Например, не больше 64 МБ
	    printk(KERN_ERR "dmadriver: buffer_size too large\n");
	    return -EINVAL;
	}
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
	if (!nc_dev) {
		printk(KERN_ERR "dmadriver: Target netcard not found\n");
		return -ENODEV;
	}
	if (pci_enable_device(nc_dev)){
		printk(KERN_ERR "dmadriver: Failed to enable PCI device \n");
		return -EIO;		
	}
	pci_set_master(nc_dev);
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
	err = dmadriver_configure_netcard();
	if (err) {
		printk(KERN_ERR "dmadriver: configure failed, rolling back\n");
		pci_iounmap(nc_dev, nc_base);
		pci_release_regions(nc_dev);
		pci_dev_put(nc_dev);
		return err;
	}
	status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
	printk(KERN_INFO "dmadriver: new PCI netcard status is 0x%x\n", status);	
	printk(KERN_INFO "dmadriver: Driver mounted\n");
	/* Test write in DMA */
	if (test_size > 0) {
	    dmadriver_run_benchmarks(test_size);
	}
	return 0;
}

static void __exit dmadriver_exit(void)
{
	printk(KERN_INFO "dmadriver: Driver unmounting\n");
	
	/* 1. Disable transmitter BEFORE touching memory and registers */
	u32 tctl = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
	tctl &= ~(1 << 1); // Clear EN bit
	iowrite32(tctl, DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
	
	/* 2. Reset ring pointers to initial state */
	iowrite32(0, DMADRIVER_PCI_FIELD(DMADRIVER_TDH));
	iowrite32(0, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));
	
	/* 3. Give hardware time to finish all transfers */
	msleep(10);
	
	/* 4. Now it's safe to free memory */
	dma_free_coherent(&nc_dev->dev, buffer_size, tx_buffer, tx_buffer_dma);
	dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), tx_ring,  tx_ring_dma);
	
	/* 5. Unmap registers */
	pci_iounmap(nc_dev, nc_base);
	
	/* 6. Release regions and device */
	pci_release_regions(nc_dev);
	pci_dev_put(nc_dev);	
	
	printk(KERN_INFO "dmadriver: Driver unmounted\n");
}

module_init(dmadriver_init);
module_exit(dmadriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Shein");
MODULE_DESCRIPTION("DMA driver for Intel e1000 PCIe Ethernet controller");
