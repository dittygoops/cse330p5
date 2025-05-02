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
MODULE_AUTHOR("");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
char* device = "/dev/sdb";
module_param(device, charp, S_IRUGO);

#define SECTOR_SIZE 512 // Define sector size

/* USB device system handler */
// static unsigned int cur_dev_sector = 0; // Unused, commented out
static struct block_device *bdevice = NULL;
static struct bio *usb_bio = NULL;
static struct file *usb_file = NULL;


bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);
long rw_usb(char* data, unsigned int size, unsigned int offset, bool flag);

static bool open_usb(void)
{
    long err_code = 0; // For capturing error codes
    printk(KERN_INFO "kmod-main: Entering open_usb(). Device path: %s\n", device);

    /* Open a file for the path of the usb */
    printk(KERN_INFO "kmod-main: Calling bdev_file_open_by_path...\n");
    usb_file = bdev_file_open_by_path(device, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
    printk(KERN_INFO "kmod-main: bdev_file_open_by_path returned: %p\n", usb_file);

    if (IS_ERR(usb_file)) {
        err_code = PTR_ERR(usb_file); // Capture error code
        printk(KERN_ERR "kmod-main: error: failed to open the device (%s) using bdev_file_open_by_path(). Error code: %ld\n",
               device, err_code);
        usb_file = NULL;
        printk(KERN_INFO "kmod-main: Exiting open_usb() - return false (bdev_file_open_by_path failed).\n");
        return false;
    }

    printk(KERN_INFO "kmod-main: Calling file_bdev...\n");
    bdevice = file_bdev(usb_file);
    printk(KERN_INFO "kmod-main: file_bdev returned: %p\n", bdevice);

    /* Perform various sanity checks to make sure the device works */
    if (IS_ERR(bdevice) || !bdevice) {
        err_code = IS_ERR(bdevice) ? PTR_ERR(bdevice) : 0; // Capture error if IS_ERR
        printk(KERN_ERR "kmod-main: error: failed to get block device from file. IS_ERR=%d, bdevice_ptr=%p, Error code: %ld\n",
               IS_ERR(bdevice), bdevice, err_code);
        // Cleanup already opened file before returning
        filp_close(usb_file, NULL);
        usb_file = NULL;
        printk(KERN_INFO "kmod-main: Exiting open_usb() - return false (file_bdev failed).\n");
        return false;
    }

    // Check if bd_disk is valid before accessing disk_name
    if (!bdevice->bd_disk) {
         printk(KERN_ERR "kmod-main: error: bdevice->bd_disk is NULL!\n");
         filp_close(usb_file, NULL);
         usb_file = NULL;
         printk(KERN_INFO "kmod-main: Exiting open_usb() - return false (bd_disk NULL).\n");
         return false;
    }
    printk(KERN_INFO "kmod-main: success: opened %s as a block device.\n", bdevice->bd_disk->disk_name);

    printk(KERN_INFO "kmod-main: Calling bio_alloc(bdevice=%p, vecs=256, opf=REQ_OP_WRITE, gfp=GFP_NOIO)...\n", bdevice);
    usb_bio = bio_alloc(bdevice, 256, REQ_OP_WRITE, GFP_NOIO); // Reverted Instructor's version
    printk(KERN_INFO "kmod-main: bio_alloc returned: %p\n", usb_bio);

    // Original check (potentially incorrect if bio_alloc returns NULL)
    if (!usb_bio || IS_ERR(usb_bio)) {
        err_code = IS_ERR(usb_bio) ? PTR_ERR(usb_bio) : 0; // Capture error if IS_ERR, otherwise assume NULL means error
        printk(KERN_ERR "kmod-main: error: failed to allocate a bio structure. !usb_bio=%d, IS_ERR=%d, Error code: %ld\n",
               !usb_bio, IS_ERR(usb_bio), err_code);
        fput(usb_file); // Use fput for cleanup matching bdev_file_open_by_path
        usb_file = NULL; // Nullify after fput
        printk(KERN_INFO "kmod-main: Exiting open_usb() - return false (bio_alloc failed).\n");
        return false;
    }

    printk(KERN_INFO "kmod-main: success: allocated usb_bio.\n");
    printk(KERN_INFO "kmod-main: Exiting open_usb() - return true.\n");
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
    long ret = 0;                   // Stores return codes (use long to match return type)

    sector_t current_sector;        // Sector on disk for the current chunk
    struct page *kern_page;         // Physical page backing the vmalloced buffer
    unsigned int offset_in_page;    // Byte offset within the physical page
    unsigned int bytes_this_chunk;  // How many bytes to process in this iteration
    unsigned int bio_add_page_result;
    unsigned long current_abs_offset; // Absolute byte offset for current chunk

    blk_status_t bio_status;        // Status from BIO completion

    printk(KERN_INFO "kmod-main: rw_usb: Request START - %s %u bytes at offset %u\n",
           flag ? "WRITE" : "READ", size, offset);

    /* Basic checks */
    if (!bdevice || !usb_bio) {
        printk(KERN_ERR "kmod-main: rw_usb: Device or BIO not initialized!\n");
        return -EIO; // I/O error
    }
     if (!data) {
         printk(KERN_ERR "kmod-main: rw_usb: Invalid data buffer (NULL).\n");
         return -EFAULT; // Bad address (although checked in ioctl, good to have)
     }
     if (size == 0) {
         printk(KERN_INFO "kmod-main: rw_usb: Request END - size is 0.\n");
         return 0; // Nothing to do
     }

    /* Process data in chunks suitable for bio_add_page (max 512 bytes per chunk, within page boundaries) */
    while (remaining > 0) {
        printk(KERN_DEBUG "kmod-main: rw_usb: Loop Start - remaining=%u, processed=%u\n", remaining, processed);

        // Calculate absolute byte offset for the start of this chunk
        current_abs_offset = (unsigned long)offset + processed;

        // Calculate the disk sector for this chunk
        current_sector = current_abs_offset / SECTOR_SIZE;

        printk(KERN_DEBUG "kmod-main: rw_usb: Calculating page for data=%p + processed=%u\n", data, processed);
        // Get the physical page corresponding to the current position in the vmalloc buffer
        kern_page = vmalloc_to_page(data + processed);
        if (!kern_page) {
            printk(KERN_ERR "kmod-main: rw_usb: Failed to get page for vmalloc address %p + %u\n", data, processed);
            ret = -EFAULT; // Bad address or memory issue
            goto out_error;
        }
        printk(KERN_DEBUG "kmod-main: rw_usb: Got page %p for address %p\n", kern_page, data + processed);

        // Calculate the byte offset within this specific physical page
        offset_in_page = (unsigned long)(data + processed) & (PAGE_SIZE - 1);

        // Determine chunk size: Min of remaining, SECTOR_SIZE, and bytes left in the current physical page
        bytes_this_chunk = min(remaining, (unsigned int)SECTOR_SIZE);
        bytes_this_chunk = min(bytes_this_chunk, (unsigned int)(PAGE_SIZE - offset_in_page));

        if (bytes_this_chunk == 0) {
             printk(KERN_ERR "kmod-main: rw_usb: Calculated zero chunk size! remaining=%u, PAGE_SIZE=%lu, offset_in_page=%u\n",
                    remaining, PAGE_SIZE, offset_in_page);
             ret = -EIO;
             goto out_error;
        }

        printk(KERN_DEBUG "kmod-main: rw_usb: Chunk - Process %u bytes, AbsOffset %lu, Sector %llu, PageOffset %u\n",
               bytes_this_chunk, current_abs_offset, (unsigned long long)current_sector, offset_in_page);

        /* Reset BIO fields for this chunk */
        printk(KERN_DEBUG "kmod-main: rw_usb: Setting BIO fields: dev=%p, sector=%llu, size=%u, opf=%d\n",
               bdevice, (unsigned long long)current_sector, bytes_this_chunk, flag ? (REQ_OP_WRITE | REQ_SYNC) : (REQ_OP_READ | REQ_SYNC));
        // bio_reset(usb_bio); // Let's try without unless errors occur.
        bio_set_dev(usb_bio, bdevice);
        usb_bio->bi_iter.bi_sector = current_sector;
        usb_bio->bi_iter.bi_size = bytes_this_chunk; // Inform BIO layer of intended size
        usb_bio->bi_opf = flag ? (REQ_OP_WRITE | REQ_SYNC) : (REQ_OP_READ | REQ_SYNC);

        /* Add the page chunk to the BIO */
        printk(KERN_DEBUG "kmod-main: rw_usb: Calling bio_add_page(bio=%p, page=%p, size=%u, offset=%u)\n",
               usb_bio, kern_page, bytes_this_chunk, offset_in_page);
        bio_add_page_result = bio_add_page(usb_bio, kern_page, bytes_this_chunk, offset_in_page);
        printk(KERN_DEBUG "kmod-main: rw_usb: bio_add_page returned %u\n", bio_add_page_result);
        if (bio_add_page_result != bytes_this_chunk) {
            printk(KERN_ERR "kmod-main: rw_usb: bio_add_page failed! Wanted %u, got %u\n", bytes_this_chunk, bio_add_page_result);
            ret = -EIO; // Treat as I/O error
            goto out_error;
        }

        /* Submit BIO and wait for completion */
        printk(KERN_DEBUG "kmod-main: rw_usb: Calling submit_bio_wait(bio=%p)\n", usb_bio);
        submit_bio_wait(usb_bio);
        printk(KERN_DEBUG "kmod-main: rw_usb: submit_bio_wait returned. Checking status...\n");

        bio_status = usb_bio->bi_status; // Check status after completion
        printk(KERN_DEBUG "kmod-main: rw_usb: BIO status = %d\n", bio_status);

        // Check BIO status for errors
        if (bio_status != BLK_STS_OK) {
            printk(KERN_ERR "kmod-main: rw_usb: BIO submission failed! status=%d Sector=%llu Size=%u\n",
                   bio_status, (unsigned long long)current_sector, bytes_this_chunk);
            ret = -EIO; // Map block layer errors to general I/O error
            // Consider bio_reset(usb_bio); here?
            goto out_error;
        }

        /* Success for this chunk */
        processed += bytes_this_chunk;
        remaining -= bytes_this_chunk;

        printk(KERN_DEBUG "kmod-main: rw_usb: Chunk DONE - Processed %u, Remaining %u\n", processed, remaining);

    } // End while (remaining > 0) loop

    printk(KERN_INFO "kmod-main: rw_usb: Request END - Success, processed %u bytes\n", processed);
    return processed; // Return total bytes processed successfully

out_error:
    // If an error occurred, return bytes processed *before* the error,
    // or the negative error code if no bytes were processed at all.
    printk(KERN_ERR "kmod-main: rw_usb: Request END - Error %ld after processing %u bytes\n", ret, processed);
    // Ensure ret is long for return
    return processed > 0 ? (long)processed : ret;
}



static void close_usb(void)
{
    printk(KERN_INFO "kmod-main: Entering close_usb().\n");

    /* Free the BIO structure if it was allocated */
    // IMPORTANT: Add bio_put back if you fix bio_alloc later!
    // For now, only adding printk as requested.
    printk(KERN_INFO "kmod-main: close_usb: Checking usb_bio (%p). NOTE: bio_put is needed here!\n", usb_bio);
    if (usb_bio) {
        // bio_put(usb_bio); // This SHOULD be here
        // usb_bio = NULL;
         printk(KERN_INFO "kmod-main: close_usb: (Skipping bio_put as requested for now)\n");
    } else {
        printk(KERN_INFO "kmod-main: close_usb: No BIO structure was allocated or already freed.\n");
    }

    /* Close the file pointer if it was opened */
    printk(KERN_INFO "kmod-main: close_usb: Checking usb_file (%p).\n", usb_file);
    if (usb_file) {
        printk(KERN_INFO "kmod-main: close_usb: Calling filp_close for file pointer %p.\n", usb_file);
        filp_close(usb_file, NULL);
        usb_file = NULL; // Mark as closed
        printk(KERN_INFO "kmod-main: close_usb: File pointer closed.\n");
    } else {
        printk(KERN_INFO "kmod-main: close_usb: No USB file pointer was open.\n");
    }
    printk(KERN_INFO "kmod-main: Exiting close_usb().\n");
}

static int __init kmod_init(void)
{
    int ioctl_ret = 0; // Capture return from kmod_ioctl_init
    printk(KERN_INFO "kmod-main: Entering kmod_init(). Module loading...\n");
    pr_info("kmod-main: Hello World!\n");

    printk(KERN_INFO "kmod-main: Calling open_usb()...\n");
    if (!open_usb()) {
        pr_err("kmod-main: open_usb() failed.\n");
        printk(KERN_INFO "kmod-main: Exiting kmod_init() - Failed.\n");
        return -ENODEV; // Or appropriate error
    }
    printk(KERN_INFO "kmod-main: open_usb() succeeded.\n");

    printk(KERN_INFO "kmod-main: Calling kmod_ioctl_init()...\n");
    // Assuming kmod_ioctl_init returns bool (true/false) based on original code
    if (!kmod_ioctl_init()) {
         pr_err("kmod-main: kmod_ioctl_init() failed.\n");
         // Need cleanup if ioctl init fails after open_usb succeeded
         close_usb();
         printk(KERN_INFO "kmod-main: Exiting kmod_init() - Failed.\n");
         return -ENODEV; // Or appropriate error
    }
    printk(KERN_INFO "kmod-main: kmod_ioctl_init() succeeded.\n");

    printk(KERN_INFO "kmod-main: Exiting kmod_init() - Success.\n");
    return 0;
}

static void __exit kmod_fini(void)
{
    printk(KERN_INFO "kmod-main: Entering kmod_fini(). Module unloading...\n");

    printk(KERN_INFO "kmod-main: Calling close_usb()...\n");
    close_usb();
    printk(KERN_INFO "kmod-main: Returned from close_usb().\n");

    printk(KERN_INFO "kmod-main: Calling kmod_ioctl_teardown()...\n");
    kmod_ioctl_teardown();
    printk(KERN_INFO "kmod-main: Returned from kmod_ioctl_teardown().\n");

    printk(KERN_INFO "kmod-main: Goodbye, World!\n");
    printk(KERN_INFO "kmod-main: Exiting kmod_fini().\n");
}

module_init(kmod_init);
module_exit(kmod_fini);