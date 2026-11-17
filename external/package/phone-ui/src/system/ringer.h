/*
 * Ringtones (ringer.c): tones made here, no files, played on where phone-ui's sound goes at
 * the volume's level. Chosen in Settings > Sound & vibration, which plays them; nothing
 * rings with one yet.
 */

#ifndef PHONE_UI_RINGER_H
#define PHONE_UI_RINGER_H

#include <stdbool.h>
#include <stdint.h>

unsigned ui_ringtone_count(void);
const char *ui_ringtone_name(unsigned i); /* in English: _() where shown */
uint32_t ui_ringtone_ms(unsigned i);      /* how long it is */
unsigned ui_ringtone(void);               /* the one chosen (the "ringtone" setting) */
void ui_ringtone_set(unsigned i);

/* Plays one once, instead of the one playing; without a sound device free, nothing */
void ui_ringer_play(unsigned i);
void ui_ringer_stop(void);
/* From ui_ringer_play() until its thread let go of the sound device */
bool ui_ringer_playing(void);

#endif
