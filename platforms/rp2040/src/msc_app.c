#include <stdio.h>
#include <inttypes.h>
#include "tusb.h"
#include "pico/time.h"
#include "ff.h"
#include "diskio.h"

static FATFS msc_fatfs_volumes[CFG_TUH_DEVICE_MAX];
static volatile bool msc_volume_busy[CFG_TUH_DEVICE_MAX];
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

    char drive_path[3] = "0:";
    drive_path[0] += dev_addr;
    FRESULT result = f_mount(&msc_fatfs_volumes[dev_addr], drive_path, 1);
    if (result != FR_OK) {
        printf("MSC filesystem mount failed (%d)\r\n", result);
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
    char drive_path[3] = "0:";
    drive_path[0] += dev_addr;
    f_unmount(drive_path);
}

// Give up on a transfer after this long: on the RP2040 host a bulk transfer can
// stay pending forever when it collides with interrupt endpoint polling (HID
// keyboard), which would freeze the caller.
#define MSC_IO_TIMEOUT_US 1000000

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
    return true;
}

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data) {
    (void)cb_data;
    msc_volume_busy[dev_addr] = false;
    return true;
}

DSTATUS disk_status(BYTE pdrv) {
    uint8_t dev_addr = pdrv;
    return tuh_msc_mounted(dev_addr) ? 0 : STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)(pdrv);
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    uint8_t const dev_addr = pdrv;
    uint8_t const lun = 0;
    msc_volume_busy[pdrv] = true;
    if (!tuh_msc_read10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0)) {
        msc_volume_busy[pdrv] = false;
        return RES_ERROR;
    }
    return wait_for_disk_io(pdrv) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    uint8_t const dev_addr = pdrv;
    uint8_t const lun = 0;
    msc_volume_busy[pdrv] = true;
    if (!tuh_msc_write10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0)) {
        msc_volume_busy[pdrv] = false;
        return RES_ERROR;
    }
    return wait_for_disk_io(pdrv) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    uint8_t const dev_addr = pdrv;
    uint8_t const lun = 0;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD *)buff) = (WORD)tuh_msc_get_block_count(dev_addr, lun);
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
