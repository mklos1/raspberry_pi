#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>

#include "lcd_hdpcf.h"

MODULE_AUTHOR("Marcin Kłos");
MODULE_DESCRIPTION("HD44780 on I2C (with PCF8574T gpio expander)");
MODULE_LICENSE("GPL");


/* LCD functions */

#define LCD_RS             0x01
#define LCD_RW             0x02
#define LCD_CS             0x04
#define LCD_BL             0x08
#define LCD_D4             0x10
#define LCD_D5             0x20
#define LCD_D6             0x40
#define LCD_D7             0x80

#define LCD_MODE_CMD       0x00
#define LCD_MODE_DATA      0x01

#define LCD_CURSOR         0x02
#define LCD_CURSOR_BLINK   0x01
#define LCD_DISPLAY        0x04

static int hd44780_i2c_probe(struct i2c_client* _client,
      const struct i2c_device_id* _id);
static int hd44780_i2c_remove(struct i2c_client* _client);

/* --- */

/* Typicaly i2c to hd44780 module uses PCF8475T with address
   0x27. User has to take care to modify this driver, when
   address is different. Usually A0, A1 and A2 address lines
   are set by resistors on pcb. */
static const unsigned short normal_i2c[] = { 0x27, I2C_CLIENT_END };

static const struct i2c_device_id lcd_id[] = {
   { "hdpcf", 0 },
   { }
};
MODULE_DEVICE_TABLE(i2c, lcd_id);

struct i2c_board_info info = {
   .type = "hdpcf",
   .addr = 0x27,
};

static struct i2c_client *client;

struct hd44780_data {
   struct i2c_client* client;
   unsigned char disp_data[2][16];
   unsigned char backlight;
   unsigned char pcf_state;
   unsigned char cursor_state;
   unsigned char cursor_blink;
   unsigned char display_state;
};

static struct i2c_driver hd44780_i2c_driver = {
   .class = I2C_CLASS_HWMON,
   .driver = {
      .name = "hdpcf",
   },
   .probe = hd44780_i2c_probe,
   .remove = hd44780_i2c_remove,
   .address_list = normal_i2c,
   .id_table = lcd_id,
};

/* Function return negative if error */
static int hd44780_i2c_send(struct i2c_client* _client, char _mode,
      char _data) {
   int ret = 0;
   struct hd44780_data* data = i2c_get_clientdata(_client);
   unsigned char bl = data->backlight;
   switch (_mode) {
      case 0:
         ret |= i2c_smbus_write_byte(_client, (0xf0 & _data) | bl | (LCD_CS
            & ~LCD_RS));
         ret |= i2c_smbus_write_byte(_client, (0xf0 & _data) | (bl & ~LCD_CS
            & ~LCD_RS));
         ret |= i2c_smbus_write_byte(_client, ((0x0f & _data) << 4) | bl
            | (LCD_CS & ~LCD_RS));
         ret |= i2c_smbus_write_byte(_client, ((0x0f & _data) << 4) | (bl
            & ~LCD_CS & ~LCD_RS));
      break;
      default:
         ret |= i2c_smbus_write_byte(_client, (0xf0 & _data) | (bl
            | LCD_CS | LCD_RS));
         ret |= i2c_smbus_write_byte(_client, (0xf0 & _data) | (bl
            & (~LCD_CS)) | (LCD_RS));
         ret |= i2c_smbus_write_byte(_client, ((0x0f & _data) << 4)
            | (bl | LCD_CS | LCD_RS));
         ret |= i2c_smbus_write_byte(_client, ((0x0f & _data) << 4)
            | ((bl) & (~LCD_CS)) | (LCD_RS));
      break;
   }
   return ret;
}


/* We assume that userland want to write max two lines. Max size is 34 (two
full lines and two \n). If userland wants to write more, -ENOSPACE is
returned. After writing cursor is set to home position. Lines shorter than
16 characters are filled with spaces to write full display. It's nessesary
for clearing old content without CLEAR command. CLEAR command causes visible
blinking. */
static ssize_t lcd_update_display(struct lcd_hdpcf* _lcd) {
   struct i2c_client* _client = client;
   int i;
   int ret = 0;
   for (i = 0; i < 16; i++){
      ret = hd44780_i2c_send(_client, LCD_MODE_DATA, _lcd->buffer[0][i]);
   }
   hd44780_i2c_send(_client, LCD_MODE_CMD, 0xC0);
   for (i = 0; i < 16; i++) {
      ret = hd44780_i2c_send(_client, LCD_MODE_DATA, _lcd->buffer[1][i]);
   }
   return 0;
}

/* Set cursor state to dash on or off. Blink overrides curror setting.
If I2C error, -EIO returned. */
static ssize_t lcd_update_state(struct lcd_hdpcf* _lcd) {
   struct i2c_client* _client = client;
   struct hd44780_data* _data = i2c_get_clientdata(_client);
   int ret;
   _data->cursor_state = (_lcd->cursor_state) ? LCD_CURSOR : 0;
   _data->cursor_blink = (_lcd->cursor_blink) ? LCD_CURSOR_BLINK : 0;
   _data->display_state = (_lcd->display_state) ? LCD_DISPLAY : 0;
   _data->backlight = (_lcd->backlight_state) ? LCD_BL : 0;
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x08 | _data->cursor_state
      | _data->cursor_blink | _data->display_state);
   if (ret < 0) return -EIO;
   ret = i2c_smbus_write_byte(_client, 0xf0 | LCD_CS | _data->backlight);
   if (ret < 0) return -EIO;
   return 0;
}

/* Sets curor position. If I2C error -EIO returned. */
static int lcd_gotoxy(unsigned char _x, unsigned char _y) {
   struct i2c_client* _client = client;
   int ret = 0;
   if (_x > 15 ) _x = 15;
   if (_y > 1 ) _y = 1;
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x80 + ((64 * _y) + _x));
   if (ret < 0) return -EIO;
   return 0;
}

/* Shifts lcd content to left (0) or right (1). If I2C error -EIO returned. */
static ssize_t lcd_shift(unsigned char _dir) {
   struct i2c_client* _client = client;
   int ret = 0;
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x18 | ((_dir == 0) ? 0 : 4));
   if (ret < 0) return -EIO;
   return 0;
}


/* Clears display. If I2C error, -EIO returned. */
static ssize_t lcd_clear(void) {
  struct i2c_client* _client = client;
  int ret = 0;
  ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x1);
  if (ret < 0) return -EIO;
  return 0;
}

/* Sets user defined char to CGRAM. If bad CGRAM address -ENXIO is returned,
if I2C error, -EIO returned */
static ssize_t lcd_set_char(struct user_char* _char) {
  struct i2c_client* _client = client;
  int ret = 0;
  int i = 0;
  if (_char->address < 0 || _char->address > 7) return -ENXIO;
  ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x40 + _char->address);
  for (i = 0; i < 8; i++) {
     ret = hd44780_i2c_send(_client, LCD_MODE_DATA, _char->chr[i]);
     if (ret < 0) return -EIO;
  }
  return 0;
}


/* Typical initialization procedure of hd44780 with 4-bit interface */
static int hd44780_i2c_init(struct i2c_client* _client) {
   int ret = 0;
   ret = i2c_smbus_write_byte(_client, 0x30 | LCD_CS);
   if (ret < 0) goto init_error;
   ret = i2c_smbus_write_byte(_client, 0x30 & ~LCD_CS);
   if (ret < 0) goto init_error;
   msleep(5);
   ret = i2c_smbus_write_byte(_client, 0x30 | LCD_CS);
   if (ret < 0) goto init_error;
   ret = i2c_smbus_write_byte(_client, 0x30 & ~LCD_CS);
   if (ret < 0) goto init_error;
   udelay(200);
   ret = i2c_smbus_write_byte(_client, 0x30 | LCD_CS);
   if (ret < 0) goto init_error;
   ret = i2c_smbus_write_byte(_client, 0x30 & ~LCD_CS);
   if (ret < 0) goto init_error;
   udelay(200);
   ret = i2c_smbus_write_byte(_client, 0x20 | LCD_CS);
   if (ret < 0) goto init_error;
   ret = i2c_smbus_write_byte(_client, 0x20 & ~LCD_CS);
   if (ret < 0) goto init_error;
   udelay(700);
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x28);
   if (ret < 0) goto init_error;
   udelay(700);
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x08);
   if (ret < 0) goto init_error;
   udelay(700);
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x01);
   if (ret < 0) goto init_error;
   udelay(700);
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x06);
   if (ret < 0) goto init_error;
   udelay(700);
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x0E);
   if (ret < 0) goto init_error;
   return 0;

init_error:
   dev_err(&_client->dev, "lcd_drv: Error in lcd initialization, errno: %d\n",
      ret);
   return ret;
}

/* Deinitialization is not needed, but when lcd is no longer avialiable in the
system, i think there is no need to keep last displayed data. Clear display,
off cursor and off display. */
static int hd44780_i2c_deinit(struct i2c_client* _client) {
   int ret = 0;
   //clear display
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x01);
   if (ret < 0) goto deinit_error;
   //off display, off cursor
   msleep(1);
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x08);
   if (ret < 0) goto deinit_error;
   msleep(1);
   ret = i2c_smbus_write_byte(_client, (0xf0 | (LCD_CS & ~LCD_BL)));
   if (ret < 0) goto deinit_error;
   return 0;

deinit_error:
   dev_err(&_client->dev, "lcd_dev: unable to deinit lcd, errno: %d\n", ret);
   return ret;

}

/* Nothing special to probe() function. Allocate resources and prepare device
to operate */
static int hd44780_i2c_probe(struct i2c_client* _client,
      const struct i2c_device_id* _id) {
   struct device* dev = &_client->dev;
   struct hd44780_data* data;
   int ret = 0;

   data = devm_kzalloc(dev, sizeof(struct hd44780_data), GFP_KERNEL);
   if (!data) {
      printk(KERN_CRIT "lcd_drv: Out of memory\n");
      return -ENOMEM;
   }
   data->client = _client;
   data->backlight = LCD_BL;
   data->cursor_state = 0;
   data->cursor_blink = 0;
   data->display_state = 1;
   i2c_set_clientdata(_client, data);
   ret = hd44780_i2c_init(_client);
   if (ret < 0) goto probe_error;
  return 0;

probe_error:
   dev_err(&_client->dev, "lcd_drv: Probe error, errno: %d\n", ret);
   return ret;

}

/* Deinitiazation on remove */
static int hd44780_i2c_remove(struct i2c_client* _client) {
   int ret = 0;
   ret = hd44780_i2c_deinit(_client);
   if (ret < 0) {
      dev_err(&_client->dev, "lcd_drv: Error while removing device, \
         errno %d\n", ret);
      return ret;
   }
   return 0;
}

long hdpcf_ioctl(struct file* _file, unsigned int _cmd,
   unsigned long _args) {
   struct lcd_hdpcf* lcd;
   int ret;
   switch (_cmd) {
      case IOCTL_LCD_UPDATE_STATE:
         lcd = (struct lcd_hdpcf*) _args;
         ret = lcd_update_state(lcd);
         if (ret < 0) return ret;
         break;
      case IOCTL_LCD_UPDATE_DISPLAY:
         lcd = (struct lcd_hdpcf*) _args;
         ret = lcd_update_display(lcd);
         if (ret < 0) return ret;
         break;
      case IOCTL_LCD_CLEAR:
         ret = lcd_clear();
         msleep(5);
         if (ret < 0) return ret;
         break;
      case IOCTL_LCD_HOME:
         ret = lcd_gotoxy(0, 0);
         if (ret < 0) return ret;
         break;
      case IOCTL_LCD_SHIFT:
         ret = lcd_shift(_args);
         if (ret < 0) return ret;
         break;
      case IOCTL_LCD_SET_CHAR:
         ret = lcd_set_char((struct user_char*)_args);
         if (ret < 0) return ret;
         break;
      default:
         printk (KERN_INFO "hdpcf: Unknown IOCTL\n");
         break;
   }
   return 0;
}


struct file_operations ops = {
	.owner = THIS_MODULE,
   .unlocked_ioctl = hdpcf_ioctl,
};

static struct class* dev_cl;
static struct cdev dev_cdev;

dev_t dev_reg = MKDEV(60, 0);

static int dev_ones_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}


static int hd44780_i2c_driver_init(void) {
   struct i2c_adapter *adapter = NULL;
   int ret = 0;
   adapter = i2c_get_adapter(1);
   if (!adapter) {
      printk(KERN_ERR "lcd_drv: Error while getting i2c adapter\n");
      return -ENODEV;
   }
   client = i2c_new_device(adapter, &info);
   if (!client) {
      printk(KERN_ERR "lcd_drv: Error while adding device\n");
   }
   ret = i2c_add_driver(&hd44780_i2c_driver);
   if (ret < 0) {
      printk(KERN_ERR "lcd_drv: Error while adding driver, errno: %d", ret);
   }

   if (alloc_chrdev_region(&dev_reg, 0, 1, "char_dev") < 0) {
		return -1;
	}
	if ((dev_cl = class_create(THIS_MODULE, "chardrv")) == NULL) {
		unregister_chrdev_region(dev_reg, 1);
		return -1;
	}
	dev_cl->dev_uevent = dev_ones_uevent;
	if (device_create(dev_cl, NULL, dev_reg, NULL, "hdpcf") == NULL) {
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
	printk (KERN_INFO "hdpcf: starting...\n");
   return 0;
}

static void hd44780_i2c_driver_exit(void) {
   i2c_unregister_device(client);
   i2c_del_driver(&hd44780_i2c_driver);
 	device_destroy(dev_cl, dev_reg);
	class_destroy(dev_cl);
	unregister_chrdev_region(dev_reg, 1);
	printk(KERN_INFO "hdpcf: unloaded.\n");
}

module_init(hd44780_i2c_driver_init);
module_exit(hd44780_i2c_driver_exit);
