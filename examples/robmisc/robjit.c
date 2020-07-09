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
typedef enum jit_files {
    JIT_BUSY,
    JIT_SCHED,
    JIT_QUEUE,
    JIT_SCHEDTO
} jit_files_t;

// Print 1 line of data after sleeping 1 second.
// Sleep method is determined by data pointer.
int jit_fn_show(struct seq_file *sf, void *v) {
    unsigned long j0, j1; // jiffies
    wait_queue_head_t waitq_head;
    jit_files_t data = (jit_files_t) (sf->private);
    seq_printf(sf, "busy = %d. sched = %d. queue = %d. schedto = %d. data = %d\n", JIT_BUSY, JIT_SCHED, JIT_QUEUE, JIT_SCHEDTO, data);

    init_waitqueue_head(&waitq_head);
    j0 = jiffies;
    j1 = j0 + delay;

    switch(data) {
    case JIT_BUSY: // Do a busy wait with while and cpu_relax()
        seq_printf(sf, "doing a busy wait for delay = %d\n", delay);
        while(time_before(jiffies, j1)) {
            cpu_relax();
        }
        break;
    case JIT_SCHED: // Wait by using schedule, which just gives up the processor to the next task. Prevents idle task from running, and still gets entered a whole lot, so still not so good.
        seq_printf(sf, "doing a schedule() wait for delay = %d\n", delay);
        while(time_before(jiffies, j1)) {
            schedule();
        }
        break;
    case JIT_QUEUE:
        seq_printf(sf, "doing a waitqueue wait for delay = %d\n", delay);
        wait_event_interruptible_timeout(waitq_head, 0, delay);
        break;
    case JIT_SCHEDTO:
        seq_printf(sf, "doing a schedule_timeout wait for delay = %d\n", delay);
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
    .read = seq_read,
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
int tdelay = 10;
module_param(tdelay, int, 0);

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
    unsigned long currenttime = jiffies;
    seq_printf(data->sf, "%9li %3li %i %6i %i %s\n",
            currenttime, currenttime - data->prevjiffies, in_interrupt() ? 1 : 0,
            current->pid, smp_processor_id(), current->comm);

    // Reregister ourself JIT_ASYNC_LOOPS-1 times (so loop JIT_ASYNC_LOOPS times).
    if(--(data->loops)) {
        data->timer.expires += tdelay;
        data->prevjiffies = currenttime;
        add_timer(&(data->timer));
    } else {
        // We have looped enough, so wake up the driver.
        wake_up_interruptible(&(data->waitq_head));
    }
}

// We are using dynamic allocation everywhere, which allows concurrency. TODO why?
int jit_timer_show(struct seq_file *sf, void *v) {
    struct jit_data *data;
    unsigned long currenttime = jiffies;

    data = kmalloc(sizeof(*data), GFP_KERNEL);
    if(!data) {
        return -ENOMEM;
    }

    init_waitqueue_head(&(data->waitq_head));

    // Write the first few lines to the buffer.
    seq_puts(sf, "   time   delta  inirq    pid   cpu command\n");
    seq_printf(sf, "%9li  %3li     %i    %6i   %i   %s\n",
            currenttime, 0L, in_interrupt() ? 1 : 0,
            current->pid, smp_processor_id(), current->comm);

    // Fill the data for our timer function.
    data->prevjiffies = currenttime;
    data->sf = sf;
    data->loops = JIT_ASYNC_LOOPS;

    // Register the timer.
    timer_setup(&(data->timer), jit_timer_fn, 0);
    data->timer.expires = currenttime + tdelay;
    add_timer(&(data->timer));

    // Wait for the loops member to decrement to 0 (done by timer).
    wait_event_interruptible(data->waitq_head, !data->loops);
    if(signal_pending(current)) {
        return -ERESTARTSYS;
    }

    // We have finished waiting, the timer has run *loops* times, clean up and return.
    kfree(data);
    return 0;
}


static int jit_timer_open(struct inode *inode, struct file *file) {
    // implement simplest seq_file interface: only show is needed, not next or stop.
    return single_open(file, jit_timer_show, NULL);
}

static const struct file_operations jit_timer_fops = {
    .open = jit_timer_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

void jit_tasklet_fn(unsigned long arg) {
    struct jit_data *data = (struct jit_data *)arg;
    unsigned long currenttime = jiffies;
    
    seq_printf(data->sf, "%9li  %3li     %i    %6i   %i   %s\n",
            currenttime, currenttime - data->prevjiffies, in_interrupt() ? 1 : 0,
            current->pid, smp_processor_id(), current->comm);

    // Repeat JIT_ASYNC_LOOPS times.
    if (--data->loops) {
        data->prevjiffies = currenttime;
        // schedule again.
        if(data->hi) {
            tasklet_hi_schedule(&(data->tlet));
        } else {
            tasklet_schedule(&(data->tlet));
        }
    } else {
        // Done. Tell driver we're done.
        wake_up_interruptible(&(data->waitq_head));
    }
}

// Once again, allocate everything dynamically for concurrency, TODO why does that allow it?
int jit_tasklet_show(struct seq_file *sf, void *v) {
    struct jit_data *data;
    unsigned long currenttime = jiffies;
    long hi = (long) sf->private; // TODO what is this?
    
    data = kmalloc(sizeof(*data), GFP_KERNEL);
    if(!data) {
        return -ENOMEM;
    }

    init_waitqueue_head(&(data->waitq_head));

    // write the first lines in the buffer, the tasklet will write more later.
    seq_puts(sf, "   time   delta  inirq    pid   cpu command\n");
    seq_printf(sf, "%9li  %3li     %i    %6i   %i   %s\n",
            currenttime, 0L, in_interrupt() ? 1 : 0,
            current->pid, smp_processor_id(), current->comm);

    // Fill the data for our tasklet function.
    data->prevjiffies = currenttime;
    data->sf = sf;
    data->loops = JIT_ASYNC_LOOPS;

    // Register the tasklet.
    tasklet_init(&(data->tlet), jit_tasklet_fn, (unsigned long) data);
    data->hi = hi;
    if (hi) {
        tasklet_hi_schedule(&(data->tlet));
    } else {
        tasklet_schedule(&(data->tlet));
    }

    // Wait for all the loops to complete.
    wait_event_interruptible(data->waitq_head, !data->loops);

    // if wait was ended by a signal, just go back to waiting.
    if(signal_pending(current)) {
        return -ERESTARTSYS;
    }

    // We are done waiting and with our duties. Clean up and return.
    kfree(data);
    return 0;
}

static int jit_tasklet_open(struct inode *inode, struct file *file) {
    return single_open(file, jit_tasklet_show, PDE_DATA(inode));
}

static const struct file_operations jit_tasklet_fops = {
    .open = jit_tasklet_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

#define NUM_DEVICES 1
#define JIT_MINOR 0
dev_t dev = 0;

int __init jit_init(void) {
    int result;
    int jit_major;

    result = alloc_chrdev_region(&dev, JIT_MINOR, NUM_DEVICES, "robjit"); // 
    if(result) {
        printk(KERN_ERR "alloc_chrdev_region failed in jit_init in robjit.c\n");
    }

    jit_major = MAJOR(dev);
    printk(KERN_INFO "major number is %d\n", jit_major);

    proc_create_data("robcurrenttime", 0, NULL, &jit_currentime_fops, NULL);

    proc_create_data("robbusy", 0, NULL, &jit_fn_fops, (void*)JIT_BUSY);
    proc_create_data("robsched", 0, NULL, &jit_fn_fops, (void*)JIT_SCHED);
    proc_create_data("robqueue", 0, NULL, &jit_fn_fops, (void*)JIT_QUEUE);
    proc_create_data("robschedto", 0, NULL, &jit_fn_fops, (void*)JIT_SCHEDTO);

    proc_create_data("robtimer", 0, NULL, &jit_timer_fops, NULL);
    proc_create_data("robtasklet", 0, NULL, &jit_tasklet_fops, NULL);
    proc_create_data("robtasklethi", 0, NULL, &jit_tasklet_fops, (void *)1);
    return 0;
}

void __exit jit_cleanup(void) {
    unregister_chrdev_region(dev, NUM_DEVICES);

    remove_proc_entry("robcurrenttime", NULL);

    remove_proc_entry("robbusy", NULL);
    remove_proc_entry("robsched", NULL);
    remove_proc_entry("robqueue", NULL);
    remove_proc_entry("robschedto", NULL);

    remove_proc_entry("robtimer", NULL);
    remove_proc_entry("robtasklet", NULL);
    remove_proc_entry("robtasklethi", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);
