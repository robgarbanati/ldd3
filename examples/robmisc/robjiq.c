#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>     /* everything... */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/errno.h>  /* error codes */
#include <linux/workqueue.h>
#include <linux/preempt.h>
#include <linux/interrupt.h> /* tasklets */

MODULE_AUTHOR("Rob Garbanati");
MODULE_LICENSE("Dual BSD/GPL");

static long delay = 1;
module_param(delay, long, 0);

#define LIMIT (PAGE_SIZE-128) // Don't print any more after this size.

// Print information about the current environment. This is called from withing the task
// queues. If the limit is reached, awake the reading process.
static DECLARE_WAIT_QUEUE_HEAD(jiq_waitq_head); // TODO what is this?

static struct clientdata {
    struct work_struct jiq_work;
    struct delayed_work jiq_delayed_work;
    struct timer_list jiq_timer;
    struct seq_file *sf;
    int len;
    unsigned long jiffies;
    long delay;
} jiq_data;

// Do the printing. Return non-zero if task should be rescheduled.
static int jiq_print(void *ptr) {
    struct clientdata *data = ptr;
    int len = data->len;
    struct seq_file *sf = data->sf;
    unsigned long currenttime = jiffies;

    // Print until we reach LIMIT, then stop scheduling workqueues to do work and wake up device.
    if(len > LIMIT) {
        wake_up_interruptible(&jiq_waitq_head);
        return 0;
    }

    // len == 0 signifies the first one, which should print header?
    if(len == 0) {
        seq_puts(sf, "    time  delta preempt   pid cpu command\n");
        // read count from seq_file struct, which gets updated with seq_puts.
        len = sf->count;
    } else {
        len = 0;
    }

    // TODO what is preempt_count()?
    // seq_puts and seq_printf update sf->count.
    // then len is set to sf->count (or it double-counts the header).
    // then data->len is INCREMENTED by len.
    seq_printf(sf, "%9li  %4li     %3i %5i %3i %s. len = %d, sf->count = %ld. data->len = %d\n",
            currenttime, currenttime - data->jiffies,
            preempt_count(), current->pid, smp_processor_id(),
            current->comm, len, sf->count, data->len);
    len += sf->count; // TODO, does this double count??

    data->len += len;
    data->jiffies = currenttime;

    return 1;
}

// Function to be called from the workqueue.
static void jiq_print_wq(struct work_struct *work) {
    struct clientdata *data = container_of(work, struct clientdata, jiq_work);

    // if jiq_print failed or says to stop, then stop.
    if(!jiq_print(data)) {
        return;
    } else {
        // otherwise reschedule on system workqueue.
        schedule_work(&jiq_data.jiq_work);
    }
}

static void jiq_print_wq_delayed(struct work_struct *work) {
    struct clientdata *data = container_of(work, struct clientdata, jiq_delayed_work.work);
    /*jiq_print(data);*/
    /*seq_puts(data->sf, "printing from wq delayed\n");*/
    /*wake_up_interruptible(&jiq_waitq_head);*/

    // jiq_print will return 0 when no more printing should be done. It will also call wake_up_interruptible.
    if(!jiq_print(data)) {
        return;
    }

    schedule_delayed_work(&jiq_data.jiq_delayed_work, data->delay);
}

static int jiq_read_wq_show(struct seq_file *sf, void *v) {
    DEFINE_WAIT(wait_entry);

    jiq_data.len = 0;
    jiq_data.sf = sf;
    jiq_data.jiffies = jiffies;
    jiq_data.delay = 0;

    // Add our wait_entry to jiq_waitq_head.
    prepare_to_wait(&jiq_waitq_head, &wait_entry, TASK_INTERRUPTIBLE);

    // submit our work struct to the system work queue.
    schedule_work(&jiq_data.jiq_work);

    // Yield the processor.
    schedule();

    // Done sleeping. Cleanup.
    // Set current thread back to running state. 
    // Remove the wait descriptor from the given waitqueue if still queued.
    finish_wait(&jiq_waitq_head, &wait_entry);

    return 0;
}

static int jiq_read_wq_open(struct inode *inode, struct file *file) {
    return single_open(file, jiq_read_wq_show, NULL);
}

static const struct file_operations jiq_read_wq_fops = {
    .open = jiq_read_wq_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int jiq_read_wq_delayed_show(struct seq_file *sf, void *v) {
    DEFINE_WAIT(wait_entry);

    jiq_data.len = 0;
    jiq_data.sf = sf;
    jiq_data.jiffies = jiffies;
    jiq_data.delay = delay;

    // Change state to sleeping. Add wait_entry to wait queue.
    prepare_to_wait(&jiq_waitq_head, &wait_entry, TASK_INTERRUPTIBLE);
    // add work (runs jiq_print_wq_delayed).
    schedule_delayed_work(&jiq_data.jiq_delayed_work, delay);
    // Yield processor.
    schedule();
    // Change state to running. remove from wait queue.
    finish_wait(&jiq_waitq_head, &wait_entry);

    return 0;
}

static int jiq_read_wq_delayed_open(struct inode *inode, struct file *file) {
    int result;
    result = single_open(file, jiq_read_wq_delayed_show, NULL);

    return result;
}

static const struct file_operations jiq_read_wq_delayed_fops = {
    .open = jiq_read_wq_delayed_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static void jiq_timedout(struct timer_list *t) {
    struct clientdata *data = from_timer(data, t, jiq_timer);
    jiq_print((void *)data); // print a line
    wake_up_interruptible(&jiq_waitq_head); // awaken the process.
}

static int jiq_read_run_timer_show(struct seq_file *sf, void *v) {
    DEFINE_WAIT(wait_entry);

    jiq_data.len = 0;
    jiq_data.sf = sf;
    jiq_data.jiffies = jiffies;

    // Set up a timer to expire in 1 second.
    timer_setup(&jiq_data.jiq_timer, jiq_timedout, 0);
    jiq_data.jiq_timer.expires = jiffies + HZ;

    jiq_print(&jiq_data); // print and go to sleep.

    // Change state to sleeping. Add wait_entry to wait queue.
    prepare_to_wait(&jiq_waitq_head, &wait_entry, TASK_INTERRUPTIBLE);
    // add timer (runs jiq_timedout).
    add_timer(&jiq_data.jiq_timer);
    // Yield processor.
    schedule();
    // Change state to running. remove from wait queue.
    finish_wait(&jiq_waitq_head, &wait_entry);

    del_timer_sync(&jiq_data.jiq_timer); // In case a signal woke us up TODO why?

    return 0;
}

static int jiq_read_run_timer_open(struct inode *inode, struct file *file) {
    return single_open(file, jiq_read_run_timer_show, NULL);
}

static const struct file_operations jiq_read_run_timer_fops = {
    .open = jiq_read_run_timer_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static void jiq_print_tasklet(unsigned long ptr);
static DECLARE_TASKLET(jiq_tasklet, jiq_print_tasklet, (unsigned long) &jiq_data);

static void jiq_print_tasklet(unsigned long ptr) {
    if (jiq_print((void *)ptr) ) {
        tasklet_schedule(&jiq_tasklet);
    }
}

static int jiq_read_tasklet_show(struct seq_file *sf, void *v) {
    DEFINE_WAIT(wait_entry);
    jiq_data.len = 0;
    jiq_data.sf = sf;
    jiq_data.jiffies = jiffies;

    seq_puts(sf, "printing directly from show\n");

    prepare_to_wait(&jiq_waitq_head, &wait_entry, TASK_INTERRUPTIBLE);
    tasklet_schedule(&jiq_tasklet);
    schedule();
    finish_wait(&jiq_waitq_head, &wait_entry);
    /*wait_event_interruptible(jiq_waitq_head, 0);*/

    return 0;
}


static int jiq_read_tasklet_open(struct inode *inode, struct file *file)
{
	return single_open(file, jiq_read_tasklet_show, NULL);
}

static const struct file_operations jiq_read_tasklet_fops = {
	.open		= jiq_read_tasklet_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#define NUM_DEVICES 1
#define JIT_MINOR 0
dev_t dev = 0;

static int jiq_init(void) {
    int result;
    int jit_major;

    result = alloc_chrdev_region(&dev, JIT_MINOR, NUM_DEVICES, "robjiq"); // 
    if(result) {
        printk(KERN_ERR "alloc_chrdev_region failed in jiq_init in robjiq.c\n");
    }

    jit_major = MAJOR(dev);
    printk(KERN_INFO "major number is %d\n", jit_major);
    INIT_WORK(&jiq_data.jiq_work, jiq_print_wq);
    INIT_DELAYED_WORK(&jiq_data.jiq_delayed_work, jiq_print_wq_delayed);
    
    proc_create("robqwq", 0, NULL, &jiq_read_wq_fops);
    proc_create("robqwqdelay", 0, NULL, &jiq_read_wq_delayed_fops);
    proc_create("robqtimer", 0, NULL, &jiq_read_run_timer_fops);
    proc_create("robqtasklet", 0, NULL, &jiq_read_tasklet_fops);

    return 0;
}

static void jiq_exit(void) {
    unregister_chrdev_region(dev, NUM_DEVICES);

    remove_proc_entry("robqwq", NULL);
    remove_proc_entry("robqwqdelay", NULL);
    remove_proc_entry("robqtimer", NULL);
    remove_proc_entry("robqtasklet", NULL);
}


module_init(jiq_init);
module_exit(jiq_exit);
