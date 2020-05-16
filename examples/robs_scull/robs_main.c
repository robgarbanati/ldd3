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
#include "access_ok_version.h"


//====================================================================================================//
//======================================= DEFINES AND TYPEDEFS =======================================//
//====================================================================================================//
#define SCULL_NR_DEVS 1
#define SCULL_QUANTUM 4000
#define SCULL_QSET    1000

typedef struct _scull_qset_t {
    void ** data;
    scull_qset *next;
} scull_qset;

typedef struct _scull_dev_t {
    scull_qset *data;           // Pointer to first quantum set.
    int quantum;                // The current quantum size.
    int qset;                   // The current array size.
    unsigned long size;         // Amount of data stored here. TODO what's the difference between this and qset?
    unsigned int access_key;    // Used by sculluid and scullpriv TODO how?
    struct mutex lock;          // mutex, duh
    struct cdev cdev;           // Char device structure. TODO what is that?
} scull_dev_t;

//====================================================================================================//
//========================================== LOCAL VARIABLES =========================================// 
//====================================================================================================//

int scull_major =   0;
int scull_minor =   0;
int scull_nr_devs = SCULL_NR_DEVS; // number of bare scull devices.
int scull_quantum = SCULL_QUANTUM;
int scull_qset = SCULL_QSET;
struct scull_dev *scull_devices; // allocated in scull_init_module.

//====================================================================================================//
//========================================= PRIVATE FUNCTIONS ========================================// 
//====================================================================================================//

static void scull_setup_cdev(scull_dev_t *dev, int index) {
    int err;
    int devno = MKDEV(scull_major, scull_minor + index);

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &scull_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    // Fail gracefully if need by.
    if (err) {
        printk(KERN_NOTICE "Error %d adding scull%d", err, index);
    }
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

void scull_cleanup_module(void) {
    int i;
    dev_t devno = MKDEV(scull_major, scull_minor);

    // Get rid of char dev entries TODO what does this mean and why do we want to do it?
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

    // Dynamically allocate major number. Minors will be defined within range TODO check that that's
    // right.
    result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
    scull_major = MAJOR(dev);

    if (result < 0) {
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return result;
    }

    // dynamically allocate devices. Helpful for later when we specify number to create at load
    // time.
    scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if (!scull_devices) {
        result = -ENOMEM; // TODO why negative?
        goto fail; // TODO use CHECK macro.
    }
    memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));
    
    // Initialize each device.
    for (i=0, i<scull_nr_devs; i++) {
        scull_devices[i].quantum = scull_quantum;
        scull_devices[i].qset = scull_qset;
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
