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
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

/* Prototypes */
int dmadriver_open(struct inode *inode, struct file *filp);
ssize_t dmadriver_write(struct file *filp, const char __user *buf, size_t len, loff_t *);
#define VENDOR_ID 0x8086
#define DEVICE_ID 0x100E
#define DMADRIVER_PCI_FIELD(offset) (nc_base + (offset))
/* Define offsets */
#define DMADRIVER_PCI_STATUS 0x0008
#define DMADRIVER_TCTL 0x0400
#define DMADRIVER_TIPG 0x0410
/* Transmit descriptor base address low */
#define DMADRIVER_TDBAL 0x3800
/* Transmit descriptor base address high */
#define DMADRIVER_TDBAH 0x3804
/* Size of descriptor buffer */
#define DMADRIVER_TDLEN 0x3808
/* Ring head */
#define DMADRIVER_TDH 0x3810
/* Ring tail */
#define DMADRIVER_TDT 0x3818
/* Enable transmitter */
#define ENABLE_EN(reg) ((reg) |= (1 << 1))
/* Enable padding for short packets */
#define ENABLE_PSP(reg) ((reg) |= (1 << 3))
/* Enable extended descriptor */
#define CMD_DEXT (1 << 5)
/* Flag, that packet is a segment */
#define CMD_TSE (1 << 2)
/* Report status flag */
#define CMD_RS (1 << 3)
/* Insert FCS */
#define CMD_IFCS (1 << 1)
/* End of packet */
#define CMD_EOP (1 << 0)

/* Set end of packet */
#define DMADRIVER_EOP 0x01
#define DMADRIVER_IFCS 0x02
/* Ask netcard to fill status register */
#define DMADRIVER_RS 0x08
/* TIPG default magic number for gigabit ethernet */
#define TIPG_DEFAULT 0x0060200A
/* Ring buffer size */
#define TX_RING_SIZE 16
#define TX_BUFFER_SIZE ((1UL << 20) + 4096) // 1 MB + 4096
/* Buffer count for scatter-gather */
#define SG_BUFFER_COUNT 2
#define TX_PACKET_SIZE 1500
#define TIMEOUT_SIZE 1000000
#define MAX_MEM_SIZE (1UL << 26)
#define DMADRIVER_SG_MAX_SEGMENTS (TX_RING_SIZE - 2)
/* Check whether tail reached head or not */
#define TAIL_REACHED_HEAD(tail) ((((tail) + 1) % TX_RING_SIZE) == ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH)))
/* Transmitter description.
 * Copied from e1000.h */
struct e1000_tx_desc
{
  __le64 buffer_addr; /* Data buffer address */
  union
  {
    __le32 data;
    struct
    {
      __le16 length;
      u8 cso; /* Checksum */
      u8 cmd; /* Command type */
    } flags;
  } lower;
  union
  {
    __le32 data;
    struct
    {
      u8 status; /* Command status (e.g. done) */
      u8 css;
      __le16 special;
    } fields;
  } upper;
};
/* Advanced descriptor context */
struct e1000_adv_tx_context_desc
{
  __le32 vlan_macip_lens;
  __le32 seqnum_seed;
  __le32 type_tucmd_mlhl;
  __le32 mss_l4len_idx;
};
/* Ptr to proc file */
static struct proc_dir_entry *proc = NULL;
static struct proc_ops ops = {
    .proc_open = dmadriver_open,
    .proc_write = dmadriver_write,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
static struct pci_dev *nc_dev = NULL;
static void __iomem *nc_base = NULL;
/* Descriptor ring virtual address */
struct e1000_tx_desc *tx_ring = NULL;
/* Physical memory address of descriptor ring */
dma_addr_t tx_ring_dma = 0;
/* Ring buffer virtual address */
static u8 *tx_buffer = NULL;
/* Physical memory address of ring buffer */
dma_addr_t tx_buffer_dma = 0;
/* --- Scatter-Gather Globals --- */
static struct e1000_tx_desc *sg_tx_ring = NULL;
static dma_addr_t sg_tx_ring_dma = 0;
static void **sg_bufs = NULL;
static struct scatterlist *sg_list = NULL;
/* Tail index in ring buffer */
int tx_tail = 0;
/* Number of bytes sent to netcard */
size_t bytes_sent = 0;
/* Wait timeout */
int timeout = TIMEOUT_SIZE;
/* target slot defines, which network card
 * driver will be...replaced */
static char *target_slot = "0000:00:06.0";
/* Counters */
static atomic_t total_transfers = ATOMIC_INIT(0);
static atomic64_t total_bytes_sent = ATOMIC64_INIT(0);
/* Transfer result struct */
struct dma_stats
{
  size_t size;
  s64 dma_time_ns;
  u64 memcpy_time_ns;
  s64 dma_speed_mbps;
  u64 memcpy_speed_mbps;
};
static struct dma_stats current_stats = {0};
/* Parameters
 * 0444 - read-only */
module_param(target_slot, charp, 0444);
MODULE_PARM_DESC(target_slot, "PCI slot of the target network card (e.g. 0000:00:06.0)");
static unsigned int buffer_size = (1UL << 20);
module_param(buffer_size, uint, 0644);
MODULE_PARM_DESC(buffer_size, "Size of DMA buffer in bytes");
static unsigned int alloc_mode = 0;
module_param(alloc_mode, int, 0644);
MODULE_PARM_DESC(alloc_mode, "Type of dma memory allocation: 0 - coherent, 1 - streaming, 2 - scatter-gather");

/* Syncs cpu cache and memory so card can see new data*/
static void sync_for_device(void)
{
  if (alloc_mode == 1)
  {
    /* Sync both buf and descriptors */
    dma_sync_single_for_device(&nc_dev->dev, tx_buffer_dma, buffer_size, DMA_TO_DEVICE);
    // dma_sync_single_for_device(&nc_dev->dev, tx_ring_dma, TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMA_TO_DEVICE);
  }
}
/* Syncs cpu cache and memory so cpu can see new data */
static void sync_for_cpu(void)
{
  if (alloc_mode == 1)
  {
    dma_sync_single_for_cpu(&nc_dev->dev, tx_buffer_dma, buffer_size, DMA_TO_DEVICE);
    dma_sync_single_for_cpu(&nc_dev->dev, tx_ring_dma, TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMA_TO_DEVICE);
  }
}

static int ring_buffer_store(dma_addr_t phys_addr, size_t len)
{
  /* First we should find out, if
   * tail has not reaches head
   * from behind */
  while (TAIL_REACHED_HEAD(tx_tail))
  {
    if ((--timeout) == 0)
    {
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
  if (alloc_mode == 1)
  {
    dma_sync_single_for_device(&nc_dev->dev, tx_ring_dma, TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMA_TO_DEVICE);
  }
  tx_tail = (tx_tail + 1) % TX_RING_SIZE;
  /* Move real tail */
  iowrite32(tx_tail, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));
  return 0;
}

static int ring_buffer_send_data(size_t block_len)
{
  size_t offset = 0;
  while (block_len > 0)
  {
    /* We can copy not more than 1500 bytes at once */
    size_t chunk = min(block_len, (size_t)TX_PACKET_SIZE);

    /* Calculate physical address of the chunk inside tx_buffer */
    dma_addr_t chunk_dma = tx_buffer_dma + offset;

    int err = ring_buffer_store(chunk_dma, chunk);
    if (err)
    {
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
  while (total_len > 0)
  {
    size_t block = min(total_len, (size_t)buffer_size);
    sync_for_cpu();
    memset(tx_buffer, 0xAA, block);
    sync_for_device();
    err = ring_buffer_send_data(block);
    if (err)
    {
      printk(KERN_ERR "dmadriver: send_data failed\n");
      return err;
    }
    /* Reset timeout for each block */
    timeout = TIMEOUT_SIZE;
    while (ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH)) != tx_tail)
    {
      if ((--timeout) == 0)
      {
        printk(KERN_ERR "dmadriver: Timeout waiting for netcard\n");
        return -EIO;
      }
      cpu_relax();
    }
    sync_for_cpu();
    offset += block;
    total_len -= block;
  }
  return 0;
}

static int ring_buffer_memory_allocate(void)
{
  if (alloc_mode == 0)
  {
    /* Allocate memory for descriptor ring */
    tx_ring = dma_alloc_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), &tx_ring_dma, GFP_KERNEL);
    if (!tx_ring)
    {
      printk(KERN_ERR "dmadriver: dma_alloc_coherent failed\n");
      return -ENOMEM;
    }
    /* Allocate memory for ring buffer */
    tx_buffer = dma_alloc_coherent(&nc_dev->dev, buffer_size, &tx_buffer_dma, GFP_KERNEL);
    if (!tx_buffer)
    {
      printk(KERN_ERR "dmadriver: dma_alloc_coherent failed for tx_buffer\n");
      dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), tx_ring, tx_ring_dma);
      return -ENOMEM;
    }
  }
  else if (alloc_mode == 1)
  {
    /* Allocate simple memory */
    tx_ring = kmalloc(TX_RING_SIZE * sizeof(struct e1000_tx_desc), GFP_KERNEL);
    if (!tx_ring)
    {
      printk(KERN_ERR "dmadriver: kmalloc failed\n");
      return -ENOMEM;
    }
    /* Bind virtual address with physical address */
    tx_ring_dma = dma_map_single(&nc_dev->dev, tx_ring, TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMA_TO_DEVICE);
    if (dma_mapping_error(&nc_dev->dev, tx_ring_dma))
    {
      printk(KERN_ERR "dmadriver: dma_map_single failed for tx_ring\n");
      kfree(tx_ring);
      return -ENOMEM;
    }
    /* Allocate simple memory */
    tx_buffer = kmalloc(buffer_size, GFP_KERNEL);
    if (!tx_buffer)
    {
      printk(KERN_ERR "dmadriver: kmalloc failed\n");
      dma_unmap_single(&nc_dev->dev, tx_ring_dma, TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMA_TO_DEVICE);
      kfree(tx_ring);
      return -ENOMEM;
    }
    /* Bind virtual address with physical address */
    tx_buffer_dma = dma_map_single(&nc_dev->dev, tx_buffer, buffer_size, DMA_TO_DEVICE);
    if (dma_mapping_error(&nc_dev->dev, tx_buffer_dma))
    {
      printk(KERN_ERR "dmadriver: dma_map_single failed for tx_buffer\n");
      dma_unmap_single(&nc_dev->dev, tx_ring_dma, TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMA_TO_DEVICE);
      kfree(tx_ring);
      kfree(tx_buffer);
      return -ENOMEM;
    }
  }

  printk(KERN_INFO "dmadriver: Physical address for dma  = %pad", &tx_ring_dma);
  return 0;
}
/* Allocate memory for scatter-gather: ring, scatterlist, and data buffers */
static int sg_memory_allocate(void)
{
  /* 1. Allocate memory for descriptor ring (coherent, size = TX_RING_SIZE) */
  sg_tx_ring = dma_alloc_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), &sg_tx_ring_dma, GFP_KERNEL);
  if (!sg_tx_ring)
  {
    printk(KERN_ERR "dmadriver: dma_alloc_coherent failed for sg_tx_ring\n");
    return -ENOMEM;
  }
  memset(sg_tx_ring, 0, TX_RING_SIZE * sizeof(struct e1000_tx_desc));

  /* 2. Allocate array of pointers for SG buffers */
  sg_bufs = kcalloc(DMADRIVER_SG_MAX_SEGMENTS, sizeof(void *), GFP_KERNEL);
  if (!sg_bufs)
  {
    printk(KERN_ERR "dmadriver: kcalloc failed for sg_bufs\n");
    dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), sg_tx_ring, sg_tx_ring_dma);
    return -ENOMEM;
  }

  /* 3. Allocate scatterlist array */
  sg_list = kcalloc(DMADRIVER_SG_MAX_SEGMENTS, sizeof(struct scatterlist), GFP_KERNEL);
  if (!sg_list)
  {
    printk(KERN_ERR "dmadriver: kcalloc failed for sg_list\n");
    kfree(sg_bufs);
    dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), sg_tx_ring, sg_tx_ring_dma);
    return -ENOMEM;
  }

  /* 4. Allocate actual data buffers */
  for (int i = 0; i < DMADRIVER_SG_MAX_SEGMENTS; i++)
  {
    sg_bufs[i] = kmalloc(buffer_size, GFP_KERNEL);
    if (!sg_bufs[i])
    {
      printk(KERN_ERR "dmadriver: kmalloc failed for sg_bufs[%d]\n", i);
      /* Rollback */
      while (--i >= 0)
        kfree(sg_bufs[i]);
      kfree(sg_list);
      kfree(sg_bufs);
      dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), sg_tx_ring, sg_tx_ring_dma);
      return -ENOMEM;
    }
  }
  return 0;
}

static void sg_memory_free(void)
{
	if (sg_bufs) {
		for (int i = 0; i < DMADRIVER_SG_MAX_SEGMENTS; i++) {
			if (sg_bufs[i]) kfree(sg_bufs[i]);
		}
		kfree(sg_bufs);
	}
	if (sg_list) {
		kfree(sg_list);
	}
	if (sg_tx_ring) {
		dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), sg_tx_ring, sg_tx_ring_dma);
	}
}

static int sg_transfer_test(void)
{
	/* Number of segments limited by ring size (leave room for context and safety) */
	//int num_segments = TX_RING_SIZE - 2; 
	int nents, i, ret = 0;
	int timeout = TIMEOUT_SIZE;

	/* 1. Fill buffers (already allocated in __init) */
	for (i = 0; i < DMADRIVER_SG_MAX_SEGMENTS; i++) {
		memset(sg_bufs[i], 0x11 + i, buffer_size);
	}

	/* 2. Init and map scatterlist */
	sg_init_table(sg_list, DMADRIVER_SG_MAX_SEGMENTS);
	for (i = 0; i < DMADRIVER_SG_MAX_SEGMENTS; i++) {
		sg_set_buf(&sg_list[i], sg_bufs[i], buffer_size);
	}
	
	nents = dma_map_sg(&nc_dev->dev, sg_list, DMADRIVER_SG_MAX_SEGMENTS, DMA_TO_DEVICE);
	if (nents == 0) {
		printk(KERN_ERR "dmadriver: dma_map_sg returned 0\n");
		return -EFAULT;
	}

	/* 3. Context descriptor (index 0) */
	struct e1000_adv_tx_context_desc *ctx = (struct e1000_adv_tx_context_desc *)&sg_tx_ring[0];
	memset(ctx, 0, sizeof(struct e1000_adv_tx_context_desc));
	/* DEXT (29) | TSE (31) | DTYP (20) */
	ctx->type_tucmd_mlhl = cpu_to_le32(0x20000000 | 0x80000000 | 0x00100000);
	/* MSS (16-31) = 1500, L4LEN (8-15) = 20, IDX (0-7) = 0 */
	ctx->mss_l4len_idx = cpu_to_le32((1500 << 16) | (20 << 8) | 0);

	/* 4. Data descriptors */
	for (i = 1; i < nents + 1; i++) {
		u32 flags = 0x20000000 | 0x00100000 | 0x80000000; // DEXT | DTYP_D | TSE

		/* First packet needs IFCS */
		if (i == 1) {
			flags |= 0x02000000; // IFCS
		}
		/* Last descriptor needs EOP and RS */
		if (i == nents) {
			flags |= 0x01000000; // EOP
			flags |= 0x08000000; // RS
		}

		sg_tx_ring[i].buffer_addr = cpu_to_le64(sg_dma_address(&sg_list[i - 1]));
		sg_tx_ring[i].lower.data = cpu_to_le32(flags | cpu_to_le16(sg_dma_len(&sg_list[i - 1])));
		sg_tx_ring[i].upper.fields.status = 0;
	}

	/* 5. Reset ring and give command */
	iowrite32(0, DMADRIVER_PCI_FIELD(DMADRIVER_TDH));
	iowrite32(0, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));

	dma_wmb(); 
	dma_sync_sg_for_device(&nc_dev->dev, sg_list, nents, DMA_TO_DEVICE);

	/* Send: context (1) + data (nents) */
	iowrite32(nents + 1, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));

	/* 6. Wait for completion */
	while (ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH)) != (nents + 1)) {
		if ((--timeout) == 0) {
			printk(KERN_ERR "dmadriver: Extended SG timeout!\n");
			ret = -EIO;
			goto clean_all;
		}
		cpu_relax();
	}

	/* 7. Success */
	printk(KERN_INFO "dmadriver: Extended SG transfer SUCCESS! TDH = %d\n", nents + 1);

clean_all:
	dma_unmap_sg(&nc_dev->dev, sg_list, nents, DMA_TO_DEVICE);
	return ret;
}
static int dmadriver_configure_netcard(dma_addr_t ring_dma, u32 desc_count)
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
  // int err = ring_buffer_memory_allocate();
  // if (err) {

  // 	return err;
  // }
  /* Configure tx buffer parameters */

  /* Copy lower part of physical address */
  iowrite32((u32)ring_dma, DMADRIVER_PCI_FIELD(DMADRIVER_TDBAL));
  u32 data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDBAL));
  printk(KERN_INFO "dmadriver: new tdbal is 0x%x\n", data);
  /* Copy higher part of physical address */
  iowrite32((u32)(ring_dma >> 32), DMADRIVER_PCI_FIELD(DMADRIVER_TDBAH));
  data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDBAH));
  printk(KERN_INFO "dmadriver: new tdbah is 0x%x\n", data);
  /* Set ring buffer size.
   * It should be muliple of 128 bytes,
   * so with TX_RING_SIZE = 8 and
   * sizeof(e1000_tx_desc) = 16 its ok */
  iowrite32(desc_count * sizeof(struct e1000_tx_desc), DMADRIVER_PCI_FIELD(DMADRIVER_TDLEN));
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
  if (err)
  {
    printk(KERN_ERR "dmadriver: DMA benchmark failed with error %d\n", err);
    return;
  }

  end = ktime_get();
  time_ns = ktime_to_ns(ktime_sub(end, start));
  current_stats.dma_time_ns = time_ns;
  printk(KERN_INFO "dmadriver: DMA transfer of %zu bytes took %lld ns\n", size, time_ns);
  if (time_ns > 0)
  {
    u64 mbps = div_u64((u64)size * 1000, time_ns);
    current_stats.dma_speed_mbps = mbps;
    printk(KERN_INFO "dmadriver: DMA Throughput: ~%llu MB/s\n", mbps);
  }
  /* Save stats */
  atomic_inc(&total_transfers);
  atomic64_add(size, &total_bytes_sent);
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
  if (!dst)
  {
    printk(KERN_ERR "dmadriver: memcpy_benchmark: kmalloc failed for %zu bytes\n", size);
    return;
  }

  /* Start timer */
  start = ktime_get();

  /* We must copy block by block, because tx_buffer
   * is only buffer_size bytes long. If we try to
   * memcpy the whole size, we will read out of bounds */
  while (offset < size)
  {
    size_t block = min(size - offset, (size_t)buffer_size);
    /* Copy from our tx_buffer to the new dst buffer */
    memcpy(dst + offset, tx_buffer, block);
    offset += block;
  }

  end = ktime_get();
  time_ns = ktime_to_ns(ktime_sub(end, start));
  current_stats.memcpy_time_ns = time_ns;
  printk(KERN_INFO "dmadriver: memcpy of %zu bytes took %lld ns\n", size, time_ns);
  if (time_ns > 0)
  {
    u64 mbps = div_u64((u64)size * 1000, time_ns);
    current_stats.memcpy_speed_mbps = mbps;
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

  current_stats.size = size;

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
  if (buffer_size > (64 * 1024 * 1024))
  { // Например, не больше 64 МБ
    printk(KERN_ERR "dmadriver: buffer_size too large\n");
    return -EINVAL;
  }
  /* Create proc file */
  proc = proc_create("dmadriver", 0666, NULL, &ops);
  if (!proc)
  {
    printk(KERN_ERR "dmadriver: proc_create failed\n");
    return -ENOMEM;
  }
  /* Find network card
   * We look through the network card list
   * and compare their pci numbers with the
   * target */
  struct pci_dev *dev = NULL;
  while ((dev = pci_get_device(VENDOR_ID, DEVICE_ID, dev)))
  {
    if (strcmp(pci_name(dev), target_slot) == 0)
    {
      printk(KERN_INFO "dmadriver: Found netcard with number %s\n", target_slot);
      nc_dev = dev;
      break;
    }
    /* If its not our card, release it */
    pci_dev_put(dev);
  }
  if (!nc_dev)
  {
    printk(KERN_ERR "dmadriver: Target netcard not found\n");
    remove_proc_entry("dmadriver", NULL);
    return -ENODEV;
  }
  if (pci_enable_device(nc_dev))
  {
    printk(KERN_ERR "dmadriver: Failed to enable PCI device \n");
    remove_proc_entry("dmadriver", NULL);
    return -EIO;
  }
  pci_set_master(nc_dev);
  /* Occupy network card memory */
  int err = pci_request_regions(nc_dev, "dmadriver");
  if (err)
  {
    printk(KERN_ERR "dmadriver: pci_request_regions failed. Regions are occupied\n");
    pci_dev_put(nc_dev);
    remove_proc_entry("dmadriver", NULL);
    return err;
  }
  /* Get base register address */
  nc_base = pci_iomap(nc_dev, 0, 0);
  if (!nc_base)
  {
    printk(KERN_ERR "dmadriver: Failed to obtain virtual address\n");
    pci_release_regions(nc_dev);
    pci_dev_put(nc_dev);
    remove_proc_entry("dmadriver", NULL);
    return -ENOMEM;
  }
  /* Read netcard inital status */
  u32 status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
  printk(KERN_INFO "dmadriver: old PCI netcard status is 0x%x\n", status);
  if (alloc_mode == 2)
  {
    // Выделяем память под SG (sg_tx_ring, sg_part1, sg_part2)
    err = sg_memory_allocate();
    if (err)
      return err;

    // Настраиваем карту под SG-кольцо
    err = dmadriver_configure_netcard(sg_tx_ring_dma, TX_RING_SIZE);
    if (err)
      return err;

    // Запускаем тест
    sg_transfer_test();
  }
  else
  {
    // Твоя текущая логика для coherent/streaming
    err = ring_buffer_memory_allocate();
    if (err)
      return err;

    err = dmadriver_configure_netcard(tx_ring_dma, TX_RING_SIZE);
    if (err)
      return err;
  }
  status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
  printk(KERN_INFO "dmadriver: new PCI netcard status is 0x%x\n", status);
  printk(KERN_INFO "dmadriver: Driver mounted\n");
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
  if (alloc_mode == 0)
  {
    dma_free_coherent(&nc_dev->dev, buffer_size, tx_buffer, tx_buffer_dma);
    dma_free_coherent(&nc_dev->dev, TX_RING_SIZE * sizeof(struct e1000_tx_desc), tx_ring, tx_ring_dma);
  }
  else if (alloc_mode == 1)
  {
    dma_unmap_single(&nc_dev->dev, tx_buffer_dma, buffer_size, DMA_TO_DEVICE);
    kfree(tx_buffer);
    dma_unmap_single(&nc_dev->dev, tx_ring_dma, TX_RING_SIZE * sizeof(struct e1000_tx_desc), DMA_TO_DEVICE);
    kfree(tx_ring);
  }
  else if (alloc_mode == 2)
  {
    sg_memory_free();
  }

  /* 5. Unmap registers */
  pci_iounmap(nc_dev, nc_base);

  /* 6. Release regions and device */
  pci_release_regions(nc_dev);
  pci_dev_put(nc_dev);

  remove_proc_entry("dmadriver", NULL);
  printk(KERN_INFO "dmadriver: Driver unmounted\n");
}

/* Proc operations */
static int dmadriver_show_stats(struct seq_file *file, void *)
{
  /* Device info */
  seq_printf(file, "Device: VID=0x%04x, DID=0x%04x, BAR0=0x%llx\n",
             nc_dev->vendor,
             nc_dev->device,
             (unsigned long long)pci_resource_start(nc_dev, 0));
  seq_printf(file, "=== Last Transfer Stats ===\n");
  seq_printf(file, "Size: %zu bytes\n", current_stats.size);
  seq_printf(file, "DMA time: %lld ns (Speed: %llu MB/s)\n",
             current_stats.dma_time_ns, current_stats.dma_speed_mbps);
  seq_printf(file, "memcpy time: %lld ns (Speed: %llu MB/s)\n",
             current_stats.memcpy_time_ns, current_stats.memcpy_speed_mbps);
  seq_printf(file, "\n=== Totals ===\n");
  seq_printf(file, "Total transfers: %d\n", atomic_read(&total_transfers));
  seq_printf(file, "Total bytes: %lld\n", atomic64_read(&total_bytes_sent));
  return 0;
}

int dmadriver_open(struct inode *inode, struct file *filp)
{
  (void)inode;
  return single_open(filp, dmadriver_show_stats, NULL);
}

ssize_t dmadriver_write(struct file *filp, const char __user *buf, size_t len, loff_t *)
{
  unsigned int size = 0;
  int err = 0;
  err = kstrtoint_from_user(buf, len, 10, &size);
  if (err < 0)
  {
    printk(KERN_ERR "dmadriver: kstrtoint_from_user failed\n");
    return err;
  }
  /* Verify size */
  if (size <= 0 || size > MAX_MEM_SIZE)
  {
    printk(KERN_ERR "dmadriver: Invalid buffer size\n");
    return -EINVAL;
  }
  /* Reset counters */
  bytes_sent = 0;
  timeout = TIMEOUT_SIZE;
  dmadriver_run_benchmarks(size);
  return len;
}

module_init(dmadriver_init);
module_exit(dmadriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Shein");
MODULE_DESCRIPTION("DMA driver for Intel e1000 PCIe Ethernet controller");
