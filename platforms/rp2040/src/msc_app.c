#include <stdio.h>
#include <inttypes.h>
#include "tusb.h"
#include "host/hcd.h"   // hcd_edpt_clear_stall (Bulk-Only reset recovery)
#include "pico/time.h"
#include "ff.h"
#include "diskio.h"

// FatFs drive (pdrv) = slot, given to a USB drive when it is mounted (its USB
// address, 1 to CFG_TUH_DEVICE_MAX, is kept in msc_slot_addr; indexing the tables
// by the address overflowed them with 5 devices behind the hub). MSC_VOLUMES = 1:
// one FATFS only (588 bytes each), taken by the first drive mounted, whatever its
// address (as Neo6502TeleStrat).
#ifndef MSC_VOLUMES
#define MSC_VOLUMES (CFG_TUH_DEVICE_MAX < FF_VOLUMES ? CFG_TUH_DEVICE_MAX : FF_VOLUMES)
#endif
#define MSC_SLOTS MSC_VOLUMES
static FATFS msc_fatfs_volumes[MSC_SLOTS];
static volatile bool msc_volume_busy[MSC_SLOTS];
static volatile uint8_t msc_io_status[MSC_SLOTS];   // CSW status of the last transfer (0: passed)
static uint8_t msc_slot_addr[MSC_SLOTS];   // USB address of the drive in each slot, 0: free

static int msc_slot(uint8_t dev_addr) {
    for (int i = 0; dev_addr && i < MSC_SLOTS; i++) {
        if (msc_slot_addr[i] == dev_addr) return i;
    }
    return -1;
}

static int msc_slot_take(uint8_t dev_addr) {
    int slot = msc_slot(dev_addr);
    for (int i = 0; slot < 0 && i < MSC_SLOTS; i++) {
        if (!msc_slot_addr[i]) {
            msc_slot_addr[i] = dev_addr;
            slot = i;
        }
    }
    return slot;
}
static scsi_inquiry_resp_t msc_inquiry_resp;

bool msc_inquiry_complete = false;

// The volume is mounted from the main loop (msc_poll), not inside this TinyUSB
// callback: f_mount reads sectors, which runs tuh_task again from within it
static volatile uint8_t msc_pending_mount;   // dev_addr + 1, 0 = none

bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data) {
    if (cb_data->csw->status != 0) {
        printf("MSC SCSI inquiry failed\r\n");
        return false;
    }

    uint32_t block_count = tuh_msc_get_block_count(dev_addr, cb_data->cbw->lun);
    uint32_t block_size = tuh_msc_get_block_size(dev_addr, cb_data->cbw->lun);
    uint32_t size = block_size ? block_count / ((1024 * 1024) / block_size) : 0;

    printf("MSC %luMB %.8s %.16s rev %.4s\r\n", (unsigned long)size, msc_inquiry_resp.vendor_id, msc_inquiry_resp.product_id,
           msc_inquiry_resp.product_rev);
    msc_pending_mount = (uint8_t)(dev_addr + 1);
    return true;
}

// Call from the main loop (outside tuh_task): mounts a volume that answered INQUIRY
void msc_poll(void) {
    if (!msc_pending_mount) return;
    uint8_t dev_addr = (uint8_t)(msc_pending_mount - 1);
    msc_pending_mount = 0;
    if (!tuh_msc_mounted(dev_addr)) return;   // Pulled out since its INQUIRY: no slot for a dead address
    const int slot = msc_slot_take(dev_addr);
    if (slot < 0) {
        printf("MSC: no free volume for USB address %u (%d volumes)\r\n", dev_addr, MSC_SLOTS);
        return;
    }

    char drive_path[3] = "0:";
    drive_path[0] += slot;
    FRESULT result = f_mount(&msc_fatfs_volumes[slot], drive_path, 1);
    if (result != FR_OK) {
        printf("MSC filesystem mount failed (%d)\r\n", result);
        msc_slot_addr[slot] = 0;   // Slot given back: the drive may be plugged in again
        return;
    }

    char s[2];
    if (FR_OK != f_getcwd(s, 2)) {
        f_chdrive(drive_path);
        f_chdir("/");
    }

    msc_inquiry_complete = true;
}

void tuh_msc_mount_cb(uint8_t dev_addr) {
    uint8_t const lun = 0;
    printf("MSC mounted, inquiring\r\n");
    tuh_msc_inquiry(dev_addr, lun, &msc_inquiry_resp, inquiry_complete_cb, 0);
}

void tuh_msc_umount_cb(uint8_t dev_addr) {
    if (msc_pending_mount == (uint8_t)(dev_addr + 1)) msc_pending_mount = 0;   // Pulled out before msc_poll
    const int slot = msc_slot(dev_addr);
    if (slot < 0) return;
    char drive_path[3] = "0:";
    drive_path[0] += slot;
    f_unmount(drive_path);
    msc_slot_addr[slot] = 0;
    bool any = false;
    for (int i = 0; i < MSC_SLOTS; i++) any = any || msc_slot_addr[i];
    msc_inquiry_complete = any;
}

// Give up on a transfer after this long: on the RP2040 host a bulk transfer can
// stay pending forever when it collides with interrupt endpoint polling (HID
// keyboard), which would freeze the caller.
#define MSC_IO_TIMEOUT_US 1000000

// True if the transfer completed and the drive reported success (CSW status 0):
// a refused write (locked card, bad sector) must not pass for a written one
static bool wait_for_disk_io(BYTE pdrv) {
    uint32_t start = time_us_32();
    while (msc_volume_busy[pdrv]) {
        tuh_task();
        if (time_us_32() - start > MSC_IO_TIMEOUT_US) {
            msc_volume_busy[pdrv] = false;
            printf("MSC transfer timeout\r\n");
            return false;
        }
    }
    if (msc_io_status[pdrv] != 0) {
        printf("MSC transfer failed (CSW status %u)\r\n", msc_io_status[pdrv]);
        return false;
    }
    return true;
}

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data) {
    const int slot = msc_slot(dev_addr);
    if (slot >= 0) {
        msc_io_status[slot] = (cb_data && cb_data->csw) ? cb_data->csw->status : 0xFF;
        msc_volume_busy[slot] = false;
    }
    return true;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv >= MSC_SLOTS || !msc_slot_addr[pdrv]) return STA_NOINIT;
    uint8_t dev_addr = msc_slot_addr[pdrv];
    return tuh_msc_mounted(dev_addr) ? 0 : STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)(pdrv);
    return 0;
}

/*-- Bulk-Only reset recovery (BOT 1.0, 5.3.4) ----------------------------------
 * A transfer failed (TinyUSB : data PID out of sequence, patches/tinyusb-data-seq.sh) or
 * timed out : the drive may still be in the middle of the command. Command aborted, Bulk-
 * Only Mass Storage Reset, CLEAR_FEATURE(ENDPOINT_HALT) on both bulk endpoints (data toggles
 * back to DATA0 on both sides), then the read or write is tried again */
// Needs the TinyUSB patch (tuh_msc_bot_info, tuh_msc_abort, hcd_edpt_clear_stall) : with an
// unpatched TinyUSB (another project's copy), a single try as before
#ifdef TUH_MSC_NEO6502_RECOVERY
#define MSC_IO_TRIES 3
#else
#define MSC_IO_TRIES 1
#endif
volatile uint32_t msc_recoveries, msc_io_errors;   // Counters (bench over SWD)

#ifdef TUH_MSC_NEO6502_RECOVERY

static volatile bool msc_ctrl_done;
static void msc_ctrl_cb(tuh_xfer_t *xfer) { (void)xfer; msc_ctrl_done = true; }

static bool msc_ctrl(uint8_t daddr, uint8_t bm_request_type, uint8_t request, uint16_t value, uint16_t index) {
    static tusb_control_request_t req;   // Read by TinyUSB until the transfer completes
    req.bmRequestType = bm_request_type;
    req.bRequest = request;
    req.wValue = value;
    req.wIndex = index;
    req.wLength = 0;
    tuh_xfer_t xfer = {.daddr = daddr, .ep_addr = 0, .setup = &req, .buffer = NULL, .complete_cb = msc_ctrl_cb, .user_data = 0};
    msc_ctrl_done = false;
    if (!tuh_control_xfer(&xfer)) return false;
    const uint32_t start = time_us_32();
    while (!msc_ctrl_done) {
        tuh_task();
        if (time_us_32() - start > MSC_IO_TIMEOUT_US) return false;
    }
    return true;
}

static void msc_reset_recovery(uint8_t daddr) {
    uint8_t itf, ep_in, ep_out;
    if (!tuh_msc_bot_info(daddr, &itf, &ep_in, &ep_out)) return;
    msc_recoveries++;
    tuh_msc_abort(daddr);
    msc_ctrl(daddr, 0x21, 0xFF, 0, itf);      // Class request : Bulk-Only Mass Storage Reset
    msc_ctrl(daddr, 0x02, 0x01, 0, ep_in);    // CLEAR_FEATURE(ENDPOINT_HALT), bulk IN
    hcd_edpt_clear_stall(0, daddr, ep_in);
    msc_ctrl(daddr, 0x02, 0x01, 0, ep_out);   // CLEAR_FEATURE(ENDPOINT_HALT), bulk OUT
    hcd_edpt_clear_stall(0, daddr, ep_out);
}
#else
static void msc_reset_recovery(uint8_t daddr) { (void)daddr; }
#endif

static DRESULT disk_rw(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count, bool write) {
    if (pdrv >= MSC_SLOTS || !msc_slot_addr[pdrv]) return RES_PARERR;
    uint8_t const dev_addr = msc_slot_addr[pdrv];
    uint8_t const lun = 0;
    for (int tries = 0; tries < MSC_IO_TRIES; tries++) {
        if (tries) msc_reset_recovery(dev_addr);
        if (!tuh_msc_mounted(dev_addr)) return RES_NOTRDY;   // Pulled out
        msc_volume_busy[pdrv] = true;
        msc_io_status[pdrv] = 0xFF;
        const bool started = write ? tuh_msc_write10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0)
                                   : tuh_msc_read10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0);
        if (!started) {
            msc_volume_busy[pdrv] = false;
            msc_io_errors++;
            continue;
        }
        if (wait_for_disk_io(pdrv)) return RES_OK;
        msc_io_errors++;
    }
    return RES_ERROR;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) { return disk_rw(pdrv, buff, sector, count, false); }

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    return disk_rw(pdrv, (BYTE *)buff, sector, count, true);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv >= MSC_SLOTS || !msc_slot_addr[pdrv]) return RES_PARERR;
    uint8_t const dev_addr = msc_slot_addr[pdrv];
    uint8_t const lun = 0;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD *)buff) = (DWORD)tuh_msc_get_block_count(dev_addr, lun);   // Was cut to 16 bits (keys > 32 MB)
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD *)buff) = (WORD)tuh_msc_get_block_size(dev_addr, lun);
            return RES_OK;
        case GET_BLOCK_SIZE:
            *((DWORD *)buff) = 1;  // 1 sector
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
