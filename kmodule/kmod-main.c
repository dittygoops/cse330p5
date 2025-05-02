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
#include <linux/uaccess.h> // Needed for access_ok and probe_kernel_read (if used)
#include <linux/err.h>     // Needed for IS_ERR, PTR_ERR
#include <linux/sched.h>   // Maybe needed by included headers


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aditya Gupta");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
static char* device = "/dev/sdb";
module_param(device, charp, S_IRUGO);
MODULE_PARM_DESC(device, "Path to the USB block device (e.g., /dev/sdb)");


/* USB device system handler */
static unsigned int cur_dev_sector = 0; // Sector offset
static struct block_device *bdevice = NULL;
static struct file *usb_file = NULL;
static DEFINE_MUTEX(kmod_usb_mutex);


/* Forward declarations for functions in kmod-ioctl.c */
bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

/* Function definition visible to kmod-ioctl.c */
long rw_usb(char* data, unsigned int size, unsigned int offset, bool flag);


// FULL open_usb function restored
static bool open_usb(void)
{
    int file_err = 0;
    struct inode *inode = NULL;

    printk(KERN_INFO "kmod_main open_usb: MARKER A - Entering function.\n");
    printk(KERN_INFO "kmod_main open_usb: MARKER B - Attempting filp_open for: %s\n", device);
    usb_file = filp_open(device, O_RDWR, 0);
    printk(KERN_INFO "kmod_main open_usb: MARKER C - filp_open returned %p\n", usb_file);

    if (IS_ERR(usb_file)) {
        file_err = PTR_ERR(usb_file);
        printk(KERN_ERR "kmod_main open_usb: MARKER D - filp_open error %d for (%s).\n", file_err, device);
        usb_file = NULL;
        return false;
    }
    printk(KERN_INFO "kmod_main open_usb: MARKER E - filp_open check passed.\n");

    printk(KERN_INFO "kmod_main open_usb: MARKER F - Getting inode via file_inode...\n");
    inode = file_inode(usb_file);
    printk(KERN_INFO "kmod_main open_usb: MARKER G - file_inode returned %p\n", inode);
    if (!inode) {
         printk(KERN_ERR "kmod_main open_usb: MARKER H - Failed to get inode from file (%s).\n", device);
         filp_close(usb_file, NULL);
         usb_file = NULL;
         return false;
    }
    printk(KERN_INFO "kmod_main open_usb: MARKER I - Got inode successfully.\n");


    printk(KERN_INFO "kmod_main open_usb: MARKER J - Checking S_ISBLK...\n");
    if (!S_ISBLK(inode->i_mode)) {
        printk(KERN_ERR "kmod_main open_usb: MARKER K - Path (%s) is not a block device.\n", device);
        filp_close(usb_file, NULL);
        usb_file = NULL;
        return false;
    }
    printk(KERN_INFO "kmod_main open_usb: MARKER L - File type is block device.\n");


    printk(KERN_INFO "kmod_main open_usb: MARKER M - Checking inode mapping/host...\n");
    if (!inode->i_mapping || !inode->i_mapping->host) {
         printk(KERN_ERR "kmod_main open_usb: MARKER N - Inode mapping or host is NULL for (%s).\n", device);
         filp_close(usb_file, NULL);
         usb_file = NULL;
         return false;
    }
    printk(KERN_INFO "kmod_main open_usb: MARKER O - Inode mapping/host look ok.\n");


    printk(KERN_INFO "kmod_main open_usb: MARKER P - Getting bdevice via I_BDEV...\n");
    bdevice = I_BDEV(inode->i_mapping->host);
    printk(KERN_INFO "kmod_main open_usb: MARKER Q - I_BDEV returned %p\n", bdevice);

    if (!bdevice) {
        printk(KERN_ERR "kmod_main open_usb: MARKER R - Failed to get block_device from file (%s).\n", device);
        filp_close(usb_file, NULL);
        usb_file = NULL;
        return false;
    }
    printk(KERN_INFO "kmod_main open_usb: MARKER S - Got bdevice successfully.\n");

    // Use the simpler safe printk (without probe_kernel_read)
    const char *disk_name_str = "unknown_disk_ptr";
    const char *device_str = device ? device : "null_device_param";
    if (bdevice && bdevice->bd_disk) {
        if (bdevice->bd_disk->disk_name) {
             disk_name_str = bdevice->bd_disk->disk_name;
             if (disk_name_str[0] == '\0') {
                disk_name_str = "[empty_disk_name]";
             }
        } else {
             disk_name_str = "[null_disk_name_ptr]";
        }
    } else {
         disk_name_str = "[null_bd_disk_ptr]";
    }
    printk(KERN_INFO "kmod_main open_usb: MARKER T - Device details checked.\n");
    printk(KERN_INFO "kmod_main open_usb: success: opened %s (%s) as a block device.\n", disk_name_str, device_str);


    cur_dev_sector = 0;
    printk(KERN_INFO "kmod_main open_usb: MARKER U - Finished open_usb successfully.\n");
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
    int bio_ret = 0;
    struct bio *bio = NULL;
    unsigned int current_sector;
    unsigned int bytes_in_chunk;
    long total_processed = 0;
    blk_opf_t current_opf;
    struct page *data_page = NULL;
    unsigned int offset_in_page = 0;
    int add_page_ret = 0; // <<< DECLARED HERE

    printk(KERN_DEBUG "rw_usb: Entered: size=%u, offset=%u, flag=%d\n", size, offset, flag); // <<< RW_USB MARKER 1

    // Basic check: data pointer
    if (!data) {
        printk(KERN_ERR "rw_usb: error: NULL data buffer provided.\n");
        return -EINVAL;
    }
    // Basic check: Ensure bdevice was initialized by open_usb
    if (!bdevice) {
        printk(KERN_ERR "rw_usb: error: bdevice is NULL. Was open_usb successful?\n");
        return -ENODEV;
    }


    printk(KERN_DEBUG "rw_usb: Acquiring mutex...\n"); // <<< RW_USB MARKER 2
    mutex_lock(&kmod_usb_mutex);

    if (offset == (unsigned int)-1) {
        current_sector = cur_dev_sector;
        printk(KERN_DEBUG "rw_usb: Using current sector: %u\n", current_sector); // <<< RW_USB MARKER 3a
    } else {
        if ((offset % 512) != 0) {
            printk(KERN_WARNING "rw_usb: Warning: Offset %u is not sector aligned.\n", offset);
        }
        current_sector = offset / 512;
        printk(KERN_DEBUG "rw_usb: Using provided offset: %u -> sector: %u\n", offset, current_sector); // <<< RW_USB MARKER 3b
        cur_dev_sector = current_sector; // Update global current sector
    }

    /* Break down read/write into chunks */
    while (remaining > 0) {
         printk(KERN_DEBUG "rw_usb: Loop start: remaining=%u, processed=%u, current_sector=%u\n", remaining, processed, current_sector); // <<< RW_USB MARKER 4

        if (flag == true) {
            current_opf = REQ_OP_WRITE | REQ_SYNC;
        } else {
            current_opf = REQ_OP_READ;
        }
         printk(KERN_DEBUG "rw_usb: OPF=0x%x (%s)\n", current_opf, (flag ? "WRITE" : "READ")); // <<< RW_USB MARKER 5


         printk(KERN_DEBUG "rw_usb: Allocating bio...\n"); // <<< RW_USB MARKER 6
         bio = bio_alloc(bdevice, 1, current_opf, GFP_KERNEL);
         if (IS_ERR_OR_NULL(bio)) {
             long bio_err = bio ? PTR_ERR(bio) : -ENOMEM;
             printk(KERN_ERR "rw_usb: error: failed to allocate bio (ret = %ld).\n", bio_err);
             total_processed = bio_err;
             goto out;
         }
         printk(KERN_DEBUG "rw_usb: bio allocated: %p\n", bio); // <<< RW_USB MARKER 7

        bio->bi_iter.bi_sector = (sector_t)current_sector;
         printk(KERN_DEBUG "rw_usb: Set bio sector to %llu\n", bio->bi_iter.bi_sector); // <<< RW_USB MARKER 8

        bytes_in_chunk = (remaining > 512) ? 512 : remaining;
         printk(KERN_DEBUG "rw_usb: Calculated bytes_in_chunk = %u\n", bytes_in_chunk); // <<< RW_USB MARKER 9

         printk(KERN_DEBUG "rw_usb: Getting page for data + %u (%p)\n", processed, data + processed); // <<< RW_USB MARKER 10
         data_page = vmalloc_to_page(data + processed);
         if (!data_page) {
             printk(KERN_ERR "rw_usb: error: failed to get page for vmalloc address %p\n", data + processed);
             bio_put(bio);
             total_processed = -EFAULT;
             goto out;
         }
         printk(KERN_DEBUG "rw_usb: Got page %p\n", data_page); // <<< RW_USB MARKER 11

        offset_in_page = offset_in_page(data + processed);
         printk(KERN_DEBUG "rw_usb: Calculated offset_in_page = %u\n", offset_in_page); // <<< RW_USB MARKER 12


         printk(KERN_DEBUG "rw_usb: Adding page %p to bio for %u bytes at offset %u...\n", data_page, bytes_in_chunk, offset_in_page); // <<< RW_USB MARKER 13
         add_page_ret = bio_add_page(bio, data_page, bytes_in_chunk, offset_in_page); // Use declared variable
         if (add_page_ret != bytes_in_chunk) {
                printk(KERN_ERR "rw_usb: Error: bio_add_page added %d bytes, expected %u\n", add_page_ret, bytes_in_chunk);
                bio_put(bio);
                total_processed = -EIO;
                goto out;
            }
         printk(KERN_DEBUG "rw_usb: bio_add_page succeeded.\n"); // <<< RW_USB MARKER 14


         printk(KERN_DEBUG "rw_usb: >>> Submitting bio %p for sector %llu...\n", bio, bio->bi_iter.bi_sector); // <<< RW_USB MARKER 15
         bio_ret = submit_bio_wait(bio);
         printk(KERN_DEBUG "rw_usb: <<< submit_bio_wait returned %d\n", bio_ret); // <<< RW_USB MARKER 16


        if (bio_ret < 0 && bio_ret != -EOPNOTSUPP) {
             printk(KERN_ERR "rw_usb: error: submit_bio_wait failed (ret = %d) for sector %llu\n", bio_ret, bio->bi_iter.bi_sector);
             bio_put(bio);
             total_processed = (total_processed > 0) ? total_processed : bio_ret;
             goto out;
        } else if (bio_ret == -EOPNOTSUPP) {
             printk(KERN_WARNING "rw_usb: Warning: Operation not supported (ret = %d) for sector %llu\n", bio_ret, bio->bi_iter.bi_sector);
             bio_put(bio);
             total_processed = (total_processed > 0) ? total_processed : bio_ret;
             goto out;
        }

        bio_put(bio);
        bio = NULL;

        processed += bytes_in_chunk;
        remaining -= bytes_in_chunk;
        total_processed += bytes_in_chunk;
        current_sector++;

         printk(KERN_DEBUG "rw_usb: Loop end: processed=%u, remaining=%u, next_sector=%u\n", processed, remaining, current_sector); // <<< RW_USB MARKER 17

    } // end while (remaining > 0)

    if (offset == (unsigned int)-1 && total_processed == size) {
         cur_dev_sector = current_sector;
         printk(KERN_DEBUG "rw_usb: Updated cur_dev_sector to %u\n", cur_dev_sector); // <<< RW_USB MARKER 18a
     } else if (offset != (unsigned int)-1 && total_processed == size) {
          cur_dev_sector = current_sector;
           printk(KERN_DEBUG "rw_usb: RWOFFSET finished, updated cur_dev_sector to %u\n", cur_dev_sector); // <<< RW_USB MARKER 18b
     }

out:
    if (bio) {
        bio_put(bio);
    }
    printk(KERN_DEBUG "rw_usb: Releasing mutex...\n"); // <<< RW_USB MARKER 19
    mutex_unlock(&kmod_usb_mutex);

    printk(KERN_DEBUG "rw_usb: Returning %ld\n", total_processed); // <<< RW_USB MARKER 20
    return total_processed;
}


static void close_usb(void)
{
    printk(KERN_INFO "kmod_main: Closing USB device resources...\n");
    mutex_lock(&kmod_usb_mutex);

    if (usb_file && !IS_ERR(usb_file)) {
         printk(KERN_INFO "kmod_main: Closing USB device file pointer.\n");
        filp_close(usb_file, NULL);
        usb_file = NULL;
        bdevice = NULL;
    } else {
         printk(KERN_INFO "kmod_main: No USB device file pointer to close.\n");
    }

    mutex_unlock(&kmod_usb_mutex);
    printk(KERN_INFO "kmod_main: USB device resources closed.\n");
}

static int __init kmod_init(void)
{
    int ret = 0;
    printk(KERN_INFO "kmod_main: Loading kmod module...\n"); // <<< INIT MARKER 1

    printk(KERN_INFO "kmod_main: Initializing mutex...\n"); // <<< INIT MARKER 2
    mutex_init(&kmod_usb_mutex);
    printk(KERN_INFO "kmod_main: Mutex initialized.\n"); // <<< INIT MARKER 3

    printk(KERN_INFO "kmod_main: Calling open_usb...\n"); // <<< INIT MARKER 4
    if (!open_usb()) {
        ret = -ENODEV;
        goto exit;
    }
    printk(KERN_INFO "kmod_main: open_usb finished successfully.\n"); // <<< INIT MARKER 5

    printk(KERN_INFO "kmod_main: Calling kmod_ioctl_init...\n"); // <<< INIT MARKER 6
    if (!kmod_ioctl_init()) {
         pr_err("kmod_main: Failed to initialize IOCTL interface\n");
         ret = -EFAULT;
         goto exit_close_usb;
    }
    printk(KERN_INFO "kmod_main: kmod_ioctl_init finished successfully.\n"); // <<< INIT MARKER 7


    printk(KERN_INFO "kmod_main: Kernel module loaded successfully.\n"); // <<< INIT MARKER 8
    return 0; // Success

exit_close_usb:
    close_usb();
exit:
    printk(KERN_ERR "kmod_main: Module load failed with error %d\n", ret);
    return ret;
}

static void __exit kmod_fini(void)
{
    pr_info("kmod_main: Unloading kmod module...\n");
    kmod_ioctl_teardown();
    close_usb();
    pr_info("kmod_main: Kernel module unloaded.\n");
}

module_init(kmod_init);
module_exit(kmod_fini);