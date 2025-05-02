#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/skbuff.h>
#include <linux/freezer.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>

/* File IO-related headers */
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/blkpg.h>
#include <linux/namei.h>
#include <linux/printk.h> // Ensure printk header is included
#include <linux/errno.h>  // For error codes
#include <linux/mm.h>     // For PAGE_SIZE, struct page
#include <linux/pagemap.h> // For vmalloc_to_page
#include <linux/minmax.h> // For min()

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aditya Gupta");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
char* device = "/dev/sdb";
module_param(device, charp, S_IRUGO);

/* USB device system handler */
static unsigned int cur_dev_sector = 0; // Unused, commented out
static struct block_device *bdevice = NULL;
static struct bio *usb_bio = NULL;
static struct file *usb_file = NULL;


bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);
long rw_usb(char* data, unsigned int size, unsigned int offset, bool flag);

static bool open_usb(void)
{
    /* Open a file for the path of the usb */
    usb_file = bdev_file_open_by_path(device, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
    if (IS_ERR(usb_file)) {
        printk("error: failed to open the device (%s) using bdev_file_open_by_path().\n",
               device);
        usb_file = NULL;
        return false;
    }
    bdevice = file_bdev(usb_file);
    
    /* Perform various sanity checks to make sure the device works */
    if (IS_ERR(bdevice) || !bdevice) {
        printk("error: failed to open the device (%s) using lookup_bdev().\n", device);
        return false;
    }
    printk("success: opened %s as a block device.\n", bdevice->bd_disk->disk_name);

    usb_bio = bio_alloc(bdevice, 256, REQ_OP_WRITE, GFP_NOIO);
    if (!usb_bio || IS_ERR(usb_bio)) {
        printk("error: failed to allocate a bio structure.\n");
        fput(usb_file);
        return false;
    }
    printk("success: allocated usb_bio.\n");
    return true;
}

// API to write to the attached USB device
long rw_usb(
    char* data,   /* kernel buffer with data (vmalloced) */
    unsigned int  size,   /* total size to read/write in bytes */
    unsigned int  offset, /* starting byte offset on device */
    bool          flag    /* true=write (1), false=read (0) */
)
{
    unsigned int remaining = size;  // Bytes left to process
    unsigned int processed = 0;     // Bytes successfully processed so far
    int len;
    int chunk_size = 512; // Size of the current chunk to process
    unsigned long page_offset = 0; // Offset within the page
    unsigned int total = size;

    if (IS_ERR(data)) {
        printk(KERN_ERR "kmod-main: rw_usb: Invalid data pointer (IS_ERR)\n");
        return -1; // Bad address or memory issue
    }

    /* Process data in chunks suitable for bio_add_page (max 512 bytes per chunk, within page boundaries) */
    while (remaining > 0) {
        bio_set_dev(usb_bio, bdevice);
        
        if (offset == -1) {
            usb_bio->bi_iter.bi_sector = cur_dev_sector; // Set sector for the bio
        } else {
            usb_bio->bi_iter.bi_sector = offset / 512; // Set sector for the bio
            cur_dev_sector = offset / 512; // Update current sector
            offset = -1; // Reset offset for future calls
        }

        usb_bio->bi_opf = flag ? REQ_OP_WRITE : REQ_OP_READ; // Set operation type
        printk(KERN_INFO "kmod-main: rw_usb: %s (size = %u, offset = %llu)\n",
               flag ? "WRITE" : "READ", size, usb_bio->bi_iter.bi_sector);

        struct page *page = vmalloc_to_page(data + processed); // Get the page from the virtual address
        if (!page) {
            printk(KERN_ERR "kmod-main: rw_usb: Failed to get page from vmalloc_to_page\n");
            return -1; // Memory issue
        }

        page_offset = (unsigned long)(data + processed) % (PAGE_SIZE); // Calculate page offset

        int added = bio_add_page(usb_bio, page, chunk_size, page_offset); // Add page to bio
        if (added <= 0) {
            printk(KERN_ERR "kmod-main: rw_usb: bio_add_page failed with error %d\n", added);
            return -1; // Error adding page to bio
        }

        len = submit_bio_wait(usb_bio); // Submit the bio and wait for completion

        if (len < 0) {
            printk(KERN_ERR "kmod-main: rw_usb: submit_bio_wait failed with error %d\n", len);
            return -1; // Error during bio submission
        }

        bio_reset(usb_bio, bdevice, GFP_NOIO); // Reset the bio for the next iteration

        processed += chunk_size; // Update total processed bytes
        remaining -= chunk_size; // Decrease remaining bytes
        cur_dev_sector++;

    } // End while (remaining > 0) loop

    return processed; // Return total bytes processed successfully
}



static void close_usb(void)
{
    printk(KERN_INFO "kmod-main: Entering close_usb().\n");

    /* Free the BIO structure if it was allocated */
    // IMPORTANT: Add bio_put back if you fix bio_alloc later!
    // For now, only adding printk as requested.
    printk(KERN_INFO "kmod-main: close_usb: Checking usb_bio (%p). NOTE: bio_put is needed here!\n", usb_bio);
    if (usb_bio) {
        bio_put(usb_bio);
        usb_bio = NULL;
         printk(KERN_INFO "kmod-main: close_usb: (Skipping bio_put as requested for now)\n");
    }

    /* Close the file pointer if it was opened */
    printk(KERN_INFO "kmod-main: close_usb: Checking usb_file (%p).\n", usb_file);
    if (usb_file) {
        printk(KERN_INFO "kmod-main: close_usb: Calling filp_close for file pointer %p.\n", usb_file);
        fput(usb_file); // Close the file pointer
        usb_file = NULL; // Set to NULL after closing
        printk(KERN_INFO "kmod-main: close_usb: File pointer closed.\n");
    }
    printk(KERN_INFO "kmod-main: Exiting close_usb().\n");
}

static int __init kmod_init(void)
{
    pr_info("Hello World!\n");
    if (!open_usb()) {
        pr_err("Failed to open USB block device\n");
        return -ENODEV;
    }
    kmod_ioctl_init();
    return 0;
}

static void __exit kmod_fini(void)
{
    close_usb();
    kmod_ioctl_teardown();
    printk("Goodbye, World!\n");
}

module_init(kmod_init);
module_exit(kmod_fini);
