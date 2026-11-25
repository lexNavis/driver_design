#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/string.h>
#include <linux/processor.h>
#include <linux/minmax.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/bitops.h>

/* Prototypes */
static int dmadriver_open(struct inode *inode, struct file *filp);
static int dmadriver_release(struct inode *inode, struct file *filp);
static ssize_t dmadriver_write(struct file *filp, const char __user *buf,
                               size_t len, loff_t *ppos);

typedef int (*transfer_func_t)(size_t);

#define VENDOR_ID 0x8086
#define DEVICE_ID 0x100E
#define DMADRIVER_PCI_FIELD(offset) (nc_base + (offset))

/* Define offsets */
#define DMADRIVER_PCI_STATUS 0x0008
#define DMADRIVER_TCTL        0x0400
#define DMADRIVER_TIPG        0x0410
#define DMADRIVER_CTRL        0x0000

/* Transmit descriptor base address low */
#define DMADRIVER_TDBAL       0x3800
/* Transmit descriptor base address high */
#define DMADRIVER_TDBAH       0x3804
/* Size of descriptor buffer */
#define DMADRIVER_TDLEN       0x3808
/* Ring head */
#define DMADRIVER_TDH         0x3810
/* Ring tail */
#define DMADRIVER_TDT         0x3818

/* Device Reset bit: writing 1 to this bit initiates a global reset of the device */
#define CTRL_RST              BIT(26)

/* TCTL Register Bits */
/* Transmit Enable: enables the transmitter */
#define TCTL_EN_BIT           BIT(1)
/* Pad Short Packets: pads short packets to the minimum Ethernet frame length */
#define TCTL_PSP_BIT          BIT(3)

/* --- Legacy Data Descriptor Bits ---
 * These bits are stored in the command byte of a legacy TX descriptor.
 */
#define DMADRIVER_EOP         0x01
#define DMADRIVER_IFCS        0x02
#define DMADRIVER_RS          0x08

/* --- Extended Data Descriptor Bits ---
 * These bits are stored in the 32-bit lower.data field of an advanced TX descriptor.
 * A Context Descriptor is deliberately not used by this raw Scatter-Gather test:
 * the test payload is a sequence of fill bytes, not a valid TCP/IP super-packet.
 * TSO requires valid Ethernet/IP/TCP headers and matching checksum/header offsets.
 */
#define DATA_DTYP_D           0x00100000
#define DATA_DEXT             0x20000000
#define DATA_EOP              0x01000000
#define DATA_IFCS             0x02000000
#define DATA_RS               0x08000000

/* TIPG default magic number for gigabit Ethernet */
#define TIPG_DEFAULT          0x0060200A

/* Ring buffer size.
 * The e1000 TDLEN register must contain a byte length divisible by 128.
 * 1024 descriptors occupy 16 KiB, which satisfies that requirement.
 */
#define TX_RING_SIZE          2048
#define TX_PACKET_SIZE        1500
#define TX_TIMEOUT_MS         30000
#define MAX_MEM_SIZE          (1UL << 26)
#define SG_MAX_PAGES          (TX_RING_SIZE - 1)

/* Transmit descriptor.
 * The layout matches the legacy/advanced e1000 transmit descriptor format.
 */
struct e1000_tx_desc
{
    __le64 buffer_addr;
    union
    {
        __le32 data;
        struct
        {
            __le16 length;
            u8 cso;
            u8 cmd;
        } flags;
    } lower;
    union
    {
        __le32 data;
        struct
        {
            u8 status;
            u8 css;
            __le16 special;
        } fields;
    } upper;
};

/* Ptr to proc file */
static struct proc_dir_entry *proc;
static const struct proc_ops ops = {
    .proc_open = dmadriver_open,
    .proc_write = dmadriver_write,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = dmadriver_release,
};

static struct pci_dev *nc_dev;
static void __iomem *nc_base;

/* Descriptor ring virtual address and DMA address.
 * The descriptor ring is always coherent: the CPU updates descriptors while
 * the device reads them, so a streaming mapping would require extra ownership
 * transitions and cache synchronization for every descriptor update.
 */
static struct e1000_tx_desc *tx_ring;
static dma_addr_t tx_ring_dma;
static bool tx_ring_allocated;

/* Ring buffer virtual address and DMA address for coherent/streaming modes */
static u8 *tx_buffer;
static dma_addr_t tx_buffer_dma;
static bool tx_buffer_allocated;
static bool tx_buffer_mapped;

/* --- Scatter-Gather Globals ---
 * SG data is backed by individually allocated pages. No allocation requires
 * one physically contiguous region, which avoids high-order kmalloc requests.
 */
static struct page **sg_pages;
static struct scatterlist *sg_list;
static unsigned int sg_page_count;

/* Tail index in the descriptor ring */
static unsigned int tx_tail;

/* PCI resource state is tracked explicitly so every init failure follows the
 * same reverse-order cleanup path.
 */
static bool pci_enabled;
static bool pci_regions_requested;
static bool pci_master_enabled;
static bool bar_mapped;
static bool device_failed;

/* Serialize transfers, statistics updates and device shutdown */
static DEFINE_MUTEX(driver_lock);

/* Counters */
static atomic_t total_transfers = ATOMIC_INIT(0);
static atomic64_t total_bytes_sent = ATOMIC64_INIT(0);

/* Transfer result struct */
struct dma_stats
{
    size_t size;
    s64 dma_time_ns;
    u64 memcpy_time_ns;
    u64 dma_speed_mbps;
    u64 memcpy_speed_mbps;
};
static struct dma_stats current_stats;

/* Parameters
 * 0444 - read-only: changing allocation mode or buffer size after allocation
 * would make the matching unmap/free operation use the wrong parameters.
 */
static char *target_slot = "0000:00:06.0";
module_param(target_slot, charp, 0444);
MODULE_PARM_DESC(target_slot, "PCI slot of the target network card (e.g. 0000:00:06.0)");

static unsigned int buffer_size = (1U << 20);
module_param(buffer_size, uint, 0444);
MODULE_PARM_DESC(buffer_size, "DMA working-buffer size in bytes");

static unsigned int alloc_mode;
module_param(alloc_mode, uint, 0444);
MODULE_PARM_DESC(alloc_mode, "DMA allocation mode: 0 - coherent, 1 - streaming, 2 - scatter-gather");

/* Syncs the streaming buffer so the CPU can safely update it. */
static void sync_for_cpu(void)
{
    if (alloc_mode == 1 && tx_buffer_mapped)
        dma_sync_single_for_cpu(&nc_dev->dev, tx_buffer_dma,
                                buffer_size, DMA_TO_DEVICE);
}

/* Syncs the streaming buffer so the device can read the newly written data. */
static void sync_for_device(void)
{
    if (alloc_mode == 1 && tx_buffer_mapped)
        dma_sync_single_for_device(&nc_dev->dev, tx_buffer_dma,
                                   buffer_size, DMA_TO_DEVICE);
}

/* Waits until the netcard's TDH matches the expected tail index.
 * Returns 0 on success and -ETIMEDOUT if the transmit engine stops advancing.
 */
static int wait_for_tdh(unsigned int expected_tail)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(TX_TIMEOUT_MS);

    for (;;) {
        unsigned int head = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH));

        if (head == expected_tail)
            return 0;

        if (time_after_eq(jiffies, deadline))
            return -ETIMEDOUT;

        cpu_relax();
    }
}

/* Waits until the next descriptor slot is not owned by the device.
 * One ring entry is intentionally left unused to distinguish full from empty.
 */
static int wait_for_ring_space(void)
{
    unsigned int next_tail = (tx_tail + 1) % TX_RING_SIZE;
    unsigned long deadline = jiffies + msecs_to_jiffies(TX_TIMEOUT_MS);

    for (;;) {
        unsigned int head = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH));

        if (next_tail != head)
            return 0;

        if (time_after_eq(jiffies, deadline))
            return -ETIMEDOUT;

        cpu_relax();
    }
}

/* Stops and resets the controller before any DMA memory is unmapped.
 * A successful reset guarantees that the old transmit descriptor chain is
 * no longer being processed by the e1000 engine.
 */
static int netcard_reset_hw(void)
{
    u32 ctrl;
    unsigned long deadline;

    if (!nc_base)
        return 0;

    ctrl = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_CTRL));
    iowrite32(ctrl | CTRL_RST, DMADRIVER_PCI_FIELD(DMADRIVER_CTRL));
    (void)ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_CTRL));

    deadline = jiffies + msecs_to_jiffies(1000);
    for (;;) {
        ctrl = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_CTRL));
        if (!(ctrl & CTRL_RST)) {
            /* Allow reset side effects to settle before programming registers. */
            msleep(1);
            return 0;
        }

        if (time_after_eq(jiffies, deadline))
            return -ETIMEDOUT;

        usleep_range(1000, 2000);
    }
}

/* Enables the transmitter and configures padding for short packets. */
static void netcard_enable_transmitter(void)
{
    u32 tctl = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));

    tctl |= TCTL_EN_BIT | TCTL_PSP_BIT;
    iowrite32(tctl, DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
    (void)ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
}

/* Disables the transmitter before resetting the controller or releasing DMA memory. */
static void netcard_disable_transmitter(void)
{
    u32 tctl;

    if (!nc_base)
        return;

    tctl = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
    tctl &= ~TCTL_EN_BIT;
    iowrite32(tctl, DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
    (void)ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TCTL));
}

static int dmadriver_configure_netcard(dma_addr_t ring_dma, u32 desc_count)
{
    int err;
    u32 data;

    /* Reset the netcard to discard any previous transmit state. */
    netcard_disable_transmitter();
    err = netcard_reset_hw();
    if (err) {
        printk(KERN_ERR "dmadriver: device reset timed out\n");
        return err;
    }

    /* Clear software descriptors before exposing the ring to the device. */
    memset(tx_ring, 0, desc_count * sizeof(struct e1000_tx_desc));
    tx_tail = 0;

    /* Configure inter-packet gap with the standard e1000 default value. */
    iowrite32(TIPG_DEFAULT, DMADRIVER_PCI_FIELD(DMADRIVER_TIPG));

    /* Copy lower part of the DMA address of the descriptor ring. */
    iowrite32(lower_32_bits(ring_dma), DMADRIVER_PCI_FIELD(DMADRIVER_TDBAL));
    data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDBAL));
    printk(KERN_INFO "dmadriver: new tdbal is 0x%x\n", data);

    /* Copy higher part of the DMA address of the descriptor ring. */
    iowrite32(upper_32_bits(ring_dma), DMADRIVER_PCI_FIELD(DMADRIVER_TDBAH));
    data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDBAH));
    printk(KERN_INFO "dmadriver: new tdbah is 0x%x\n", data);

    /* Set ring size in bytes. The value is a multiple of 128 bytes. */
    iowrite32(desc_count * sizeof(struct e1000_tx_desc),
              DMADRIVER_PCI_FIELD(DMADRIVER_TDLEN));
    data = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDLEN));
    printk(KERN_INFO "dmadriver: new tdlen is 0x%x\n", data);

    /* TDH is reset by the controller reset; initialize only the software tail
     * and the device tail register. TDH is a hardware-owned register.
     */
    iowrite32(0, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));
    (void)ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDT));

    /* Enable transmission only after the ring base, length and tail are valid. */
    netcard_enable_transmitter();
    return 0;
}

/* Reinitializes the transmit engine after a timeout.
 * The DMA mapping is kept active until this reset has completed, so the device
 * cannot continue reading a buffer after dma_unmap_*() has been called.
 */
static int recover_netcard(void)
{
    int err;

    err = dmadriver_configure_netcard(tx_ring_dma, TX_RING_SIZE);
    if (err) {
        printk(KERN_ERR "dmadriver: failed to recover transmit engine\n");
        netcard_disable_transmitter();
        pci_clear_master(nc_dev);
        pci_master_enabled = false;
        if (pci_wait_for_pending_transaction(nc_dev))
            printk(KERN_ERR "dmadriver: PCI transactions remain pending after bus-master shutdown\n");
        device_failed = true;
        return err;
    }

    printk(KERN_WARNING "dmadriver: transmit engine was reset after timeout\n");
    return 0;
}

static int ring_buffer_store(dma_addr_t dma_addr, size_t len)
{
    struct e1000_tx_desc *desc;
    unsigned int next_tail;
    int err;

    if (!len || len > 0xFFFF)
        return -EINVAL;

    /* Make sure the next descriptor slot has already been released by hardware. */
    err = wait_for_ring_space();
    if (err)
        return err;

    desc = &tx_ring[tx_tail];
    memset(desc, 0, sizeof(*desc));

    /* Fill a legacy data descriptor with one complete Ethernet frame. */
    desc->buffer_addr = cpu_to_le64(dma_addr);
    desc->lower.flags.length = cpu_to_le16((u16)len);
    desc->lower.flags.cso = 0;
    desc->lower.flags.cmd = DMADRIVER_EOP | DMADRIVER_IFCS | DMADRIVER_RS;
    desc->upper.data = 0;

    next_tail = (tx_tail + 1) % TX_RING_SIZE;

    /* Ensure descriptor contents are visible before updating the device tail. */
    dma_wmb();
    iowrite32(next_tail, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));
    tx_tail = next_tail;

    return 0;
}

static int ring_buffer_send_data(size_t block_len)
{
    size_t offset = 0;

    while (block_len > 0) {
        /* A legacy e1000 data descriptor contains a 16-bit length field. */
        size_t chunk = min_t(size_t, block_len, TX_PACKET_SIZE);
        dma_addr_t chunk_dma = tx_buffer_dma + offset;
        int err;

        err = ring_buffer_store(chunk_dma, chunk);
        if (err) {
            printk(KERN_ERR "dmadriver: ring_buffer_store failed: %d\n", err);
            return err;
        }

        offset += chunk;
        block_len -= chunk;
    }

    return 0;
}

static int ring_buffer_transfer_data(size_t total_len)
{
    size_t offset = 0;

    while (total_len > 0) {
        size_t block = min_t(size_t, total_len, buffer_size);
        int err;

        /* For streaming mappings, ownership returns to the CPU before refill. */
        sync_for_cpu();
        memset(tx_buffer, 0xAA, block);
        sync_for_device();

        err = ring_buffer_send_data(block);
        if (err) {
            printk(KERN_ERR "dmadriver: send_data failed: %d\n", err);
            if (err == -ETIMEDOUT)
                recover_netcard();
            return err;
        }

        /* Do not reuse the buffer until every descriptor in this block is done. */
        err = wait_for_tdh(tx_tail);
        if (err) {
            printk(KERN_ERR "dmadriver: timeout waiting for netcard\n");
            recover_netcard();
            return err;
        }

        sync_for_cpu();
        offset += block;
        total_len -= block;
    }

    return 0;
}

/* Releases every allocation made by ring_buffer_memory_allocate().
 * The flags distinguish valid DMA mappings from DMA addresses, because DMA
 * address zero is not a reliable indication that a mapping was not created.
 */
static void ring_buffer_memory_free(void)
{
    unsigned int i;

    if (!nc_dev)
        return;

    if (alloc_mode == 0) {
        if (tx_buffer_allocated) {
            dma_free_coherent(&nc_dev->dev, buffer_size,
                              tx_buffer, tx_buffer_dma);
            tx_buffer_allocated = false;
            tx_buffer = NULL;
        }
    } else if (alloc_mode == 1) {
        if (tx_buffer_mapped) {
            dma_unmap_single(&nc_dev->dev, tx_buffer_dma,
                             buffer_size, DMA_TO_DEVICE);
            tx_buffer_mapped = false;
        }
        if (tx_buffer_allocated) {
            kfree(tx_buffer);
            tx_buffer_allocated = false;
            tx_buffer = NULL;
        }
    } else if (alloc_mode == 2) {
        if (sg_pages) {
            for (i = 0; i < sg_page_count; i++) {
                if (sg_pages[i])
                    __free_page(sg_pages[i]);
            }
            kfree(sg_pages);
            sg_pages = NULL;
        }

        kfree(sg_list);
        sg_list = NULL;
        sg_page_count = 0;
    }

    if (tx_ring_allocated) {
        dma_free_coherent(&nc_dev->dev,
                          TX_RING_SIZE * sizeof(struct e1000_tx_desc),
                          tx_ring, tx_ring_dma);
        tx_ring_allocated = false;
        tx_ring = NULL;
    }
}

static int ring_buffer_memory_allocate(void)
{
    unsigned int i;

    /* The descriptor ring is coherent in all modes. */
    tx_ring = dma_alloc_coherent(&nc_dev->dev,
                                 TX_RING_SIZE * sizeof(struct e1000_tx_desc),
                                 &tx_ring_dma, GFP_KERNEL);
    if (!tx_ring) {
        printk(KERN_ERR "dmadriver: dma_alloc_coherent failed for tx_ring\n");
        return -ENOMEM;
    }
    tx_ring_allocated = true;
    memset(tx_ring, 0, TX_RING_SIZE * sizeof(struct e1000_tx_desc));

    if (alloc_mode == 0) {
        tx_buffer = dma_alloc_coherent(&nc_dev->dev, buffer_size,
                                       &tx_buffer_dma, GFP_KERNEL);
        if (!tx_buffer) {
            printk(KERN_ERR "dmadriver: dma_alloc_coherent failed for tx_buffer\n");
            goto err_out;
        }
        tx_buffer_allocated = true;
    } else if (alloc_mode == 1) {
        tx_buffer = kmalloc(buffer_size, GFP_KERNEL);
        if (!tx_buffer) {
            printk(KERN_ERR "dmadriver: kmalloc failed for tx_buffer\n");
            goto err_out;
        }
        tx_buffer_allocated = true;

        tx_buffer_dma = dma_map_single(&nc_dev->dev, tx_buffer,
                                       buffer_size, DMA_TO_DEVICE);
        if (dma_mapping_error(&nc_dev->dev, tx_buffer_dma)) {
            printk(KERN_ERR "dmadriver: dma_map_single failed for tx_buffer\n");
            tx_buffer_dma = 0;
            goto err_out;
        }
        tx_buffer_mapped = true;
    } else if (alloc_mode == 2) {
        /* Each SG element is backed by one independently allocated page.
         * The maximum number of pages is bounded by the descriptor ring size,
         * so every page can be represented even when the DMA API cannot merge
         * adjacent pages into a larger DMA segment.
         */
        sg_page_count = DIV_ROUND_UP(buffer_size, PAGE_SIZE);
        if (!sg_page_count || sg_page_count > SG_MAX_PAGES) {
            printk(KERN_ERR "dmadriver: SG buffer requires %u pages; maximum is %u\n",
                   sg_page_count, SG_MAX_PAGES);
            goto err_out;
        }

        sg_pages = kcalloc(sg_page_count, sizeof(*sg_pages), GFP_KERNEL);
        if (!sg_pages) {
            printk(KERN_ERR "dmadriver: kcalloc failed for sg_pages\n");
            goto err_out;
        }

        sg_list = kcalloc(sg_page_count, sizeof(*sg_list), GFP_KERNEL);
        if (!sg_list) {
            printk(KERN_ERR "dmadriver: kcalloc failed for sg_list\n");
            goto err_out;
        }

        for (i = 0; i < sg_page_count; i++) {
            sg_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
            if (!sg_pages[i]) {
                printk(KERN_ERR "dmadriver: alloc_page failed for sg_pages[%u]\n", i);
                goto err_out;
            }
        }
    } else {
        printk(KERN_ERR "dmadriver: invalid alloc_mode %u\n", alloc_mode);
        goto err_out;
    }

    printk(KERN_INFO "dmadriver: TX ring DMA address = %pad\n", &tx_ring_dma);
    return 0;

err_out:
    /* Partial allocations are released through the same mode-aware function. */
    ring_buffer_memory_free();
    return -ENOMEM;
}

/* Writes advanced data descriptors for the mapped SG list.
 * Each test packet is kept within TX_PACKET_SIZE bytes, so the e1000 model
 * receives frame-sized transfers rather than one oversized 1-2 MiB frame.
 * The fill pattern is not intended to be a valid TCP/IP packet; therefore no
 * TSO context is attached. A frame may span several DMA segments; EOP is set
 * on the final fragment of each frame, not just at the end of the benchmark.
 */
static int sg_build_data_descs(size_t block, int mapped_nents,
                               unsigned int *new_tail,
                               unsigned int *desc_count)
{
    struct scatterlist *sg;
    size_t bytes_left = block;
    size_t packet_left = min_t(size_t, block, TX_PACKET_SIZE);
    unsigned int index = tx_tail;
    unsigned int count = 0;
    int i;

    for_each_sg(sg_list, sg, mapped_nents, i) {
        dma_addr_t dma_addr = sg_dma_address(sg);
        unsigned int remaining = sg_dma_len(sg);

        while (remaining && bytes_left) {
            unsigned int chunk = min_t(size_t, remaining, packet_left);
            bool end_of_packet = (chunk == packet_left);
            struct e1000_tx_desc *desc;
            u32 command = DATA_DEXT | DATA_DTYP_D;

            if (count >= TX_RING_SIZE - 1)
                return -E2BIG;

            if (end_of_packet)
                command |= DATA_EOP | DATA_IFCS | DATA_RS;

            desc = &tx_ring[index];
            memset(desc, 0, sizeof(*desc));
            desc->buffer_addr = cpu_to_le64(dma_addr);
            desc->lower.data = cpu_to_le32(command | (chunk & 0xFFFF));
            desc->upper.data = 0;

            index = (index + 1) % TX_RING_SIZE;
            count++;
            dma_addr += chunk;
            remaining -= chunk;
            bytes_left -= chunk;
            packet_left -= chunk;

            if (!packet_left && bytes_left)
                packet_left = min_t(size_t, bytes_left, TX_PACKET_SIZE);
        }
    }

    if (bytes_left || !count)
        return -EINVAL;

    *new_tail = index;
    *desc_count = count;
    return 0;
}

static int sg_transfer(size_t size)
{
    size_t offset = 0;
    unsigned int batch_number = 0;

    while (offset < size) {
        size_t block = min_t(size_t, size - offset, buffer_size);
        unsigned int orig_nents = DIV_ROUND_UP(block, PAGE_SIZE);
        unsigned int desc_count;
        unsigned int new_tail;
        size_t page_offset = 0;
        int mapped_nents;
        int err;
        unsigned int i;

        /* 1. Fill independently allocated pages for this transfer batch. */
        for (i = 0; i < orig_nents; i++) {
            size_t page_len = min_t(size_t, PAGE_SIZE, block - page_offset);
            void *page_addr = kmap_local_page(sg_pages[i]);

            memset(page_addr, 0x11 + ((batch_number + i) & 0x7F), page_len);
            kunmap_local(page_addr);
            page_offset += page_len;
        }

        /* 2. Build the original scatterlist and map it for device access. */
        sg_init_table(sg_list, orig_nents);
        page_offset = 0;
        for (i = 0; i < orig_nents; i++) {
            unsigned int page_len = min_t(size_t, PAGE_SIZE, block - page_offset);

            sg_set_page(&sg_list[i], sg_pages[i], page_len, 0);
            page_offset += page_len;
        }

        mapped_nents = dma_map_sg(&nc_dev->dev, sg_list, orig_nents,
                                  DMA_TO_DEVICE);
        if (!mapped_nents) {
            printk(KERN_ERR "dmadriver: dma_map_sg failed for %u entries\n",
                   orig_nents);
            return -EIO;
        }

        /* 3. Build packet-sized Extended Data descriptors from the mapped SG.
         * The original scatterlist count is retained for dma_unmap_sg(); the
         * mapped count is used only to walk the DMA segments returned by DMA API.
         */
        err = sg_build_data_descs(block, mapped_nents,
                                  &new_tail, &desc_count);
        if (err) {
            printk(KERN_ERR "dmadriver: failed to build SG descriptors: %d\n", err);
            dma_unmap_sg(&nc_dev->dev, sg_list, orig_nents, DMA_TO_DEVICE);
            return err;
        }

        /* The previous batch is complete before this one starts, so all ring
         * entries are free. The descriptor builder leaves one entry unused.
         */
        dma_wmb();
        iowrite32(new_tail, DMADRIVER_PCI_FIELD(DMADRIVER_TDT));
        tx_tail = new_tail;

        /* 4. Wait for the device to consume the complete SG batch. */
        err = wait_for_tdh(new_tail);
        if (err) {
            printk(KERN_ERR "dmadriver: Extended SG timeout\n");
            recover_netcard();
            dma_unmap_sg(&nc_dev->dev, sg_list, orig_nents, DMA_TO_DEVICE);
            return err;
        }

        /* No CPU access occurred after dma_map_sg(), so no sync operation is
         * needed. Unmapping returns the pages to CPU ownership for refilling.
         */
        dma_unmap_sg(&nc_dev->dev, sg_list, orig_nents, DMA_TO_DEVICE);

        printk(KERN_INFO "dmadriver: SG batch %u completed: %zu bytes, original entries=%u, DMA entries=%d, TX descriptors=%u\n",
               batch_number, block, orig_nents, mapped_nents, desc_count);
        offset += block;
        batch_number++;
    }

    printk(KERN_INFO "dmadriver: Extended SG transfer SUCCESS, bytes=%zu, TDH=%u\n",
           size, ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_TDH)));
    return 0;
}

static int dmadriver_benchmark(size_t size, transfer_func_t func)
{
    u64 start_ns;
    u64 end_ns;
    s64 time_ns;
    int err;

    printk(KERN_INFO "dmadriver: --- DMA Benchmark for %zu bytes ---\n", size);
    start_ns = ktime_get_ns();

    err = func(size);
    if (err) {
        printk(KERN_ERR "dmadriver: DMA benchmark failed with error %d\n", err);
        current_stats.dma_time_ns = -1;
        current_stats.dma_speed_mbps = 0;
        return err;
    }

    end_ns = ktime_get_ns();
    time_ns = (s64)(end_ns - start_ns);
    current_stats.dma_time_ns = time_ns;
    current_stats.dma_speed_mbps = 0;

    printk(KERN_INFO "dmadriver: DMA transfer of %zu bytes took %lld ns\n",
           size, time_ns);
    if (time_ns > 0) {
        u64 mbps = div_u64((u64)size * 1000, time_ns);
        current_stats.dma_speed_mbps = mbps;
        printk(KERN_INFO "dmadriver: DMA Throughput: ~%llu MB/s\n", mbps);
    }

    /* Update totals only after the full transfer completed successfully. */
    atomic_inc(&total_transfers);
    atomic64_add(size, &total_bytes_sent);
    return 0;
}

static void memcpy_benchmark(void *src, void *dst, size_t size)
{
    u64 start_ns;
    u64 end_ns;
    s64 time_ns;

    printk(KERN_INFO "dmadriver: --- memcpy Benchmark for %zu bytes ---\n", size);

    start_ns = ktime_get_ns();
    memcpy(dst, src, size);
    end_ns = ktime_get_ns();

    time_ns = (s64)(end_ns - start_ns);
    current_stats.memcpy_time_ns = time_ns;
    current_stats.memcpy_speed_mbps = 0;

    printk(KERN_INFO "dmadriver: memcpy of %zu bytes took %lld ns\n",
           size, time_ns);
    if (time_ns > 0) {
        u64 mbps = div_u64((u64)size * 1000, time_ns);
        current_stats.memcpy_speed_mbps = mbps;
        printk(KERN_INFO "dmadriver: memcpy Throughput: ~%llu MB/s\n", mbps);
    }
}

static int dmadriver_run_benchmarks(size_t size)
{
    void *src = NULL;
    void *dst = NULL;
    int memcpy_ready = 0;
    int err;

    printk(KERN_INFO "dmadriver: ========================================\n");
    printk(KERN_INFO "dmadriver: Starting benchmarks for %zu bytes\n", size);
    printk(KERN_INFO "dmadriver: ========================================\n");

    current_stats.size = size;
    current_stats.memcpy_time_ns = 0;
    current_stats.memcpy_speed_mbps = 0;

    /* Prepare independent virtual buffers for the CPU memcpy comparison. */
    src = vmalloc(size);
    dst = vmalloc(size);
    if (src && dst) {
        memset(src, 0xAA, size);
        memcpy_ready = 1;
    } else {
        printk(KERN_ERR "dmadriver: vmalloc failed, skipping memcpy test\n");
        vfree(src);
        vfree(dst);
        src = NULL;
        dst = NULL;
    }

    /* Run the selected DMA allocation mode. */
    if (device_failed) {
        err = -EIO;
    } else if (alloc_mode == 2) {
        err = dmadriver_benchmark(size, sg_transfer);
    } else {
        err = dmadriver_benchmark(size, ring_buffer_transfer_data);
    }

    /* The memcpy benchmark is independent of DMA completion. */
    if (memcpy_ready) {
        memcpy_benchmark(src, dst, size);
        vfree(dst);
        vfree(src);
    }

    printk(KERN_INFO "dmadriver: ========================================\n");
    printk(KERN_INFO "dmadriver: Benchmarks finished\n");
    printk(KERN_INFO "dmadriver: ========================================\n");
    return err;
}

/* Releases PCI and DMA resources in reverse allocation order.
 * The transmit engine is stopped and reset before any DMA buffer is freed.
 */
static void dmadriver_cleanup(void)
{
    if (proc) {
        remove_proc_entry("dmadriver", NULL);
        proc = NULL;
    }

    if (nc_dev && nc_base) {
        netcard_disable_transmitter();
        if (netcard_reset_hw())
            printk(KERN_ERR "dmadriver: device reset did not complete during cleanup\n");
    }

    if (nc_dev && pci_master_enabled) {
        pci_clear_master(nc_dev);
        pci_master_enabled = false;
        if (pci_wait_for_pending_transaction(nc_dev))
            printk(KERN_ERR "dmadriver: PCI transactions remain pending during cleanup\n");
        /* Allow any already-issued bus transactions to drain before freeing DMA memory. */
        msleep(10);
    }

    ring_buffer_memory_free();

    if (nc_dev && bar_mapped) {
        pci_iounmap(nc_dev, nc_base);
        nc_base = NULL;
        bar_mapped = false;
    }

    if (nc_dev && pci_regions_requested) {
        pci_release_regions(nc_dev);
        pci_regions_requested = false;
    }

    if (nc_dev && pci_enabled) {
        pci_disable_device(nc_dev);
        pci_enabled = false;
    }

    if (nc_dev) {
        pci_dev_put(nc_dev);
        nc_dev = NULL;
    }
}

static int __init dmadriver_init(void)
{
    struct pci_dev *dev = NULL;
    int err;
    u32 status;

    printk(KERN_INFO "dmadriver: Driver mounting\n");

    /* Validate all module parameters before acquiring PCI or DMA resources. */
    if (!buffer_size || buffer_size > MAX_MEM_SIZE) {
        printk(KERN_ERR "dmadriver: buffer_size must be between 1 and %lu bytes\n",
               (unsigned long)MAX_MEM_SIZE);
        return -EINVAL;
    }

    if (alloc_mode > 2) {
        printk(KERN_ERR "dmadriver: alloc_mode must be 0, 1 or 2\n");
        return -EINVAL;
    }

    /* Bound the worst-case descriptor count before allocating SG pages.
     * Page boundaries can split mapped DMA segments, while each 1500-byte
     * packet boundary can split them again. The sum is a conservative upper
     * bound that keeps one ring entry unused to distinguish full from empty.
     */
    if (alloc_mode == 2 &&
        DIV_ROUND_UP(buffer_size, PAGE_SIZE) +
        DIV_ROUND_UP(buffer_size, TX_PACKET_SIZE) > TX_RING_SIZE - 1) {
        printk(KERN_ERR "dmadriver: SG buffer_size %u needs too many descriptors for the TX ring\n",
               buffer_size);
        return -EINVAL;
    }

    /* Find the requested Intel e1000 PCI function. pci_get_device() releases
     * the previous iterator reference on the next call; the loop must not put
     * non-matching devices manually.
     */
    while ((dev = pci_get_device(VENDOR_ID, DEVICE_ID, dev))) {
        if (!strcmp(pci_name(dev), target_slot)) {
            nc_dev = dev;
            break;
        }
    }

    if (!nc_dev) {
        printk(KERN_ERR "dmadriver: target netcard %s not found\n", target_slot);
        return -ENODEV;
    }

    err = pci_enable_device(nc_dev);
    if (err) {
        printk(KERN_ERR "dmadriver: pci_enable_device failed: %d\n", err);
        goto err_out;
    }
    pci_enabled = true;

    /* The e1000 descriptor format contains 64-bit DMA addresses. Prefer a
     * 64-bit DMA mask, but fall back to 32-bit addressing if the platform or
     * the PCI function cannot provide a 64-bit DMA window.
     */
    err = dma_set_mask_and_coherent(&nc_dev->dev, DMA_BIT_MASK(64));
    if (err) {
        err = dma_set_mask_and_coherent(&nc_dev->dev, DMA_BIT_MASK(32));
        if (err) {
            printk(KERN_ERR "dmadriver: neither 64-bit nor 32-bit DMA is available\n");
            goto err_out;
        }
        printk(KERN_INFO "dmadriver: using 32-bit DMA addressing\n");
    } else {
        printk(KERN_INFO "dmadriver: using 64-bit DMA addressing\n");
    }

    /* Occupy the PCI BAR resources before mapping the device registers. */
    err = pci_request_regions(nc_dev, "dmadriver");
    if (err) {
        printk(KERN_ERR "dmadriver: pci_request_regions failed: %d\n", err);
        goto err_out;
    }
    pci_regions_requested = true;

    /* Get the memory-mapped register window. */
    nc_base = pci_iomap(nc_dev, 0, 0);
    if (!nc_base) {
        printk(KERN_ERR "dmadriver: failed to map BAR0\n");
        err = -ENOMEM;
        goto err_out;
    }
    bar_mapped = true;

    /* Enable PCI bus mastering only after DMA addressing has been configured. */
    pci_set_master(nc_dev);
    pci_master_enabled = true;

    status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
    printk(KERN_INFO "dmadriver: old device status is 0x%x\n", status);

    err = ring_buffer_memory_allocate();
    if (err)
        goto err_out;

    err = dmadriver_configure_netcard(tx_ring_dma, TX_RING_SIZE);
    if (err)
        goto err_out;

    status = ioread32(DMADRIVER_PCI_FIELD(DMADRIVER_PCI_STATUS));
    printk(KERN_INFO "dmadriver: new device status is 0x%x\n", status);

    /* Publish the proc interface only after the device and all DMA buffers are
     * fully initialized. An open proc entry can therefore never observe a
     * partially initialized device.
     */
    proc = proc_create("dmadriver", 0666, NULL, &ops);
    if (!proc) {
        printk(KERN_ERR "dmadriver: proc_create failed\n");
        err = -ENOMEM;
        goto err_out;
    }

    printk(KERN_INFO "dmadriver: Driver mounted\n");
    return 0;

err_out:
    dmadriver_cleanup();
    return err;
}

static void __exit dmadriver_exit(void)
{
    printk(KERN_INFO "dmadriver: Driver unmounting\n");

    /* Remove the entry first, then wait for an already-running benchmark to
     * finish before stopping DMA and freeing its buffers.
     */
    if (proc) {
        remove_proc_entry("dmadriver", NULL);
        proc = NULL;
    }

    mutex_lock(&driver_lock);
    dmadriver_cleanup();
    mutex_unlock(&driver_lock);

    printk(KERN_INFO "dmadriver: Driver unmounted\n");
}

/* Proc operations */
static int dmadriver_show_stats(struct seq_file *file, void *unused)
{
    (void)unused;

    mutex_lock(&driver_lock);
    if (nc_dev) {
        seq_printf(file, "Device: VID=0x%04x, DID=0x%04x, BAR0=0x%llx\n",
                   nc_dev->vendor,
                   nc_dev->device,
                   (unsigned long long)pci_resource_start(nc_dev, 0));
    }
    seq_printf(file, "=== Last Transfer Stats ===\n");
    seq_printf(file, "Size: %zu bytes\n", current_stats.size);
    seq_printf(file, "DMA time: %lld ns (Speed: %llu MB/s)\n",
               current_stats.dma_time_ns, current_stats.dma_speed_mbps);
    seq_printf(file, "memcpy time: %llu ns (Speed: %llu MB/s)\n",
               current_stats.memcpy_time_ns, current_stats.memcpy_speed_mbps);
    seq_printf(file, "\n=== Totals ===\n");
    seq_printf(file, "Total transfers: %d\n", atomic_read(&total_transfers));
    seq_printf(file, "Total bytes: %lld\n", atomic64_read(&total_bytes_sent));
    mutex_unlock(&driver_lock);

    return 0;
}

static int dmadriver_open(struct inode *inode, struct file *filp)
{
    int err;

    (void)inode;

    /* proc_ops has no owner field, so open files hold an explicit module ref. */
    if (!try_module_get(THIS_MODULE))
        return -ENODEV;

    err = single_open(filp, dmadriver_show_stats, NULL);
    if (err)
        module_put(THIS_MODULE);

    return err;
}

static int dmadriver_release(struct inode *inode, struct file *filp)
{
    int err = single_release(inode, filp);

    module_put(THIS_MODULE);
    return err;
}

static ssize_t dmadriver_write(struct file *filp, const char __user *buf,
                               size_t len, loff_t *ppos)
{
    int requested_size = 0;
    int err;

    (void)filp;
    (void)ppos;

    err = kstrtoint_from_user(buf, len, 10, &requested_size);
    if (err < 0) {
        printk(KERN_ERR "dmadriver: kstrtoint_from_user failed: %d\n", err);
        return err;
    }

    /* Verify size before allocating the independent memcpy benchmark buffers. */
    if (requested_size <= 0 || requested_size > MAX_MEM_SIZE) {
        printk(KERN_ERR "dmadriver: invalid transfer size %d\n", requested_size);
        return -EINVAL;
    }

    mutex_lock(&driver_lock);
    if (device_failed) {
        err = -EIO;
    } else {
        err = dmadriver_run_benchmarks((size_t)requested_size);
    }
    mutex_unlock(&driver_lock);

    if (err)
        return err;

    return len;
}

module_init(dmadriver_init);
module_exit(dmadriver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis Shein");
MODULE_DESCRIPTION("DMA driver for Intel e1000 PCI Ethernet controller");
