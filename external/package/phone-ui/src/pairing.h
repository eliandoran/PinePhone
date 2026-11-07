/* Computers allowed SSH access over USB (pairing.c) */

#ifndef PAIRING_H
#define PAIRING_H

#include <stdbool.h>

void pairing_init(void); /* ui_init(): listens for them asking */

/* The computers allowed: their keys in authorized_keys */
struct pairing_computer {
	char name[128]; /* the key's comment, "" if none */
	char fp[64];    /* "SHA256:...", as ssh-keygen -l */
	char b64[4096]; /* the key */
	bool image;     /* the image's key, added again at each boot */
};
struct pairing_computer *pairing_computers(int *count); /* malloc'd, NULL if none */
bool pairing_revoke(const char *b64); /* its lines removed; false if it could not */

#endif
