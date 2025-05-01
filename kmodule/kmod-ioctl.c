#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/dcache.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/freezer.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/version.h> // Include for KERNEL_VERSION
#include <linux/err.h>     // Include for IS_ERR, PTR_ERR

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>

#include <linux/cdev.h>
#include <linux/nospec.h>

#include "../ioctl-defines.h"

#include <linux/vmalloc.h>

/* Device-related definitions */
static dev_t           dev = 0;
static struct class* kmod_class = NULL; // Initialize to NULL
static struct cdev       kmod_cdev;

/* Buffers for different operation requests */
struct block_rw_ops rw_request;
struct block_rwoffset_ops rwoffset_request;

/* Forward declaration from kmod-main.c */
extern long rw_usb(char* data, unsigned int size, unsigned int offset, bool flag);


bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

static long kmod_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    char* kernbuf = NULL;
    long ret = 0; // Use long for return value consistency

    switch (cmd)
    {
        case BREAD:
        case BWRITE:
            /* Get request from user */
            if (copy_from_user((void*) &rw_request, (void __user*) arg, sizeof(struct block_rw_ops))) { // Added __user annotation
                printk(KERN_ERR "kmod_ioctl: Error: Incorrect request parameters for RW.\n"); // Use KERN_ERR for errors
                return -EFAULT; // Return standard error code
            }

            /* Input validation */
            if (rw_request.size == 0 || rw_request.size > (1024 * 1024 * 1024)) { // Basic sanity check for size
                 printk(KERN_ERR "kmod_ioctl: Invalid size requested: %u\n", rw_request.size);
                 return -EINVAL;
            }
            // Check user pointer validity before allocation
            if (!access_ok((void __user*)rw_request.data, rw_request.size)) {
                 printk(KERN_ERR "kmod_ioctl: User buffer check failed for RW.\n");
                 return -EFAULT;
            }


            /* Debugging */
            printk(KERN_INFO "kmod_ioctl: REQUEST: RW (data=%p, size=%u)\n", rw_request.data, rw_request.size); // Use KERN_INFO

            /* Allocate a kernel buffer to read/write user data */
            kernbuf = (char*) vmalloc(rw_request.size);
            if (!kernbuf) { // Check !kernbuf, IS_ERR is for pointer-encoded errors, vmalloc returns NULL on failure
                printk(KERN_ERR "kmod_ioctl: error: could not allocate memory (%u bytes) for the RW operation.\n", rw_request.size);
                return -ENOMEM; // Standard error code for memory allocation failure
            }

            /* Perform the block operation */
            if (cmd == BWRITE) {
                /* WRITE */
                // Use copy_from_user() to copy rw_request.size bytes from rw_request.data into kernbuf
                if (copy_from_user(kernbuf, (void __user*) rw_request.data, rw_request.size)) {
                    printk(KERN_ERR "kmod_ioctl: Error: Failed to copy data from user for BWRITE.\n");
                    vfree(kernbuf); // Free allocated buffer before returning
                    return -EFAULT;
                }
                // Call rw_usb() with -1 offset to use the current offset, flag=true for write
                ret = rw_usb(kernbuf, rw_request.size, (unsigned int)-1, true); // Explicit cast for -1 offset
            } else {
                /* READ */
                // Call rw_usb() with -1 offset to use the current offset, flag=false for read
                ret = rw_usb(kernbuf, rw_request.size, (unsigned int)-1, false); // Explicit cast for -1 offset
                if (ret >= 0) { // Check if rw_usb succeeded (returned bytes processed or 0)
                     // Use copy_to_user() to transfer ret bytes (actual bytes read)
                    if (copy_to_user((void __user*) rw_request.data, kernbuf, ret)) { // Use ret as size
                        printk(KERN_ERR "kmod_ioctl: Error: Failed to copy data to user for BREAD.\n");
                        vfree(kernbuf);
                        return -EFAULT;
                    }
                }
            }

            vfree(kernbuf); // Free the buffer after use

            // Check the return value from rw_usb
            if (ret < 0) {
                 printk(KERN_ERR "kmod_ioctl: rw_usb failed with error %ld\n", ret);
                 return ret; // Propagate the error from rw_usb
            }
            // If rw_usb succeeded, ret >= 0 (bytes processed). IOCTL should return 0 on success.
            return 0;

        case BREADOFFSET:
        case BWRITEOFFSET:
            /* Get request from user */
            if (copy_from_user((void*) &rwoffset_request, (void __user*) arg, sizeof(struct block_rwoffset_ops))) { // Added __user annotation
                printk(KERN_ERR "kmod_ioctl: Error: Incorrect request parameters for RWOFFSET.\n"); // Use KERN_ERR
                return -EFAULT; // Return standard error code
            }

             /* Input validation */
            if (rwoffset_request.size == 0 || rwoffset_request.size > (1024 * 1024 * 1024)) { // Basic sanity check for size
                 printk(KERN_ERR "kmod_ioctl: Invalid size requested: %u\n", rwoffset_request.size);
                 return -EINVAL;
            }
             // Add check for offset validity if possible (e.g., less than device size, though size isn't known here)
             // Check alignment if necessary (offset should ideally be sector aligned)
            if ((rwoffset_request.offset % 512) != 0) {
                 // This is just a warning, the block layer might handle it, or rw_usb might align it.
                 printk(KERN_WARNING "kmod_ioctl: Warning: Offset %u is not sector aligned (512 bytes).\n", rwoffset_request.offset);
            }
            // Check user pointer validity before allocation
            if (!access_ok((void __user*)rwoffset_request.data, rwoffset_request.size)) {
                 printk(KERN_ERR "kmod_ioctl: User buffer check failed for RWOFFSET.\n");
                 return -EFAULT;
            }


            /* Debugging */
            printk(KERN_INFO "kmod_ioctl: REQUEST: RWOFFSET (data=%p, size=%u, offset=%u)\n", // Use KERN_INFO
                   rwoffset_request.data,
                   rwoffset_request.size,
                   rwoffset_request.offset);

            /* Allocate a kernel buffer to read/write user data */
            kernbuf = (char*) vmalloc(rwoffset_request.size);
             if (!kernbuf) { // Check !kernbuf
                printk(KERN_ERR "kmod_ioctl: error: could not allocate memory (%u bytes) for the RWOFFSET operation.\n", rwoffset_request.size);
                return -ENOMEM;
            }


            /* Perform the block operation */
            if (cmd == BWRITEOFFSET) {
                /* WRITEOFFSET */
                 // Copy data from user
                if (copy_from_user(kernbuf, (void __user*) rwoffset_request.data, rwoffset_request.size)) {
                     printk(KERN_ERR "kmod_ioctl: Error: Failed to copy data from user for BWRITEOFFSET.\n");
                     vfree(kernbuf);
                     return -EFAULT;
                 }
                 // Call rw_usb with specific offset, flag=true for write
                ret = rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, true);
            } else {
                /* READOFFSET */
                 // Call rw_usb with specific offset, flag=false for read
                ret = rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, false);
                if (ret >= 0) { // Check if rw_usb succeeded (returned bytes processed or 0)
                    // Copy data to user
                    if (copy_to_user((void __user*) rwoffset_request.data, kernbuf, ret)) { // Use ret as size
                         printk(KERN_ERR "kmod_ioctl: Error: Failed to copy data to user for BREADOFFSET.\n");
                         vfree(kernbuf);
                         return -EFAULT;
                    }
                }
            }

            vfree(kernbuf); // Free the buffer after use

            // Check the return value from rw_usb
            if (ret < 0) {
                 printk(KERN_ERR "kmod_ioctl: rw_usb failed with error %ld\n", ret);
                 return ret; // Propagate the error from rw_usb
            }
            // If rw_usb succeeded, ret >= 0 (bytes processed). IOCTL should return 0 on success.
            return 0;

        default:
            printk(KERN_WARNING "kmod_ioctl: Error: incorrect operation requested (%u), returning.\n", cmd); // Use KERN_WARNING or KERN_ERR
            return -EINVAL; // Standard error code for invalid argument/command
    }
}

static int kmod_open(struct inode* inode, struct file* file) {
    printk(KERN_INFO "kmod: Opened kmod device. \n"); // Use KERN_INFO
    return 0;
}

static int kmod_release(struct inode* inode, struct file* file) {
    printk(KERN_INFO "kmod: Closed kmod device. \n"); // Use KERN_INFO
    return 0;
}

static struct file_operations fops =
{
    .owner          = THIS_MODULE,
    .open           = kmod_open,
    .release        = kmod_release,
    .unlocked_ioctl = kmod_ioctl,
    // Add .compat_ioctl = kmod_ioctl if supporting 32-bit userspace on 64-bit kernel
};

/* Initialize the module for IOCTL commands */
bool kmod_ioctl_init(void) {
    long ret = 0; // <<< DECLARED ret HERE

    /* Allocate a character device. */
    if (alloc_chrdev_region(&dev, 0, 1, "usbaccess") < 0) {
        printk(KERN_ERR "kmod_ioctl: error: couldn't allocate 'usbaccess' character device.\n"); // Use KERN_ERR
        return false;
    }
    printk(KERN_INFO "kmod_ioctl: Allocated chrdev region major=%d minor=%d\n", MAJOR(dev), MINOR(dev)); // Added info log

    /* Initialize the chardev with my fops. */
    cdev_init(&kmod_cdev, &fops);
    kmod_cdev.owner = THIS_MODULE; // Explicitly set owner
    if (cdev_add(&kmod_cdev, dev, 1) < 0) {
        printk(KERN_ERR "kmod_ioctl: error: couldn't add kmod_cdev.\n");
        // Don't goto cdevfailed yet, just unregister region
        unregister_chrdev_region(dev, 1);
        return false;
    }
     printk(KERN_INFO "kmod_ioctl: cdev added successfully.\n"); // Added info log


#if LINUX_VERSION_CODE <= KERNEL_VERSION(6,3,0) // Adjusted version check based on when class_create signature changed
    // Older kernels used class_create(owner, name)
    kmod_class = class_create(THIS_MODULE, "kmod_class");
    if (IS_ERR(kmod_class)) { // Check return value using IS_ERR for class_create
         pr_err("kmod_ioctl: error: couldn't create kmod_class with owner.\n");
         ret = PTR_ERR(kmod_class); // Now 'ret' is declared
         kmod_class = NULL; // Set to NULL for cleanup check
         goto cdevfailed; // Cleanup cdev and region
    }
#else
    // Newer kernels use class_create(name)
    kmod_class = class_create("kmod_class");
     if (IS_ERR(kmod_class)) { // Check return value using IS_ERR
         pr_err("kmod_ioctl: error: couldn't create kmod_class.\n");
         ret = PTR_ERR(kmod_class); // Now 'ret' is declared
         kmod_class = NULL;
         goto cdevfailed; // Cleanup cdev and region
    }
#endif
    printk(KERN_INFO "kmod_ioctl: kmod_class created.\n");

    // Create the device file node
    struct device *kmod_device = device_create(kmod_class, NULL, dev, NULL, "kmod");
    if (IS_ERR(kmod_device)) { // Check stored return value using IS_ERR
        printk(KERN_ERR "kmod_ioctl: error: couldn't create device file /dev/kmod.\n");
        ret = PTR_ERR(kmod_device); // Get error code
        goto classfailed; // Cleanup class, cdev, region
    }
    printk(KERN_INFO "kmod_ioctl: Device /dev/kmod created.\n");


    printk(KERN_INFO "[*] kmod_ioctl: IOCTL device initialization complete.\n"); // Use KERN_INFO
    return true;

classfailed: // label for class creation failure
    class_destroy(kmod_class);
    kmod_class = NULL; // Ensure kmod_class is NULL for teardown check
cdevfailed: // label for cdev add failure (or class creation failure)
    cdev_del(&kmod_cdev);
    unregister_chrdev_region(dev, 1);
    printk(KERN_ERR "kmod_ioctl: IOCTL device initialization failed (error %ld).\n", ret);
    return false; // Keep return type bool as per original code
}

void kmod_ioctl_teardown(void) {
    printk(KERN_INFO "[*] kmod_ioctl: Starting teardown...\n");
    /* Destroy the classes too (IOCTL-specific). */
    if (kmod_class) { // Check if class was created successfully
        device_destroy(kmod_class, dev);
         printk(KERN_INFO "kmod_ioctl: Device /dev/kmod destroyed.\n");
        class_destroy(kmod_class);
         printk(KERN_INFO "kmod_ioctl: kmod_class destroyed.\n");
    }
    cdev_del(&kmod_cdev);
     printk(KERN_INFO "kmod_ioctl: kmod_cdev deleted.\n");
    unregister_chrdev_region(dev,1);
     printk(KERN_INFO "kmod_ioctl: Character device region unregistered.\n");

    printk(KERN_INFO "[*] kmod_ioctl: IOCTL device teardown complete.\n");
}