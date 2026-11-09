/*
 * A QR code (qr.c): `text` encoded (qrcodegen.c, error correction medium or better if it
 * fits the same size), drawn dark on white with its quiet zone of 4 modules, each module
 * a whole number of pixels, centered in a size x size square. NULL if the text does not
 * fit in a QR code.
 */

#ifndef PHONE_UI_QR_H
#define PHONE_UI_QR_H

#include "ui.h"

#define UI_QR_QUIET 4 /* modules of white around the code */

lv_obj_t *ui_qr_create(lv_obj_t *parent, const char *text, int32_t size);

#endif
