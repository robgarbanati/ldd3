#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/timer.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rob Garbanati");

//====================================================================================================//
//======================================== DEFINES AND TYPEDEFS ======================================// 
//====================================================================================================//
#define LOG_ERR(ret, fmt, ...)							\
	(void)printk(KERN_ALERT							\
	  "file: %s, " /*+*/			        			\
	  "function: %s, " /*+*/		        			\
	  "line: %d, " /*+*/			        			\
	  "retval: %d, " /*+*/                          			\
	  "message: " fmt /*+*/                          			\
	  "\n", __FILE__, __func__, __LINE__, ret, ##__VA_ARGS__)	\

#define CHECK(A, tag, ret, fmt, ...)							\
    if (!(A)) {									\
        LOG_ERR(ret, fmt, ##__VA_ARGS__);    				        \
        retval = ret;                                                           \
        goto tag;								\
    }

#define KERNEL_SECTOR_SIZE      512
#define INVALIDATE_DELAY        30*HZ
#define SBULL_MINORS            16

#define MINOR_SHIFT	4
#define DEVNUM(kdevnum)	(MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT

/*
 * The different "request modes" we can use.
 */
enum {
	RM_SIMPLE  = 0,	/* The extra-simple request function */
	RM_FULL    = 1,	/* The full-blown version */
	RM_NOQUEUE = 2,	/* Use make_request */
};

// Create a struct to represent our device
struct sbull_dev {
    int size;                   // Device size in number of sectors.
    u8 *data;                   // Pointer to the actual data array.
    short users;                 // Keep track of how many users are using this device? TODO explain better.
    short media_change;         // Flag a media change?
    spinlock_t lock;           // For locking TODO what?
    struct request_queue *queue;// The device request queue.
    struct gendisk *gd;         // The gendisk structure.
    /*struct timer_list timer;    // For simulated media changes.*/
};


//====================================================================================================//
//========================================== LOCAL VARIABLES =========================================// 
//====================================================================================================//
static int request_mode = RM_SIMPLE;

static int sbull_major = 0;
module_param(sbull_major, int, 0);
static int hardsect_size = 512; // matches default linux sector size.
module_param(hardsect_size, int, 0);
static int nsectors = 1024; // Describes size of the drive.
module_param(nsectors, int, 0);
static int ndevices = 4; // TODO why 4 devices?
module_param(ndevices, int, 0);

static struct sbull_dev *Devices = NULL;

//====================================================================================================//
//========================================= PRIVATE FUNCTIONS ========================================// 
//====================================================================================================//
static void sbull_transfer(struct sbull_dev *dev, unsigned long sector, unsigned long nsect,
                            char *buffer, int write) {
    int retval;
    unsigned long offset = sector*KERNEL_SECTOR_SIZE;
    unsigned long nbytes = nsect*KERNEL_SECTOR_SIZE;

    CHECK(offset + nbytes <= dev->size, error, 0, "Beyond-end write (%ld %ld)", offset, nbytes);
    if(write) {
        memcpy(dev->data + offset, buffer, nbytes);
    } else {
        memcpy(buffer, dev->data + offset, nbytes);
    }

error:
    return;
}

// Simply call sbull_transfer on all requests in the queue, one at a time, in a serial, blocking manner.
static void sbull_request(struct request_queue *rq) {
    struct request *req;
    u8 *data;
    int direction;

    // For each request in the queue...
    while( (req = blk_fetch_request(rq)) != NULL) {

        struct sbull_dev *sd = req->rq_disk->private_data;

        // Ignore requests we choose not to support.
/*
 *        // cmd_type is obsolete TODO find another way to do this?
 *        if(req->cmd_type != REQ_TYPE_FS) {
 *            printk(KERN_NOTICE "Skip non-fs request\n");
 *            __blk_end_request_cur(req, -EIO);
 *            continue;
 *        }
 */

        // Call sbull_transfer, getting all necessary information from the request.
        /*sbull_transfer(sd, blk_rq_pos(req), blk_rq_cur_sectors(req), req->buffer, rq_data_dir(req));*/

        direction = rq_data_dir(req);

        if (direction == WRITE) {
            printk("Writing data to the block device\n");
        } else {
            printk("Reading data from the block devicen\n");
        }

        data = bio_data(req->bio); /* Data buffer to perform I/O operations */

        sbull_transfer(sd, blk_rq_pos(req), blk_rq_cur_sectors(req), data, direction);
        
        __blk_end_request_cur(req, 0);
    }
}

/*
 * Open and close.
 */

static int sbull_open(struct block_device *bdev, fmode_t mode)
{
	struct sbull_dev *dev = bdev->bd_disk->private_data;

	/*del_timer_sync(&dev->timer);*/
	//filp->private_data = dev;
	spin_lock(&dev->lock);
	if (! dev->users) 
		check_disk_change(bdev);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

static void sbull_release(struct gendisk *disk, fmode_t mode)
{
	struct sbull_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;

        /*
	 *if (!dev->users) {
	 *        dev->timer.expires = jiffies + INVALIDATE_DELAY;
	 *        add_timer(&dev->timer);
	 *}
         */
	spin_unlock(&dev->lock);
}

/*
 * Look for a (simulated) media change.
 */
int sbull_media_changed(struct gendisk *gd)
{
	struct sbull_dev *dev = gd->private_data;
	
	return dev->media_change;
}

/*
 * Revalidate.  WE DO NOT TAKE THE LOCK HERE, for fear of deadlocking
 * with open.  That needs to be reevaluated.
 */
int sbull_revalidate(struct gendisk *gd)
{
	struct sbull_dev *dev = gd->private_data;
	
	if (dev->media_change) {
		dev->media_change = 0;
		memset (dev->data, 0, dev->size);
	}
	return 0;
}

/*
 * The "invalidate" function runs out of the device timer; it sets
 * a flag to simulate the removal of the media.
 */
/*
 *void sbull_invalidate(unsigned long ldev)
 *{
 *        struct sbull_dev *dev = (struct sbull_dev *) ldev;
 *
 *        spin_lock(&dev->lock);
 *        if (dev->users || !dev->data) 
 *                printk (KERN_WARNING "sbull: timer sanity check failed\n");
 *        else
 *                dev->media_change = 1;
 *        spin_unlock(&dev->lock);
 *}
 */

/*
 * The ioctl() implementation
 */

int sbull_ioctl (struct block_device *bdev, fmode_t mode,
                 unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct sbull_dev *dev = bdev->bd_disk->private_data;

	switch(cmd) {
	    case HDIO_GETGEO:
        	/*
		 * Get geometry: since we are a virtual device, we have to make
		 * up something plausible.  So we claim 16 sectors, four heads,
		 * and calculate the corresponding number of cylinders.  We set the
		 * start of data at sector four.
		 */
		size = dev->size*(hardsect_size/KERNEL_SECTOR_SIZE);
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	}

	return -ENOTTY; /* unknown command */
}

/*
 * The device operations structure.
 */
static struct block_device_operations sbull_ops = {
	.owner           = THIS_MODULE,
	.open 	         = sbull_open,
	.release 	 = sbull_release,
	.media_changed   = sbull_media_changed,
	.revalidate_disk = sbull_revalidate,
	.ioctl	         = sbull_ioctl
};

// Set up our internal device. TODO describe better.
// Allocate memory for data pointer of our device.
// Create timer which invalidates device after 30 seconds of nonuse.
// Implement request queue.
// Create gendisk structure.
static void setup_device(struct sbull_dev *dev, int device_index) {
    int retval;

    // Get some memory.
    memset(dev, 0, sizeof (struct sbull_dev));
    dev->size = nsectors*hardsect_size;
    dev->data = vmalloc(dev->size);
    CHECK(dev->data != NULL, error, 0, "vmalloc failure in setup_device.");

    spin_lock_init(&dev->lock);

    // Create the timer which "invalidates" the device.
    /*
     *init_timer(&dev->timer);
     *dev->timer.data = (unsigned long) dev;
     *dev->timer.function = sbull_invalidate;
     */

    // Implement the I/O queue in various ways.
    switch(request_mode) {
        /*
         *case RM_NOQUEUE:
         *    dev->queue = blk_alloc_queue(GFP_KERNEL);
         *    CHECK(dev->queue != NULL, out_vfree, 0, "blk_alloc_queue failed in setup_device.");
         *    blk_queue_make_request(dev->queue, sbull_make_request);
         *    break;
         */

        /*
         * // TODO make a nice comment here.
         *case RM_FULL:
         *    dev->queue = blk_init_queue(sbull_full_request, &dev->lock);
         *    CHECK(dev->queue != NULL, out_vfree, 0, "blk_init_queue failed in setup_device.");
         *    break;
         */

        default:
            printk(KERN_NOTICE "Bad request mode %d, using RM_SIMPLE\n", request_mode);

        case RM_SIMPLE:
            dev->queue = blk_init_queue(sbull_request, &dev->lock);
            CHECK(dev->queue != NULL, out_vfree, 0, "blk_init_queue failed in case RM_SIMPLE in setup_device.");
            break;
    }

    blk_queue_logical_block_size(dev->queue, hardsect_size);
    dev->queue->queuedata = dev;

    // Create the gendisk structure.
    dev->gd = alloc_disk(SBULL_MINORS);
    CHECK(dev->gd, out_vfree, 0, "alloc_disk failed in setup_device.");

    dev->gd->major = sbull_major;
    dev->gd->first_minor = device_index*SBULL_MINORS; // TODO why multiply?
    dev->gd->fops = &sbull_ops;
    dev->gd->queue = dev->queue;
    dev->gd->private_data = dev;
    snprintf(dev->gd->disk_name, 32, "sbull%c", device_index + 'a');
    set_capacity(dev->gd, nsectors*(hardsect_size/KERNEL_SECTOR_SIZE));
    add_disk(dev->gd);
    return;

out_vfree:
    if(dev->data) {
        vfree(dev->data);
    }

error:
    return;
}

static int __init sbull_init(void) {
    int i;
    int retval;

    // Register ourselves in kernel.
    sbull_major = register_blkdev(sbull_major, "robs_sbull");
    CHECK(sbull_major > 0, error, -EBUSY, "sbull: unable to get major number");
    
    // Allocate the device array and initialize each device.
    Devices = kmalloc(ndevices*sizeof(struct sbull_dev), GFP_KERNEL);
    CHECK(Devices != NULL, out_unregister, -ENOMEM, "sbull: kmallof of Devices failed.");

    // Fill in params for each device? TODO what is this actually?
    for(i=0; i<ndevices; i++) {
        setup_device(Devices + i, i);
    }

    return 0;

out_unregister:
    unregister_blkdev(sbull_major, "sbd");
error:
    return retval;
}

static void sbull_exit(void) {
    int i;

    for(i=0; i<ndevices; i++) {
        struct sbull_dev *dev = Devices + i;

        /*del_timer_sync(&dev->timer);*/

        // Is there a gendisk?
        if(dev->gd) {
            // Delete it.
            del_gendisk(dev->gd);
            put_disk(dev->gd);
        }

        // Is there a queue?
        if(dev->queue) {
            // Delete it.
            if(request_mode == RM_NOQUEUE) {
                // TODO what is kobject_put?
                kobject_put(&dev->queue->kobj);
            } else {
                blk_cleanup_queue(dev->queue);
            }
            if(dev->data) {
                vfree(dev->data);
            }
        }
    }
    unregister_blkdev(sbull_major, "sbull");
    kfree(Devices);
}

module_init(sbull_init);
module_exit(sbull_exit);
