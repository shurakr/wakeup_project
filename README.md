# ESP32-S3 Ultimate Controller (PC HID, ACPI, Media & Samsung TV)

[🇺🇸 English](README.md) | [🇷🇺 Русский](README.ru.md)
---

A high-performance hardware controller powered by **ESP32-S3** for PC power management (Sleep, Wake, Shutdown), multimedia control, Samsung Smart TV control, and network-based input emulation (Keyboard + Mouse). Seamlessly integrates your PC and TV into **Home Assistant** via native MQTT Auto-Discovery or standalone web access.

The core highlight of this project is a 100% reliable **hardware wake-up from ACPI S3 sleep state (Remote Wakeup)** with built-in TinyUSB buffer protection against host disconnects and freezes.

---

## 🌟 Key Features (v1.2)

*   **Composite USB HID (4-in-1):** Emulates Keyboard, Mouse, Consumer Control (Media keys), and ACPI System Control over a single native USB link.
*   **Guaranteed Wake-on-USB:** Generates native bus interrupts (`Remote Wakeup`), a mouse click, and an ACPI Wake signal to reliably resume modern PCs and laptops from deep sleep.
*   **Samsung Smart TV Integration:** Dedicated virtual remote control interface (D-Pad navigation, OK, Back, Home, 123 Menu, Color buttons A/B/C/D, Channels).
*   **Host Power Detection:** Real-time hardware tracking of the USB host power state (`ON`/`OFF` binary sensor in Home Assistant and Web UI via `tud_mounted`).
*   **Anti-Freeze Protection:** Non-blocking TinyUSB FIFO buffer handling (`tud_hid_ready()`) ensures the controller never crashes when sending commands to a sleeping or disconnected device.
*   **Home Assistant MQTT Auto-Discovery:** Automatically creates buttons, inputs, and state sensors (Status, Host Power, Last Action) without manual YAML setup.
*   **Dynamic Hardware & LED Support:** Automatic runtime pin detection for on-board WS2812 RGB LED (GPIO21 on Waveshare Zero, GPIO48 on DevKitC-1 N16R8) with intuitive state color coding:
    *   🟠 **Orange:** Booting / Reconnecting Wi-Fi.
    *   🔵 **Blue:** Connected to Wi-Fi, connecting to MQTT.
    *   🟢 **Green:** Fully online and synchronized.
    *   🔴 **Red:** Access Point (Setup) Mode or Factory Reset.
*   **Modern Web UI:** Three-tab dark dashboard (PC Control, Samsung TV, Settings) with live status indicators (IP, Wi-Fi, MQTT, Host Power).
*   **PlatformIO Ready:** Pre-configured environments for 4MB/QSPI and 16MB/OPI boards.
*   **OTA & Recovery:** Web-based OTA firmware updates (`.bin`) and hardware factory reset (hold BOOT for 5 seconds).

---

## 📸 Screenshots

| PC Control | Samsung TV | Settings | Home Assistant |
| :---: | :---: | :---: | :---: |
| ![Control](Control.png) | ![SamsungTV](SamsungTV.png) | ![Settings](Settings.png) | ![Home Assistant](HA.png) |

---

## 🛠 Hardware Compatibility

Supported boards:
*   **Waveshare ESP32-S3-Zero** (4MB Flash, 2MB QSPI PSRAM, RGB on GPIO21).
*   **ESP32-S3-DevKitC-1 / N16R8 Clones** (16MB Flash, 8MB OPI PSRAM, RGB on GPIO48).
    *   *Note for N16R8 boards:* Solder the on-board `RGB` jumper pad if the addressable LED does not illuminate out of the box.
*   **Connection:** Connect via the **Native USB** port of the ESP32-S3 (labeled `USB`, not `UART/COM`).

---

## 🚀 Building & Flashing

### Recommended: PlatformIO (VS Code)
1. Open the project root in VS Code with the PlatformIO extension installed.
2. Select your target environment in the bottom status bar:
   * `env:waveshare_zero` (for 4MB/2MB boards)
   * `env:esp32s3_n16r8` (for 16MB/8MB boards)
3. Click **Build** (`✓`) or **Upload** (`➔`).

### Alternative: Arduino IDE (v2.x)
Ensure **ESP32 Core 3.x+** is installed:
* **Board:** `ESP32S3 Dev Module`
* **USB Mode:** `USB-OTG (TinyUSB)`
* **USB CDC On Boot:** `Disabled`
* **Partition Scheme:** `Minimal SPIFFS (1.9MB APP)` for 4MB boards, or `16M Flash (3MB APP)` for N16R8.
* **PSRAM:** `QSPI` (Zero) or `OPI` (N16R8).

---

## ⚙️ Initial Configuration

1. On initial boot (or if saved Wi-Fi is unreachable), the controller opens a setup hotspot:
   * **SSID:** `ESP32-Setup-AP`
   * **Password:** `12345678`
2. Open `http://192.168.4.1` (Default credentials: `admin` / `wakeup123`).
3. Fill in your **Wi-Fi** credentials and **MQTT Broker** settings under the `Settings` tab.

---

## 📡 MQTT Interface

### Command Topic: `pc/command`
*   **PC Power:** `wake`, `sleep`, `power`, `enter`.
*   **Media:** `vol_up`, `vol_down`, `mute`, `play_pause`, `next`, `prev`.
*   **Samsung TV:** `tv_power`, `tv_123`, `tv_home`, `tv_back`, `tv_play`, `tv_ch_up`, `tv_ch_down`, `tv_up`, `tv_down`, `tv_left`, `tv_right`, `tv_enter`, `tv_a`, `tv_b`, `tv_c`, `tv_d`.

### Text Typing: `pc/type`
Publishes ASCII characters directly to the active host window (ideal for sending shell commands or passwords).

### Feedback Topics
*   `pc/status` — Controller health (`online`, `offline` via MQTT LWT).
*   `pc/host_power` — Host power state (`ON` / `OFF`).
*   `pc/last_action` — Echoes the last executed command string.

---

## 💻 Windows & BIOS Wake Configuration

1.  **BIOS/UEFI:** Ensure *USB Wake Support*, *Always On USB*, or *Power On By USB* is enabled.
2.  **Windows Device Manager:**
    *   Under **Keyboards** $\rightarrow$ `Logitech Total Keyboard V1.2` $\rightarrow$ **Properties** $\rightarrow$ **Power Management**.
    *   Check **"Allow this device to wake the computer"**.
    *   Repeat under **Mice and other pointing devices**.

---

## ⚖️ License
Distributed under the MIT License.