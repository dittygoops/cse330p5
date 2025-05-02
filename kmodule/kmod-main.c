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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
char* device = "/dev/sdb";
module_param(device, charp, S_IRUGO);


/* USB device system handler */
unsigned int cur_dev_sector = 0;
static struct block_device *bdevice = NULL;
static struct bio *usb_bio = NULL;
static struct file *usb_file = NULL;


bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);
long rw_usb(char* data, unsigned int size, unsigned int  offset, bool flag);

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

    usb_bio = bio_alloc(GFP_KERNEL, 1);
    if (!usb_bio) {
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
    int ret = 0;                    // Stores return codes

    sector_t current_sector;        // Sector on disk for the current chunk
    struct page *kern_page;         // Physical page backing the vmalloced buffer
    unsigned int offset_in_page;    // Byte offset within the physical page
    unsigned int bytes_this_chunk;  // How many bytes to process in this iteration
    unsigned int bio_add_page_result;
    unsigned long current_abs_offset; // Absolute byte offset for current chunk

    blk_status_t bio_status;        // Status from BIO completion

    printk(KERN_INFO "rw_usb: Request START - %s %u bytes at offset %u\n",
           flag ? "WRITE" : "READ", size, offset);

    /* Basic checks */
    if (!bdevice || !usb_bio) {
        printk(KERN_ERR "rw_usb: Device or BIO not initialized!\n");
        return -EIO; // I/O error
    }
     if (!data) {
         printk(KERN_ERR "rw_usb: Invalid data buffer (NULL).\n");
         return -EFAULT; // Bad address (although checked in ioctl, good to have)
     }
     if (size == 0) {
         return 0; // Nothing to do
     }

    /* Process data in chunks suitable for bio_add_page (max 512 bytes per chunk, within page boundaries) */
    while (remaining > 0) {

        // Calculate absolute byte offset for the start of this chunk
        current_abs_offset = (unsigned long)offset + processed;

        // Calculate the disk sector for this chunk
        current_sector = current_abs_offset / SECTOR_SIZE;

        // Get the physical page corresponding to the current position in the vmalloc buffer
        kern_page = vmalloc_to_page(data + processed);
        if (!kern_page) {
            printk(KERN_ERR "rw_usb: Failed to get page for vmalloc address %p + %u\n", data, processed);
            ret = -EFAULT; // Bad address or memory issue
            goto out_error;
        }

        // Calculate the byte offset within this specific physical page
        offset_in_page = (unsigned long)(data + processed) & (PAGE_SIZE - 1);

        // Determine chunk size: Min of remaining, 512, and bytes left in the current physical page
        bytes_this_chunk = min(remaining, (unsigned int)SECTOR_SIZE);
        bytes_this_chunk = min(bytes_this_chunk, (unsigned int)(PAGE_SIZE - offset_in_page));

        if (bytes_this_chunk == 0) {
             // Should not happen if remaining > 0, but safety check
             printk(KERN_ERR "rw_usb: Calculated zero chunk size!\n");
             ret = -EIO;
             goto out_error;
        }

        printk(KERN_DEBUG "rw_usb: Chunk - Process %u bytes, AbsOffset %lu, Sector %llu, PageOffset %u\n",
               bytes_this_chunk, current_abs_offset, (unsigned long long)current_sector, offset_in_page);

        /* Reset BIO fields for this chunk */
        // bio_reset(usb_bio); // Only needed if previous op failed? Let's try without.
        bio_set_dev(usb_bio, bdevice);
        usb_bio->bi_iter.bi_sector = current_sector;
        usb_bio->bi_iter.bi_size = bytes_this_chunk; // Inform BIO layer of intended size
        usb_bio->bi_opf = flag ? (REQ_OP_WRITE | REQ_SYNC) : (REQ_OP_READ | REQ_SYNC);
        // Consider REQ_PREFLUSH for writes if needed? REQ_SYNC makes it wait.

        /* Add the page chunk to the BIO */
        // bio_add_page clears previous entries, so safe to call directly
        bio_add_page_result = bio_add_page(usb_bio, kern_page, bytes_this_chunk, offset_in_page);
        if (bio_add_page_result != bytes_this_chunk) {
            printk(KERN_ERR "rw_usb: bio_add_page failed! Wanted %u, got %u\n", bytes_this_chunk, bio_add_page_result);
            ret = -EIO; // Treat as I/O error
            goto out_error;
        }

        /* Submit BIO and wait for completion */
        submit_bio_wait(usb_bio);

        bio_status = usb_bio->bi_status; // Check status after completion

        // Check BIO status for errors
        if (bio_status != BLK_STS_OK) {
            printk(KERN_ERR "rw_usb: BIO submission failed! status=%d Sector=%llu Size=%u\n",
                   bio_status, (unsigned long long)current_sector, bytes_this_chunk);
            ret = -EIO; // Map block layer errors to general I/O error
            // bio_reset(usb_bio); // Attempt reset after error? Optional.
            goto out_error;
        }

        /* Success for this chunk */
        processed += bytes_this_chunk;
        remaining -= bytes_this_chunk;

        printk(KERN_DEBUG "rw_usb: Chunk DONE - Processed %u, Remaining %u\n", processed, remaining);

    } // End while (remaining > 0) loop

    printk(KERN_INFO "rw_usb: Request END - Success, processed %u bytes\n", processed);
    return processed; // Return total bytes processed successfully

out_error:
    // If an error occurred, return bytes processed *before* the error,
    // or the negative error code if no bytes were processed at all.
    printk(KERN_ERR "rw_usb: Request END - Error %d after processing %u bytes\n", ret, processed);
    return processed > 0 ? processed : ret;
}



static void close_usb(void)
{
    /* Close the file and device communication interface */

    // TODO: Check if the usb_file is valid
    // TODO: Use bdev_fput() to release the file reference
    printk(KERN_INFO "close_usb: Attempting cleanup.\n");

    /* Free the BIO structure if it was allocated */
    if (usb_bio) {
        printk(KERN_INFO "close_usb: Freeing BIO structure %p.\n", usb_bio);
        bio_put(usb_bio);
        usb_bio = NULL;
    } else {
        printk(KERN_WARNING "close_usb: No BIO structure was allocated.\n");
    }

    /* Close the file pointer if it was opened */
    if (usb_file) {
        printk(KERN_INFO "close_usb: Closing file pointer %p.\n", usb_file);
        filp_close(usb_file, NULL);
        usb_file = NULL; // Mark as closed
        printk(KERN_INFO "close_usb: File pointer closed.\n");
    } else {
        printk(KERN_WARNING "close_usb: No USB file pointer was open.\n");
    }
    
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