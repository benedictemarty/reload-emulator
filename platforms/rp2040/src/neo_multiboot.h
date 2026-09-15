// neo_multiboot.h — Neo6502 multi-boot support (Neo6502firmware F-80/F-82).
//
// When this image is linked for a flash slot (cmake -DNEO_SLOT_<SYSTEM>=n, see
// platforms/rp2040/CMakeLists.txt) it is started by the `neoboot` selector ; the Pause
// key asks the selector to go back to the default slot (the Neo6502 firmware) :
// scratch[0] = 0 then a watchdog reboot. Harmless when the image is flashed alone
// (it simply restarts itself).
#pragma once
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "class/hid/hid.h"

#define NEO_MULTIBOOT_RETURN_KEY HID_KEY_PAUSE

static inline void neo_multiboot_return(void) {
    watchdog_hw->scratch[0] = 0;
    watchdog_reboot(0, 0, 10);
    while (1) tight_loop_contents();
}
