#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/security.h>
#include <linux/kthread.h>

#include "vm.h"
#include "request.h"
#include "coroutine.h"

#define WASM_LOAD_CODE 0x1001
#define WASM_RUN_CODE 0x1002

const char *CLASS_NAME = "wasm";
const char *DEVICE_NAME = "wasmctl";

static int major_number;
static struct class *dev_class = NULL;
static struct device *dev_handle = NULL;
static int uapi_initialized = 0;

int uapi_init(void);
void uapi_cleanup(void);
static int wd_open(struct inode *, struct file *);
static int wd_release(struct inode *, struct file *);
static ssize_t wd_read(struct file *, char *, size_t, loff_t *);
static ssize_t wd_write(struct file *, const char *, size_t, loff_t *);
static ssize_t wd_ioctl(struct file *, unsigned int cmd, unsigned long arg);

static struct file_operations wasm_ops = {
    .open = wd_open,
    .read = wd_read,
    .write = wd_write,
    .release = wd_release,
    .unlocked_ioctl = wd_ioctl
};

int uapi_init(void) {
    major_number = register_chrdev(0, DEVICE_NAME, &wasm_ops);
    if(major_number < 0) {
        printk(KERN_ALERT "linux-ext-wasm: Device registration failed\n");
        return major_number;
    }

    dev_class = class_create(THIS_MODULE, CLASS_NAME);
    if(IS_ERR(dev_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "linux-ext-wasm: Device class creation failed\n");
        return PTR_ERR(dev_class);
    }

    dev_handle = device_create(
        dev_class,
        NULL,
        MKDEV(major_number, 0),
        NULL,
        DEVICE_NAME
    );
    if(IS_ERR(dev_handle)) {
        class_destroy(dev_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "linux-ext-wasm: Device creation failed\n");
        return PTR_ERR(dev_handle);
    }

    printk(KERN_INFO "linux-ext-wasm: uapi initialized\n");
    uapi_initialized = 1;

    return 0;
}

void uapi_cleanup(void) {
    if(!uapi_initialized) return;

    // TODO: Is it possible that we still have open handles
    // to the UAPI device at this point?
    device_destroy(dev_class, MKDEV(major_number, 0));
    class_unregister(dev_class);
    class_destroy(dev_class);
    unregister_chrdev(major_number, DEVICE_NAME);
}

static int wd_open(struct inode *_inode, struct file *f) {
    struct privileged_session *sess;

    sess = kmalloc(sizeof(struct privileged_session), GFP_KERNEL);
    if(!sess) return -ENOMEM;

    init_privileged_session(sess);
    f->private_data = sess;
    return 0;
}

static int wd_release(struct inode *_inode, struct file *f) {
    struct privileged_session *sess = f->private_data;

    if(sess->ready) {
        printk(KERN_INFO "Released execution engine %px\n", &sess->ee);
        destroy_execution_engine(&sess->ee);
    }

    kfree(sess);

    return 0;
}

static ssize_t wd_read(struct file *_file, char *_data, size_t _len, loff_t *_offset) {
    return 0;
}

static ssize_t wd_write(struct file *_file, const char *data, size_t len, loff_t *offset) {
    return -EINVAL;
}

static ssize_t handle_wasm_load_code(struct file *f, void *arg) {
    int err;
    struct load_code_request req;
    struct privileged_session *sess = f->private_data;

    if(sess->ready) {
        err = -EINVAL;
        goto fail;
    }

    if(copy_from_user(&req, arg, sizeof(struct load_code_request))) {
        err = -EFAULT;
        goto fail;
    }
    if((err = init_execution_engine(&req, &sess->ee)) < 0) {
        goto fail;
    }
    printk(KERN_INFO
        "Initialized execution engine %px, code = %px, global_backing = %px, global_ptr_backing = %px, code_size = %u, memory_size = %lu\n",
        &sess->ee,
        sess->ee.code,
        sess->ee.local_global_backing,
        sess->ee.local_global_ptr_backing,
        sess->ee.code_len,
        sess->ee.local_memory_backing.bound
    );

    sess->ready = 1;
    return 0;

    fail:
    return err;
}

struct code_runner_task {
    struct semaphore sem;
    struct execution_engine *ee;
    struct run_code_request *req;
    uint64_t ret;
};

void code_runner_inner(struct Coroutine *co) {
    struct code_runner_task *task = co->private_data;
    up(&task->sem);
    if(task->req->param_count != 0) {
        printk(KERN_INFO "invalid param count\n");
    } else {
        task->ret = ee_call0(task->ee, task->req->entry_offset);
        up(&task->sem);
    }
}

static int code_runner(void *data) {
    struct code_runner_task *task = data;
    struct Coroutine co = {
        .stack = task->ee->stack_end,
        .entry = code_runner_inner,
        .terminated = 0,
        .private_data = task,
    };
    printk(KERN_INFO "stack: %px-%px\n", task->ee->stack_begin, task->ee->stack_end);
    start_coroutine(&co);
    while(!co.terminated) {
        co_switch(&co.stack);
    }
    return 0;
}

static ssize_t handle_wasm_run_code(struct file *f, void *arg) {
    int ret;
    int made_nx = 0;
    struct run_code_request req;
    struct code_runner_task task;
    struct privileged_session *sess = f->private_data;
    struct task_struct *runner_ts;

    if(copy_from_user(&req, arg, sizeof(struct run_code_request))) {
        return -EFAULT;
    }

    if(!sess->ready) {
        return -EINVAL;
    }

    task.ee = &sess->ee;
    task.req = &req;
    sema_init(&task.sem, 0);

    runner_ts = kthread_create(code_runner, &task, "code_runner");
    if(!runner_ts || IS_ERR(runner_ts)) {
        printk(KERN_INFO "Unable to start code runner\n");
        return -EINVAL;
    }
    get_task_struct(runner_ts);
    wake_up_process(runner_ts);

    down(&task.sem); // wait for execution start

    if(down_interruptible(&task.sem) < 0) { // wait for execution end
        // interrupted by signal
        ee_make_code_nx(&sess->ee); // trigger a page fault
        made_nx = 1;
    }

    ret = kthread_stop(runner_ts); // FIXME: Is it correct to use kthread_stop in this way?
    if(ret != 0) {
        printk(KERN_INFO "bad result from runner thread: %d\n", ret);
    } else {
        printk(KERN_INFO "result = %llu\n", task.ret);
    }

    put_task_struct(runner_ts);
    while(down_trylock(&task.sem) == 0); // address the race condition in forceful termination
    if(made_nx) {
        ee_make_code_x(&sess->ee);
    }
    return 0;
}

#define DISPATCH_CMD(cmd, f) case cmd: return (f)(file, (void *) arg);

static ssize_t wd_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    switch(cmd) {
        DISPATCH_CMD(WASM_LOAD_CODE, handle_wasm_load_code)
        DISPATCH_CMD(WASM_RUN_CODE, handle_wasm_run_code)
        default:
            return -EINVAL;
    }

    return -EINVAL;
}