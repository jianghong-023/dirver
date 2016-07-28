#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>  /* current and everything */
#include <linux/kernel.h> /* printk() */
#include <linux/fs.h>     /* everything... */
#include <linux/types.h>  /* size_t */
#include <linux/completion.h>

/*
 * 使用方法
 * echo 8 > /proc/sys/kernel/printk
 * insmod complete.ko
 * mknod /dev/complete c 250 2
 * cat /proc/complete &
 * echo "hello" > /dev/complete
 * 遵循以上顺序就可看到效果
 */

static int complete_major = 0;

DECLARE_COMPLETION(comp);

ssize_t complete_read (struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	printk(KERN_WARNING"process %i (%s) going to sleep\n",current->pid, current->comm);
	wait_for_completion(&comp);
	printk(KERN_WARNING "awoken %i (%s)\n", current->pid, current->comm);
	return 0; 

}

ssize_t complete_write (struct file *filp, const char __user *buf, size_t count,
		loff_t *pos)
{

	printk(KERN_WARNING "process %i (%s) awakening the readers...\n",	current->pid, current->comm);
	complete(&comp);
	return count;
}

static struct file_operations  fops = {
	.owner = THIS_MODULE,
	.read =  complete_read,
	.write = complete_write,

};

static int __init complete_init(void)
{
	int result;

	result = register_chrdev(complete_major, "complete", &fops);
	if(result < 0){
		return result;
	}

	if(complete_major == 0){
		complete_major = result;
	}

	return 0;

}

static void __exit complete_cleanup(void)
{
	unregister_chrdev(complete_major, "complete");
}



module_init(complete_init);
module_exit(complete_cleanup);

MODULE_LICENSE("GPL");