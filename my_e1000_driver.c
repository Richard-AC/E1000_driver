#include <linux/kernel.h> 
#include <linux/module.h>
#include <linux/fs.h>
#include <asm/uaccess.h> 
#include <asm/io.h>  /* for ioremap and iounmap */
#include <linux/delay.h>  /* for msleep */
#include <linux/slab.h>

#include "e1k_utils.h"
#define NB_MAX_DESC 256

#define DEVICE_NAME "my_e1000_driver_file"
#define MAJOR_NUM 100
#define DEVICE_FILE_NAME "char_dev"

// Functions
static uint8_t * map_mmio(void);
static void e1k_configure(void);
static void send_data(void);

// Globals
uint8_t * bar0, * tx_buffer;
struct e1000_desc * tx_ring;

/* Is the device open right now? Used to prevent
 * concurent access into the same device */
static int Device_Open = 0;

/* This is called whenever a process attempts to open the device file */
static int device_open(struct inode *inode, struct file *file)
{
    /* We don't want to talk to two processes at the same time */
    if (Device_Open)
        return -EBUSY;

    Device_Open++;
    try_module_get(THIS_MODULE);
    return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
    /* We're now ready for our next caller */
    Device_Open--;

    module_put(THIS_MODULE);
    return 0;
}


void kernel_sleep(uint32_t ms){
    msleep(ms);
}

/* Module Declarations */
struct file_operations Fops = {
    .open = device_open,
    .release = device_release,    
};

/* Initialize the module - Register the character device */
int init_module()
{
    int ret_val;
    ret_val = register_chrdev(MAJOR_NUM, DEVICE_NAME, &Fops);

    if (ret_val < 0) {
        printk(KERN_ALERT "%s failed with %d\n",
               "Sorry, registering the character device ", ret_val);
        return ret_val;
    }

    pr_info("%s The major device number is %d.\n",
           "Registeration is a success", MAJOR_NUM);
    pr_info("If you want to talk to the device driver,\n");
    pr_info("you'll have to create a device file. \n");
    pr_info("We suggest you use:\n");
    pr_info("mknod %s c %d 0\n", DEVICE_FILE_NAME, MAJOR_NUM);

    // Configure device
    bar0 = map_mmio();
    if (!bar0) {
        pr_info("e1k : failed to map mmio");
        return -1;
    }
    e1k_configure();

    send_data();

    return 0;
}

/* Cleanup - unregister the appropriate file from /proc */
void cleanup_module()
{
    unregister_chrdev(MAJOR_NUM, DEVICE_NAME);
}

static uint8_t * map_mmio(void)
{
	off_t phys_addr = 0xF0200000;
	size_t length = 0x20000;

	uint8_t* virt_addr = ioremap(phys_addr, length);
	if (!virt_addr) {
		pr_info("e1k : ioremap failed to map MMIO\n");
		return NULL;
	}

	return virt_addr;
}

static void e1k_configure(void)
{
	// Configure general purpose registers
	uint32_t ctrl, tctl, tdlen;
	uint64_t tdba;
	int i;

	ctrl = get_register(CTRL) | CTRL_RST;
	set_register(CTRL, ctrl);

	ctrl = get_register(CTRL) | CTRL_ASDE | CTRL_SLU | CTRL_FD;
	set_register(CTRL, ctrl);

	// Configure TX registers
	tx_ring = kmalloc(DESC_SIZE * NB_MAX_DESC, GFP_KERNEL);
	if (!tx_ring) {
		pr_info("e1k : failed to allocate TX Ring\n");
		return;
	}
	// Transmit setup
	for (i = 0; i < NB_MAX_DESC; ++i) {
		tx_ring[i].ctxt.cmd_and_length = DESC_DONE;
	}

	tx_buffer = kmalloc(PAYLOAD_LEN + 0x1000, GFP_KERNEL);
	if (!tx_buffer) {
		pr_info("e1k : failed to allocate TX Buffer\n");
		return;
	}

	tdba = (uint64_t)((uintptr_t) virt_to_phys(tx_ring));
	set_register(TDBAL, (uint32_t) ((tdba & 0xFFFFFFFFULL)));
	set_register(TDBAH, (uint32_t) (tdba >> 32));

	tdlen = DESC_SIZE * NB_MAX_DESC;
	set_register(TDLEN, tdlen);

	set_register(TDT, 0);
	set_register(TDH, 0);

	tctl = get_register(TCTL) | TCTL_EN | TCTL_PSP | ((0x40 << 12) & TCTL_COLD) | ((0x10 << 8) & TCTL_CT) | TCTL_RTLC;
	set_register(TCTL, tctl);
}

static void send_data(void)
{
	int i;
	uint32_t	tdt;
	uint64_t 	physical_address;

	struct e1000_ctxt_desc*	ctxt_1 = &(tx_ring[0].ctxt);
	struct e1000_data_desc*	data_2 = &(tx_ring[1].data);
	struct e1000_data_desc*	data_3 = &(tx_ring[2].data);
	struct e1000_data_desc*	data_4 = &(tx_ring[3].data);
	struct e1000_data_desc*	data_5 = &(tx_ring[4].data);

	for (i = 0; i < PAYLOAD_LEN; ++i) {
		tx_buffer[i] = 0x41; // Fill with 'A'
	}

	physical_address = virt_to_phys(tx_buffer);

	ctxt_1->lower_setup.ip_config	= (uint32_t) 0;
	ctxt_1->upper_setup.tcp_config	= (uint32_t) 0;
	ctxt_1->cmd_and_length		= (uint32_t) (TCP_IP | REPORT_STATUS | DESC_CTX | TSE | PAYLOAD_LEN);
	ctxt_1->tcp_seg_setup.data	= (uint32_t) (E1K_MAX_TX_PKT_SIZE - 4 - 1);

	data_2->buffer_addr		= (uint64_t) physical_address;
	data_2->lower.data		= (uint32_t) (REPORT_STATUS | DESC_DATA | 0x10 | TSE);
	data_2->lower.data		= (uint32_t) (REPORT_STATUS | DESC_DATA | (E1K_MAX_TX_PKT_SIZE - 4 - 2) | TSE);
	data_2->upper.data		= (uint32_t) 0;

	data_3->buffer_addr		= (uint64_t) physical_address;
	data_2->lower.data		= (uint32_t) (REPORT_STATUS | DESC_DATA | 2);
	data_3->upper.data		= (uint32_t) 0;

	data_4->buffer_addr		= (uint64_t) physical_address;
	data_2->lower.data		= (uint32_t) (REPORT_STATUS | DESC_DATA | 0x1000 | TSE);
	data_4->upper.data		= (uint32_t) 0;

	data_5->buffer_addr		= (uint64_t) physical_address;
	data_5->lower.data		= (uint32_t) (EOP | REPORT_STATUS | DESC_DATA | TSE);
	data_5->upper.data		= (uint32_t) 0;

	tdt = (get_register(TDT) + 5) & 0xFFFF;
	set_register(TDT, tdt);
}

