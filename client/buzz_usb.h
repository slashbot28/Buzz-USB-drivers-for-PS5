/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include "pad_types.h"

/* Sony PlayStation Buzz! USB Controller VID/PIDs */
#define BUZZ_USB_VID               0x054Cu
#define BUZZ_USB_PID_WIRED         0x0002u
#define BUZZ_USB_PID_WIRELESS      0x1000u

#define BUZZ_MAX_HANDSETS          4u
#define BUZZ_INPUT_REPORT_LEN      5u
#define BUZZ_LED_REPORT_LEN        8u

/* 5-bit per-handset button masks in Buzz! HID input reports */
#define BUZZ_BTN_RED               0x01u
#define BUZZ_BTN_YELLOW            0x02u
#define BUZZ_BTN_GREEN             0x04u
#define BUZZ_BTN_ORANGE            0x08u
#define BUZZ_BTN_BLUE              0x10u
#define BUZZ_BTN_MASK              0x1Fu

/* ScePadData.buttons bitmasks */
#define BUZZ_PAD_BTN_R1            0x00000800u
#define BUZZ_PAD_BTN_TRIANGLE      0x00001000u
#define BUZZ_PAD_BTN_CIRCLE        0x00002000u
#define BUZZ_PAD_BTN_CROSS         0x00004000u
#define BUZZ_PAD_BTN_SQUARE        0x00008000u

typedef struct {
    uint8_t raw_buttons[BUZZ_MAX_HANDSETS];
    uint8_t led_mask;
    uint8_t connected;
    uint16_t product_id;
    uint32_t seq;
} BuzzUsbState;

/* Pure report/mapping helpers (host-testable without PS5 USB hardware). */
int buzz_usb_parse_report(const uint8_t *report, uint32_t report_len,
                          uint8_t out_handsets[BUZZ_MAX_HANDSETS]);
uint32_t buzz_usb_map_buttons(uint8_t buzz_buttons);
void buzz_usb_build_pad_data(uint8_t buzz_buttons, uint64_t now_ms,
                             uint32_t frame_count, ScePadData *out_pad);
void buzz_usb_build_led_report(uint8_t led_mask,
                               uint8_t out_report[BUZZ_LED_REPORT_LEN]);

/* Runtime USB lifecycle and polling (PS5 libSceUsbd.sprx on __PROSPERO__). */
int buzz_usb_init(void);
void buzz_usb_shutdown(void);
int buzz_usb_poll(uint64_t now_ms);
int buzz_usb_is_connected(void);
uint32_t buzz_usb_seq(void);
uint16_t buzz_usb_product_id(void);
int buzz_usb_get_handset_pad_data(uint32_t handset_idx, uint64_t now_ms,
                                  uint32_t frame_count, ScePadData *out_pad);
int buzz_usb_set_leds(uint8_t led_mask);
