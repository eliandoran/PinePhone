/* Included before every phone-ui source (the Makefile's -include): what LVGL leaves out
 * without its Linux drivers, which phone-ui still names (stubs.c) */
#ifndef PHONE_UI_WASM_H
#define PHONE_UI_WASM_H

#include "lvgl.h"

void lv_evdev_set_rollover(lv_indev_t *indev, bool rollover);

#endif
