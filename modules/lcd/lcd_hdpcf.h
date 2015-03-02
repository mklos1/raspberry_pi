#ifndef _LCD_HDPCF_H_
#define _LCD_HDPCF_H_

#include <linux/ioctl.h>

#define IOCTL_MAGIC                   178
#define LCD_UPDATE_STATE              0
#define LCD_UPDATE_DISPLAY            1
#define LCD_CLEAR                     2
#define LCD_HOME                      3
#define LCD_SHIFT                     4
#define LCD_SET_CHAR                  5

/* Updates LCD state without changing content. It takes pointer to lcd_hdpcf
structure. */
#define IOCTL_LCD_UPDATE_STATE        _IOWR(IOCTL_MAGIC, LCD_UPDATE_STATE, unsigned long)

/* Updates LCD content from lcd_hdpcf stuct buffer. Pointer to lcd_hdpcf
structure as argument */
#define IOCTL_LCD_UPDATE_DISPLAY      _IOWR(IOCTL_MAGIC, LCD_UPDATE_DISPLAY, unsigned long)

/* Clears LCD. Any value as argument */
#define IOCTL_LCD_CLEAR               _IOWR(IOCTL_MAGIC, LCD_CLEAR, unsigned long)

/* Sets cursor to home position. Any value as argument */
#define IOCTL_LCD_HOME                _IOWR(IOCTL_MAGIC, LCD_HOME, unsigned long)

/* Shifts LCD content. "0" as argument - shift left, any other shift right */
#define IOCTL_LCD_SHIFT               _IOWR(IOCTL_MAGIC, LCD_SHIFT, unsigned long)

/* Sets LCD CGRAM. 8 custom charaters are avaliable. Pointer to user_char
structure as argument. Address range from 0x0 to 0x7. */
#define IOCTL_LCD_SET_CHAR            _IOWR(IOCTL_MAGIC, LCD_SET_CHAR, unsigned long)

struct lcd_hdpcf {
   unsigned char buffer[2][17];
   bool cursor_state;
   bool cursor_blink;
   bool display_state;
   bool backlight_state;
};

struct user_char {
   unsigned char chr[8];
   unsigned char address;
};



#endif
