#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>

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

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif

// Create an internal representation of our block device.
// Will hold any useful data.
struct block_dev {
    sector_t capacity; // TODO what is a sector_t?
    u8 *data;
    struct blk_mq_tag_set tag_set; // TODO what's this?
    struct request_queue *queue;
    struct gendisk *gdisk;
};


//====================================================================================================//
//========================================== LOCAL VARIABLES =========================================// 
//====================================================================================================//
static int dev_major = 0;
// Our device.
static struct block_dev *block_device = NULL;

//====================================================================================================//
//========================================= PRIVATE FUNCTIONS ========================================// 
//====================================================================================================//
static int blockdev_open(struct block_device *dev, fmode_t mode) {
    printk(">>> blockdev_open\n");

    return 0;
}

static void blockdev_release(struct gendisk *gdisk, fmode_t mode) {
    printk(">>> blockdev_release\n");
}


//====================================================================================================//
//========================================== PUBLIC FUNCTIONS ========================================// 
//====================================================================================================//
// Simply print the command passed in.
int blockdev_ioctl(struct block_device *bdev, fmode_t mode, unsigned cmd, unsigned long arg) {
    printk("ioctl cmd 0x%08x\n", cmd);

    return -ENOTTY;
}

 /*Set up file I/O.*/
static struct block_device_operations blockdev_ops = {
    .owner = THIS_MODULE,
    .open = blockdev_open,
    .release = blockdev_release,
    .ioctl = blockdev_ioctl
};

// Serve requests.
// For each sector in each bio, perform the requested memory transfer with memcpy.
static int do_request(struct request *rq, unsigned int *nr_bytes) {
    int ret = 0;
    /*int retval;*/
    struct bio_vec bvec; // TODO what's this?
    struct req_iterator iter; // TODO what's this?
    struct block_dev *dev = rq->q->queuedata;
    loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT; // I think this converts position in sectors to position in bytes.
    loff_t dev_size = (loff_t)(dev->capacity << SECTOR_SHIFT); // I think this converts size in sectors to size in bytes.

    printk(KERN_WARNING "sblkdev: request start from sector %lld  pos = %lld  dev_size = %lld\n", blk_rq_pos(rq), pos, dev_size);

    // Iterate over all requests segments.
    // bvec and iter start off uninitialized.

    // For each sector in each bio?
    if ((rq->bio)) {
        // Walk across bio list in rq.
        for (iter.bio = (rq)->bio; iter.bio; iter.bio = iter.bio->bi_next) {
            // Walk iter.iter along each sector?
            for (   iter.iter = (iter.bio->bi_iter);
                    iter.iter.bi_size &&
                    ((bvec = bio_iter_iovec((iter.bio), (iter.iter))), 1); // update bvec with page, length, and offset.
                    bio_advance_iter((iter.bio), &(iter.iter), (bvec).bv_len) // iterate through sectors.
                ) {
 /**    rq_for_each_segment(bvec, rq, iter) {*/
                // Do the iterative work?
                unsigned long b_len = bvec.bv_len;

                // Get pointer to the data. 
                void *b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

                // Keep b_len within memory bounds.
                if((pos + b_len) > dev_size) {
                    b_len = (unsigned long) (dev_size - pos);
                }

                // Are we writing to the device?
                if(rq_data_dir(rq) == WRITE) {
                    // Copy data to correct position in the device storage buffer.
                    memcpy(dev->data + pos, b_buf, b_len);
                } else {
                    // Read data from device storage buffer and copy into system memory.
                    memcpy(b_buf, dev->data + pos, b_len);
                }

                // Adjust position in device storage buffer.
                pos += b_len;
                // Update number of bytes so that way the kernel knows how many of them were processed.
                *nr_bytes += b_len;
            }
        }
    }
    return ret;
}


/*
 *    rq_for_each_segment(bvec, rq, iter) {
 *
 *    __rq_for_each_bio(iter.bio, rq)
 *        bio_for_each_segment(bvec, iter.bio, iter.iter)
 *
 *    __rq_for_each_bio(iter.bio, rq)
 *        __bio_for_each_segment(bvec, iter.bio, iter.iter, (iter.bio)->bi_iter)
 *
 *    __rq_for_each_bio(iter.bio, rq)
 *        for (iter.iter = (iter.bio->bi_iter);						\
 *             (iter.iter).bi_size &&						\
 *                ((bvec = bio_iter_iovec((iter.bio), (iter.iter))), 1);		\
 *             bio_advance_iter((iter.bio), &(iter.iter), (bvec).bv_len))
 */

// Queue callback function.
static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd) {
    unsigned int nr_bytes = 0;
    blk_status_t status = BLK_STS_OK;
    struct request *rq = bd->rq;

    // Start request serving procedure.
    // Issue pending block IO request operation to device driver.
    // Called when block operation request rq from queue q is sent to a device driver for processing.
    // Get stats (optional)
    // Do something with dma?
    blk_mq_start_request(rq);

    if(do_request(rq, &nr_bytes) != 0) {
        status = BLK_STS_IOERR;
    }

    // Notify kernel about processed nr_bytes.
    if(blk_update_request(rq, status, nr_bytes)) {
        // Shouldn't fail. TODO Why not?
        BUG();
    }

    // Stop request serving procedure.
    __blk_mq_end_request(rq, status);

    return status;
}

static struct blk_mq_ops mq_ops = {
    .queue_rq = queue_rq,
};

static int __init myblock_driver_init(void) {
    int retval;
    // register.
    dev_major = register_blkdev(dev_major, "robblkdev");

    // initialize device.
    block_device = kmalloc(sizeof(struct block_dev), GFP_KERNEL);
    CHECK(block_device != NULL, error, -ENOMEM, "Failed to allocate block_dev in myblock_driver_init.");

    // Set a random capacity on the device.
    block_device->capacity = (112 * PAGE_SIZE) >> 9; // nsectors * SECTOR_SIZE, expressed as number of sectors... I think? Why PAGE_SIZE?
    // Allocate corresponding data buffer.
    block_device->data = kmalloc(block_device->capacity << 9, GFP_KERNEL);
    CHECK(block_device->data != NULL, error_free_device, -ENOMEM, "Failed to allocate device IO buffer");

    printk("Initializing queue\n");

    // Set up a queue. queue request function is in mq_ops.
    // blk_mq_init_sq_queue will initialize tag_set, with mq_ops, queue_depth (128), flags (SHOULDMERGE), and some other sq specific stuff.
    // It will also pass mq_ops (via tag_set) into block_device->queue.
    // Seems to set up actual requests on block queue(s?) as well.
    block_device->queue = blk_mq_init_sq_queue(&block_device->tag_set, &mq_ops, 128, BLK_MQ_F_SHOULD_MERGE);
    CHECK(block_device->queue != NULL, error_free_queue, -ENOMEM, "Failed to allocate device queue.");

    // Put block device structure into user data of the queue.
    block_device->queue->queuedata = block_device;

    // Allocate new disk.
    block_device->gdisk = alloc_disk(1);

    // Set up flags and data.
    block_device->gdisk->flags = GENHD_FL_NO_PART_SCAN; // TODO what's this?
    block_device->gdisk->major = dev_major;
    block_device->gdisk->first_minor = 0;

    block_device->gdisk->fops = &blockdev_ops;
    block_device->gdisk->queue = block_device->queue;
    block_device->gdisk->private_data = block_device;

    // Set device name as it will be represented in /dev.
    strncpy(block_device->gdisk->disk_name, "robblockdev\0", 9);

    printk("Adding disk %s\n", block_device->gdisk->disk_name);

    // Set device capacity
    set_capacity(block_device->gdisk, block_device->capacity);

    // Notify kernel about new disk device.
    add_disk(block_device->gdisk);

    return 0;

error_free_queue:
    kfree(block_device->data);
error_free_device:
    kfree(block_device);
error:
    unregister_blkdev(dev_major, "robblkdev");
    return retval;
}

static void __exit myblock_driver_exit(void) {
    if(block_device->gdisk) {
        del_gendisk(block_device->gdisk);
        put_disk(block_device->gdisk);
    }

    if(block_device->queue) {
        blk_cleanup_queue(block_device->queue);
    }

    kfree(block_device->data);

    unregister_blkdev(dev_major, "robblkdev");
    kfree(block_device);
}

module_init(myblock_driver_init);
module_exit(myblock_driver_exit);
MODULE_LICENSE("GPL");
