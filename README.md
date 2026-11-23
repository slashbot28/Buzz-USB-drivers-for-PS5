# Buzz! USB Drivers for PS5

A standalone **PlayStation 5** USB driver payload (`buzz_bridge.elf`) and C client library for **Sony PlayStation Buzz! USB quiz controllers** (both wireless `054C:1000` and wired `054C:0002`).

Because PS5 homebrew apps run inside a sandbox without direct access to `libSceUsbd.sprx`, `buzz_bridge.elf` runs outside the app sandbox via `elfldr` (port `9021`), reads all 4 Buzz! handsets over USB, controls handset LEDs, and forwards button states to PS5 homebrew apps over localhost UDP.

---

## Repository Structure

```text
Buzz-USB-drivers-for-PS5/
├── buzz_bridge.elf          # Prebuilt PS5 payload (send to elfldr port 9021)
├── send_buzz_bridge.ps1     # PowerShell helper script to deploy buzz_bridge.elf to PS5
├── buzz_bridge/
│   ├── buzz_bridge.c        # Standalone PS5 libSceUsbd bridge payload source
│   └── Makefile             # Builds and sends buzz_bridge.elf
└── client/
    ├── buzz_usb.h           # C helper header for HID report parsing, LED reports & ScePadData mapping
    ├── buzz_usb.c           # C helper implementation
    └── pad_types.h          # ScePadData struct definition
```

---

## Supported Hardware

| Controller | Vendor ID | Product ID |
| :--- | :--- | :--- |
| Sony Buzz! Wireless USB Receiver (4 handsets) | `0x054C` | `0x1000` |
| Sony Buzz! Wired USB Controllers (4 handsets) | `0x054C` | `0x0002` |

---

## Quick Start (Prebuilt Payload)

1. Plug your **Buzz! USB receiver** (or wired Buzz! controllers) into a USB port on your PS5.
2. Make sure `elfldr` is running on your PS5 (default port `9021`).
3. Send `buzz_bridge.elf` to your PS5:

   **Windows (PowerShell):**
   ```powershell
   .\send_buzz_bridge.ps1 -PS5_HOST 192.168.1.20
   ```

   **Linux / macOS:**
   ```bash
   nc -w 3 192.168.1.20 9021 < buzz_bridge.elf
   ```

4. A PS5 notification appears: **`Buzz Bridge gestart - sluit de Buzz! USB-ontvanger aan`**.
5. Sending `buzz_bridge.elf` again automatically stops and replaces any previous running instance.
6. Runtime logs are written to `/data/buzzbridge/log.txt` (and PS5 `klog`).

---

## Network Protocol

| Transport | Endpoint | Direction | Description |
| :--- | :--- | :--- | :--- |
| **UDP** | `127.0.0.1:45555` | Bridge → App | Button state packets (`"BUZZ"` + 4 handset masks + connected flag + seq) |
| **UDP** | `127.0.0.1:45556` | App → Bridge | Commands: `"LED"` + 1-byte bitmask (`0x01`..`0x08`), `"QUIT"`, or `"DBG"` + text |
| **TCP** | `0.0.0.0:45557` | Bridge → PC | Live text debug stream (`nc <PS5_IP> 45557`) |

### Button Bitmasks (per handset)

| Bitmask | Buzz! Button | Default `ScePadData` Mapping (`client/buzz_usb.c`) |
| :--- | :--- | :--- |
| `0x01` | Red Buzzer | `R1` (`0x00000800`) |
| `0x02` | Yellow | `Square` (`0x00008000`) |
| `0x04` | Green | `Triangle` (`0x00001000`) |
| `0x08` | Orange | `Circle` (`0x00002000`) |
| `0x10` | Blue | `Cross` (`0x00004000`) |

### Live Debugging from PC

Connect to TCP port `45557` to watch real-time button presses, LED state, and raw 5-byte HID reports:

```bash
nc <PS5_IP> 45557
```

---

## Building from Source

Requires an installed [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```bash
cd buzz_bridge
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

To build and send directly to the PS5:

```bash
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk PS5_HOST=192.168.1.20 send
```

---

## License

**GPL-3.0-or-later**
