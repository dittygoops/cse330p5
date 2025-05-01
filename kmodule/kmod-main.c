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
#include <linux/gfp.h>    // Needed for GFP_KERNEL
#include <linux/vmalloc.h> // Needed for vmalloc_to_page
#include <linux/uaccess.h> // Needed for access_ok (although primarily used in ioctl)
#include <linux/err.h>     // Needed for IS_ERR, PTR_ERR


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name/Group"); // Fill in author
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
static char* device = "/dev/sdb"; // Made static
module_param(device, charp, S_IRUGO);
MODULE_PARM_DESC(device, "Path to the USB block device (e.g., /dev/sdb)");


/* USB device system handler */
// Make these static as they are internal to this file
static unsigned int cur_dev_sector = 0; // Sector offset
static struct block_device *bdevice = NULL;
// No need for static usb_bio - allocate dynamically per operation or per rw_usb call if needed
static struct file *usb_file = NULL;
// static struct bio_set *usb_bio_set = NULL; // Removed bioset
static DEFINE_MUTEX(kmod_usb_mutex); // Mutex to protect concurrent access to USB device and cur_dev_sector


// Make declarations static if only used within this file
bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);
// REMOVED static keyword - needs to be visible to kmod-ioctl.c
long rw_usb(char* data, unsigned int size, unsigned int offset, bool flag);

static bool open_usb(void)
{
    int ret = 0;
    /* Open a file for the path of the usb */
    printk(KERN_INFO "kmod_main: Attempting to open block device: %s\n", device);
    // Use FMODE_READ | FMODE_WRITE instead of BLK_OPEN_* flags
    usb_file = filp_open(device, O_RDWR, 0); // Use filp_open for general file path opening
    if (IS_ERR(usb_file)) {
        ret = PTR_ERR(usb_file);
        printk(KERN_ERR "kmod_main: error %d: failed to open the device file (%s).\n", ret, device);
        usb_file = NULL;
        return false;
    }

    // Check if it's a block device file
     if (!S_ISBLK(file_inode(usb_file)->i_mode)) {
        printk(KERN_ERR "kmod_main: error: specified path (%s) is not a block device.\n", device);
        filp_close(usb_file, NULL);
        usb_file = NULL;
        return false;
    }

    bdevice = I_BDEV(file_inode(usb_file)->i_mapping->host);
    if (!bdevice) {
        printk(KERN_ERR "kmod_main: error: could not get block_device from file (%s).\n", device);
        filp_close(usb_file, NULL);
        usb_file = NULL;
        return false;
    }

    // Inside open_usb, replace the previous "success" printk block:
    const char *disk_name_str = "unknown_disk_ptr";
    const char *device_str = device ? device : "null_device_param"; // Check device param first

    // Check bdevice and bd_disk pointers
    if (bdevice && bdevice->bd_disk) {
        // Check disk_name pointer itself
        if (bdevice->bd_disk->disk_name) {
            // As an extra check, try reading the first byte carefully
            // This uses probe_kernel_read, requires #include <linux/uaccess.h>
            char first_char;
            // Make sure uaccess.h is included at the top of the file!
            if (probe_kernel_read(&first_char, bdevice->bd_disk->disk_name, 1) == 0) {
                // Read was successful, pointer is likely valid memory
                disk_name_str = bdevice->bd_disk->disk_name;
                // Check for empty string just in case
                if (first_char == '\0') {
                    disk_name_str = "empty_disk_name";
                }
            } else {
                // Read failed, pointer points to bad memory!
                disk_name_str = "invalid_disk_name_ptr";
                printk(KERN_WARNING "kmod_main: Warning: bdevice->bd_disk->disk_name pointer (%p) is invalid!\n",
                    bdevice->bd_disk->disk_name);
            }
        } else {
            disk_name_str = "null_disk_name_ptr";
        }
    } else {
        disk_name_str = "null_bd_disk_ptr";
    }

    // Now print using the safely determined strings
    printk(KERN_INFO "kmod_main: success: opened %s (%s) as a block device.\n", disk_name_str, device_str);


    // // Removed bioset creation
    // usb_bio_set = bioset_create(1, 0, BIOSET_NEED_BVECS); // Pool size 1, minimal flags
    // if (!usb_bio_set) {
    //     printk(KERN_ERR "kmod_main: error: failed to create bio_set.\n");
    //     filp_close(usb_file, NULL);
    //     usb_file = NULL;
    //     bdevice = NULL;
    //     return false;
    // }
    // printk(KERN_INFO "kmod_main: success: created bioset.\n");

    cur_dev_sector = 0; // Initialize current sector offset

    return true;
}

// API to read/write to the attached USB device
long rw_usb(
    char* data,   /* kernel message buffer (vmalloced) */
    unsigned int size,   /* message length in bytes */
    unsigned int offset, /* byte offset for operation, or -1 to use/update current */
    bool         flag    /* true=write, false=read */
)
{
    unsigned int remaining = size;
    unsigned int processed = 0;
    int bio_ret = 0; // To store return value of submit_bio_wait
    struct bio *bio = NULL; // Allocate bio within the function
    unsigned int current_sector;
    unsigned int bytes_in_chunk;
    long total_processed = 0; // Use long to match return type
    blk_opf_t current_opf; // To store operation flags

    // Basic check: data pointer should be valid (already checked in ioctl usually)
    if (!data) {
        printk(KERN_ERR "rw_usb: error: NULL data buffer provided.\n");
        return -EINVAL;
    }

    // Acquire mutex to protect device access and cur_dev_sector update
    mutex_lock(&kmod_usb_mutex);

    if (offset == (unsigned int)-1) {
        /* Use the current offset */
        current_sector = cur_dev_sector;
    } else {
        /* Use the provided offset */
         // Check alignment (optional but recommended)
        if ((offset % 512) != 0) {
            printk(KERN_WARNING "rw_usb: Warning: Provided offset %u is not sector aligned. Aligning down.\n", offset);
            // offset = (offset / 512) * 512; // Or handle as error depending on requirement
        }
        current_sector = offset / 512; // Convert byte offset to sector offset
        printk(KERN_INFO "rw_usb: Using provided offset (bytes = %u, start_sector = %u)\n", offset, current_sector);
        // No need to reset offset variable here, current_sector tracks the start
         // Update cur_dev_sector to the new starting position
        cur_dev_sector = current_sector;
    }


    /* Break down read/write into chunks (max 512 bytes as per PDF, though BIO can handle more) */
    while (remaining > 0) {

         // Determine opf for this chunk first
        if (flag == true) {
            current_opf = REQ_OP_WRITE | REQ_SYNC; // Add REQ_SYNC for synchronous write
             printk(KERN_DEBUG "rw_usb: WRITE chunk (remaining=%u, sector=%u)\n", // Use %u for unsigned int sector
                   remaining, current_sector);
        } else {
            current_opf = REQ_OP_READ;
             printk(KERN_DEBUG "rw_usb: READ chunk (remaining=%u, sector=%u)\n",
                   remaining, current_sector);
        }

         // Allocate a new bio for each chunk using the older API signature
         // Need 1 page vector for a 512-byte chunk.
         bio = bio_alloc(bdevice, 1, current_opf, GFP_KERNEL); // Use this signature
         if (IS_ERR_OR_NULL(bio)) { // Check for NULL or error pointer
             long bio_err = bio ? PTR_ERR(bio) : -ENOMEM; // Handle NULL case too
             printk(KERN_ERR "rw_usb: error: failed to allocate bio for chunk (ret = %ld).\n", bio_err);
             mutex_unlock(&kmod_usb_mutex);
             return bio_err;
         }
         // bio_alloc should associate bdev and opf based on signature

        // Set bi_iter.bi_sector for the current chunk
        // NOTE: sector_t is typically u64, printk uses %llu. Ensure current_sector is treated correctly.
        bio->bi_iter.bi_sector = (sector_t)current_sector; // Explicit cast if needed


        // Determine size for this chunk (max 512 bytes as per PDF note)
         bytes_in_chunk = (remaining > 512) ? 512 : remaining;

        // Add data page(s) to the bio
        struct page *data_page = vmalloc_to_page(data + processed); // Get page for the current data offset
        if (!data_page) {
             printk(KERN_ERR "rw_usb: error: failed to get page for vmalloc address %p\n", data + processed);
             bio_put(bio); // Free bio
             mutex_unlock(&kmod_usb_mutex);
             return -EFAULT;
        }

        // Calculate offset within the page for the start of the chunk's data
        unsigned int offset_in_page = offset_in_page(data + processed);

         // Add page to BIO
         int add_page_ret = bio_add_page(bio, data_page, bytes_in_chunk, offset_in_page);
         if (add_page_ret != bytes_in_chunk) {
                printk(KERN_ERR "rw_usb: Error: bio_add_page added %d bytes, expected %u\n", add_page_ret, bytes_in_chunk);
                bio_put(bio);
                mutex_unlock(&kmod_usb_mutex);
                return -EIO; // I/O error
            }


        /* Submit BIO and wait for op completion */
         printk(KERN_DEBUG "rw_usb: Submitting bio for sector %llu, size %u\n", bio->bi_iter.bi_sector, bytes_in_chunk);
         bio_ret = submit_bio_wait(bio); // Returns 0 on success, < 0 on error

        if (bio_ret < 0 && bio_ret != -EOPNOTSUPP) { // Check for errors, ignore EOPNOTSUPP if applicable
             printk(KERN_ERR "rw_usb: error: submit_bio_wait failed (ret = %d) for sector %llu\n", bio_ret, bio->bi_iter.bi_sector);
             bio_put(bio); // Free bio
             mutex_unlock(&kmod_usb_mutex);
             return (total_processed > 0) ? total_processed : bio_ret; // Return bytes processed so far or the error
        } else if (bio_ret == -EOPNOTSUPP) {
             printk(KERN_WARNING "rw_usb: Warning: Operation potentially not supported (ret = %d) for sector %llu\n", bio_ret, bio->bi_iter.bi_sector);
             // Decide how to handle this - maybe try continuing? For now, treat as error.
             bio_put(bio);
             mutex_unlock(&kmod_usb_mutex);
             return (total_processed > 0) ? total_processed : bio_ret;
        }


        bio_put(bio); // Free the bio after use

        /* Update counters */
        processed += bytes_in_chunk;
        remaining -= bytes_in_chunk;
        total_processed += bytes_in_chunk;

        // Increment current_sector for the next chunk (1 sector = 512 bytes)
        current_sector++;

         printk(KERN_DEBUG "rw_usb: Chunk processed. total_processed=%ld, remaining=%u, next_sector=%u\n",
               total_processed, remaining, current_sector);

    } // end while (remaining > 0)


     // Update the global current offset only if the operation was sequential (offset == -1 initially)
     // and if the operation completed successfully.
     if (offset == (unsigned int)-1 && total_processed == size) {
         cur_dev_sector = current_sector; // Update global sector offset
         printk(KERN_INFO "rw_usb: Updated cur_dev_sector to %u\n", cur_dev_sector);
     } else if (offset != (unsigned int)-1 && total_processed == size) {
          // If a specific offset was provided, cur_dev_sector was already updated at the start
          // Optionally update it to the end sector: cur_dev_sector = current_sector;
          cur_dev_sector = current_sector; // Update to the end sector after offset operation
           printk(KERN_INFO "rw_usb: RWOFFSET finished, updated cur_dev_sector to %u\n", cur_dev_sector);
     }


    mutex_unlock(&kmod_usb_mutex);

    printk(KERN_INFO "rw_usb: Finished %s operation. Total bytes processed: %ld\n", (flag ? "WRITE" : "READ"), total_processed);

    return total_processed; // Return total bytes successfully processed
}


static void close_usb(void)
{
    printk(KERN_INFO "kmod_main: Closing USB device resources...\n");
    mutex_lock(&kmod_usb_mutex); // Ensure exclusive access during cleanup

    // Check if the usb_file is valid and close it
    if (usb_file && !IS_ERR(usb_file)) {
         printk(KERN_INFO "kmod_main: Closing USB device file pointer.\n");
        filp_close(usb_file, NULL); // Use filp_close as opened with filp_open
        usb_file = NULL;
        bdevice = NULL; // Invalidate bdevice as it came from the file
    }

     // // Removed bioset cleanup
     // if (usb_bio_set) {
     //     printk(KERN_INFO "kmod_main: Freeing bioset.\n");
     //     bioset_free(usb_bio_set);
     //     usb_bio_set = NULL;
     // }

    mutex_unlock(&kmod_usb_mutex);
    printk(KERN_INFO "kmod_main: USB device resources closed.\n");
}

static int __init kmod_init(void)
{
    pr_info("kmod_main: Loading kmod module...\n"); // Use standard pr_info

    // Initialize mutex before using it
    mutex_init(&kmod_usb_mutex);


    if (!open_usb()) {
        pr_err("kmod_main: Failed to open USB block device %s\n", device); // Use pr_err
        return -ENODEV; // Standard error code for no such device
    }

    // Initialize the IOCTL interface AFTER successfully opening the USB device
    if (!kmod_ioctl_init()) {
         pr_err("kmod_main: Failed to initialize IOCTL interface\n");
         close_usb(); // Clean up USB resources if IOCTL init fails
         return -EFAULT; // Or appropriate error from kmod_ioctl_init
    }

    pr_info("kmod_main: Kernel module loaded successfully.\n");
    return 0;
}

static void __exit kmod_fini(void)
{
    pr_info("kmod_main: Unloading kmod module...\n");
    // Teardown IOCTL first, then close USB resources
    kmod_ioctl_teardown();
    close_usb();
    // Mutex doesn't need explicit destroy if statically defined? Check docs.
    // mutex_destroy(&kmod_usb_mutex); // If dynamically allocated or needed
    pr_info("kmod_main: Kernel module unloaded.\n");
}

module_init(kmod_init);
module_exit(kmod_fini);