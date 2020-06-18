#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <asm/hardirq.h>

int delay = HZ;

module_param(delay, int, 0);

MODULE_AUTHOR("Rob Garbanati");
MODULE_LICENSE("Dual BSD/GPL");

// Implement 4 files
enum jit_files {
    JIT_BUSY,
    JIT_SCHED,
    JIT_QUEUE,
    JIT_SCHEDTO
};

// Print 1 line of data after sleeping 1 second.
// Sleep method is determined by data pointer.
int jit_fn_show(struct seq_file *sf, void *v) {
    unsigned long j0, j1; // jiffies
    wait_queue_head_t waitq_head;
    long data = (long)sf->private;

    init_waitqueue_head(&waitq_head);
    j0 = jiffies;
    j1 = j0 + delay;

    switch(data) {
    case JIT_BUSY: // Do a busy wait with while and cpu_relax()
        while(time_before(jiffies, j1)) {
            cpu_relax();
        }
        break;
    case JIT_SCHED: // Wait by using schedule, which just gives up the processor to the next task. Prevents idle task from running, and still gets entered a whole lot, so still not so good.
        while(time_before(jiffies, j1)) {
            schedule();
        }
        break;
    case JIT_QUEUE:
        seq_printf(sf, "delay = %d\n", delay);
        wait_event_interruptible_timeout(waitq_head, 0, delay);
        break;
    case JIT_SCHEDTO:
        seq_printf(sf, "delay = %d\n", delay);
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(delay);
        break;
    }
    j1 = jiffies; // record actual value after we delayed

    seq_printf(sf, "%9li %9li\n", j0, j1);
    return 0;
}

// all we need is the show function.
static int jit_fn_open(struct inode *inode, struct file *file) {
    return single_open(file, jit_fn_show, PDE_DATA(inode));
}

static const struct file_operations jit_fn_fops = {
    .open = jit_fn_open,
    .read = seq_read, // library function for reading sequential files. TODO, what makes this a sequential file?
    .llseek = seq_lseek,
    .release = single_release,
};

int jit_currentime_show(struct seq_file *sf, void *v) {
    unsigned long j1; // jiffy
    u64 j2;
    struct timespec64 ts1, ts2;

    // Get usec and nsec representations of system time.
    j1 = jiffies;
    j2 = get_jiffies_64();
    ktime_get_real_ts64(&ts1);
    ktime_get_coarse_real_ts64(&ts2);
    seq_printf(sf, "0x%08lx 0x%016Lx %10i.%06i\n"
                    "%40i.%09i\n",
                    j1, j2,
                    (int) ts1.tv_sec, (int) ts1.tv_nsec,
                    (int) ts2.tv_sec, (int) ts2.tv_nsec);
    return 0;
}

static int jit_currentime_open(struct inode *inode, struct file *file) {
    return single_open(file, jit_currentime_show, NULL);
}

static const struct file_operations jit_currentime_fops = {
    .open = jit_currentime_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

// timer example
/*
int tdelay = 10;
moduleparam(tdelay, int, 0);

// this holds the data for the timer and tasklet functions.
struct jit_data {
    struct timer_list timer;
    struct tasklet_struct tlet;
    struct seq_file *sf;
    int hi; // tasklet or tasklet_hi TODO what does that mean?
    wait_queue_head_t waitq_head;
    unsigned long prevjiffies;
    int loops;
};
#define JIT_ASYNC_LOOPS 5

void jit_timer_fn(struct timer_list *t) {
    // get jit_data struct from passed in timer_list ptr.
    struct jit_data *data = from_timer(data, t, timer);
    unsigned long j = jiffies;
    seq_printf(data->sf, "%9li %3li %i %6i %i %s\n",
            j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
            current->pid, smp_processor_id(), current->comm);
    if(--data->loops) { // TODO add parens to make ordering explicit with the decrement)
        data->timer.expires += tdelay
*/

#define NUM_DEVICES 1
#define JIT_MINOR 0
dev_t dev = 0;

int __init jit_init(void) {
    int result;
    int jit_major;

    result = alloc_chrdev_region(&dev, JIT_MINOR, NUM_DEVICES, "robjit");
    jit_major = MAJOR(dev);
    printk(KERN_INFO "major number is %d\n", jit_major);

    proc_create_data("robcurrenttime", 0, NULL, &jit_currentime_fops, NULL);
    return 0;
}

void __exit jit_cleanup(void) {
    unregister_chrdev_region(dev, NUM_DEVICES);

    remove_proc_entry("robcurrenttime", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);
