#ifndef USB_MODE_H
#define USB_MODE_H

#include <stdbool.h>

/* File transfer over USB (MTP) through the board's script: usb_mode.c */
void usb_mode_init(void);                 /* ui_shade_init(): an earlier run's mode */
bool usb_mode_available(void);            /* the board has the script */
bool usb_computer(void);                  /* a computer configured the USB gadget, not a charger */
bool usb_file_transfer(void);             /* on, or being turned on */
bool usb_mode_busy(void);                 /* the script switching it */
void usb_file_transfer_set(bool on);

#endif
