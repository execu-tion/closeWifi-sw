// dma_pl_minimal.c
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/signal.h>
#include "acquisition_ioctl.h"

#define DEVICE_NAME "data_acquisition"
#define CLASS_NAME  "data_acquisition"

// class data_acq
struct  data_acq{
    struct platform_device* pdev;
    struct device* dev;

    struct cdev cdev;
    dev_t devno;
    struct class *class;
    
    // regmap
    void __iomem *reg_base;
    
    // DMA engine
    struct dma_chan *dma_chan;
    const char* dma_name;
    
    // DMA buffer
    char*        dma_buffer;
    size_t       dma_buffer_size;
    dma_addr_t   dma_buffer_addr;
    dma_cookie_t cookie;
    bool         dma_complete;
    
    // Wait on dma transition
    struct wait_queue_head waitq;

    // user notify
    struct task_struct * user_tsk;
    int         user_signo  ;
    
    // transfer length
    size_t available_length;
    size_t transfer_length;

    // data type
    int data_type;
    
    // sysfs
    struct kobject kobj;
};



static int dma_open(struct data_acq* data_acq){
    if( !data_acq ){
        printk("obtain priv data from inode error!\n");
        return -1;
    };

    // dma chennel is obtained!
    if( data_acq->dma_chan ){
        return 0;
    };

    // dma request channel
    data_acq->dma_chan = dma_request_chan(&data_acq->pdev->dev,data_acq->dma_name);
    if( IS_ERR(data_acq->dma_chan) ){
        data_acq->dma_chan = NULL;
        printk("Error opening the %s channel\n",data_acq->dma_name);
        return -EINVAL;
    };
    return 0;
}

static int dma_close(struct data_acq* data_acq){
    if( !data_acq ){
        printk("obtain priv data from inode error!\n");
        return -1;
    };
    // dma chennel is closed!
    if( !data_acq->dma_chan ){
        return 0;
    };
    dmaengine_terminate_sync(data_acq->dma_chan);
    dma_release_channel(data_acq->dma_chan);
    data_acq->dma_chan = NULL;

    return 0;
}

// file operations
static int device_open(struct inode *inode, struct file *file)
{
    file->private_data = container_of( inode->i_cdev, struct data_acq, cdev);
    return dma_open(container_of( inode->i_cdev, struct data_acq, cdev));
}

static int device_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
    // return dma_close(container_of( inode->i_cdev, struct data_acq, cdev));
}

static ssize_t device_read(struct file *filp, char __user *buf, 
                          size_t count, loff_t *f_pos)
{
    struct data_acq* data_acq = filp->private_data;
    if( !data_acq )
        return 0;
    
    if( data_acq->dma_buffer == NULL || data_acq->available_length <= 0 )
        return 0;
    
    int num = data_acq->available_length - 
        copy_to_user(buf,data_acq->dma_buffer,data_acq->available_length);

    data_acq->available_length -= num;
    return num;
}

// set or get data type
static long ioctl_data_type(struct data_acq* data_acq, int data_type){
    if( data_type >= TYPE_DATA && data_type < TYPE_MAX ){
        data_acq->data_type = data_type;
        writel(data_acq->data_type,data_acq->reg_base);
    }else{
        int val = readl(data_acq->reg_base);
        if( (unsigned)val >= TYPE_MAX ){
            printk("Reg status invalid, written to 0\n");
            data_acq->data_type = TYPE_DATA;
        }else{
            data_acq->data_type = val;
        }
    };
    return data_acq->data_type;
}

// attach buffer to driver
static long ioctl_set_buffer(struct data_acq* data_acq, int buffer_length){
    if( data_acq->dma_buffer && buffer_length <= data_acq->dma_buffer_size ){
        return data_acq->dma_buffer_size;
    };

    if( buffer_length%8 != 0 ){
        printk("Buffer must aligned with 8 Bytes!\n");
        return data_acq->dma_buffer_size;
    };

    // if dma mapped
    if( data_acq->dma_buffer_addr && data_acq->dma_buffer )
        dma_free_coherent(&data_acq->pdev->dev, data_acq->dma_buffer_size,
                    data_acq->dma_buffer, data_acq->dma_buffer_addr);
    
    // create new buffer
    data_acq->dma_buffer_size = buffer_length;
    data_acq->dma_buffer = dma_alloc_coherent(&data_acq->pdev->dev, buffer_length, &data_acq->dma_buffer_addr, GFP_KERNEL);

    if( !data_acq->dma_buffer ){
        data_acq->dma_buffer = NULL;
        data_acq->dma_buffer_addr = 0;
        data_acq->dma_buffer_size = 0;
        return -1;
    }   

    return data_acq->dma_buffer_size;
};

static long ioctl_set_length(struct data_acq* data_acq, int length){
    if( length < 0 )
        return data_acq->transfer_length;
    
    if( length%8 != 0 ){
        printk("Transfer length must aligned with 8 Bytes!\n");
        return data_acq->transfer_length;
    };

    if( length > data_acq->dma_buffer_size ){
        long new_buffer = ioctl_set_buffer(data_acq, length);
        data_acq->transfer_length = new_buffer;
    }else{
        data_acq->transfer_length = length;
    };

    return data_acq->transfer_length;
};

static long ioctl_set_signo(struct data_acq* data_acq, int signo){
    data_acq->user_signo = signo;
    data_acq->user_tsk = current;
    return data_acq->user_signo;
};

static void dma_async_cb(void* cb_data){
    enum dma_status status;
    struct data_acq* data_acq = cb_data;
    // check dma status
    status = dma_async_is_tx_complete(data_acq->dma_chan, data_acq->cookie, NULL, NULL);
    if( status != DMA_COMPLETE ){
        data_acq->available_length = 0;
        printk("dma transfer status is [%s], cookie [%d] \n",
            (char*([])){"DMA_COMPLETE","DMA_IN_PROGRESS","DMA_PAUSED",
            "DMA_ERROR","DMA_OUT_OF_ORDER"}[status],data_acq->cookie);
        return;
    };
    // mark dma complete
    data_acq->dma_complete = true;

    /* update available length and wake any waiters */
    data_acq->available_length = data_acq->transfer_length;
    wake_up_interruptible(&data_acq->waitq);

    /* optional user signal: only send if user explicitly set a non-zero signo */
    if (data_acq->user_tsk && data_acq->user_signo)
        send_sig(data_acq->user_signo, data_acq->user_tsk, 0);

};

static int dma_prep_single(struct data_acq* data_acq){
    struct dma_chan *chan;
    dma_cookie_t dma_cookie;
    enum dma_ctrl_flags dma_flags;

    // got chan
    chan = data_acq->dma_chan;
    if (!chan) {
        pr_err("dma_prep_single: no dma channel\n");
        return -ENODEV;
    }
    
    // we prepare a slave scatter-gather transfer.
    struct dma_async_tx_descriptor *dma_txnd;
    dma_flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    dma_txnd = dmaengine_prep_slave_single(chan, data_acq->dma_buffer_addr, 
                data_acq->transfer_length, DMA_DEV_TO_MEM, dma_flags);

    if ( dma_txnd == NULL ) {
        printk("Unable to prepare the dma engine for the tx buffer.\n");
        return -EBUSY;
    }

    dma_txnd->callback_param = data_acq;
    dma_txnd->callback = dma_async_cb;
    dma_cookie = dmaengine_submit(dma_txnd);
    if (dma_submit_error(dma_cookie)) {
        pr_err("Unable to submit the tx request to the engine.\n");
        return -EBUSY;
    }

    // Return the DMA cookie for the transaction
    data_acq->cookie = dma_cookie;
    data_acq->dma_complete = false;
    dma_async_issue_pending(chan);
    return 0;
}

static int ioctl_start_transfer(struct data_acq* data_acq){
    if( !data_acq || !data_acq->dma_buffer ){
        printk("No priv or buffer found!\n");
        return -EFAULT;
    };
    if( data_acq->transfer_length > data_acq->dma_buffer_size ){
        printk("Buffer size is not enough!\n");
        return -EFAULT;
    }
    return dma_prep_single(data_acq);
};

static int ioctl_waiton_transfer(struct data_acq* data_acq){
    if( !data_acq || !data_acq->dma_buffer ){
        printk("No priv or buffer found!\n");
        return -EFAULT;
    };
    if( data_acq->transfer_length > data_acq->dma_buffer_size ){
        printk("Buffer size is not enough!\n");
        return -EFAULT;
    };

    int ret = dma_prep_single(data_acq);
    if( ret != 0 ) // submit dma transmission error
        return ret;

    // wait on completion
    ret = wait_event_interruptible_timeout(data_acq->waitq, 
        data_acq->dma_complete, msecs_to_jiffies(5000));

    if( ret > 0 ){ // wait on success
        data_acq->dma_complete = false;
        return 0;
    };

    if( ret == 0 ){ // wait on timedout
        // wait timed out
        dmaengine_terminate_sync(data_acq->dma_chan);
        return -ETIMEDOUT;
    };

    if ( ret == -ERESTARTSYS ) { // wait on signal interrupted
        // interrupted
        if( signal_pending(current) ){
            sigset_t pending;
            
            spin_lock_irq(&current->sighand->siglock);
            pending = current->pending.signal;
            spin_unlock_irq(&current->sighand->siglock);

            // check for user signal awake
            if( data_acq->user_tsk && 
                sigismember(&pending, data_acq->user_signo) ){ // has valid signal
                return 0;
            };
        };
        return -ERESTARTSYS;
    };
    // unknow error encourted
    return -EFAULT;
};

static int ioctl_get_avaliable(struct data_acq* data_acq, int* len){
    if( !data_acq || !data_acq->dma_buffer ){
        printk("No priv or buffer found!\n");
        return -EFAULT;
    };
    *len = data_acq->available_length;
    return data_acq->available_length;
}

static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long flags){
    struct data_acq* data_acq = filp->private_data;
    if( !data_acq )
        return -EFAULT;

    switch(cmd){
        case IOCTL_DATA_TYPE:{
            return ioctl_data_type(data_acq,(int)flags);
        };
        case IOCTL_SET_BUFFER:{
            return ioctl_set_buffer(data_acq,(int)flags);
        };
        case IOCTL_SET_LENGTH:{
            return ioctl_set_length(data_acq,(int)flags);
        };
        case IOCTL_SET_SIGNO:{
            return ioctl_set_signo(data_acq,(int)flags);
        };
        case IOCTL_START_TRANSFER:{
            return ioctl_start_transfer(data_acq);
        };
        case IOCTL_WAITON_TRANSFER:{
            return ioctl_waiton_transfer(data_acq);
        };
        case IOCTL_GET_AVALIABLE:{
            int avil;
            int ret = ioctl_get_avaliable(data_acq,&avil);
            if (copy_to_user((int*)flags, &avil, sizeof(int))) {
                return -EFAULT;
            }else{
                return ret;
            };

        };
        default:
            return 0;
    }

    return 0;
};

static int device_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct data_acq* data_acq = filp->private_data;
    if( !data_acq || !data_acq->dma_buffer )
        return -EFAULT;

    unsigned long size = vma->vm_end - vma->vm_start;
    if( size > data_acq->dma_buffer_size ){
        return -EINVAL;
    };

    unsigned long phy = virt_to_phys(data_acq->dma_buffer);
    
    // vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    
    /* map */
    if( remap_pfn_range(vma, vma->vm_start, phy>>PAGE_SHIFT,
                    size, vma->vm_page_prot) < 0 ){
        printk("mmap remap_pfn_range failed\n");
        return -ENOBUFS;
    };
    return 0;
}


static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .unlocked_ioctl = device_ioctl,
    .compat_ioctl = device_ioctl,
    .mmap = device_mmap,
};

// sys function definations
static ssize_t transfer_data_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf){
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        return sprintf(buf, "%s\n","obtain priv data from kobj error!");
    };

    ioctl_data_type(data_acq,-1);
    return sprintf(buf,"%s\n",(char*([])){"TYPE_DATA","TYPE_CONST",
            "TYPE_FREQ","TYPE_CHAN"}[data_acq->data_type]);
};

static ssize_t transfer_data_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count){
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        printk("obtain priv data from kobj error!");
        return -EFAULT;
    };

    char* TYPES[] = {"TYPE_DATA","TYPE_CONST","TYPE_FREQ","TYPE_CHAN"};

    for( int i = 0; i < TYPE_MAX; i++){
        if( strncasecmp(buf,TYPES[i],strlen(TYPES[i])) == 0){
            data_acq->data_type = i;
            writel(data_acq->data_type,data_acq->reg_base);
            return count;
        };
    };

    printk("VALID TYPES: TYPE_DATA:0, TYPE_CONST:1, TYPE_FREQ:2, TYPE_CHAN:3\n");
    return count;
};


static ssize_t transfer_dma_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf){
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        return sprintf(buf, "%s\n","obtain priv data from kobj error!");
    };

    if( !data_acq->dma_chan ){
        return sprintf(buf, "%s\n","No dma channel attached!");
    }else{
        return sprintf(buf, "DMA CHANNEL %s\n",data_acq->dma_name);
    };

};

static ssize_t transfer_dma_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count){
    int ret = 0;
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        printk("obtain priv data from kobj error!");
        return -EFAULT;
    };
    if( *buf == '1' ){
        // force probe dma
        int was_attached = !!data_acq->dma_chan;
        ret = dma_open(data_acq);
        if( ret == 0 ){
            if (!was_attached)
                printk("DMA Channel %s Attached!\n", data_acq->dma_name);
            else
                printk("DMA Channel %s already attached\n", data_acq->dma_name);
        }else{
            printk("DMA Channel %s Attach Failed!\n", data_acq->dma_name);
        };

    }else if( *buf == '0' ){
        // force remove dma
        if( !data_acq->dma_chan ){
            printk("DMA Channel %s already detached!\n", data_acq->dma_name);
        } else {
            dma_close(data_acq);
            data_acq->dma_chan = NULL;
            printk("DMA Channel %s detached!\n", data_acq->dma_name);
        };
    };

    /* sysfs store functions should return count (number of bytes consumed) */
    return count;
};

static ssize_t transfer_length_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf)
{
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        return sprintf(buf,"%s\n","obtain priv data from kobj error!");
    };

    return sprintf(buf, "DMA transfer length %d\n",data_acq->transfer_length);
}

static ssize_t transfer_length_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count)
{
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        printk("obtain priv data from kobj error!\n");
        return -EFAULT;
    }

    // cause dma buffer length change
    int new_length = 0;
    int ret = kstrtouint(buf,10,&new_length);
    if( ret )
        return ret;

    if( new_length%8 != 0 ){
        printk("Transfer length should aligned with 8 bytes\n");
        return count;
    }
    
    if( new_length > data_acq->dma_buffer_size ){
        printk("Driver alloc bigger buffer as %d\n",new_length);
        ioctl_set_length(data_acq, new_length);
    }else{
        data_acq->transfer_length = new_length;
    };
    
    return count;
}

static ssize_t buffer_length_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf)
{
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        return sprintf(buf,"%s","obtain priv data from kobj error!\n");
    }

    return sprintf(buf, "DMA buffer length %d\n",data_acq->dma_buffer_size);
}

static ssize_t buffer_length_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count)
{
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        printk("obtain priv data from kobj error!\n");
        return -EFAULT;
    }

    // change buffer length
    int ret = 0;
    int length = 0;
    
    ret = kstrtouint(buf,10,&length);
    if( ret )
        return ret;

    if( length > 0 && count != data_acq->dma_buffer_size ){
        ioctl_set_buffer(data_acq, length);
    };
    
    return count;
};


static ssize_t trigger_transfer_store(struct kobject *kobj,
                                    struct kobj_attribute *attr,
                                    const char *buf, size_t count)
{
    // TODO: start dma transfer
    struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    if( !data_acq ){
        printk("obtain priv data from kobj error!\n");
        return -EFAULT;
    };

    if( *buf != '1'){
        printk("invalid flag written!\n");
        return -EFAULT;
    } else {
        // start dma transfer
        if( !data_acq->dma_buffer || !data_acq->dma_chan ){
            printk("DMA buffer or DMA engine not prepared!\n");
            return -EFAULT;
        }else{
            ioctl_start_transfer(data_acq);
        };
    };

    return count;
};

static ssize_t transfer_avaliable_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf)
{
     struct data_acq* data_acq = container_of(kobj,struct data_acq, kobj);
    return sprintf(buf,"%d Bytes avaliable\n",data_acq->available_length);
};

// define sys attrs
static struct kobj_attribute transfer_data_attribute = 
    __ATTR(transfer_data, 0664, transfer_data_show, transfer_data_store);

static struct kobj_attribute transfer_dma_attribute = 
    __ATTR(transfer_dma, 0664, transfer_dma_show, transfer_dma_store);

static struct kobj_attribute transfer_length_attribute = 
    __ATTR(transfer_length, 0664, transfer_length_show, transfer_length_store);

static struct kobj_attribute buffer_length_attribute = 
    __ATTR(buffer_length, 0664, buffer_length_show, buffer_length_store);

static struct kobj_attribute transfer_available_attribute = 
    __ATTR(transfer_available, 0444, transfer_avaliable_show, NULL);


static struct kobj_attribute trigger_transfer_attribute = 
    __ATTR(trigger_transfer_now, 0220, NULL, trigger_transfer_store);

static struct attribute *attrs[] = {
    &transfer_data_attribute.attr,
    &transfer_dma_attribute.attr,
    &transfer_length_attribute.attr,
    &buffer_length_attribute.attr,
    &transfer_available_attribute.attr,
    &trigger_transfer_attribute.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

static void data_ktype_release(struct kobject *kobj){
    return;
}

static struct kobj_type data_ktype = {
    .release = data_ktype_release,
    .sysfs_ops = &kobj_sysfs_ops,
    .default_groups = NULL,
};

// compatible match
static const struct of_device_id data_acquisition_of_match[] = {
    { .compatible = "none,data-acquisition" },
    { },
};
MODULE_DEVICE_TABLE(of, data_acquisition_of_match);

// platform driver probe
static int data_acquisition_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct resource *res;
    int ret = 0;
    struct data_acq* data_acq;
    // alloc priv data (zeroed to ensure embedded kobject and fields are initialized)
    data_acq = devm_kzalloc(&pdev->dev, sizeof(struct data_acq), GFP_KERNEL);
    if (!data_acq)
        return -ENOMEM;
    
    data_acq->pdev = pdev;
    // data_acq->dev  = &pdev->dev;
    platform_set_drvdata(pdev,data_acq);

    data_acq->reg_base = NULL;
    data_acq->data_type = TYPE_DATA;
    data_acq->available_length = 0;
    data_acq->cookie = 0;
    data_acq->dma_buffer = NULL;
    data_acq->dma_buffer_addr = 0;
    data_acq->dma_buffer_size = 0;
    data_acq->dma_chan = NULL;
    data_acq->dma_name = "rx";
    data_acq->transfer_length = 0;
    data_acq->user_tsk = NULL;
    data_acq->user_signo = 0;

    // regmap
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        ret = -ENODEV;
        goto error_unbind;
    }
    
    // data_acq->reg_base = devm_ioremap_resource(dev, res);
    data_acq->reg_base = devm_ioremap(dev, res->start, resource_size(res));

    if (IS_ERR(data_acq->reg_base)) {
        pr_err("Error Map regs!\n");
        ret = PTR_ERR(data_acq->reg_base);
        goto error_unbind;
    }
    
    // create char device
    ret = alloc_chrdev_region(&data_acq->devno, 0, 1, DEVICE_NAME);
    if (ret < 0)
        goto error_unbind;
    
    cdev_init(&data_acq->cdev, &fops);
    data_acq->cdev.owner = THIS_MODULE;
    
    ret = cdev_add(&data_acq->cdev, data_acq->devno, 1);
    if (ret < 0)
        goto error_unreg;
    
    // create sys class
    data_acq->class = class_create(CLASS_NAME);
    if (IS_ERR(data_acq->class)) {
        ret = PTR_ERR(data_acq->class);
        goto error_cdev;
    }
    
    // create dev node
    data_acq->dev = device_create(data_acq->class, NULL, data_acq->devno, 
                                   NULL, DEVICE_NAME);
    if (IS_ERR(data_acq->dev)) {
        ret = PTR_ERR(data_acq->dev);
        goto error_class;
    }
    
    // create sys dir under the device's kobject (avoid global kernel name conflicts)
    ret = kobject_init_and_add(&data_acq->kobj, &data_ktype, &pdev->dev.kobj, DEVICE_NAME);
    if ( ret ) {
        ret = -EINVAL;
        goto error_device;
    };
    
    ret = sysfs_create_group(&data_acq->kobj, &attr_group);
    if (ret)
        goto error_kobj;
    
    init_waitqueue_head(&data_acq->waitq);

    dev_info(dev, "Zynq PL data acquisition driver loaded\n");
    return 0;
    
error_kobj:
    kobject_put(&data_acq->kobj);
error_device:
    device_destroy(data_acq->class, data_acq->devno);
error_class:
    class_destroy(data_acq->class);
error_cdev:
    cdev_del(&data_acq->cdev);
error_unreg:
    unregister_chrdev_region(data_acq->devno, 1);
error_unbind:
    data_acq->pdev = NULL;
    data_acq->dev  = NULL;
    platform_set_drvdata(pdev,NULL);
    return ret;
}

static void data_acquisition_remove(struct platform_device *pdev)
{
    struct data_acq* data_acq = platform_get_drvdata(pdev);

    if (!data_acq)
        return;
    
    // remove sysfs attrs
    sysfs_remove_group(&data_acq->kobj, &attr_group);
    kobject_put(&data_acq->kobj);
    
    // cleanup char device
    device_destroy(data_acq->class, data_acq->devno);
    class_destroy(data_acq->class);
    cdev_del(&data_acq->cdev);
    unregister_chrdev_region(data_acq->devno, 1);
    
    // put dma buffer to null
    if( data_acq->dma_buffer ){
        dma_free_coherent(&data_acq->pdev->dev, data_acq->dma_buffer_size,
            data_acq->dma_buffer, data_acq->dma_buffer_addr);
    };

    if( data_acq->dma_chan ){
        dma_close(data_acq);
        data_acq->dma_chan = NULL;
    };

    data_acq->dma_buffer = NULL;
    data_acq->transfer_length = 0;
    data_acq->dma_chan = 0;
    data_acq->dma_buffer_size = 0;
    data_acq->dma_buffer_addr = 0;
    
    /* data_acq was allocated with devm_kzalloc and is device-managed; no explicit kfree here */
    
    return;
}

static struct platform_driver data_acquisition_driver = {
    .probe = data_acquisition_probe,
    .remove = data_acquisition_remove,
    .driver = {
        .name = DEVICE_NAME,
        .owner = THIS_MODULE,
        .of_match_table = data_acquisition_of_match,
    },
};

module_platform_driver(data_acquisition_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("None");
MODULE_DESCRIPTION("Zynq PL data acquisition Driver");
