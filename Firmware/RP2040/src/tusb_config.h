// =============================================================================
// bb_tusb_config.h — Override debugprobe's tusb_config.h
//
// Adds a second vendor interface for BugBuster LA data streaming.
// This file is included INSTEAD of debugprobe's tusb_config.h via -include.
// =============================================================================

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE     OPT_MODE_DEVICE

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_PICO
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

// Class instances
#define CFG_TUD_HID              1
#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           2   // 1 for CMSIS-DAP + 1 for BugBuster LA data

// Buffer sizes
#define CFG_TUD_CDC_RX_BUFSIZE   64
#define CFG_TUD_CDC_TX_BUFSIZE   4096
#define CFG_TUD_VENDOR_RX_BUFSIZE 8192
#define CFG_TUD_VENDOR_TX_BUFSIZE 8192

#ifndef TUD_OPT_RP2040_USB_DEVICE_UFRAME_FIX
#define TUD_OPT_RP2040_USB_DEVICE_UFRAME_FIX 1
#endif

#ifdef __cplusplus
}
#endif

#endif
