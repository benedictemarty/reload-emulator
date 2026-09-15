// hid_app_bbc.c
//
// TinyUSB HID host callbacks for the BBC Micro: raw HID key codes and
// modifier keys are reported (positional keyboard), no ASCII translation.
//
// ## zlib/libpng license
//
// Copyright (c) 2026 bmarty
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the
// use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//     1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software in a
//     product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//     2. Altered source versions must be plainly marked as such, and must not
//     be misrepresented as being the original software.
//     3. This notice may not be removed or altered from any source
//     distribution.

#include <string.h>
#include "tusb.h"
#include "class/hid/hid.h"

static hid_keyboard_report_t prev_report = {0, 0, {0}};

// Implemented by bbc.c: HID usage code (0x04 = A ...), modifiers as HID_KEY_SHIFT_LEFT etc.
extern void hid_raw_key_down(uint8_t keycode);
extern void hid_raw_key_up(uint8_t keycode);

static inline bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }
    return false;
}

// Keys (and modifier bits) present in r1 but not in r2
static void process_kbd_report(hid_keyboard_report_t const* r1, hid_keyboard_report_t const* r2,
                               void (*cb)(uint8_t keycode)) {
    static const uint8_t modifier_keys[8] = {HID_KEY_CONTROL_LEFT, HID_KEY_SHIFT_LEFT, HID_KEY_ALT_LEFT,
                                             HID_KEY_GUI_LEFT,     HID_KEY_CONTROL_RIGHT, HID_KEY_SHIFT_RIGHT,
                                             HID_KEY_ALT_RIGHT,    HID_KEY_GUI_RIGHT};
    for (int b = 0; b < 8; b++) {
        if ((r1->modifier & (1 << b)) && !(r2->modifier & (1 << b))) {
            cb(modifier_keys[b]);
        }
    }
    for (int i = 0; i < 6; i++) {
        if (r1->keycode[i] && !find_key_in_report(r2, r1->keycode[i])) {
            cb(r1->keycode[i]);
        }
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    (void)desc_len;
    (void)desc_report;
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    (void)len;
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        const hid_keyboard_report_t* r = (const hid_keyboard_report_t*)report;
        process_kbd_report(r, &prev_report, hid_raw_key_down);
        process_kbd_report(&prev_report, r, hid_raw_key_up);
        memcpy(&prev_report, r, sizeof(prev_report));
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    (void)instance;
}
