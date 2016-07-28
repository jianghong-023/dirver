#include <linux/fs.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/moduleparam.h>


#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>               /* printk */
#include <linux/mm.h>
#include <linux/ioport.h>               /* requset_**_region */
#include <linux/uaccess.h>              /* copy_form_user ... */
#include <linux/poll.h>                 /* POLLIN ... */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <asm/io.h>                     /* readb,ioremap... */

/* ����ַ */
static unsigned long	base		= 0x384;
unsigned long		var_base	= 0;
module_param( base, long, 0 );          /* 0,Ĭ��Ȩ�� */

/* �жϺ� */
static unsigned int	irq	= -1;
unsigned int		var_irq = -1;
module_param( irq, int, 0 );            /* 0,Ĭ��Ȩ�� */

/* io�ڴ�/io�˿� */
static int user_mem = 0;
module_param( user_mem, int, 0 );       /* 0,Ĭ��Ȩ�� */

/* ʹ�ö˿� */
#define NR_PORTS (8)

/* ���豸�� */
static int major = 0;
module_param( major, int, 0 );          /* 0,Ĭ��Ȩ�� */

unsigned long		short_buffer = 0;
unsigned long volatile	short_head;
volatile unsigned long	short_tail;

/* �����ж�ʱ��ѡ���� */
static int probe = 0;
module_param( probe, int, 0 );  /* 0,Ĭ��Ȩ�� */

/* �����ж� */
static int share = 0;
module_param( share, int, 0 );  /* 0,Ĭ��Ȩ�� */

static int wq = 0;              /* select at load time whether a workqueue is used */
module_param( wq, int, 0 );

static int tasklet = 0;         /* select whether a tasklet is used */
module_param( tasklet, int, 0 );


DECLARE_WAIT_QUEUE_HEAD( short_queue );


enum short_modes { SHORT_DEFAULT=0, SHORT_PAUSE, SHORT_STRING, SHORT_MEMORY };

static inline void short_incr_bp( volatile unsigned long *index, int delta );


static inline void short_incr_tv( volatile struct timeval **tvp );


ssize_t do_short_read( struct inode *inode, struct file *filp, char __user *buf,
		       size_t count, loff_t *f_pos )
{
	int		retval		= count, minor = iminor( inode );
	unsigned long	port		= var_base + (minor & 0x0f);
	void		*address	= (void *) var_base + (minor & 0x0f);/* ����ַ + ƫ��(4��bit) */
	int		mode		= (minor & 0x70) >> 4;

	/* kmalloc ��Χ 32B~128KB*/
	unsigned char *kbuf = kmalloc( count, GFP_KERNEL ), *ptr;

	if ( !kbuf )
		return(-ENOMEM);
	ptr = kbuf;

	if ( user_mem )
		mode = SHORT_MEMORY;

	switch ( mode )
	{
	case SHORT_STRING:
		insb( port, ptr, count );
		rmb();/* �ڴ����ϣ�Ҫ���ϸ�˳��д�룬��ͬ */
		break;

	case SHORT_DEFAULT:
		while ( count-- )
		{
			*(ptr++) = inb( port );
			rmb();
		}
		break;

	case SHORT_MEMORY:
		while ( count-- )
		{
			*ptr++ = ioread8( address );
			rmb();
		}
		break;
	case SHORT_PAUSE:
		while ( count-- )
		{
			*(ptr++) = inb_p( port );
			rmb();
		}
		break;

	default: /* no more modes defined by now */
		retval = -EINVAL;
		break;
	}
	if ( (retval > 0) && copy_to_user( buf, kbuf, retval ) )
		retval = -EFAULT;
	kfree( kbuf );
	return(retval);
}


ssize_t do_short_write( struct inode *inode, struct file *filp, const char __user *buf,
			size_t count, loff_t *f_pos )
{
	int		retval		= count, minor = iminor( inode );
	unsigned long	port		= var_base + (minor & 0x0f);
	void		*address	= (void *) var_base + (minor & 0x0f);
	int		mode		= (minor & 0x70) >> 4;
	unsigned char	*kbuf		= kmalloc( count, GFP_KERNEL ), *ptr;

	if ( !kbuf )
		return(-ENOMEM);
	if ( copy_from_user( kbuf, buf, count ) )
		return(-EFAULT);
	ptr = kbuf;

	if ( user_mem )
		mode = SHORT_MEMORY;

	switch ( mode )
	{
	case SHORT_PAUSE:
		while ( count-- )
		{
			outb_p( *(ptr++), port );
			wmb();
		}
		break;

	case SHORT_STRING:
		outsb( port, ptr, count );
		wmb();
		break;

	case SHORT_DEFAULT:
		while ( count-- )
		{
			outb( *(ptr++), port );
			wmb();
		}
		break;

	case SHORT_MEMORY:
		while ( count-- )
		{
			iowrite8( *ptr++, address );
			wmb();
		}
		break;

	default: /* no more modes defined by now */
		retval = -EINVAL;
		break;
	}
	kfree( kbuf );
	return(retval);
}


static ssize_t inter_tasklet_work_read( struct file *filp, char __user *buf, size_t count, loff_t *offset )
{
	return(do_short_read( filp->f_inode, filp, buf, count, offset ) );
}


static ssize_t inter_tasklet_work_write( struct file *filp, const char __user *buf, size_t count, loff_t * offset )
{
	return(do_short_write( filp->f_inode, filp, buf, count, offset ) );
}


/*
 * ������ͬ�� poll_table ��ÿ��. poll_table �ṹֻ�Ƕ�һ�������ķ�װ, ���������
 * ����ʵ�ʵ����ݽṹ. �Ǹ����ݽṹ, ���� poll�� select, ��һ���ڴ�ҳ������, ����
 * ���� poll_table_entry �ṹ. ÿ�� poll_table_entry ���б����ݸ� poll_wait ��
 * struct file �� wait_queue_head_t ָ��, �Լ�һ�������ĵȴ��������. ��
 * poll_wait �ĵ�����ʱ�����������̵������ĵȴ�����. �����Ľṹ�������ں�ά����
 * ����������̿ɱ������еĶ�����ȥ��, �� poll ���� select ����֮ǰ
 */
static unsigned int inter_tasklet_work_poll( struct file *filp, struct poll_table_struct *wait )
{
	return(POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM);
}


/* ��һЩ��ʼ������ */
static int inter_tasklet_work_open( struct inode *inode, struct file *filp )
{
	return(0);
}


/* �������ͷ�ʱ�Ż����  */
static int inter_tasklet_work_release( struct inode *inode, struct file *filp )
{
	return(0);
}


static struct file_operations fops = {
	.read		= inter_tasklet_work_read,
	.write		= inter_tasklet_work_write,
	.poll		= inter_tasklet_work_poll,
	.open		= inter_tasklet_work_open,
	.release	= inter_tasklet_work_release,
};


void short_do_tasklet( unsigned long noset );


DECLARE_TASKLET( short_tasklet, short_do_tasklet, 0 );


#define NR_TIMEVAL 512                          /* length of the array of time values */

struct timeval		tv_data[NR_TIMEVAL];    /* too lazy to allocate it */
volatile struct timeval *tv_head	= tv_data;
volatile struct timeval *tv_tail	= tv_data;

static struct work_struct short_wq;

int short_wq_count = 0;


/* �Զ�̽���жϺ� */
static void kernel_probe_interrupt( void )
{
	int count = 0;

	do
	{
		unsigned long mask;


		/* 1.probe_irq_on �� probe_irq_off Ҫ�ɶԳ��֣�
		 * 2���������������ڳ��ֽ�����һ���жϣ����û�У�������ɹ�̽���жϺ�
		 */
		mask = probe_irq_on();
		outb_p( 0x10, var_base + 2 );   /* enable reporting */
		outb_p( 0x00, var_base );       /* clear the bit */
		outb_p( 0xFF, var_base );       /* set the bit: interrupt! */
		outb_p( 0x00, var_base + 2 );   /* disable reporting */
		udelay( 5 );
		var_irq = probe_irq_off( mask );

		if ( var_irq == 0 )             /* û�л���жϺ� */
		{
			printk( KERN_INFO "short: no irq reported by probe\n" );
			var_irq = -1;
		}
	}
	while ( var_irq < 0 && count++ < 5 );

	if ( var_irq < 0 )
		printk( "short: probe failed %i times, giving up\n", count );
}


static irqreturn_t short_probing( int irq, void *dev_id )
{
	if ( var_irq == 0 )
		var_irq = irq;          /* found */
	if ( var_irq != irq )
		var_irq = -irq;         /* ģ�����ж� */

	return(IRQ_RETVAL( 0 ) );
}


/* �ֶ�̽���жϺ� */
static void man_probe_interrupt( void )
{
	int	trials[]	= { 3, 5, 7, 9, 0 }; /* ��֪���в������ж�ֵ���� */
	int	tried[]		= { 0, 0, 0, 0, 0 };
	int	i, count = 0;

/* ���ڴ������п�����𲻴󣬼������ڶ��ǿ��ж� SA_INTERRUPT */


/*
 * SA_INTERRUPT   ��ʾ��ֹ�����жϣ�(��Ӧ�� IRQF_DISABLED )
 * SA_SHIRQ             ��ʾ������ͬ���жϺ� (��Ӧ�� IRQF_SHARED )
 * SA_SAMPLE_RANDOM   �˺��Ӱ�쵽 RANDOM �Ĵ���( ��Ӧ�� IRQF_SAMPLE_RANDOM )��
 */

/* �ѱ������IRQ�����������Ա�����ͷ� */
	for ( i = 0; trials[i]; i++ )
		tried[i] = request_irq( trials[i], short_probing, IRQF_DISABLED, "short probe", NULL );

	do
	{
		var_irq = 0;                    /* none got, yet */
		outb_p( 0x10, var_base + 2 );   /* enable */
		outb_p( 0x00, var_base );
		outb_p( 0xFF, var_base );       /* toggle the bit */
		outb_p( 0x00, var_base + 2 );   /* disable */
		udelay( 5 );                    /* give it some time */

		/* the value has been set by the handler */
		if ( var_irq == 0 )             /* none of them? */
		{
			printk( KERN_INFO "short: no irq reported by probe\n" );
		}


		/*
		 * If more than one line has been activated, the result is
		 * negative. We should service the interrupt (but the lpt port
		 * doesn't need it) and loop over again. Do it at most 5 times
		 */
	}
	while ( var_irq <= 0 && count++ < 5 );

	/* end of loop, uninstall the handler */
	for ( i = 0; trials[i]; i++ )
		if ( tried[i] == 0 )
			free_irq( trials[i], NULL );

	if ( var_irq < 0 )
		printk( "short: probe failed %i times, giving up\n", count );
}


static inline void short_incr_tv( volatile struct timeval **tvp )
{
	if ( *tvp == (tv_data + NR_TIMEVAL - 1) )
		*tvp = tv_data; /* Wrap */
	else
		(*tvp)++;
}


static inline void short_incr_bp( volatile unsigned long *index, int delta )
{
	unsigned long new = *index + delta;
	barrier(); /* Don't optimize these two together */
	*index = (new >= (short_buffer + PAGE_SIZE) ) ? short_buffer : new;
}


void short_do_tasklet( unsigned long noset )
{
	int savecount = short_wq_count, written;
	short_wq_count = 0; /* we have already been removed from the queue */


	/*
	 * The bottom half reads the tv array, filled by the top half,
	 * and prints it to the circular text buffer, which is then consumed
	 * by reading processes
	 */

	/* First write the number of interrupts that occurred before this bh */
	written = sprintf( (char *) short_head, "bh after %6i\n", savecount );
	short_incr_bp( &short_head, written );


	/*
	 * Then, write the time values. Write exactly 16 bytes at a time,
	 * so it aligns with PAGE_SIZE
	 */

	do
	{
		written = sprintf( (char *) short_head, "%08u.%06u\n",
				   (int) (tv_tail->tv_sec % 100000000),
				   (int) (tv_tail->tv_usec) );
		short_incr_bp( &short_head, written );
		short_incr_tv( &tv_tail );
	}
	while ( tv_tail != tv_head );

	wake_up_interruptible( &short_queue );  /* awake any reading process */
}


void shortdo_tasklet( struct work_struct *work )
{
	int savecount = short_wq_count, written;
	short_wq_count = 0;                     /* we have already been removed from the queue */


	/*
	 * The bottom half reads the tv array, filled by the top half,
	 * and prints it to the circular text buffer, which is then consumed
	 * by reading processes
	 */

	/* First write the number of interrupts that occurred before this bh */
	written = sprintf( (char *) short_head, "bh after %6i\n", savecount );
	short_incr_bp( &short_head, written );


	/*
	 * Then, write the time values. Write exactly 16 bytes at a time,
	 * so it aligns with PAGE_SIZE
	 */

	do
	{
		written = sprintf( (char *) short_head, "%08u.%06u\n",
				   (int) (tv_tail->tv_sec % 100000000),
				   (int) (tv_tail->tv_usec) );
		short_incr_bp( &short_head, written );
		short_incr_tv( &tv_tail );
	}
	while ( tv_tail != tv_head );

	wake_up_interruptible( &short_queue ); /* awake any reading process */
}


static irqreturn_t short_sh_interrupt( int irq, void *dev_id )
{
	int		value, written;
	struct timeval	tv;

	/* If it wasn't short, return immediately */
	value = inb( var_base );
	if ( !(value & 0x80) )
		return(IRQ_NONE);

	/* clear the interrupting bit */
	outb( value & 0x7F, var_base );

	/* the rest is unchanged */

	do_gettimeofday( &tv );
	written = sprintf( (char *) short_head, "%08u.%06u\n",
			   (int) (tv.tv_sec % 100000000), (int) (tv.tv_usec) );
	short_incr_bp( &short_head, written );
	wake_up_interruptible( &short_queue ); /* awake any reading process */

	return(IRQ_RETVAL( 0 ) );
}


irqreturn_t short_interrupt( int irq, void *dev_id )
{
	struct timeval	tv;
	int		written;

	do_gettimeofday( &tv );

	/* Write a 16 byte record. Assume PAGE_SIZE is a multiple of 16 */
	written = sprintf( (char *) short_head, "%08u.%06u\n",
			   (int) (tv.tv_sec % 100000000), (int) (tv.tv_usec) );
	BUG_ON( written != 16 );
	short_incr_bp( &short_head, written );
	wake_up_interruptible( &short_queue ); /* awake any reading process */
	return(IRQ_RETVAL( 0 ) );
}


/*
 * Tasklet top half,���еͰ벿������ԭ�ӵģ�����˯�ߵ�
 */
irqreturn_t short_tl_interrupt( int irq, void *dev_id )
{
	do_gettimeofday( (struct timeval *) tv_head );  /* cast to stop 'volatile' warning */
	short_incr_tv( &tv_head );
	tasklet_schedule( &short_tasklet );
	short_wq_count++;                               /* record that an interrupt arrived */
	return(IRQ_HANDLED);
}


irqreturn_t short_wq_interrupt( int irq, void *dev_id )
{
	/* Grab the current time information. */
	do_gettimeofday( (struct timeval *) tv_head );
	short_incr_tv( &tv_head );

	/* Queue the bh. Don't worry about multiple enqueueing */
	schedule_work( &short_wq );

	short_wq_count++; /* record that an interrupt arrived */
	return(IRQ_HANDLED);
}


static int __init init_inter_tasklet_work( void )
{
	int		result;
	struct resource *resource_p;
	var_base	= base;
	var_irq		= irq;

/* ����io/�˿��ڴ��� ,���Բ鿴����Ķ˿� "cat /proc/ioports" */
	switch ( user_mem )
	{
	case 0: {
		resource_p = request_region( var_base, NR_PORTS, "short" );
		if ( !resource_p )
		{
			printk( KERN_INFO "short: can't get I/O port address 0x%lx\n",
				var_base );
			return(-ENODEV);
		}
	}
	break;

	case 1: {
		resource_p = request_mem_region( var_base, NR_PORTS, "short" );
		if ( !resource_p )
		{
			printk( KERN_INFO "short: can't get I/O mem address 0x%lx\n",
				var_base );
			return(-ENODEV);
		}
	}

	break;
	}

/* ��õ������ַӳ�䵽�����ַ�ռ� */
	var_base = (unsigned long ) ioremap( var_base, NR_PORTS );
	if ( !var_base )
	{
		printk( KERN_ERR "ioremap error \n" );
	}

	result = register_chrdev( major, "short", &fops );
	if ( result < 0 )
	{
		printk( KERN_INFO "short: can't get major number\n" );

		release_region( var_base, NR_PORTS );

		return(result);
	}

	if ( major == 0 )
		major = result;

/* ����һ��ҳ */
	short_buffer	= __get_free_pages( GFP_KERNEL, 0 ); /* never fails */  /* FIXME */
	short_head	= short_tail = short_buffer;

	INIT_WORK( &short_wq, shortdo_tasklet );


/* �жϺŵ�̽��
 * 1.�ں��Զ�̽��
 * 2. �ֶ�̽��
 */

	if ( var_irq < 0 && probe == 1 )
		kernel_probe_interrupt();       /* �ں��Զ�̽�� */

	if ( var_irq < 0 && probe == 2 )
		man_probe_interrupt();


	if ( var_irq < 0 )                      /* ��û��ָ���ж�ʱ��ǿ��ʹ��������жϺ� */
		switch ( var_base )
		{
		case 0x378: var_irq	= 7; break;
		case 0x278: var_irq	= 2; break;
		case 0x3bc: var_irq	= 5; break;
		}

	/* ���ʹ�ù����жϽ�ִ�����²���*/
	if ( var_irq > 0 && share > 0 )
	{
		result = request_irq( var_irq, short_sh_interrupt, IRQF_SHARED | IRQF_DISABLED, "short", short_sh_interrupt );
		if ( result )
		{
			printk( KERN_INFO "short: can't get assigned irq %i\n", var_irq );
			var_irq = -1;
		}else {         /* actually enable it -- assume this *is* a parallel port */
			outb( 0x10, var_base + 2 );
		}
		return(0);      /* the rest of the function only installs handlers */
	}


	if ( var_irq >= 0 )
	{
		result = request_irq( var_irq, short_interrupt,
				      IRQF_DISABLED, "short", NULL );
		if ( result )
		{
			printk( KERN_INFO "short: can't get assigned irq %i\n",
				var_irq );
			var_irq = -1;
		}else { /* actually enable it -- assume this *is* a parallel port */
			outb( 0x10, var_base + 2 );
		}
	}


/* ok,������ʱ����ʾ �ж϶��벿�͵Ͱ벿��tasklet���������е�ʱ����*/

	if ( var_irq >= 0 && (wq + tasklet) > 0 )
	{
		free_irq( var_irq, NULL );
		result = request_irq( var_irq,
				      tasklet ? short_tl_interrupt : short_wq_interrupt,
				      IRQF_DISABLED, "short-bh", NULL );
		if ( result )
		{
			printk( KERN_INFO "short-bh: can't get assigned irq %i\n",
				var_irq );
			var_irq = -1;
		}
	}


	return(0);
}


/* ���� */
static void __exit exit_inter_tasklet_work( void )
{
	iounmap( (void __iomem *) var_base );

	if ( user_mem )
	{
		release_mem_region( var_base, NR_PORTS );
	}else{
		release_region( var_base, NR_PORTS );
	}
}


MODULE_LICENSE( "GPL" );
module_init( init_inter_tasklet_work );
module_exit( exit_inter_tasklet_work );

