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

/*
 * Reading them (qr_scan.c): a camera's frames handed over, their brightness at half
 * the size, to a thread of its own that looks for a code in each (quirc/), so that
 * the preview is not held up. A frame is taken only while the thread is idle.
 */
struct ui_qr_scan;
struct ui_qr_scan *ui_qr_scan_create(void);
void ui_qr_scan_delete(struct ui_qr_scan *s);
/* A UYVY frame, width x height at `stride` bytes a line; false if the thread is busy */
bool ui_qr_scan_uyvy(struct ui_qr_scan *s, const uint8_t *uyvy, int width, int height, int stride);
/* The outcome of the last frame taken, once (false until then): `*found` and the code's
 * text and corners in the frame's pixels, clockwise from the top left as the code is */
bool ui_qr_scan_result(struct ui_qr_scan *s, bool *found, char *text, size_t size, float corners[4][2]);

/* What a code's text is: a web address, a phone number, a Wi-Fi network (ZXing's
 * WIFI:T:WPA;S:name;P:password;H:true;;, unescaped), a contact (a vCard, any version, or
 * a MECARD) or anything else */
enum ui_qr_kind { UI_QR_TEXT, UI_QR_URL, UI_QR_PHONE, UI_QR_WIFI, UI_QR_CONTACT };
struct ui_qr_content {
	enum ui_qr_kind kind;
	char ssid[33], password[65], type[8]; /* WIFI: T is WPA, SAE, WEP, nopass or "" */
	bool hidden;
	char name[65];   /* a contact's: its FN, else "given family"; may be "" */
	char number[32]; /* tel:, a contact's mobile number (else its first; may be "") */
};
void ui_qr_parse(const char *text, struct ui_qr_content *c);

/* A contact as a vCard 3.0 for a QR code: N (the last word the family name), FN, its
 * number as TEL;TYPE=CELL */
void ui_qr_vcard(char *out, size_t size, const char *name, const char *number);

#endif
