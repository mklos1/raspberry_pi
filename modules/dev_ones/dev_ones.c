#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kdev_t.h>
#include <linux/device.h>
#include <linux/cdev.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marcin Kłos");
MODULE_DESCRIPTION("Device producing ones like /dev/zeros");
MODULE_VERSION("1.0.0");

int dev_ones_init(void);
void dev_ones_exit(void);
static ssize_t dev_ones_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_ones_write(struct file *, const char *, size_t, loff_t *);
static int dev_ones_open(struct inode *, struct file *);
static int dev_ones_release(struct inode *, struct file *);


struct file_operations ops = {
	.owner = THIS_MODULE,
	.open = dev_ones_open,
	.read = dev_ones_read,
	.write = dev_ones_write,
	.release = dev_ones_release
};

static struct class* dev_cl;
static struct cdev dev_cdev;

dev_t dev_reg = MKDEV(60, 0);

static int dev_ones_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEVMODE=%#o", 0444);
	return 0;
}

int dev_ones_init(void)
{
	if (alloc_chrdev_region(&dev_reg, 0, 1, "char_dev") < 0) {
		return -1;
	}
	if ((dev_cl = class_create(THIS_MODULE, "chardrv")) == NULL) {
		unregister_chrdev_region(dev_reg, 1);
		return -1;
	}
	dev_cl->dev_uevent = dev_ones_uevent;
	if (device_create(dev_cl, NULL, dev_reg, NULL, "ones") == NULL) {
		class_destroy(dev_cl);
		unregister_chrdev_region(dev_reg, 1);
		return -1;
	}
	cdev_init(&dev_cdev, &ops);
	if (cdev_add(&dev_cdev, dev_reg, 1) == -1) {
		device_destroy(dev_cl, dev_reg);
		class_destroy(dev_cl);
		unregister_chrdev_region(dev_reg, 1);
		return -1;
	}
	printk (KERN_INFO "DEVONES: starting...\n");
	return 0;
}

static ssize_t dev_ones_read(struct file * _file, char * _buff, size_t _size, loff_t * _offset) {
	size_t i = 0;
	printk (KERN_NOTICE "DEVONES: bytes to read %d\n", _size);
	for (i = 0; i < _size; i++) {
		_buff[i] = 0xff;
	}
	return _size;
}

static ssize_t dev_ones_write(struct file * _file, const char * _buff, size_t _size, loff_t * _offset) {
	printk (KERN_NOTICE "DEVONES: It's read-only device.\n");
	return -EIO;
}

static int dev_ones_open(struct inode * _node, struct file * _file) {
	return 0;
}

static int dev_ones_release(struct inode * _node, struct file * _file) {
	return 0;
}

void dev_ones_exit(void)
{
	device_destroy(dev_cl, dev_reg);
	class_destroy(dev_cl);
	unregister_chrdev_region(dev_reg, 1);
	printk(KERN_INFO "DEVONES: unloaded.\n");
}

module_init(dev_ones_init);
module_exit(dev_ones_exit);
