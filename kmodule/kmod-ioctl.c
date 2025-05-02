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

#include "../ioctl-defines.h" // Make sure this path is correct

#include <linux/vmalloc.h>

/* Device-related definitions */
static dev_t           dev = 0;
static struct class* kmod_class = NULL;
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
    long ret = 0;

    printk(KERN_DEBUG "kmod_ioctl: Entered: cmd=%u, arg=0x%lx\n", cmd, arg); // <<< IOCTL MARKER 1

    switch (cmd)
    {
        case BREAD:
        case BWRITE:
            printk(KERN_DEBUG "kmod_ioctl: Case BREAD/BWRITE\n"); // <<< IOCTL MARKER 2a

            printk(KERN_DEBUG "kmod_ioctl: Copying request from user...\n"); // <<< IOCTL MARKER 3
            if (copy_from_user((void*) &rw_request, (void __user*) arg, sizeof(struct block_rw_ops))) {
                printk(KERN_ERR "kmod_ioctl: Error: copy_from_user failed for RW.\n");
                return -EFAULT;
            }
            printk(KERN_DEBUG "kmod_ioctl: Request: data=%p, size=%u\n", rw_request.data, rw_request.size); // <<< IOCTL MARKER 4


            if (rw_request.size == 0 || rw_request.size > (1024 * 1024 * 1024)) {
                 printk(KERN_ERR "kmod_ioctl: Invalid size requested: %u\n", rw_request.size);
                 return -EINVAL;
            }
            printk(KERN_DEBUG "kmod_ioctl: Checking access_ok...\n"); // <<< IOCTL MARKER 5
            if (!access_ok((void __user*)rw_request.data, rw_request.size)) {
                 printk(KERN_ERR "kmod_ioctl: User buffer check failed for RW.\n");
                 return -EFAULT;
            }
            printk(KERN_DEBUG "kmod_ioctl: access_ok passed.\n"); // <<< IOCTL MARKER 6


            printk(KERN_DEBUG "kmod_ioctl: Allocating kernbuf (size %u)...\n", rw_request.size); // <<< IOCTL MARKER 7
            kernbuf = (char*) vmalloc(rw_request.size);
            if (!kernbuf) {
                printk(KERN_ERR "kmod_ioctl: error: vmalloc failed for RW.\n");
                return -ENOMEM;
            }
            printk(KERN_DEBUG "kmod_ioctl: kernbuf allocated: %p\n", kernbuf); // <<< IOCTL MARKER 8


            if (cmd == BWRITE) {
                printk(KERN_DEBUG "kmod_ioctl: BWRITE: Copying data from user buffer %p to kernbuf %p\n", rw_request.data, kernbuf); // <<< IOCTL MARKER 9a
                if (copy_from_user(kernbuf, (void __user*) rw_request.data, rw_request.size)) {
                    printk(KERN_ERR "kmod_ioctl: Error: copy_from_user failed for BWRITE data.\n");
                    vfree(kernbuf);
                    return -EFAULT;
                }
                printk(KERN_DEBUG "kmod_ioctl: BWRITE: Calling rw_usb...\n"); // <<< IOCTL MARKER 10a
                ret = rw_usb(kernbuf, rw_request.size, (unsigned int)-1, true);
                printk(KERN_DEBUG "kmod_ioctl: BWRITE: rw_usb returned %ld\n", ret); // <<< IOCTL MARKER 11a
            } else { // BREAD
                 printk(KERN_DEBUG "kmod_ioctl: BREAD: Calling rw_usb...\n"); // <<< IOCTL MARKER 10b
                ret = rw_usb(kernbuf, rw_request.size, (unsigned int)-1, false);
                 printk(KERN_DEBUG "kmod_ioctl: BREAD: rw_usb returned %ld\n", ret); // <<< IOCTL MARKER 11b

                if (ret >= 0) {
                     printk(KERN_DEBUG "kmod_ioctl: BREAD: Copying %ld bytes from kernbuf %p to user buffer %p\n", ret, kernbuf, rw_request.data); // <<< IOCTL MARKER 12
                    if (copy_to_user((void __user*) rw_request.data, kernbuf, ret)) {
                        printk(KERN_ERR "kmod_ioctl: Error: copy_to_user failed for BREAD.\n");
                        vfree(kernbuf);
                        return -EFAULT;
                    }
                    printk(KERN_DEBUG "kmod_ioctl: BREAD: copy_to_user succeeded.\n"); // <<< IOCTL MARKER 13
                }
            }

            printk(KERN_DEBUG "kmod_ioctl: Freeing kernbuf %p...\n", kernbuf); // <<< IOCTL MARKER 14
            vfree(kernbuf);
            kernbuf = NULL; // Avoid double free if error happens below

            if (ret < 0) {
                 printk(KERN_ERR "kmod_ioctl: rw_usb failed with error %ld\n", ret);
                 return ret;
            }
             printk(KERN_DEBUG "kmod_ioctl: RW op returning 0 (Success)\n"); // <<< IOCTL MARKER 15
            return 0;

        case BREADOFFSET:
        case BWRITEOFFSET:
             printk(KERN_DEBUG "kmod_ioctl: Case BREADOFFSET/BWRITEOFFSET\n"); // <<< IOCTL MARKER 2b
             // Add similar markers (3-15) for this case as needed
             printk(KERN_DEBUG "kmod_ioctl: Copying request from user...\n");
             if (copy_from_user((void*) &rwoffset_request, (void __user*) arg, sizeof(struct block_rwoffset_ops))) {
                 printk(KERN_ERR "kmod_ioctl: Error: copy_from_user failed for RWOFFSET.\n");
                 return -EFAULT;
             }
              printk(KERN_DEBUG "kmod_ioctl: Request: data=%p, size=%u, offset=%u\n", rwoffset_request.data, rwoffset_request.size, rwoffset_request.offset);

             if (rwoffset_request.size == 0 || rwoffset_request.size > (1024 * 1024 * 1024)) {
                 printk(KERN_ERR "kmod_ioctl: Invalid size requested: %u\n", rwoffset_request.size);
                 return -EINVAL;
             }
             if ((rwoffset_request.offset % 512) != 0) {
                 printk(KERN_WARNING "kmod_ioctl: Warning: Offset %u is not sector aligned (512 bytes).\n", rwoffset_request.offset);
             }
             printk(KERN_DEBUG "kmod_ioctl: Checking access_ok...\n");
             if (!access_ok((void __user*)rwoffset_request.data, rwoffset_request.size)) {
                 printk(KERN_ERR "kmod_ioctl: User buffer check failed for RWOFFSET.\n");
                 return -EFAULT;
             }
              printk(KERN_DEBUG "kmod_ioctl: access_ok passed.\n");


             printk(KERN_DEBUG "kmod_ioctl: Allocating kernbuf (size %u)...\n", rwoffset_request.size);
             kernbuf = (char*) vmalloc(rwoffset_request.size);
              if (!kernbuf) {
                 printk(KERN_ERR "kmod_ioctl: error: vmalloc failed for RWOFFSET.\n");
                 return -ENOMEM;
             }
              printk(KERN_DEBUG "kmod_ioctl: kernbuf allocated: %p\n", kernbuf);


             if (cmd == BWRITEOFFSET) {
                 printk(KERN_DEBUG "kmod_ioctl: BWRITEOFFSET: Copying data from user...\n");
                 if (copy_from_user(kernbuf, (void __user*) rwoffset_request.data, rwoffset_request.size)) {
                     printk(KERN_ERR "kmod_ioctl: Error: copy_from_user failed for BWRITEOFFSET data.\n");
                     vfree(kernbuf);
                     return -EFAULT;
                 }
                  printk(KERN_DEBUG "kmod_ioctl: BWRITEOFFSET: Calling rw_usb...\n");
                 ret = rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, true);
                  printk(KERN_DEBUG "kmod_ioctl: BWRITEOFFSET: rw_usb returned %ld\n", ret);
             } else { // BREADOFFSET
                 printk(KERN_DEBUG "kmod_ioctl: BREADOFFSET: Calling rw_usb...\n");
                 ret = rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, false);
                 printk(KERN_DEBUG "kmod_ioctl: BREADOFFSET: rw_usb returned %ld\n", ret);
                 if (ret >= 0) {
                     printk(KERN_DEBUG "kmod_ioctl: BREADOFFSET: Copying %ld bytes to user...\n", ret);
                     if (copy_to_user((void __user*) rwoffset_request.data, kernbuf, ret)) {
                         printk(KERN_ERR "kmod_ioctl: Error: copy_to_user failed for BREADOFFSET.\n");
                         vfree(kernbuf);
                         return -EFAULT;
                     }
                     printk(KERN_DEBUG "kmod_ioctl: BREADOFFSET: copy_to_user succeeded.\n");
                 }
             }

              printk(KERN_DEBUG "kmod_ioctl: Freeing kernbuf %p...\n", kernbuf);
             vfree(kernbuf);
             kernbuf = NULL;

             if (ret < 0) {
                  printk(KERN_ERR "kmod_ioctl: rw_usb failed with error %ld\n", ret);
                  return ret;
             }
              printk(KERN_DEBUG "kmod_ioctl: RWOFFSET op returning 0 (Success)\n");
             return 0;

        default:
             printk(KERN_WARNING "kmod_ioctl: Error: incorrect operation requested (%u), returning -EINVAL.\n", cmd); // <<< IOCTL MARKER 2c
            return -EINVAL;
    }
}

static int kmod_open(struct inode* inode, struct file* file) {
    printk(KERN_INFO "kmod: Opened kmod device. \n");
    return 0;
}

static int kmod_release(struct inode* inode, struct file* file) {
    printk(KERN_INFO "kmod: Closed kmod device. \n");
    return 0;
}

static struct file_operations fops =
{
    .owner          = THIS_MODULE,
    .open           = kmod_open,
    .release        = kmod_release,
    .unlocked_ioctl = kmod_ioctl,
};

/* Initialize the module for IOCTL commands */
bool kmod_ioctl_init(void) {
    long ret = 0;

    /* Allocate a character device. */
    if (alloc_chrdev_region(&dev, 0, 1, "usbaccess") < 0) {
        printk(KERN_ERR "kmod_ioctl: error: couldn't allocate 'usbaccess' character device.\n");
        return false;
    }
    printk(KERN_INFO "kmod_ioctl: Allocated chrdev region major=%d minor=%d\n", MAJOR(dev), MINOR(dev));

    /* Initialize the chardev with my fops. */
    cdev_init(&kmod_cdev, &fops);
    kmod_cdev.owner = THIS_MODULE;
    if (cdev_add(&kmod_cdev, dev, 1) < 0) {
        printk(KERN_ERR "kmod_ioctl: error: couldn't add kmod_cdev.\n");
        unregister_chrdev_region(dev, 1);
        return false;
    }
     printk(KERN_INFO "kmod_ioctl: cdev added successfully.\n");


#if LINUX_VERSION_CODE <= KERNEL_VERSION(6,3,0)
    kmod_class = class_create(THIS_MODULE, "kmod_class");
    if (IS_ERR(kmod_class)) {
         pr_err("kmod_ioctl: error: couldn't create kmod_class with owner.\n");
         ret = PTR_ERR(kmod_class);
         kmod_class = NULL;
         goto cdevfailed;
    }
#else
    kmod_class = class_create("kmod_class");
     if (IS_ERR(kmod_class)) {
         pr_err("kmod_ioctl: error: couldn't create kmod_class.\n");
         ret = PTR_ERR(kmod_class);
         kmod_class = NULL;
         goto cdevfailed;
    }
#endif
    printk(KERN_INFO "kmod_ioctl: kmod_class created.\n");

    struct device *kmod_device = device_create(kmod_class, NULL, dev, NULL, "kmod");
    if (IS_ERR(kmod_device)) {
        printk(KERN_ERR "kmod_ioctl: error: couldn't create device file /dev/kmod.\n");
        ret = PTR_ERR(kmod_device);
        goto classfailed;
    }
    printk(KERN_INFO "kmod_ioctl: Device /dev/kmod created.\n");


    printk(KERN_INFO "[*] kmod_ioctl: IOCTL device initialization complete.\n");
    return true;

classfailed:
    class_destroy(kmod_class);
    kmod_class = NULL;
cdevfailed:
    cdev_del(&kmod_cdev);
    unregister_chrdev_region(dev, 1);
    printk(KERN_ERR "kmod_ioctl: IOCTL device initialization failed (error %ld).\n", ret);
    return false;
}

void kmod_ioctl_teardown(void) {
    printk(KERN_INFO "[*] kmod_ioctl: Starting teardown...\n");
    if (kmod_class) {
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