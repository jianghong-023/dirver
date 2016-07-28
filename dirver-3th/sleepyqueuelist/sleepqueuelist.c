#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/wait.h>


/***********************************
 * 演示 等待队列的使用(简单的睡眠)
 * 它不能用于原子上下文中
 * 它只能用于一个读一个写
 * 不能有多个读，如果有多个
 * 读可能会导致系统崩溃
 *
 ***********************************/

static dev_t sleep_major = 0;

DECLARE_WAIT_QUEUE_HEAD(wq);

static int flags = 0;


ssize_t sleep_read(struct file *filp, char __user *buf, size_t count, loff_t *offset)
{
	printk(KERN_WARNING"process %i (%s) going to sleep\n",current->pid, current->comm);	

	wait_event_interruptible(wq, flags !=0);
	flags = 0;
	printk(KERN_WARNING "awoken %i (%s)\n", current->pid, current->comm);

	return 0;
	
}


ssize_t sleep_write(struct file *filp, const char __user *buf, size_t count, loff_t *offset)
{
	printk(KERN_WARNING "process %i (%s) awakening the readers...\n",	current->pid, current->comm);
	flags = 1;
	wake_up_interruptible(&wq);

	return count;
}


static struct file_operations fops = {
	.owner	= THIS_MODULE,
	.read	= sleep_read,
	.write  = sleep_write,

};



static int __init sleep_init(void)
{
		int recvel;

		recvel = register_chrdev(sleep_major,"sleepy",&fops);
		if(recvel < 0){
			return recvel;
		}

		if(sleep_major == 0)
			sleep_major = recvel;

		return (0);
	
}


static void __exit sleep_cleanup(void)
{
	unregister_chrdev(sleep_major, "sleepy");

}



module_init(sleep_init);
module_exit(sleep_cleanup);
MODULE_LICENSE("GPL");