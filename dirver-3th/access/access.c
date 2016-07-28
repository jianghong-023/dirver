#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include <asm/atomic.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <linux/sched.h>  /*  curren_***() */
#include <linux/tty.h> /* tty_devnum*/
#include <linux/slab.h>


static int	scull_p_major	= 0;
static int	scull_p_nr	= 0;
static int	SCULL_N_ADEVS	= 4;
static int	scull_quantum	= 4000;
static int	scull_qset	= 100;


struct scull_qset {
	void			**data;
	struct scull_qset	*next;
};

struct scull_dev {
	struct scull_qset	*data;          /* Pointer to first quantum set */
	int			quantum;        /* the current quantum size */
	int			qset;           /* the current array size */
	unsigned long		size;           /* amount of data stored here */
	unsigned int		access_key;     /* used by sculluid and scullpriv */
	struct semaphore	sem;            /* mutual exclusion semaphore     */
	struct cdev		cdev;           /* Char device structure		*/
};

static struct scull_dev scull_s_device;


/* 此设备只允许一个进程打开
 */
atomic_t scull_s_avalueable = ATOMIC_INIT( 1 );

static int scull_s_open( struct inode *inode, struct file *filp )
{
	struct scull_dev *dev = &scull_s_device;

	if ( !atomic_dec_and_test( &scull_s_avalueable ) )
	{
		atomic_inc( &scull_s_avalueable );
		return(-EBUSY);
	}

	/* 模式检查 */
	if ( (filp->f_mode & O_ACCMODE) == O_WRONLY )
		;

	filp->private_data = dev;

	return(0);
}


static int scull_release( struct inode *inode, struct file *filp )
{
	atomic_inc( &scull_s_avalueable );

	return(0);
}


struct file_operations scull_sngl_fops = {
	.owner		= THIS_MODULE,
	.open		= scull_s_open,
	.release	= scull_release,
};


/*********************
 *
 * 此设备允许一个进程
 * 多次打开
 *
 **********************/
static struct scull_dev scull_u_device;
static int		scull_u_count;  /* initialized to 0 by default */
static unsigned int	scull_u_owner;  /* initialized to 0 by default */

DEFINE_SPINLOCK( scull_u_lock );
static int scull_u_open( struct inode *inode, struct file *filp )
{
	struct scull_dev *dev = &scull_u_device;

	spin_lock( &scull_u_lock );

	if ( scull_u_count &&
	     (scull_u_owner != current_uid() ) &&
	     (scull_u_owner != current_euid() ) )
	{
		spin_unlock( &scull_u_lock );
		return(-EBUSY);
	}

	if ( scull_u_count == 0 )
		scull_u_owner = current_uid();

	scull_u_count++;

	spin_unlock( &scull_u_lock );

	if ( (filp->f_flags & O_ACCMODE) == O_WRONLY )
		;

	filp->private_data = dev;

	return(0);
}


static int scull_u_release( struct inode *inode, struct file *filp )
{
	spin_lock( &scull_u_lock );
	scull_u_count--;
	spin_unlock( &scull_u_lock );

	return(0);
}


struct file_operations scull_user_fops = {
	.owner		= THIS_MODULE,
	.open		= scull_u_open,
	.release	= scull_u_release,
};


/*********************************************
 *
 * 用阻塞open的方法来替代EBUYS信号
 *
 *
 *
 **********************************************/
static struct scull_dev scull_w_device;
static int		scull_w_count;  /* initialized to 0 by default */
static uid_t		scull_w_owner;  /* initialized to 0 by default */

DECLARE_WAIT_QUEUE_HEAD( scull_w_wait );
DEFINE_SPINLOCK( scull_w_lock );


static inline int scull_w_available( void )
{
	return(scull_w_count == 0 ||
	       scull_w_owner == current_uid() ||
	       scull_w_owner == current_euid() ||
	       capable( CAP_DAC_OVERRIDE ) );
}


static int scull_w_open( struct inode *inode, struct file *filp )
{
	struct scull_dev *dev = &scull_u_device;

	spin_lock( &scull_w_lock );

/*
 * if(scull_w_count &&
 *      (scull_w_owner != current_uid()) &&
 *      (scull_w_owner != current_euid())){
 */

/*
 *      spin_unlock(&scull_w_lock);
 *      return -EBUSY;
 * }
 * replace :
 */
 /*  以上是对上面EBUSY替换程序 */
	while ( !scull_w_available() )
	{
		spin_unlock( &scull_w_lock );

		if ( filp->f_flags & O_NONBLOCK )
			return(-EAGAIN);

		if ( wait_event_interruptible( scull_w_wait, scull_w_available() ) )
			return(-ERESTARTSYS);

		spin_lock( &scull_w_lock );
	}


	if ( scull_w_count == 0 )
		scull_w_owner = current_uid();

	scull_w_count++;
	spin_unlock( &scull_w_lock );

	if ( (filp->f_flags & O_ACCMODE) == O_WRONLY )
		;

	filp->private_data = dev;

	return(0);
}


static int scull_w_release( struct inode *inode, struct file *filp )
{
	int tmp;

	spin_lock( &scull_w_lock );
	scull_u_count--;
	tmp = scull_u_count;
	spin_unlock( &scull_w_lock );

	if ( scull_u_count == 0 )
		wake_up_interruptible_sync( &scull_w_wait );

	return(0);
}


struct file_operations scull_wusr_fops = {
	.owner		= THIS_MODULE,
	.open		= scull_w_open,
	.release	= scull_w_release,
};

/*********************************************
 *
 * open时复制设备
 *
 ******************************************/
static LIST_HEAD(scull_c_list);
DEFINE_SPINLOCK(scull_c_lock);

static struct scull_dev scull_c_device;  

struct scull_listitem {
	struct scull_dev device;
	dev_t key;
	struct list_head list;
    
};


static struct scull_dev *scull_c_lookfor_device(dev_t key)
{
	struct scull_listitem *lptr;

	list_for_each_entry(lptr, &scull_c_list, list) {
		if (lptr->key == key)
			return &(lptr->device);
	}

	/* not found */
	lptr = kmalloc(sizeof(struct scull_listitem), GFP_KERNEL);
	if (!lptr)
		return NULL;

	/* initialize the device */
	memset(lptr, 0, sizeof(struct scull_listitem));
	lptr->key = key;

	sema_init(&(lptr->device.sem),1);

	/* place it in the list */
	list_add(&lptr->list, &scull_c_list);

	return &(lptr->device);
}

static int scull_c_open( struct inode *inode, struct file *filp )
{

	struct scull_dev *dev;
	dev_t key;
	
	if(!current->signal->tty){
		printk("Process \"%s\" has no ctl tty\n", current->comm);
		return -EINVAL;
	}

	key = tty_devnum(current->signal->tty);

	spin_lock(&scull_c_lock);
	dev = scull_c_lookfor_device(key);
	spin_unlock(&scull_c_lock);

	
	if((filp->f_flags & O_ACCMODE)== O_RDONLY)
	;
	filp->private_data = dev;

	return 0;
}

static int scull_c_release( struct inode *inode, struct file *filp )
{
	return 0;

}

struct file_operations scull_priv_fops = {
	.owner		= THIS_MODULE,
	.open		= scull_c_open,
	.release	= scull_c_release,
};


static struct scull_adev_info {
	char			*name;
	struct scull_dev	*sculldev;
	struct file_operations	*fops;
} scull_access_devs[] = {
	{ "scullsingle", &scull_s_device, &scull_sngl_fops },
	{ "sculluid",	 &scull_u_device, &scull_user_fops },
	{ "scullwuid",	 &scull_w_device, &scull_wusr_fops },
	{ "sullpriv", &scull_c_device, &scull_priv_fops } 
};


static void setup_install_dev( struct scull_adev_info *devinfo, dev_t devno )
{
	int err;

	struct scull_dev *dev = devinfo->sculldev;
	dev->quantum	= scull_quantum;
	dev->qset	= scull_qset;

	sema_init( &dev->sem, 1 );

	cdev_init( &dev->cdev, devinfo->fops );
	err = kobject_set_name( &dev->cdev.kobj, devinfo->name );
	if ( err != 0 )
		printk( KERN_NOTICE "Error %d adding %s\n", err, devinfo->name );

	dev->cdev.owner = THIS_MODULE;

	err = cdev_add( &dev->cdev, devno, 1 );
	if ( err )
	{
		printk( KERN_NOTICE "Error %d adding %s\n", err, devinfo->name );
		kobject_put( &dev->cdev.kobj );
	} else
		printk( KERN_NOTICE "%s registered at %x\n", devinfo->name, devno );
}


static int __init scull_s_init( void )
{
	int	result, i;
	dev_t	devno;


	if ( scull_p_major )
	{
		devno	= MKDEV( scull_p_major, scull_p_nr );
		result	= register_chrdev_region( devno, SCULL_N_ADEVS, "scull_sig" );
	}else{
		result		= alloc_chrdev_region( &devno, scull_p_nr, SCULL_N_ADEVS, "scull_sig" );
		scull_p_major	= MAJOR( devno );
	}

	if ( result < 0 )
	{
		printk( KERN_WARNING "sculla: device number registration failed\n" );
		return(0);
	}

	for ( i = 0; i < SCULL_N_ADEVS; i++ )
	{
		setup_install_dev( scull_access_devs + i, devno + i );
	}


	return(0);
}


static void __exit scull_s_exit( void )
{
	int i;

	for ( i = 0; i < SCULL_N_ADEVS ; i++ )
	{
		struct scull_dev *dev = scull_access_devs[i].sculldev;
		cdev_del( &dev->cdev );
	}
}


MODULE_LICENSE( "GPL" );

module_init( scull_s_init );
module_exit( scull_s_exit );

