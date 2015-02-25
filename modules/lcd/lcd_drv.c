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
   { "hd44780_i2c", 0 },
   { }
};
MODULE_DEVICE_TABLE(i2c, lcd_id);

struct i2c_board_info info = {
   .type = "hd44780_i2c",
   .addr = 0x27,
};

static struct i2c_client *client;

struct hd44780_data {
   struct i2c_client* client;
   unsigned char disp_data[16][2];
   unsigned char backlight;
   unsigned char pcf_state;
   unsigned char cursor_state;
   unsigned char cursor_blink;
   unsigned char display_state;
   unsigned char x_pos;
   unsigned char y_pos;
};

static struct i2c_driver hd44780_i2c_driver = {
   .class = I2C_CLASS_HWMON,
   .driver = {
      .name = "hd44780_i2c",
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

/* Sets curor position */
static int hd44780_i2c_gotoxy(struct i2c_client* _client, unsigned char _x,
   unsigned char _y) {
   struct hd44780_data* data = i2c_get_clientdata(_client);
   int ret = 0;
   if (ret < 0) return -EIO;
   if (_x > 15 ) _x = 15;
   if (_y > 1 ) _y = 1;
   ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x80 + ((64 * _y) + _x));
   if (ret < 0) return -EIO;
   data->x_pos = _x;
   data->y_pos = _y;
   return 0;
}

/* Writing to PCF is a bit complicated, because there is no possibility to
write single output without changing others. Readback and write won't work.
Fortunetly we can write anything to the PCF while enable pin of LCD stays 
in HIGH. */
static ssize_t write_backlight(struct device *dev, struct device_attribute*
   attr, const char *buf, size_t count) {
   struct i2c_client* _client = to_i2c_client(dev);
   struct hd44780_data* data = i2c_get_clientdata(_client);
   int ret = 0;
   if (count < 1) {
      return -EIO;
   } else {
      switch (buf[0]) {
         case 0:
         case '0':
            ret = i2c_smbus_write_byte(_client, 0xf0 | (LCD_CS & ~LCD_BL));
            data->backlight = 0;           
         break;
         default:
            ret = i2c_smbus_write_byte(_client, 0xf0 | LCD_CS | LCD_BL);
            data->backlight = LCD_BL;
         break;
      }
   }
   if (ret < 0)
      return ret;
   return count; 
}

/* We assume that userland want to write max two lines. Max size is 34 (two
full lines and two \n). If userland wants to write more, -ENOSPACE is 
returned. After writing cursor is set to home position. Lines shorter than
16 characters are filled with spaces to write full display. It's nessesary
for clearing old content without CLEAR command. CLEAR command causes visible
blinking. */
static ssize_t write_content(struct device* _dev, struct device_attribute* 
   _attr, const char* _buf, size_t _count) {
   struct i2c_client* _client = to_i2c_client(_dev);
   int i, k, c;
   int ret = 0;
   unsigned char char_cnt = 0;
   if (_count > 34) return -ENOSPC;
   if (_count > 0) {
      msleep(1);
      for (i = 0, char_cnt = 0; i < _count; i++) {
         if (_buf[i] == '\n') {
            c = char_cnt;
            for (k = c; k <= 15; k++) {
               ret = hd44780_i2c_send(_client, LCD_MODE_DATA, ' ');
               if (ret < 0) return -EIO;
            }
            ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0xC0);
            if (ret < 0) return -EIO;
            char_cnt = 0;
            continue;
         } else {
            ret = hd44780_i2c_send(_client, LCD_MODE_DATA, _buf[i]);
            if (ret < 0) return -EIO;
         }
         char_cnt++;
      }
      hd44780_i2c_gotoxy(_client, 0, 0);
      return _count;
   } else {
      return -EIO;
   }
   return -EIO;
}

/* Set cursor state to dash on or off. Blink overrides curror setting. */
static ssize_t write_cursor_state(struct device* _dev, struct device_attribute*
   _attr, const char* _buf, size_t _count) {
   struct i2c_client* _client = to_i2c_client(_dev);
   struct hd44780_data* _data = i2c_get_clientdata(_client);
   if (_count < 1) {
      return -EIO;
   } else {
      switch (_buf[0]) {
         case 0:
         case '0':
            _data->cursor_state = 0;
            break;
         default:
            _data->cursor_state = LCD_CURSOR;
            break;
      }
      hd44780_i2c_send(_client, LCD_MODE_CMD, 0x08 | _data->cursor_state 
      | _data->cursor_blink | _data->display_state);
   }
   return _count;
}

/* Set cursor to blink on or off. This setting overrides curror bit state. */
static ssize_t write_cursor_blink(struct device* _dev, struct device_attribute*
   _attr, const char* _buf, size_t _count) {
   struct i2c_client* _client = to_i2c_client(_dev);
   struct hd44780_data* _data = i2c_get_clientdata(_client);
   if (_count < 1) {
      return -EIO;
   } else {
      switch (_buf[0]) {
         case 0:
         case '0':
            _data->cursor_blink = 0;
            break;
         default:
            _data->cursor_blink = LCD_CURSOR_BLINK;
            break;
      }
      hd44780_i2c_send(_client, LCD_MODE_CMD, 0x08 | _data->cursor_state 
      | _data->cursor_blink | _data->display_state);
   }
   return _count;
}

/* Set display on or off */
static ssize_t write_display_state(struct device* _dev,
   struct device_attribute* _attr, const char* _buf, size_t _count) {
   struct i2c_client* _client = to_i2c_client(_dev);
   struct hd44780_data* _data = i2c_get_clientdata(_client);
   if (_count < 1) {
      return -EIO;
   } else {
      switch (_buf[0]) {
         case 0:
         case '0':
            _data->display_state = 0;
            break;
         default:
            _data->display_state = LCD_DISPLAY;
            break;
      }
      hd44780_i2c_send(_client, LCD_MODE_CMD, 0x08 | _data->cursor_state 
      | _data->cursor_blink | _data->display_state);
   }
   return _count;
}

/* Clears display */
static ssize_t write_display_clear(struct device* _dev,
   struct device_attribute* _attr, const char* _buf, size_t _count) {
   struct i2c_client* _client = to_i2c_client(_dev);
   int ret = 0;
   if (_count < 1) {
      return -EIO;
   } else {
      switch (_buf[0]) {
         case 0:
         case '0':
            break;
         default:
            ret = hd44780_i2c_send(_client, LCD_MODE_CMD, 0x1);
            if (ret < 0) return -EIO;
            break;
      }
   }
   return _count;
}

DEVICE_ATTR(backlight, 0220, NULL, write_backlight);
DEVICE_ATTR(content, 0220, NULL , write_content);
DEVICE_ATTR(cursor_state, 0220, NULL, write_cursor_state);
DEVICE_ATTR(cursor_blink, 0220, NULL, write_cursor_blink);
DEVICE_ATTR(display_state, 0200, NULL, write_display_state);
DEVICE_ATTR(display_clear, 0200, NULL, write_display_clear);

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
   ret = device_create_file(dev, &dev_attr_backlight);
   if (ret < 0) goto probe_error;
   ret = device_create_file(dev, &dev_attr_content);
   if (ret < 0) goto probe_error;
   ret = device_create_file(dev, &dev_attr_cursor_state);
   if (ret < 0) goto probe_error;
   ret = device_create_file(dev, &dev_attr_cursor_blink);
   if (ret < 0) goto probe_error;
   ret = device_create_file(dev, &dev_attr_display_state);
   if (ret < 0) goto probe_error;
   ret = device_create_file(dev, &dev_attr_display_clear);
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
   return 0;
}

static void hd44780_i2c_driver_exit(void) {
   i2c_unregister_device(client);
   i2c_del_driver(&hd44780_i2c_driver);

}

module_init(hd44780_i2c_driver_init);
module_exit(hd44780_i2c_driver_exit);
