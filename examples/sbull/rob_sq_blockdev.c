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

/*
 * The internal representation of our device.
 */
typedef struct rob_block_dev {
        int size;                       /* Device size in sectors */
        u8 *data;                       /* The data array */
        short users;                    /* How many users */
        short media_change;             /* Flag a media change? */
        spinlock_t lock;                /* For mutual exclusion */
        struct request_queue *queue;    /* The device request queue */
        struct gendisk *gd;             /* The gendisk structure */
        struct timer_list timer;        /* For simulated media changes */
} rbd_t;

//====================================================================================================//
//========================================== LOCAL VARIABLES =========================================// 
//====================================================================================================//
static int major_number;
static rbd_t our_device;

//====================================================================================================//
//========================================= PRIVATE FUNCTIONS ========================================// 
//====================================================================================================//
int blockdev_open(struct block_device *dev, fmode_t mode)
{
    printk("Device %s opened\n", dev->bd_disk->disk_name);
    return 0;
}

void blockdev_release(struct gendisk *gdisk, fmode_t mode)
{
    printk("Device %s closed\n", gdisk->disk_name);
}

int blockdev_ioctl (struct block_device *dev, fmode_t mode, unsigned cmd, unsigned long arg)
{
    return -ENOTTY; /* ioctl not supported */
}

static struct block_device_operations blockdev_ops = {
    .owner = THIS_MODULE,
    .open = blockdev_open,
    .release = blockdev_release,
    .ioctl = blockdev_ioctl
};

/*
 * The simple form of the request function.
 */
/*
 *static void rob_sq_request(struct request_queue *q)
 *{
 *    struct request *req;
 *
 *    while ((req = blk_fetch_request(q)) != NULL) {
 *        struct sbull_dev *dev = req->rq_disk->private_data;
 *        if (req->cmd_type != REQ_TYPE_FS) {
 *            printk (KERN_NOTICE "Skip non-fs request\n");
 *            __blk_end_request_cur(req, -EIO);
 *            continue;
 *        }
 *        //    	printk (KERN_NOTICE "Req dev %d dir %ld sec %ld, nr %d f %lx\n",
 *        //    			dev - Devices, rq_data_dir(req),
 *        //    			req->sector, req->current_nr_sectors,
 *        //    			req->flags);
 *        sbull_transfer(dev, blk_rq_pos(req), blk_rq_cur_sectors(req),
 *                req->buffer, rq_data_dir(req));
 *        __blk_end_request_cur(req, 0);
 *    }
 *}
 */

static void block_request(struct request_queue *q)
{
    int direction;
    int err = -EIO;
    u8 *data;
    struct request *req = blk_fetch_request(q); /* get one top request from the queue */

    while (req) {
        if (__blk_end_request_cur(req, err)) {  /* check for the end */
            break;
        }

        /* Data processing */

        direction = rq_data_dir(req);

        if (direction == WRITE) {
            printk("Writing data to the block device\n");
        } else {
            printk("Reading data from the block devicen\n");
        }

        data = bio_data(req->bio); /* Data buffer to perform I/O operations */

        /* */

        req = blk_fetch_request(q); /* get next request from the queue */
    }
}

static int __init block_dev_init(void) {
    int retval = 0;
    printk(KERN_INFO "in block_dev_init\n");
    spinlock_t *lk = &our_device.lock;  /* Main lock for the device */
    struct gendisk *gdisk = our_device.gd;
    major_number = register_blkdev(0, "robblockdev");
    CHECK(major_number >0, error, -EBUSY, "robblockdev: unable to get major_number");

    printk(KERN_INFO "Registered robblockdev\n");

    gdisk = alloc_disk(1);
    CHECK(gdisk, error, -ENOMEM, "robblockdev: unable to allocate disk");

    printk(KERN_INFO "Allocated disk");

    spin_lock_init(lk);

    snprintf(gdisk->disk_name, 8, "robblockdev");  /* Block device file name: "/dev/blockdev" */

    gdisk->flags = GENHD_FL_NO_PART_SCAN;  /* Kernel won't scan for partitions on the new disk */
    gdisk->major = major_number;
    gdisk->fops = &blockdev_ops;  /* Block device file operations, see below */
    gdisk->first_minor = 0;

    gdisk->queue = blk_init_queue(block_request, lk);  /* Init I/O queue, see below */

    set_capacity(our_device.gd, 1024 * 512);  /* Set some random capacity, 1024 sectors (with size of 512 bytes) */

    add_disk(gdisk);

error:
    return retval;
}

static void block_dev_exit(void) {
    if (our_device.gd) {
        del_gendisk(our_device.gd);
        put_disk(our_device.gd);
    }

    unregister_blkdev(major_number, "robblockdev");
}
	
module_init(block_dev_init);
module_exit(block_dev_exit);

