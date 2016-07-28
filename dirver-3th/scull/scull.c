#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>  /* module_param */
#include <linux/kdev_t.h>       /* dev_t */
#include <linux/fs.h>           /*file_operations*/
#include <linux/errno.h>        /*errno*/
#include <linux/cdev.h>         /*struct cdev*/
#include <linux/kernel.h>       /* printk() */
#include <linux/slab.h>         /* kmalloc() */
#include <linux/proc_fs.h>      /* proc */
#include <linux/seq_file.h>


#include <linux/uaccess.h>      /* copy_to_user..  */
#include <linux/capability.h>   /* capable */


#include "scull.h"

int	scull_quantum	= SCULL_QUANTUM;
int	scull_qset	= SCULL_QSET;
int	scull_major	= SCULL_MAJOR;
int	scull_mino	= 0;
int	scull_nr_dev	= SCULL_NR_DEVS;


/*
 * S_IRUGO
 * 如果S_IRUGO为0 ，根本就没有sysfs这一项，否则就出现在
 * /sys/module下
 */
module_param( scull_quantum, int, S_IRUGO );
module_param( scull_qset, int, S_IRUGO );


struct scull_dev *scull_devices;

int scull_trim( struct scull_dev *dev )
{
	struct scull_qset	*next, *dptr;
	int			i;
	int			qset = dev->qset;

	for ( dptr = dev->data; dptr; dptr = next )
	{
		if ( dptr )
		{
			for ( i = 0; i < qset; i++ )
				kfree( dptr->data[i] );
			kfree( dptr->data );
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree( dptr );
	}

	dev->quantum	= scull_quantum;
	dev->qset	= scull_qset;
	dev->size	= 0;
	dev->data	= NULL;

	return(0);
}


/*
 * 下一个量子
 */
struct scull_qset * scull_follow( struct scull_dev *dev, int n )
{
	struct scull_qset *qs = dev->data;

	if ( !qs )
	{
		qs = dev->data = kmalloc( sizeof(struct scull_qset), GFP_KERNEL );
		if ( qs == NULL )
			return(NULL);
		memset( qs, 0, sizeof(struct scull_qset) );
	}

	while ( n-- )
	{
		if ( !qs->next )
		{
			qs->next = kmalloc( sizeof(struct scull_qset), GFP_KERNEL );
			if ( qs->next == NULL )
			{
				return(NULL);
			}

			memset( qs->next, 0, sizeof(struct scull_qset) );
		}

		qs = qs->next;

		continue;
	}

	return(qs);
}


int scull_open( struct inode *inode, struct file *filp )
{
	struct scull_dev *dev;

	dev			= container_of( inode->i_cdev, struct scull_dev, cdev );
	filp->private_data	= dev;

	if ( (filp->f_flags & O_ACCMODE) == O_WRONLY )
	{
		if ( down_interruptible( &dev->sem ) )
			return(-ERESTARTSYS);
		scull_trim( dev );
		up( &dev->sem );
	}

	return(0);
}


int scull_release( struct inode *inode, struct file *filp )
{
	return(0);
}


ssize_t scull_read( struct file *filp, char __user *buf, size_t count, loff_t *offset )
{
	ssize_t			ret		= 0;
	struct scull_dev	*dev		= filp->private_data;
	int			quantum		= dev->quantum;
	int			qset		= dev->qset;
	int			itemsize	= quantum * qset; /* 量子列表中有多少字节数据 */
	int			item, s_pos, q_pos, rest;
	struct scull_qset	*dptr;

	if ( down_interruptible( &dev->sem ) )
		return(-ERESTARTSYS);

	if ( *offset > dev->size )
		goto out;

	if ( (*offset + count) > dev->size )
		count = dev->size - *offset;


	/*
	 * 查找量子列表，索引量子集，定位位置
	 */

	item	= (long) *offset / itemsize;    /* 量子列表中的哪一项 */
	rest	= (long) *offset % itemsize;    /* 量子列表中还剩余多少量子项 */

	q_pos	= rest / quantum;               /* 在量子中的位置 */
	s_pos	= rest % quantum;               /* 剩余多少量子 */

	dptr = scull_follow( dev, item );

	if ( dptr == NULL || (!dptr->data) || (!dptr->data[s_pos]) )
		goto out;


	/* 如果大于量子的长度 */
	if ( count > quantum - q_pos )
		count = quantum - q_pos;

	if ( copy_to_user( buf, dptr->data[s_pos] + q_pos, count ) )
	{
		ret = -EFAULT;
		goto out;
	}

	*offset += count;

	ret = count;


out:
	up( &dev->sem );
	return(ret);
}


ssize_t scull_write( struct file *filp, const char __user *buf, size_t count, loff_t *offset )
{
	ssize_t			ret		= 0;
	struct scull_dev	*dev		= filp->private_data;
	int			quantum		= dev->quantum;
	int			qset		= dev->qset;
	int			itemsize	= quantum * qset; /* 量子列表中有多少字节数据 */
	int			item, s_pos, q_pos, rest;
	struct scull_qset	*dptr;

	if ( down_interruptible( &dev->sem ) )
		return(-ERESTARTSYS);

	if ( *offset > dev->size )
		goto out;


	/*
	 * 查找量子列表，索引量子集，定位位置
	 */

	item	= (long) *offset / itemsize;    /* 量子列表中的哪一项 */
	rest	= (long) *offset % itemsize;    /* 量子列表中还剩余多少量子项 */

	q_pos	= rest / quantum;               /* 在量子中的位置 */
	s_pos	= rest % quantum;               /* 剩余多少量子 */

	dptr = scull_follow( dev, item );
	if ( dptr == NULL )
		goto out;

	if ( !dptr->data )
	{
		dptr->data = kmalloc( qset * sizeof(char *), GFP_KERNEL );
		if ( !dptr->data )
			goto out;

		memset( dptr->data, 0, qset * sizeof(char *) );
	}

	if ( !dptr->data[s_pos] )
	{
		dptr->data[s_pos] = kmalloc( quantum, GFP_KERNEL );
		if ( !dptr->data[s_pos] )
			goto out;
	}

	/* 如果大于量子的长度 */
	if ( count > quantum - q_pos )
		count = quantum - q_pos;

	if ( copy_from_user( dptr->data[s_pos] + q_pos, buf, count ) )
	{
		ret = -EFAULT;
		goto out;
	}

	*offset += count;
	ret	= count;

	/* 更新大小 */

	if ( dev->size < *offset )
		dev->size = *offset;

out:
	up( &dev->sem );
	return(ret);
}


static int scull_read_procmem( struct file *filp, char __user *buf, size_t count, loff_t *offer )
{
	int	i, j, len = 0;
	int	limit = count - 80;


	for ( i = 0; i < scull_nr_dev && len < limit; i++ )
	{
		struct scull_dev	*d	= &scull_devices[i];
		struct scull_qset	*qs	= d->data;


		if ( down_interruptible( &d->sem ) )
			return(-ERESTARTSYS);

		len += sprintf( buf + len, "\nDevice %i: qset %i, q %i, sz %li\n",
				i, d->qset, d->quantum, d->size );

		for (; qs && len <= limit; qs = qs->next )      /* scan the list */

		{
			len += sprintf( buf + len, "  item at %p, qset at %p\n",
					qs, qs->data );

			if ( qs->data && !qs->next )            /* dump only the last item */
				for ( j = 0; j < d->qset; j++ )
				{
					if ( qs->data[j] )
						len += sprintf( buf + len,
								"    % 4i: %8p\n",
								j, qs->data[j] );
				}
		}

		up( &scull_devices[i].sem );
	}

	return(len);
}


static struct file_operations proc_fops = {
	.owner	= THIS_MODULE,
	.read	= scull_read_procmem,
};

static void *scull_seq_start( struct seq_file *s, loff_t *pos )
{
	if ( *pos >= scull_nr_dev )
		return(NULL); /* No more to read */
	return(scull_devices + *pos);
}


static void *scull_seq_next( struct seq_file *s, void *v, loff_t *pos )
{
	(*pos)++;
	if ( *pos >= scull_nr_dev )
		return(NULL);
	return(scull_devices + *pos);
}


static void scull_seq_stop( struct seq_file *s, void *v )
{
	/* Actually, there's nothing to do here */
}


static int scull_seq_show( struct seq_file *s, void *v )
{
	struct scull_dev	*dev = (struct scull_dev *) v;
	struct scull_qset	*d;
	int			i;
	if ( down_interruptible( &dev->sem ) )
		return(-ERESTARTSYS);
	seq_printf( s, "\nDevice %i: qset %i, q %i, sz %li\n",
		    (int) (dev - scull_devices), dev->qset,
		    dev->quantum, dev->size );
	for ( d = dev->data; d; d = d->next )   /* scan the list */
	{
		seq_printf( s, "  item at %p, qset at %p\n", d, d->data );
		if ( d->data && !d->next )      /* dump only the last item */
			for ( i = 0; i < dev->qset; i++ )
			{
				if ( d->data[i] )
					seq_printf( s, "    % 4i: %8p\n",
						    i, d->data[i] );
			}
	}

	up( &dev->sem );
	return(0);
}


static struct seq_operations scull_seq_ops = {
	.start	= scull_seq_start,
	.next	= scull_seq_next,
	.stop	= scull_seq_stop,
	.show	= scull_seq_show
};
static int scull_proc_open( struct inode *inode, struct file *file )
{
	return(seq_open( file, &scull_seq_ops ) );
}


static struct file_operations scull_proc_ops = {
	.owner		= THIS_MODULE,
	.open		= scull_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release
};

static int scull_create_proc( void )
{
	int			ret = -ENODEV;
	struct proc_dir_entry	* proc_entry;

	proc_entry = proc_create_data( "scullmem", 0, NULL, &proc_fops, NULL );
	if ( !proc_entry )
		return(ret);

	proc_entry = proc_create( "scullseq", 0, NULL, &scull_proc_ops );
	if ( !proc_entry )
		return(ret);

	return(0);
}


/******************
 * 32bit/8bit
 * ------------------------------
 * |type|nr | dir | size|
 *-------------------------------
 **/
static long scull_ioctl( struct file *filp, unsigned int cmd, unsigned long arg )
{
	int err = 0, recvel = 0,tmp;

	/* 类型和序号检查 */
	if ( _IOC_TYPE( cmd ) != SCULL_IOC_MAGIC )
		return(-ENOTTY);
	if ( _IOC_NR( cmd ) > SCULL_IOC_MAXNR )
		return(-ENOTTY);

	/* 方向和权限检查  */
	if ( _IOC_DIR( cmd ) & _IOC_READ )
		err = access_ok( VERIFY_WRITE, (void __user *) arg, _IOC_SIZE( cmd ) );
	else if ( _IOC_DIR( cmd ) & _IOC_WRITE )
		err = access_ok( VERIFY_READ, (void __user *) arg, _IOC_SIZE( cmd ) );
	else
		err = -EFAULT;


	switch ( cmd )
	{
	case SCULL_IOCRESET:
		scull_quantum	= SCULL_QUANTUM;
		scull_qset	= SCULL_QSET;
		break;

	case SCULL_IOCSQUANTUM:
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);
		recvel = __get_user( scull_quantum, (int __user *) arg );
		break;
	case SCULL_IOCTQUANTUM:
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);
		scull_quantum = arg;
		break;
	case SCULL_IOCGQUANTUM:
		recvel = __put_user( scull_quantum, (int __user *) arg );
		break;

	case SCULL_IOCQQUANTUM: /* Query: return it (it's positive) */
		return(scull_quantum);

	case SCULL_IOCXQUANTUM: /* eXchange: use arg as pointer */
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);

		tmp	= scull_quantum;
		recvel	= __get_user( scull_quantum, (int __user *) arg );
		if ( recvel == 0 )
			recvel = __put_user( tmp, (int __user *) arg );
		break;

	case SCULL_ICOHQUANTUM:
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);
		tmp = scull_quantum;

		scull_quantum = arg;

		return(tmp);
	case SCULL_IOCSQQET:
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);
		recvel = __get_user( scull_qset, (int __user *) arg );
		break;

	case SCULL_IOCTQSET:
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);
		scull_qset = arg;
		break;

	case SCULL_IOCGQSET:
		recvel = __put_user( scull_qset, (int __user *) arg );
		break;

	case SCULL_IOCQQSET:
		return(scull_qset);

	case SCULL_IOCXQSET:
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);
		tmp	= scull_qset;
		recvel	= __get_user( scull_qset, (int __user *) arg );
		if ( recvel == 0 )
			recvel = put_user( tmp, (int __user *) arg );
		break;

	case SCULL_IOCHQSET:
		if ( !capable( CAP_SYS_ADMIN ) )
			return(-EPERM);
		tmp		= scull_qset;
		scull_qset	= arg;
		return(tmp);


	/*
	 * The following two change the buffer size for scullpipe.
	 * The scullpipe device uses this same ioctl method, just to
	 * write less code. Actually, it's the same driver, isn't it?
	 */

/*	  case SCULL_P_IOCSZIE: */
/*		scull_p_buffer = arg; */
/*		break; */

/*	  case SCULL_P_IOCQSIZE: */
/*		return scull_p_buffer; */


	default: /* redundant, as cmd was checked against MAXNR */
		return(-ENOTTY);
	}


	return(recvel);
}


static void scull_remove_proc( void )
{
	/* no problem if it was not registered */
	remove_proc_entry( "scullmem", NULL /* parent dir */ );
	remove_proc_entry( "scullseq", NULL );
}


struct file_operations scull_fops = {
	.owner		= THIS_MODULE,
	.llseek		= NULL,
	.write		= scull_write,
	.read		= scull_read,
	.unlocked_ioctl = scull_ioctl,
	.open		= scull_open,
	.release	= scull_release,
};


static void scull_setup_dev( struct scull_dev *dev, int index )
{
	int	err;
	dev_t	devno = MKDEV( scull_major, scull_mino );

	cdev_init( &dev->cdev, &scull_fops );
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops	= &scull_fops;

	err = cdev_add( &dev->cdev, devno, 1 );
	if ( err )
		printk( KERN_NOTICE "Error %d adding scull%d", err, index );
}


static int __init scull_init_module( void )
{
	int	ret, i;
	dev_t	devno = 0;

	if ( scull_major )
	{
		devno	= MKDEV( scull_major, scull_mino );
		ret	= register_chrdev_region( devno, 1, "scull" );
	}else{
		ret		= alloc_chrdev_region( &devno, scull_mino, 1, "scull" );
		scull_major	= MAJOR( devno );
	}

	if ( ret < 0 )
	{
		printk( KERN_WARNING "scull: can't get major %d\n", scull_major );
		return(ret);
	}

	scull_devices = kmalloc( sizeof(struct scull_dev) * scull_nr_dev, GFP_KERNEL );
	if ( !scull_devices )
	{
		ret = -ENOMEM;
		goto fail;
	}

	for ( i = 0; i < scull_nr_dev; i++ )
	{
		scull_devices[i].quantum	= SCULL_QUANTUM;
		scull_devices[i].qset		= SCULL_QSET;
		sema_init( &scull_devices[i].sem, 1 );
		scull_setup_dev( &scull_devices[i], i );
	}

	PDEBUG( "device open\n" );
	/*
	 * if(printk_ratelimit())
	 * PDEBUG("device open\n");
	 */

	/*
	 * print_dev_t(buffer, dev);
	 * format_dev_t(buffer, dev)
	 */
#ifdef SCULL_DEBUG
	scull_create_proc();
#endif
	return(0);

fail:
	scull_cleanup_module();
	return(ret);
}


void  scull_cleanup_module( void )
{
	dev_t devno;

	devno = MKDEV( scull_major, scull_mino );

	unregister_chrdev_region( devno, 1 );
#ifdef SCULL_DEBUG
	scull_remove_proc();
#endif
}


module_init( scull_init_module );
module_exit( scull_cleanup_module );

MODULE_LICENSE( "GPL" );
MODULE_AUTHOR( "jianghong_023 <jianghong_023@163.com>" );
MODULE_DESCRIPTION( "Scull is just a validation test version" );
MODULE_VERSION( "scull 1.0" );


/*
 * MODULE_ALIAS("platform:scull");
 * MODULE_DEVICE_TABLE(of, gpmc_dt_ids);
 */
