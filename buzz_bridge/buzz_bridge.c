/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * buzz_bridge.elf - Standalone PS5 payload voor PS3 Buzz! USB controllers.
 *
 * Draait via elfldr (poort 9021) buiten de app-sandbox, zodat het wel
 * toegang heeft tot libSceUsbd. Leest de Buzz! USB-ontvanger
 * (054C:1000 draadloos / 054C:0002 bedraad) uit en stuurt de knopstatus van
 * alle 4 handsets via UDP naar 127.0.0.1:45555 (de quiz-app luistert daar).
 *
 * Poorten:
 *   UDP 127.0.0.1:45555  bridge -> app   "BUZZ" b0 b1 b2 b3 connected seq
 *   UDP 127.0.0.1:45556  app -> bridge   "LED" mask   |  "QUIT"
 *   TCP 0.0.0.0:45557    live debug-tekst voor de PC (nc <ps5-ip> 45557)
 *
 * Opnieuw versturen van de payload vervangt automatisch de oude instantie.
 * Log: /data/buzzbridge/log.txt  (+ klog)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BRIDGE_VERSION      "1.0"

#define BUZZ_VID            0x054Cu
#define BUZZ_PID_WIRELESS   0x1000u
#define BUZZ_PID_WIRED      0x0002u
#define BUZZ_EP_IN          0x81u
#define BUZZ_IFACE          0
#define BUZZ_HANDSETS       4

#define BTN_RED             0x01u
#define BTN_YELLOW          0x02u
#define BTN_GREEN           0x04u
#define BTN_ORANGE          0x08u
#define BTN_BLUE            0x10u

#define PORT_STATE          45555
#define PORT_CMD            45556
#define PORT_DEBUG          45557

#define READ_TIMEOUT_MS     8u
#define BTN_AUTO_RELEASE_MS 140u
#define SCAN_INTERVAL_MS    1000u
#define HEARTBEAT_MS        25u
#define LED_IDLE_OFF_MS     3000u
#define MAX_READ_ERRORS     8u
#define MAX_DEBUG_CLIENTS   4

#define USBD_TIMEOUT_A      (-7)
#define USBD_TIMEOUT_B      ((int)0x80240007u)

/* ------------------------------------------------------------------ system */

int sceKernelDebugOutText(int channel, const char *text);

typedef struct {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);

static FILE *g_logf = NULL;

static void
blog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char line[560];
    snprintf(line, sizeof(line), "[BuzzBridge] %s\n", buf);
    sceKernelDebugOutText(0, line);
    printf("%s", line);
    if (g_logf) {
        fputs(line, g_logf);
        fflush(g_logf);
    }
}

static void
notify(const char *msg)
{
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static uint64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* -------------------------------------------------------------- libSceUsbd */

typedef struct SceUsbdDeviceHandle SceUsbdDeviceHandle;

static struct {
    void *module;
    int  (*init)(void);
    void (*exit)(void);
    SceUsbdDeviceHandle *(*open_vid_pid)(uint16_t vid, uint16_t pid);
    void (*close)(SceUsbdDeviceHandle *h);
    int  (*claim_interface)(SceUsbdDeviceHandle *h, int iface);
    int  (*release_interface)(SceUsbdDeviceHandle *h, int iface);
    int  (*kernel_driver_active)(SceUsbdDeviceHandle *h, int iface);
    int  (*detach_kernel_driver)(SceUsbdDeviceHandle *h, int iface);
    int  (*interrupt_transfer)(SceUsbdDeviceHandle *h, uint8_t ep, uint8_t *data,
                               int len, int *transferred, unsigned int timeout);
    int  (*control_transfer)(SceUsbdDeviceHandle *h, uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, uint8_t *data,
                             uint16_t wLength, unsigned int timeout);
    int ready;
} U;

static int
usbd_load(void)
{
    const char *names[] = {"libSceUsbd.sprx", "/system/common/lib/libSceUsbd.sprx", NULL};
    for (int i = 0; names[i] && !U.module; i++)
        U.module = dlopen(names[i], RTLD_LAZY);
    if (!U.module) {
        blog("FOUT: dlopen(libSceUsbd.sprx) mislukt");
        return -1;
    }

#define SYM(field, name) *(void **)(&U.field) = dlsym(U.module, name)
    SYM(init, "sceUsbdInit");
    SYM(exit, "sceUsbdExit");
    SYM(open_vid_pid, "sceUsbdOpenDeviceWithVidPid");
    SYM(close, "sceUsbdClose");
    SYM(claim_interface, "sceUsbdClaimInterface");
    SYM(release_interface, "sceUsbdReleaseInterface");
    SYM(kernel_driver_active, "sceUsbdKernelDriverActive");
    SYM(detach_kernel_driver, "sceUsbdDetachKernelDriver");
    SYM(interrupt_transfer, "sceUsbdInterruptTransfer");
    SYM(control_transfer, "sceUsbdControlTransfer");
#undef SYM

    if (!U.init || !U.open_vid_pid || !U.close || !U.claim_interface || !U.interrupt_transfer) {
        blog("FOUT: libSceUsbd symbolen ontbreken");
        return -1;
    }
    int r = U.init();
    if (r != 0) {
        blog("FOUT: sceUsbdInit = 0x%08x", (unsigned)r);
        return -1;
    }
    U.ready = 1;
    blog("libSceUsbd geladen en geinitialiseerd");
    return 0;
}

/* --------------------------------------------------------------- Buzz! USB */

static struct {
    SceUsbdDeviceHandle *dev;
    uint16_t pid;
    uint8_t  buttons[BUZZ_HANDSETS];
    uint8_t  last_raw[8];
    int      last_got;
    int      last_r;
    uint8_t  led_mask;
    uint8_t  led_applied;
    uint32_t seq;
    uint32_t errors;
    uint64_t last_scan;
    uint64_t last_led_cmd;
    uint64_t last_pkt_ms;
} B;

static int
buzz_send_leds(uint8_t mask)
{
    if (!B.dev || !U.control_transfer)
        return -1;
    uint8_t rep[8] = {0};
    for (int i = 0; i < BUZZ_HANDSETS; i++)
        if (mask & (1u << i))
            rep[2 + i] = 0xFF;
    int r = U.control_transfer(B.dev, 0x21 /* class|out|iface */, 0x09 /* SET_REPORT */,
                               0x0200 /* output report 0 */, BUZZ_IFACE, rep, sizeof(rep), 50);
    if (r >= 0)
        B.led_applied = mask;
    return r;
}

static void
buzz_close(const char *why)
{
    if (!B.dev)
        return;
    buzz_send_leds(0);
    if (U.release_interface)
        U.release_interface(B.dev, BUZZ_IFACE);
    U.close(B.dev);
    B.dev = NULL;
    B.errors = 0;
    memset(B.buttons, 0, sizeof(B.buttons));
    B.seq++;
    blog("Buzz! ontvanger losgekoppeld (%s)", why);
    notify("Buzz! controller losgekoppeld");
}

static void
buzz_try_open(void)
{
    static const uint16_t pids[] = {BUZZ_PID_WIRELESS, BUZZ_PID_WIRED};
    for (unsigned i = 0; i < sizeof(pids) / sizeof(pids[0]); i++) {
        SceUsbdDeviceHandle *dev = U.open_vid_pid(BUZZ_VID, pids[i]);
        if (!dev)
            continue;
        if (U.kernel_driver_active && U.detach_kernel_driver &&
            U.kernel_driver_active(dev, BUZZ_IFACE) == 1)
            U.detach_kernel_driver(dev, BUZZ_IFACE);
        int r = U.claim_interface(dev, BUZZ_IFACE);
        if (r != 0) {
            blog("claim_interface mislukt pid=0x%04x err=0x%08x (draait PoorDS4/buzzPS3.elf nog?)",
                 pids[i], (unsigned)r);
            U.close(dev);
            continue;
        }
        B.dev = dev;
        B.pid = pids[i];
        B.errors = 0;
        memset(B.buttons, 0, sizeof(B.buttons));
        B.seq++;
        blog("Buzz! ontvanger verbonden %04x:%04x", BUZZ_VID, pids[i]);
        notify("Buzz! controller verbonden");
        if (U.control_transfer)
            (void)U.control_transfer(dev, 0x21, 0x0A /* SET_IDLE */, 0x0A00 /* 40ms */,
                                     BUZZ_IFACE, NULL, 0, 50);
        /* korte LED-flits als bevestiging */
        buzz_send_leds(0x0F);
        usleep(250000);
        buzz_send_leds(0);
        return;
    }
}

/* Leest alle beschikbare USB reports tot timeout. Return 1 = knoppen gewijzigd. */
static void send_state(void);

static int
buzz_read(uint64_t t)
{
    int any_changed = 0;
    for (int iter = 0; iter < 8; iter++) {
        uint8_t buf[8] = {0};
        int got = 0;
        int r = U.interrupt_transfer(B.dev, BUZZ_EP_IN, buf, sizeof(buf), &got, READ_TIMEOUT_MS);

        if (r == 0 && got > 0) {
            memcpy(B.last_raw, buf, sizeof(B.last_raw));
            B.last_got = got;
            B.last_r = r;
        }
        if (r == 0 && got >= 5) {
            B.errors = 0;
            uint32_t packed = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8) | ((uint32_t)buf[4] << 16);
            uint8_t parsed[BUZZ_HANDSETS];
            for (int i = 0; i < BUZZ_HANDSETS; i++)
                parsed[i] = (uint8_t)((packed >> (i * 5)) & 0x1Fu);
            uint8_t any_btn = (uint8_t)(parsed[0] | parsed[1] | parsed[2] | parsed[3]);
            if (memcmp(parsed, B.buttons, sizeof(parsed)) != 0) {
                memcpy(B.buttons, parsed, sizeof(parsed));
                B.seq++;
                any_changed = 1;
                B.last_pkt_ms = t;
                blog("usb btn seq=%u got=%d raw=%02x %02x %02x %02x %02x",
                     B.seq, got, buf[0], buf[1], buf[2], buf[3], buf[4]);
            } else if (any_btn != 0 && (t - B.last_pkt_ms >= 50u)) {
                /* Zelfde knop opnieuw ontvangen zonder tussentijds release-rapport: eerst 0 sturen */
                memset(B.buttons, 0, sizeof(B.buttons));
                send_state();
                memcpy(B.buttons, parsed, sizeof(parsed));
                B.seq++;
                any_changed = 1;
                B.last_pkt_ms = t;
                blog("usb repeat seq=%u raw=%02x %02x %02x %02x %02x",
                     B.seq, buf[0], buf[1], buf[2], buf[3], buf[4]);
            } else if (any_btn != 0) {
                B.last_pkt_ms = t;
            }
            continue;
        }
        if (r == 0 && got > 0) {
            B.errors = 0;
            blog("usb short got=%d raw=%02x %02x %02x %02x %02x",
                 got, buf[0], buf[1], buf[2], buf[3], buf[4]);
            continue;
        }
        if (r == USBD_TIMEOUT_A || r == USBD_TIMEOUT_B || (r == 0 && got == 0)) {
            B.errors = 0;
            break;
        }
        B.last_r = r;
        blog("usb read err r=0x%08x got=%d", (unsigned)r, got);
        if (++B.errors >= MAX_READ_ERRORS) {
            char why[48];
            snprintf(why, sizeof(why), "leesfout 0x%08x", (unsigned)r);
            buzz_close(why);
        }
        break;
    }
    if ((B.buttons[0] | B.buttons[1] | B.buttons[2] | B.buttons[3]) != 0 &&
        (t - B.last_pkt_ms >= BTN_AUTO_RELEASE_MS)) {
        memset(B.buttons, 0, sizeof(B.buttons));
        B.seq++;
        any_changed = 1;
        blog("usb auto-release seq=%u", B.seq);
    }
    return any_changed;
}

/* ----------------------------------------------------------------- sockets */

static int g_tx = -1, g_cmd = -1, g_dbg = -1;
static int g_clients[MAX_DEBUG_CLIENTS] = {-1, -1, -1, -1};

static void
set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static struct sockaddr_in
loopback(int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    return a;
}

static int
bind_cmd_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a = loopback(PORT_CMD);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static int
sockets_open(void)
{
    g_tx = socket(AF_INET, SOCK_DGRAM, 0);

    g_cmd = bind_cmd_socket();
    if (g_cmd < 0) {
        /* Er draait al een bridge: vraag die om te stoppen en neem over. */
        blog("Oude bridge gevonden, die wordt vervangen...");
        struct sockaddr_in a = loopback(PORT_CMD);
        sendto(g_tx, "QUIT", 4, 0, (struct sockaddr *)&a, sizeof(a));
        for (int i = 0; i < 30 && g_cmd < 0; i++) {
            usleep(100000);
            g_cmd = bind_cmd_socket();
        }
        if (g_cmd < 0) {
            blog("FOUT: kan UDP %d niet binden", PORT_CMD);
            return -1;
        }
    }

    g_dbg = socket(AF_INET, SOCK_STREAM, 0);
    if (g_dbg >= 0) {
        int one = 1;
        setsockopt(g_dbg, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(PORT_DEBUG);
        if (bind(g_dbg, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(g_dbg, 4) != 0) {
            close(g_dbg);
            g_dbg = -1;
        } else {
            set_nonblock(g_dbg);
        }
    }
    return 0;
}

static void
send_state(void)
{
    if (g_tx < 0)
        return;
    uint8_t pkt[12] = {
        'B', 'U', 'Z', 'Z',
        B.buttons[0], B.buttons[1], B.buttons[2], B.buttons[3],
        (uint8_t)(B.dev ? 1 : 0),
        (uint8_t)(B.seq & 0xFF), (uint8_t)((B.seq >> 8) & 0xFF), (uint8_t)((B.seq >> 16) & 0xFF),
    };
    struct sockaddr_in a = loopback(PORT_STATE);
    sendto(g_tx, pkt, sizeof(pkt), 0, (struct sockaddr *)&a, sizeof(a));
}

static void
debug_broadcast(const char *text)
{
    for (int i = 0; i < MAX_DEBUG_CLIENTS; i++) {
        if (g_clients[i] < 0)
            continue;
        if (send(g_clients[i], text, strlen(text), MSG_NOSIGNAL) < 0) {
            close(g_clients[i]);
            g_clients[i] = -1;
        }
    }
}

static void
format_state(char *out, size_t n)
{
    char hs[BUZZ_HANDSETS][8];
    for (int i = 0; i < BUZZ_HANDSETS; i++) {
        uint8_t b = B.buttons[i];
        snprintf(hs[i], sizeof(hs[i]), "%c%c%c%c%c",
                 (b & BTN_RED) ? 'R' : '-', (b & BTN_BLUE) ? 'B' : '-',
                 (b & BTN_ORANGE) ? 'O' : '-', (b & BTN_GREEN) ? 'G' : '-',
                 (b & BTN_YELLOW) ? 'Y' : '-');
    }
    snprintf(out, n, "usb=%d seq=%u P1=%s P2=%s P3=%s P4=%s leds=0x%X raw=%02x%02x%02x%02x%02x(%d,0x%x)\n",
             B.dev ? 1 : 0, B.seq, hs[0], hs[1], hs[2], hs[3], B.led_applied,
             B.last_raw[0], B.last_raw[1], B.last_raw[2], B.last_raw[3], B.last_raw[4],
             B.last_got, (unsigned)B.last_r);
}

static void
debug_accept(void)
{
    if (g_dbg < 0)
        return;
    int fd = accept(g_dbg, NULL, NULL);
    if (fd < 0)
        return;
    for (int i = 0; i < MAX_DEBUG_CLIENTS; i++) {
        if (g_clients[i] < 0) {
            g_clients[i] = fd;
            char hello[160], st[128];
            format_state(st, sizeof(st));
            snprintf(hello, sizeof(hello), "BuzzBridge v%s pid=0x%04x\n%s",
                     BRIDGE_VERSION, B.pid, st);
            send(fd, hello, strlen(hello), MSG_NOSIGNAL);
            return;
        }
    }
    close(fd);
}

/* Return 1 als de bridge moet stoppen. */
static int
poll_commands(uint64_t t)
{
    uint8_t pkt[256];
    for (;;) {
        ssize_t n = recvfrom(g_cmd, pkt, sizeof(pkt) - 1, 0, NULL, NULL);
        if (n <= 0)
            break;
        if (n >= 4 && memcmp(pkt, "QUIT", 4) == 0)
            return 1;
        if (n >= 4 && memcmp(pkt, "LED", 3) == 0) {
            B.led_mask = pkt[3] & 0x0F;
            B.last_led_cmd = t;
        }
        if (n > 3 && memcmp(pkt, "DBG", 3) == 0) {
            pkt[n] = '\0';
            blog("app: %s", (const char *)(pkt + 3));
        }
    }
    /* App weg (geen LED-updates meer) -> LEDs uit. */
    if (B.last_led_cmd && t - B.last_led_cmd > LED_IDLE_OFF_MS)
        B.led_mask = 0;
    if (B.dev && B.led_mask != B.led_applied)
        buzz_send_leds(B.led_mask);
    return 0;
}

/* -------------------------------------------------------------------- main */

int
main(void)
{
    signal(SIGPIPE, SIG_IGN);
    mkdir("/data/buzzbridge", 0777);
    g_logf = fopen("/data/buzzbridge/log.txt", "w");

    blog("BuzzBridge v%s start", BRIDGE_VERSION);

    if (sockets_open() != 0) {
        notify("Buzz Bridge: fout bij openen sockets");
        return 1;
    }
    if (usbd_load() != 0) {
        notify("Buzz Bridge: libSceUsbd niet beschikbaar");
        return 1;
    }
    notify("Buzz Bridge gestart - sluit de Buzz! USB-ontvanger aan");

    uint64_t last_hb = 0;
    char line[160];

    for (;;) {
        uint64_t t = now_ms();

        if (!B.dev && (B.last_scan == 0 || t - B.last_scan >= SCAN_INTERVAL_MS)) {
            B.last_scan = t;
            buzz_try_open();
        }

        int changed = 0;
        if (B.dev)
            changed = buzz_read(t);
        else
            usleep(10000);

        if (changed || t - last_hb >= HEARTBEAT_MS) {
            send_state();
            last_hb = t;
        }
        if (changed) {
            format_state(line, sizeof(line));
            debug_broadcast(line);
        }

        debug_accept();
        if (poll_commands(t))
            break;
    }

    blog("QUIT ontvangen, bridge stopt");
    buzz_close("stop");
    if (U.exit)
        U.exit();
    for (int i = 0; i < MAX_DEBUG_CLIENTS; i++)
        if (g_clients[i] >= 0)
            close(g_clients[i]);
    if (g_dbg >= 0) close(g_dbg);
    if (g_cmd >= 0) close(g_cmd);
    if (g_tx >= 0) close(g_tx);
    if (g_logf) fclose(g_logf);
    return 0;
}
