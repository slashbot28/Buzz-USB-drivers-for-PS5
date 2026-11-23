/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "buzz_usb.h"

#include <string.h>

#if defined(__PROSPERO__)
#include <dlfcn.h>

extern void poords4_log(const char *format, ...);

#define BUZZ_USB_EP_IN              0x81u
#define BUZZ_USB_INTERFACE          0
#define BUZZ_USB_SCAN_INTERVAL_MS   500u
#define BUZZ_USB_READ_TIMEOUT_MS    2u
#define BUZZ_USB_CTRL_TIMEOUT_MS    50u
#define BUZZ_USB_MAX_ERRORS         8u

#define SCE_USBD_ERROR_TIMEOUT      (-7)
#define SCE_USBD_REQ_TYPE_CLASS_OUT 0x21u
#define SCE_USBD_REQ_SET_REPORT     0x09u
#define SCE_USBD_REPORT_OUTPUT      0x0200u

typedef struct SceUsbdDeviceHandle SceUsbdDeviceHandle;

typedef struct {
    void *module;
    int (*init)(void);
    void (*exit)(void);
    SceUsbdDeviceHandle *(*open_vid_pid)(uint16_t vendor_id, uint16_t product_id);
    void (*close)(SceUsbdDeviceHandle *dev_handle);
    int (*claim_interface)(SceUsbdDeviceHandle *dev_handle, int interface_number);
    int (*release_interface)(SceUsbdDeviceHandle *dev_handle, int interface_number);
    int (*kernel_driver_active)(SceUsbdDeviceHandle *dev_handle, int interface_number);
    int (*detach_kernel_driver)(SceUsbdDeviceHandle *dev_handle, int interface_number);
    int (*interrupt_transfer)(SceUsbdDeviceHandle *dev_handle, uint8_t endpoint,
                              uint8_t *data, int length, int *transferred,
                              unsigned int timeout);
    int (*control_transfer)(SceUsbdDeviceHandle *dev_handle, uint8_t bmRequestType,
                            uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                            uint8_t *data, uint16_t wLength, unsigned int timeout);
    int initialized;
    int usbd_ready;
    SceUsbdDeviceHandle *handle;
    uint64_t last_scan_ms;
    uint32_t consecutive_errors;
    uint8_t applied_led_mask;
} BuzzUsbdContext;

static BuzzUsbdContext g_usbd = {0};
#endif

static BuzzUsbState g_buzz_state = {0};

int
buzz_usb_parse_report(const uint8_t *report, uint32_t report_len,
                      uint8_t out_handsets[BUZZ_MAX_HANDSETS])
{
    if (!report || !out_handsets || report_len < BUZZ_INPUT_REPORT_LEN)
        return -1;

    uint32_t packed = ((uint32_t)report[2]) |
                      ((uint32_t)report[3] << 8) |
                      ((uint32_t)report[4] << 16);

    for (uint32_t i = 0; i < BUZZ_MAX_HANDSETS; ++i)
        out_handsets[i] = (uint8_t)((packed >> (i * 5u)) & BUZZ_BTN_MASK);

    return 0;
}

uint32_t
buzz_usb_map_buttons(uint8_t buzz_buttons)
{
    uint32_t buttons = 0;
    if (buzz_buttons & BUZZ_BTN_RED)
        buttons |= BUZZ_PAD_BTN_R1;
    if (buzz_buttons & BUZZ_BTN_BLUE)
        buttons |= BUZZ_PAD_BTN_CROSS;
    if (buzz_buttons & BUZZ_BTN_ORANGE)
        buttons |= BUZZ_PAD_BTN_CIRCLE;
    if (buzz_buttons & BUZZ_BTN_GREEN)
        buttons |= BUZZ_PAD_BTN_TRIANGLE;
    if (buzz_buttons & BUZZ_BTN_YELLOW)
        buttons |= BUZZ_PAD_BTN_SQUARE;
    return buttons;
}

void
buzz_usb_build_pad_data(uint8_t buzz_buttons, uint64_t now_ms,
                        uint32_t frame_count, ScePadData *out_pad)
{
    if (!out_pad)
        return;

    memset(out_pad, 0, sizeof(*out_pad));
    out_pad->buttons = buzz_usb_map_buttons(buzz_buttons);
    out_pad->leftStick.x = 128;
    out_pad->leftStick.y = 128;
    out_pad->rightStick.x = 128;
    out_pad->rightStick.y = 128;
    out_pad->quat.w = 1.0f;
    out_pad->connected = 1;
    out_pad->timestamp = now_ms * 1000u;
    out_pad->count = (uint8_t)(frame_count & 0xffu);
}

void
buzz_usb_build_led_report(uint8_t led_mask,
                          uint8_t out_report[BUZZ_LED_REPORT_LEN])
{
    if (!out_report)
        return;

    memset(out_report, 0, BUZZ_LED_REPORT_LEN);
    for (uint32_t i = 0; i < BUZZ_MAX_HANDSETS; ++i) {
        if (led_mask & (1u << i))
            out_report[2u + i] = 0xFFu;
    }
}

#if defined(__PROSPERO__)
static int
buzz_usbd_send_leds(uint8_t led_mask)
{
    if (!g_usbd.handle || !g_usbd.control_transfer)
        return -1;

    uint8_t report[BUZZ_LED_REPORT_LEN];
    buzz_usb_build_led_report(led_mask, report);

    int res = g_usbd.control_transfer(
        g_usbd.handle,
        SCE_USBD_REQ_TYPE_CLASS_OUT,
        SCE_USBD_REQ_SET_REPORT,
        SCE_USBD_REPORT_OUTPUT,
        BUZZ_USB_INTERFACE,
        report,
        (uint16_t)sizeof(report),
        BUZZ_USB_CTRL_TIMEOUT_MS);
    if (res >= 0) {
        g_usbd.applied_led_mask = led_mask;
        g_buzz_state.led_mask = led_mask;
        return 0;
    }
    return -1;
}

static void
buzz_usbd_close_device(void)
{
    if (g_usbd.handle) {
        if (g_usbd.applied_led_mask != 0)
            (void)buzz_usbd_send_leds(0);
        if (g_usbd.release_interface)
            (void)g_usbd.release_interface(g_usbd.handle, BUZZ_USB_INTERFACE);
        if (g_usbd.close)
            g_usbd.close(g_usbd.handle);
        g_usbd.handle = NULL;
    }
    g_usbd.consecutive_errors = 0;
    g_usbd.applied_led_mask = 0;
    g_buzz_state.connected = 0;
    g_buzz_state.product_id = 0;
    g_buzz_state.led_mask = 0;
    memset(g_buzz_state.raw_buttons, 0, sizeof(g_buzz_state.raw_buttons));
}

static int
buzz_usbd_try_open(void)
{
    static const uint16_t pids[] = {
        BUZZ_USB_PID_WIRELESS,
        BUZZ_USB_PID_WIRED,
    };

    if (!g_usbd.usbd_ready || !g_usbd.open_vid_pid || !g_usbd.claim_interface)
        return -1;

    for (uint32_t i = 0; i < (uint32_t)(sizeof(pids) / sizeof(pids[0])); ++i) {
        SceUsbdDeviceHandle *dev = g_usbd.open_vid_pid(BUZZ_USB_VID, pids[i]);
        if (!dev)
            continue;

        if (g_usbd.kernel_driver_active && g_usbd.detach_kernel_driver) {
            if (g_usbd.kernel_driver_active(dev, BUZZ_USB_INTERFACE) == 1)
                (void)g_usbd.detach_kernel_driver(dev, BUZZ_USB_INTERFACE);
        }

        int claim_res = g_usbd.claim_interface(dev, BUZZ_USB_INTERFACE);
        if (claim_res != 0) {
            poords4_log("[PoorDS4] Buzz USB claim_interface failed pid=0x%04x err=%d\n",
                        pids[i], claim_res);
            if (g_usbd.close)
                g_usbd.close(dev);
            continue;
        }

        g_usbd.handle = dev;
        g_usbd.consecutive_errors = 0;
        g_usbd.applied_led_mask = 0;
        g_buzz_state.connected = 1;
        g_buzz_state.product_id = pids[i];
        g_buzz_state.seq++;
        memset(g_buzz_state.raw_buttons, 0, sizeof(g_buzz_state.raw_buttons));
        (void)buzz_usbd_send_leds(0);

        poords4_log("[PoorDS4] Buzz PS3 USB controller connected vid=0x%04x pid=0x%04x\n",
                    BUZZ_USB_VID, pids[i]);
        return 0;
    }

    return -1;
}
#endif

int
buzz_usb_init(void)
{
#if !defined(__PROSPERO__)
    memset(&g_buzz_state, 0, sizeof(g_buzz_state));
    return 0;
#else
    if (g_usbd.initialized)
        return g_usbd.usbd_ready ? 0 : -1;

    memset(&g_usbd, 0, sizeof(g_usbd));
    memset(&g_buzz_state, 0, sizeof(g_buzz_state));
    g_usbd.initialized = 1;

    g_usbd.module = dlopen("libSceUsbd.sprx", RTLD_LAZY);
    if (!g_usbd.module) {
        poords4_log("[PoorDS4] Buzz USB unavailable: dlopen(libSceUsbd.sprx) failed\n");
        return -1;
    }

    g_usbd.init = dlsym(g_usbd.module, "sceUsbdInit");
    g_usbd.exit = dlsym(g_usbd.module, "sceUsbdExit");
    g_usbd.open_vid_pid = dlsym(g_usbd.module, "sceUsbdOpenDeviceWithVidPid");
    g_usbd.close = dlsym(g_usbd.module, "sceUsbdClose");
    g_usbd.claim_interface = dlsym(g_usbd.module, "sceUsbdClaimInterface");
    g_usbd.release_interface = dlsym(g_usbd.module, "sceUsbdReleaseInterface");
    g_usbd.kernel_driver_active = dlsym(g_usbd.module, "sceUsbdKernelDriverActive");
    g_usbd.detach_kernel_driver = dlsym(g_usbd.module, "sceUsbdDetachKernelDriver");
    g_usbd.interrupt_transfer = dlsym(g_usbd.module, "sceUsbdInterruptTransfer");
    g_usbd.control_transfer = dlsym(g_usbd.module, "sceUsbdControlTransfer");

    if (!g_usbd.init || !g_usbd.open_vid_pid || !g_usbd.close ||
        !g_usbd.claim_interface || !g_usbd.interrupt_transfer) {
        poords4_log("[PoorDS4] Buzz USB unavailable: missing libSceUsbd symbols\n");
        dlclose(g_usbd.module);
        g_usbd.module = NULL;
        return -1;
    }

    int init_res = g_usbd.init();
    if (init_res != 0) {
        poords4_log("[PoorDS4] Buzz USB sceUsbdInit failed err=%d\n", init_res);
        dlclose(g_usbd.module);
        g_usbd.module = NULL;
        return -1;
    }

    g_usbd.usbd_ready = 1;
    poords4_log("[PoorDS4] Buzz USB initialized (libSceUsbd)\n");
    return 0;
#endif
}

void
buzz_usb_shutdown(void)
{
#if defined(__PROSPERO__)
    buzz_usbd_close_device();
    if (g_usbd.usbd_ready && g_usbd.exit)
        g_usbd.exit();
    if (g_usbd.module)
        dlclose(g_usbd.module);
    memset(&g_usbd, 0, sizeof(g_usbd));
#endif
    memset(&g_buzz_state, 0, sizeof(g_buzz_state));
}

int
buzz_usb_poll(uint64_t now_ms)
{
#if !defined(__PROSPERO__)
    (void)now_ms;
    return g_buzz_state.connected ? 0 : -1;
#else
    if (!g_usbd.initialized)
        (void)buzz_usb_init();
    if (!g_usbd.usbd_ready)
        return -1;

    if (!g_usbd.handle) {
        if (g_usbd.last_scan_ms == 0 ||
            now_ms >= g_usbd.last_scan_ms + BUZZ_USB_SCAN_INTERVAL_MS) {
            g_usbd.last_scan_ms = now_ms ? now_ms : 1u;
            (void)buzz_usbd_try_open();
        }
        return g_buzz_state.connected ? 0 : -1;
    }

    uint8_t buf[8];
    int transferred = 0;
    memset(buf, 0, sizeof(buf));
    int res = g_usbd.interrupt_transfer(
        g_usbd.handle,
        BUZZ_USB_EP_IN,
        buf,
        (int)sizeof(buf),
        &transferred,
        BUZZ_USB_READ_TIMEOUT_MS);

    if (res == 0 && transferred >= (int)BUZZ_INPUT_REPORT_LEN) {
        uint8_t parsed[BUZZ_MAX_HANDSETS];
        g_usbd.consecutive_errors = 0;
        if (buzz_usb_parse_report(buf, (uint32_t)transferred, parsed) == 0) {
            if (memcmp(g_buzz_state.raw_buttons, parsed, sizeof(parsed)) != 0) {
                memcpy(g_buzz_state.raw_buttons, parsed, sizeof(parsed));
                g_buzz_state.seq++;
            }
            uint8_t led_mask = 0;
            for (uint32_t i = 0; i < BUZZ_MAX_HANDSETS; ++i) {
                if (parsed[i] & BUZZ_BTN_RED)
                    led_mask |= (uint8_t)(1u << i);
            }
            if (led_mask != g_usbd.applied_led_mask)
                (void)buzz_usbd_send_leds(led_mask);
        }
        return 0;
    }

    if (res == SCE_USBD_ERROR_TIMEOUT || (uint32_t)res == 0x80240007u) {
        g_usbd.consecutive_errors = 0;
        return 0;
    }

    g_usbd.consecutive_errors++;
    if (g_usbd.consecutive_errors >= BUZZ_USB_MAX_ERRORS) {
        poords4_log("[PoorDS4] Buzz PS3 USB controller disconnected err=%d\n", res);
        buzz_usbd_close_device();
        g_usbd.last_scan_ms = now_ms;
        return -1;
    }

    return 0;
#endif
}

int
buzz_usb_is_connected(void)
{
    return g_buzz_state.connected ? 1 : 0;
}

uint32_t
buzz_usb_seq(void)
{
    return g_buzz_state.seq;
}

uint16_t
buzz_usb_product_id(void)
{
    return g_buzz_state.product_id;
}

int
buzz_usb_get_handset_pad_data(uint32_t handset_idx, uint64_t now_ms,
                              uint32_t frame_count, ScePadData *out_pad)
{
    if (!g_buzz_state.connected || handset_idx >= BUZZ_MAX_HANDSETS || !out_pad)
        return -1;

    buzz_usb_build_pad_data(g_buzz_state.raw_buttons[handset_idx],
                            now_ms, frame_count, out_pad);
    return 0;
}

int
buzz_usb_set_leds(uint8_t led_mask)
{
    led_mask &= 0x0Fu;
#if defined(__PROSPERO__)
    if (!g_buzz_state.connected || !g_usbd.handle)
        return -1;
    if (led_mask == g_usbd.applied_led_mask)
        return 0;
    return buzz_usbd_send_leds(led_mask);
#else
    g_buzz_state.led_mask = led_mask;
    return g_buzz_state.connected ? 0 : -1;
#endif
}
