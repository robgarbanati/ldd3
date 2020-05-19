#ifndef _ROBS_SCULL_H_
#define _ROBS_SCULL_H_


//====================================================================================================//
//======================================= DEFINES AND TYPEDEFS =======================================//
//====================================================================================================//
#define SCULL_NR_DEVS 1
#define QUANTUM_SIZE 4000
#define QSET_ARRAY_SIZE    1000

#define LOG_ERR(ret, fmt, ...)							\
	(void)printk(KERN_ALERT							\
	  "file: %s, " /*+*/			        			\
	  "function: %s, " /*+*/		        			\
	  "line: %d, " /*+*/			        			\
	  "retval: %d, " /*+*/                          			\
	  "message: " fmt /*+*/                          			\
	  "\n", __FILE__, __func__, __LINE__, ret, ##__VA_ARGS__)	\

#define CHECK(A, ret, fmt, ...)							\
    if (!(A)) {									\
        LOG_ERR(ret, fmt, ##__VA_ARGS__);    				        \
        retval = ret;                                                           \
        goto error;								\
    }

typedef struct _scull_qset_t {
    void ** quantum_array;
    struct _scull_qset_t *next;
} scull_qset_t;

typedef struct _scull_dev_t {
    scull_qset_t *qset_list_head;         // Pointer to first quantum set.
    int quantum_size;                // The current quantum size.
    int qset_array_size;                   // The current array size. TODO this might be misnamed. see scull_trim
    unsigned long tail;         // Amount of data stored here.
    unsigned int access_key;    // Used by sculluid and scullpriv TODO how?
    struct mutex lock;          // mutex, duh
    struct cdev cdev;           // Char device structure. TODO what is that?
} scull_dev_t;



//====================================================================================================//
//====================================== FUNCTION DECLARATIONS =======================================//
//====================================================================================================//


ssize_t scull_read(struct file *filp, char __user *user_ptr, size_t count, loff_t *f_pos);
ssize_t scull_write(struct file *filp, const char __user *user_ptr, size_t count, loff_t *f_pos);
void scull_cleanup_module(void);
int scull_init_module(void);
int scull_open(struct inode *inode, struct file *filp);
int scull_release(struct inode *inode, struct file *filp);

#endif // _ROBS_SCULL_H
