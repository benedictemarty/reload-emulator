#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#if CFG_TUSB_MCU == OPT_MCU_LPC43XX || CFG_TUSB_MCU == OPT_MCU_LPC18XX || CFG_TUSB_MCU == OPT_MCU_MIMXRT10XX
  #define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_HOST | OPT_MODE_HIGH_SPEED)
#else
  #define CFG_TUSB_RHPORT0_MODE       OPT_MODE_HOST
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_NONE
#endif

// CFG_TUSB_DEBUG is defined by compiler in DEBUG build
// #define CFG_TUSB_DEBUG           0

// USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
// Tinyusb use follows macros to declare transferring memory so that they can be put
// into those specific section.
// e.g
// - CFG_TUSB_MEM SECTION : __attribute__ (( section(".usb_ram") ))
// - CFG_TUSB_MEM_ALIGN   : __attribute__ ((aligned(4)))

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// CONFIGURATION
//--------------------------------------------------------------------

// Size of buffer to hold descriptors and other data used for enumeration; the
// HID report descriptors of multimedia keys can exceed 256 bytes (TinyUSB then
// ignores the interface), as in ~/Neo6502TeleStrat. A system short of RAM may
// define it to 256 (Telestrat RAM 64 KB), and CFG_TUH_CDC to 1 (USB modem).
#ifndef CFG_TUH_ENUMERATION_BUFSIZE
#define CFG_TUH_ENUMERATION_BUFSIZE 512
#endif

#define CFG_TUH_HUB                 1
#ifndef CFG_TUH_CDC
#define CFG_TUH_CDC                 0
#endif
#define CFG_TUH_HID                 4 // Typical keyboard + mouse device can have 3-4 HID interfaces
#define CFG_TUH_MIDI                0 // There will be at most one MIDIStreaming Interface descriptor
#define CFG_TUH_MSC                 1
#define CFG_TUH_VENDOR              0

// Max device support (excluding hub device)
#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 5 : 1) // Hub typically has 4 ports

//------------- HID -------------//
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

#ifdef __cplusplus
 }
#endif

//------------- CDC (USB modem, when CFG_TUH_CDC) -------------//
#if CFG_TUH_CDC
#ifndef CFG_TUH_CDC_RX_BUFSIZE
#define CFG_TUH_CDC_RX_BUFSIZE      128   // Neo6502TeleStrat: 1200-9600 baud, read every ms
#endif
#ifndef CFG_TUH_CDC_TX_BUFSIZE
#define CFG_TUH_CDC_TX_BUFSIZE      128
#endif
// Line at enumeration: DTR + RTS, 9600 8N1 (default of the PicoWiFiModemUSB)
#ifndef CFG_TUH_CDC_LINE_CONTROL_ON_ENUM
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM 0x03
#endif
#ifndef CFG_TUH_CDC_LINE_CODING_ON_ENUM
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM  { 9600, CDC_LINE_CONDING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }
#endif
#endif

#endif // _TUSB_CONFIG_H_
