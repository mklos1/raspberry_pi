#ifndef _LCD_HDPCF_H_
#define _LCD_HDPCF_H_

#include <linux/ioctl.h>

#define IOCTL_MAGIC                   178
#define LCD_UPDATE_STATE              0
#define LCD_UPDATE_DISPLAY            1
#define LCD_CLEAR                     2
#define LCD_HOME                      3
#define IOCTL_LCD_UPDATE_STATE        _IOWR(IOCTL_MAGIC, LCD_UPDATE_STATE, unsigned long)
#define IOCTL_LCD_UPDATE_DISPLAY      _IOWR(IOCTL_MAGIC, LCD_UPDATE_DISPLAY, unsigned long)
#define IOCTL_LCD_CLEAR               _IOWR(IOCTL_MAGIC, LCD_CLEAR, unsigned long)
#define IOCTL_LCD_HOME                _IOWR(IOCTL_MAGIC, LCD_HOME, unsigned long)

struct lcd_hdpcf { 
   unsigned char buffer[2][17];
   bool cursor_state;
   bool cursor_blink;
   bool display_state;
   bool backlight_state; 
};



#endif
