#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/sched.h>                                /* sched */
#include <linux/semaphore.h>                            /* sema_init ... */
#include <linux/cdev.h>                                 /* cdev */
#include <linux/slab.h>                                 /* kmalloc */
#include <linux/types.h>                                /* cdev_t */
#include <linux/kernel.h>                               /* printk(), min()*/
#include <linux/errno.h>                                /* errno number */
#include <linux/uaccess.h>                              /* copy_form_user ... */
#include <linux/poll.h>									/* poll_wait... */


/**********************************************
 *
 * 这个例程演示了手动设置和自动设置阻塞和非阻塞
 * 以及异步操作，通知
 *
 *
 *
 ************************************************/



struct scull_pipe {
	wait_queue_head_t	inq, outq;              /* read and write queues */
	char			*buffer, *end;          /* begin of buf, end of buf */
	int			buffersize;             /* used in pointer arithmetic */
	char			*rp, *wp;               /* where to read, where to write */
	int			nreaders, nwriters;     /* number of openings for r/w */
	struct fasync_struct	*async_queue;           /* asynchronous readers */
	struct semaphore	sem;                    /* mutual exclusion semaphore */
	struct cdev		cdev;                   /* Char device structure */
};

struct scull_pipe	*scull_p;
static int		scull_p_major	= 0;
static int		scull_p_minnr	= 0;
static int		scull_p_buffer	= 4000;


/*
 * 自动设置
 * 阻塞和非阻塞读方式
 */
static ssize_t scull_p_read( struct file *filp, char __user *buf, size_t count, loff_t *offset )
{
	struct scull_pipe *scull = filp->private_data;

	if ( down_interruptible( &scull->sem ) )
		return(-ERESTARTSYS);

	/* 没有数据可读时 */
	while ( scull->rp == scull->wp )
	{
		up( &scull->sem );

		/* 非阻塞读方式 */
		if ( filp->f_flags & O_NONBLOCK )
			return(-EAGAIN);
		printk( "\"%s\" reading: going to sleep\n", current->comm );

		/* 阻塞队列 */
		if ( wait_event_interruptible( scull->inq, scull->rp != scull->wp ) )
			return(-ERESTARTSYS); /* if 确保是所等待的条件，而不是其它信号 */

		/* 否则循环，并再次检查有没有其它人获得了该锁 ,如果有则继续等待*/
		if ( down_interruptible( &scull->sem ) )
			return(-ERESTARTSYS);
	}

	/*  */
	if ( scull->wp > scull->rp )
		count = min( count, (size_t) (scull->wp - scull->rp) );
	else    /* the write pointer has wrapped, return data up to dev->end */
		count = min( count, (size_t) (scull->end - scull->rp) );


	/*
	 * 注意这里还在获取的信号里，并没有释放，这样做是合理的，因为
	 * copy_to_user 可能会睡眠，但不会造成系统死锁
	 */
	if ( copy_to_user( buf, scull->rp, count ) )
	{
		up( &scull->sem );
		return(-EFAULT);
	}
	scull->rp += count;
	if ( scull->rp == scull->end )
		scull->rp = scull->buffer; /* wrapped */
	up( &scull->sem );

	/* 最后要唤醒写并返回 */
	wake_up_interruptible( &scull->outq );
	printk( "\"%s\" did read %li bytes\n", current->comm, (long) count );
	return(count);
}


/* 有多少空闲空间 */
static int freespace( struct scull_pipe *scull )
{
	if ( scull->rp == scull->wp )
		return(scull->buffersize - 1);

	return( ( (scull->rp + scull->buffersize - scull->wp) % scull->buffersize) - 1);
}


/* 
 * 手动设置阻塞和非阻塞读方式
 * 等待有写的空闲空间,
 * 调用者必需持有设备信号
 * 如果信号是一个错误信号，那么在释放设备前返回
 */
static int getwritespace( struct scull_pipe *scull, struct file *filp )
{
	while ( freespace( scull ) == 0 )/* 满 */

	{
		DEFINE_WAIT( wait );
		up( &scull->sem );

		if ( filp->f_flags & O_NONBLOCK )
			return(-EAGAIN);

		printk( "\"%s\" writing: going to sleep\n", current->comm );

		prepare_to_wait( &scull->outq, &wait, TASK_INTERRUPTIBLE );


		if ( freespace( scull ) == 0 ) /* 检查在等待过程中，被其它进程唤醒，若没有这一步有可以将永远失去被唤醒的机会 */
			schedule();

		finish_wait( &scull->outq, &wait );

		/*对信号的检查，如果是一个信号将交由上层处理 */
		if ( signal_pending( current ) )
			return(-ERESTARTSYS);

		/* 获取信号，来确保要运行的进程没有被其它进程所拥有 */
		if ( down_interruptible( &scull->sem ) )
			return(-ERESTARTSYS);
	}

	return(0);
}


static ssize_t scull_p_write( struct file *filp, const char __user *buf, size_t count, loff_t *offset )
{
	int result;
	
	struct scull_pipe *scull = filp->private_data;

	if ( down_interruptible( &scull->sem ) )
		return(-ERESTARTSYS);
	
/* 确认空间可写 */
	result = getwritespace(scull,filp);
	if(result)
		return result;

	/* ok, space is there, accept something */
	count = min(count, (size_t)freespace(scull));
	if (scull->wp >= scull->rp)
		count = min(count, (size_t)(scull->end - scull->wp)); /* to end-of-buf */
	else /* the write pointer has wrapped, fill up to rp-1 */
		count = min(count, (size_t)(scull->rp - scull->wp - 1));
	printk("Going to accept %li bytes to %p from %p\n", (long)count, scull->wp, buf);
	if (copy_from_user(scull->wp, buf, count)) {
		up (&scull->sem);
		return -EFAULT;
	}


	scull->wp += count;
	if (scull->wp == scull->end)
		scull->wp = scull->buffer;
	up(&scull->sem);

	/* 好了，达了这时也应该唤醒记者了 */
	wake_up_interruptible(&scull->inq); 

	/* 通知异步读者 */
	if(scull->async_queue)
		kill_fasync(&scull->async_queue, SIGIO, POLLIN);
	

	return(count);
}

/*
 * 在设备打开时做一些初始化工作
 */
static int scull_p_open( struct inode *inode, struct file *filp )
{
	struct scull_pipe *scull;

	scull			= container_of( inode->i_cdev, struct scull_pipe, cdev );
	filp->private_data	= scull;

	if ( down_interruptible( &scull->sem ) )
		return(-ERESTARTSYS);

	if ( !scull->buffer )
	{
		scull->buffer = kmalloc( sizeof(*scull), GFP_KERNEL );
		if ( !scull->buffer )
		{
			up( &scull->sem );
			return(-ENOMEM);
		}
	}

	scull->buffersize	= scull_p_buffer;
	scull->end		= scull->buffersize + scull->buffer;
	scull->rp		= scull->wp = scull->buffer;

/* 以何种方式打开文件 */
	if ( filp->f_mode & FMODE_READ )
		scull->nreaders++;
	else if ( filp->f_mode & FMODE_WRITE )
		scull->nwriters++;

	up( &scull->sem );

	return(nonseekable_open( inode, filp ) );
}


/*
 * 用于多输入，多输出情况
 *
 */
unsigned int sucll_poll (struct file *filp, struct poll_table_struct *wait)
{
	struct scull_pipe *scull = filp->private_data;
	int mask;

	down(&scull->sem);

/* 1,将队列挂载到等待队列上，并由内核确定调用poll/selecte/epoll */
	poll_wait(filp, &scull->inq, wait);
	poll_wait(filp, &scull->outq, wait);

/* 2 返回标志设置 */
	if(scull->rp != scull->wp)
		mask = POLLIN|POLLRDNORM;
	if(freespace(scull) == 0)
		mask = POLLOUT|POLLWRNORM;
	
/* 以上两步缺一不可 */	
	up(&scull->sem);

	return mask;
		
}

/* 异步通知，用于长计算，通知快，用户无须查询等待 */
/* usr->FASYNC   ---> kerner--->  fasync_helper   -------  kill_fasync--->sigio
 * -->user
 */
static int scull_fasync (int fd, struct file *filp, int mod)
{
	struct scull_pipe *scull = filp->private_data;
	return fasync_helper(fd, filp, mod, &scull->async_queue);
}



static struct file_operations fops = {
	.owner	= THIS_MODULE,
	.read	= scull_p_read,
	.write	= scull_p_write,
	.open	= scull_p_open,
	.poll	= sucll_poll,
	.fasync	= scull_fasync,
	.llseek	= no_llseek,/* 具体设备具体实现，write/read须有一致性 */
};

static int  setup_dev( struct scull_pipe *p_scull, dev_t devno )
{
	int result = 0;

	cdev_init( &p_scull->cdev, &fops );
	p_scull->cdev.owner	= THIS_MODULE;
	result			= cdev_add( &p_scull->cdev, devno, 1 );
	if ( result < 0 )
		printk( KERN_NOTICE "Error %d adding scullpipe%d", result, devno );

	return(result);
}


static void  scull_p_cleanup( void )
{
	dev_t devno;
	devno = MKDEV( scull_p_major, scull_p_minnr );
	kfree( scull_p );
	unregister_chrdev_region( devno, 1 );
}


static int __init scull_p_init( void )

{
	dev_t	devno;
	int	result;
	devno = MKDEV( scull_p_major, scull_p_minnr );

	if ( scull_p_major )
		result = register_chrdev_region( devno, 1, "scullp" );
	else{
		result		= alloc_chrdev_region( &devno, scull_p_minnr, 1, "scullp" );
		scull_p_major	= MAJOR( devno );
	}

	if ( result < 0 )
	{
		return(result);
	}

	scull_p = kmalloc( sizeof(*scull_p), GFP_KERNEL );
	if ( !scull_p )
	{
		unregister_chrdev_region( devno, 1 );
		return(0);
	}

	memset( scull_p, 0, sizeof(*scull_p) );

	sema_init( &scull_p->sem, 1 );
	init_waitqueue_head( &scull_p->inq );
	init_waitqueue_head( &scull_p->outq );

	result = setup_dev( scull_p, devno );
	if ( result )
		scull_p_cleanup();

	return(0);
}


module_init( scull_p_init );
module_exit( scull_p_cleanup );

MODULE_LICENSE( "GPL" );