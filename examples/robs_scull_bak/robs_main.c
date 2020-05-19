#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/seq_file.h>
#include <linux/cdev.h>

#include <linux/uaccess.h>	/* copy_*_user */

/*#include "scull.h"		[> local definitions <]*/
#include "robs_scull.h"		// local definitions
#include "access_ok_version.h"

//====================================================================================================//
//========================================== LOCAL VARIABLES =========================================// 
//====================================================================================================//

int scull_major =   0;
int scull_minor =   0;
int scull_nr_devs = SCULL_NR_DEVS; // number of bare scull devices.
int quantum_size = QUANTUM_SIZE;
int qset_array_size = QSET_ARRAY_SIZE;
scull_dev_t *scull_devices; // allocated in scull_init_module.

struct file_operations scull_fops = {
    .owner = THIS_MODULE,
    /*.llseek = scull_llseek,*/
    .read = scull_read,
    .write = scull_write,
    /*.unlocked_ioctl = scull_ioctl,*/
    .open = scull_open,
    .release = scull_release,
    .llseek = NULL,
    /*.read = NULL,*/
    /*.write = NULL,*/
    .unlocked_ioctl = NULL,
    /*.open = NULL,*/
    /*.release = NULL,*/
};

//====================================================================================================//
//========================================= PRIVATE FUNCTIONS ========================================// 
//====================================================================================================//

static void scull_setup_cdev(scull_dev_t *scull_device, int index) {
    int err;
    int devno = MKDEV(scull_major, scull_minor + index);

    cdev_init(&scull_device->cdev, &scull_fops); // TODO what does this do? Does kobject_init.
    scull_device->cdev.owner = THIS_MODULE;
    scull_device->cdev.ops = &scull_fops;
    err = cdev_add (&scull_device->cdev, devno, 1);
    // Fail gracefully if need by.
    if (err) {
        printk(KERN_NOTICE "Error %d adding scull%d", err, index);
    }
}

// Free all memory in scull device.
static int scull_trim(scull_dev_t *scull_device) {
    scull_qset_t *next, *qset_ptr;
    int qset_array_size = scull_device->qset_array_size;
    int i;
    
    // from first qset in the array to the last...
    for(qset_ptr = scull_device->qset_list_head; qset_ptr; qset_ptr = next) {
        // is quantum_array not null?
        if(qset_ptr->quantum_array) {
            // free all data in current quantum_array.
            for(i=0; i<qset_array_size; i++) { // TODO this feels like the wrong end condition.
                kfree(qset_ptr->quantum_array[i]);
            }
            // free quantum array itself.
            kfree(qset_ptr->quantum_array);
            qset_ptr->quantum_array = NULL;
        }
        // free current qset.
        next = qset_ptr->next;
        kfree(qset_ptr);
    }

    scull_device->tail = 0;
    scull_device->quantum_size = quantum_size;
    scull_device->qset_array_size = qset_array_size;
    scull_device->qset_list_head = NULL;
    return 0;
}

// Traverse the list to the nth list item.
static scull_qset_t *scull_follow(scull_dev_t *scull_device, int n) {
    scull_qset_t *qs = scull_device->qset_list_head;
    int retval = 0;

    // Allocate first qset explicitly, if need be.
    if(!qs) {
        // This is a normal allocation and might block. GFP_KERNEL should be used in process context code when it is safe to sleep.
        qs = scull_device->qset_list_head = kmalloc(sizeof(scull_qset_t), GFP_KERNEL);
        CHECK(qs!=NULL, 0, "kmalloc seemed to fail.");

        memset(qs, 0, sizeof(scull_qset_t));
    }

    // Follow the list.
    while(n--) {
        // Is this the final qset in the list?
        if(!qs->next) {
            qs->next = kmalloc(sizeof(scull_qset_t), GFP_KERNEL);
            CHECK(qs->next != NULL, 0, "kmalloc seemed to fail.");
            memset(qs->next, 0, sizeof(scull_qset_t));
        }
        qs = qs->next;
        continue;
    }
    return qs;

error:
    return NULL;
}

/*
 * // Initialize the "pipe devs" (TODO what are "pipe devs")
 *static int scull_p_init(dev_t firstdev) {
 *    int i;
 *    int result;
 *
 *    result = register_chrdev_region(firstdev, scull_p_nr_devs, "scullp");
 *    if (result < 0) {
 *        printk(KERN_NOTICE "Unable to get scullp region, error %d\n", result);
 *        return 0;
 *    }
 *    scull_p_devno = firstdev;
 *    // TODO completeme
 */

//====================================================================================================//
//========================================= PUBLIC FUNCTIONS =========================================// 
//====================================================================================================//


// Open operation.
int scull_open(struct inode *inode, struct file *filp) {
    scull_dev_t *scull_device;
    int retval=0;

    printk(KERN_INFO "Invoking scull_open\n");

    // Find device from cdev field in inode.
    scull_device = container_of(inode->i_cdev, scull_dev_t, cdev);
    // Put device in private_data, so that we can have it for future file operations.
    filp->private_data = scull_device;

    // Was open write-only?
    if((filp->f_flags & O_ACCMODE) == O_WRONLY) {
        CHECK(mutex_lock_interruptible(&scull_device->lock) == 0, -ERESTARTSYS, "lock failed.");
        scull_trim(scull_device);
        mutex_unlock(&scull_device->lock);
    }

error:
    return retval;
}


int scull_release(struct inode *inode, struct file *filp)
{
    return 0;
}

loff_t struct_llseek(struct file *filp, loff_t off, int whence) {
    scull_dev_t *scull_device = filp->private_data;
    loff_t newpos;
    int retval;

    switch(whence) {
        case 0: // SEEK_SET
            newpos = off;
            break;
        case 1: // SEEK_CUR
            newpos = filp->f_pos + off;
            break;
        case 2: // SEEK_END
            newpos = scull_device->tail + off;
            break;
        default: // shouldn't happen
            CHECK(0, -EINVAL, "illegal llseek whence: %lld.", whence);
            break;
    }
    CHECK(newpos >= 0, -EINVAL, "position seeked < 0. pos = %lld.", newpos);

    // Set new file position.
    filp->f_pos = newpos;

error:
    return retval;
}

ssize_t scull_read(struct file *filp, char __user *user_ptr, size_t count, loff_t *f_pos) {
    scull_dev_t *scull_device = filp->private_data;
    scull_qset_t *qset_ptr;
    int quantum_size = scull_device->quantum_size;
    int qset_array_size = scull_device->qset_array_size;
    int qset_size = quantum_size * qset_array_size; // How many bytes in each qset.
    int qset_list_index, quantum_index, byte_pos_in_quantum, byte_pos_in_qset;
    ssize_t retval=0;

    printk(KERN_INFO "invoking scull_read\n");
    
    // Try to lock mutex, if error restart. TODO check what this code really does.
    CHECK(mutex_lock_interruptible(&scull_device->lock) == 0, -ERESTARTSYS, "mutex failed.");

    // Exit if read position outside of amount of data written.
    CHECK(*f_pos < scull_device->tail, 0, "fpos requested bigger than written data.");

    // Reduce read length to fit in readable area (written, valid data) if necessary.
    if(*f_pos + count > scull_device->tail) {
        count = scull_device->tail - *f_pos;
    }

    // find listitem, qset index, and offset in the quantum
    qset_list_index = (long)*f_pos / qset_size; // which qset?
    byte_pos_in_qset = (long)*f_pos % qset_size;
    quantum_index = byte_pos_in_qset / quantum_size; // which quantum?
    byte_pos_in_quantum = byte_pos_in_qset % quantum_size; // which byte in the quantum?
    
    // follow the list up to the right position (defined elsewhere).
    qset_ptr = scull_follow(scull_device, qset_list_index);

    // is qset and quantum_array and quantum all valid?
    CHECK(qset_ptr != NULL && qset_ptr->quantum_array && qset_ptr->quantum_array[quantum_index], 0, "qset or quantum_array or quantum isn't valid.");

    // read only up to the end of this quantum.
    if(count > quantum_size - byte_pos_in_quantum) {
        count = quantum_size - byte_pos_in_quantum;
    }

    // actually perform the "read": copy the requested data to the user ptr. TODO can this be done
    // with a double array-dereference and an &? Looks like copy_to_user wants a pointer to first
    // byte to read.
    CHECK(copy_to_user(user_ptr, qset_ptr->quantum_array[quantum_index] + byte_pos_in_quantum, count) == 0, -EFAULT, "copy_to_user errored.");

    // Update file position.
    *f_pos += count;
    retval = count;

error:
    mutex_unlock(&scull_device->lock);
    return retval;
}

ssize_t scull_write(struct file *filp, const char __user *user_ptr, size_t count, loff_t *f_pos) {
    scull_dev_t *scull_device = filp->private_data;
    scull_qset_t *qset_ptr;
    int quantum_size = scull_device->quantum_size;
    int qset_array_size = scull_device->qset_array_size;
    int qset_size = quantum_size * qset_array_size;
    int qset_list_index, quantum_index, byte_pos_in_qset, byte_pos_in_quantum;
    ssize_t retval=0;

    printk(KERN_INFO "invoking scull_write\n");
    CHECK(mutex_lock_interruptible(&scull_device->lock) == 0, -ERESTARTSYS, "lock mutex failed.");

    // find qset, quantum, and offset in the quantum.
    qset_list_index = (long)*f_pos / qset_size;
    byte_pos_in_qset = (long)*f_pos % qset_size;
    quantum_index = byte_pos_in_qset / quantum_size;
    byte_pos_in_quantum = byte_pos_in_qset % quantum_size;

    // get the pointer to the correct qset in the list
    qset_ptr = scull_follow(scull_device, qset_list_index);
    CHECK(qset_ptr!=NULL, 0, "scull_follow failed.");

    // is quantum_array not allocated in current qset?
    if(!qset_ptr->quantum_array) {
        // allocate it.
        qset_ptr->quantum_array = kmalloc(quantum_size, GFP_KERNEL);
        CHECK(qset_ptr->quantum_array, 0, "kmalloc failed.");
    }

    // Is quantum at quantum_index not allocated?
    if(!qset_ptr->quantum_array[quantum_index]) {
        // Allocate it.
        qset_ptr->quantum_array[quantum_index] = kmalloc(quantum_size, GFP_KERNEL);
        CHECK(qset_ptr->quantum_array[quantum_index], 0, "kmalloc failed.");
    }

    // Write only up to the end of this quantum.
    if(count > quantum_size - byte_pos_in_quantum) {
        count = quantum_size - byte_pos_in_quantum;
    }

    // actually grab data from user
    CHECK(copy_from_user(qset_ptr->quantum_array[quantum_index]+byte_pos_in_quantum, user_ptr, count)==0, -EFAULT, "copy_from_user failed.");

    // Update file position.
    f_pos += count;
    // Update "size" of the file.
    if(scull_device->tail < *f_pos) {
        scull_device->tail = *f_pos;
    }

    // update amount written, to be returned later.
    retval = count;
     
error:
    mutex_unlock(&scull_device->lock);
    return retval;
}

void scull_cleanup_module(void) {
    int i;
    dev_t devno = MKDEV(scull_major, scull_minor);

    printk(KERN_INFO "goodbye world\n");
    // Get rid of char dev entries TODO what does this mean and why do we want to do it? I think it
    // means delete them from /proc?
    if (scull_devices) {
        for (i = 0; i < scull_nr_devs; i++) {
            scull_trim(scull_devices + i);
            cdev_del(&scull_devices[i].cdev); // I'm guessing this deletes it from /dev.
        }
        kfree(scull_devices);
    }

#ifdef SCULL_DEBUG
    scull_remove_proc();
#endif

    // Cleanup module is never called if registering failed.
    unregister_chrdev_region(devno, scull_nr_devs);
}

int scull_init_module(void) {
    int result, i;
    dev_t dev = 0;

    printk(KERN_INFO "hello world\n");
    // Dynamically allocate major number. Minors will be defined within range TODO check that that's
    // right.
    result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
    scull_major = MAJOR(dev);
    printk(KERN_INFO "major number is %d\n", scull_major);

    if (result < 0) {
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return result;
    }

    // dynamically allocate devices. Helpful for later when we specify number to create at load
    // time.
    scull_devices = kmalloc(scull_nr_devs * sizeof(scull_dev_t), GFP_KERNEL);
    if (!scull_devices) {
        result = -ENOMEM; // TODO why negative?
        goto fail; // TODO use CHECK macro.
    }
    memset(scull_devices, 0, scull_nr_devs * sizeof(scull_dev_t));
    
    // Initialize each device.
    for (i=0; i<scull_nr_devs; i++) {
        scull_devices[i].quantum_size = quantum_size;
        scull_devices[i].qset_array_size = qset_array_size;
        mutex_init(&scull_devices[i].lock);
        scull_setup_cdev(&scull_devices[i], i);
    }

/*
 *    // Call the init function for any friend device.
 *    dev = MKDEV(scull_major, scull_minor + scull_nr_devs);
 *    dev += scull_p_init(dev);
 *    dev += scull_access_init(dev);
 */

#ifdef SCULL_DEBUG
    scull_create_proc();
#endif

    return 0;

fail:
    scull_cleanup_module();
    return result;
}


module_init(scull_init_module);
module_exit(scull_cleanup_module);
