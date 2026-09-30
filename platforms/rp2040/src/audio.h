#ifndef AUDIO_H_FILE
#define AUDIO_H_FILE

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(OLIMEX_NEO6502) || defined(OLIMEX_RP2040PC)
#define _AUDIO_PIN (20)
#else
#define _AUDIO_PIN (8)
#endif

void audio_init(uint8_t audio_pin, uint16_t sample_freq);
void audio_push_sample(const uint8_t sample);

// Output volume (every system): levels 0..HID_VOLUME_MAX (devices/hid_media.h,
// ~3 dB per step, the maximum leaves the samples unchanged) and mute; set by
// the multimedia keys of the keyboard (hid_media_key_down) or by a menu.
void audio_set_volume(uint8_t level, bool muted);
uint8_t audio_volume_level(void);
bool audio_volume_muted(void);
// Incremented at each Volume + / Volume − / Mute key (even at the limits): a
// system shows its volume banner when it changes
uint32_t audio_volume_serial(void);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_H_FILE
