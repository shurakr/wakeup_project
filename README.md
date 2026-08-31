# ESP32-S3 Ultimate PC Controller (TinyUSB HID, ACPI & Media)

[🇺🇸 English](README.md) | [🇷🇺 Русский](README.ru.md)
---

A hardware USB controller powered by **ESP32-S3** for PC power management (Sleep, Wake, Shutdown), media control (Volume, Playback), and network-based input emulation (Keyboard + Mouse). Seamlessly integrates your desktop PC into **Home Assistant** (via native MQTT Auto-Discovery) or any smart home ecosystem.

The highlight of this project is a 100% reliable **hardware wake-up from ACPI S3 sleep state (Remote Wakeup)** with built-in TinyUSB buffer protection against host disconnects/freezes.

---

## 🌟 Key Features

*   **Composite USB HID (4-in-1):** Emulates Keyboard, Mouse, Consumer Control (Media keys), and ACPI System Control over a single native USB link.
*   **Guaranteed Wake-on-USB:** Generates native bus interrupts (`Remote Wakeup`), a mouse click, and an ACPI Wake signal to reliably resume modern PCs and laptops from deep sleep.
*   **Anti-Freeze Protection:** Non-blocking TinyUSB FIFO buffer handling (`tud_hid_ready()`). The board will never crash or lock up if commands are fired while the PC is asleep or disconnected.
*   **Full Media Suite:** Native hardware volume control (`Vol +`, `Vol -`, `Mute`) and track navigation (`Play/Pause`, `Next`, `Prev`) without requiring companion software on the PC.
*   **Home Assistant MQTT Auto-Discovery:** Automatically registers all control buttons, media triggers, text inputs, and feedback sensors in Home Assistant without manual YAML editing.
*   **Modern Web UI:** Two-tab dark dashboard (Control & Settings) with real-time status badges, MQTT connection indicator, and separate network/auth configuration cards.
*   **OTA Updates & Safety:** Web-based OTA firmware flasher (`.bin`) and hardware factory reset (hold BOOT for 5 seconds).

---

## 🛠 Hardware Requirements

1.  **Board:** Any **ESP32-S3** development board.
2.  **USB Port:** Connect via the **Native USB** port of the ESP32-S3 (labeled `USB`, not `UART/COM`).
3.  **PC Port:** Connect to a motherboard or laptop port that provides standby power in sleep mode (often marked with a battery or lightning icon).

---

## ⚙️ Initial Setup

1.  Flash the sketch in Arduino IDE (ESP32 core **3.3.11+**) with:
    *   **USB Mode:** `Hardware CDC and JTAG` (or `USB OTG (TinyUSB)` depending on your board).
2.  On first boot (or if Wi-Fi is missing), connect to the AP:
    *   **SSID:** `ESP32-Setup-AP`
    *   **Password:** `12345678`
3.  Open `http://192.168.4.1` in your browser (Default login: `admin` / `wakeup123`).
4.  Configure your **Wi-Fi** and **MQTT Broker** settings under the `Settings` tab.

---

## 💻 Windows & BIOS Setup (Required for Wake-on-USB)

1.  **BIOS/UEFI:** Ensure *USB Wake Support*, *Always On USB*, or *Power On By USB* is enabled.
2.  **Windows Device Manager:**
    *   Under **Keyboards** $\rightarrow$ Right-click `Logitech Total Keyboard V5.4` (or `HID Keyboard`) $\rightarrow$ **Properties** $\rightarrow$ **Power Management**.
    *   Check **"Allow this device to wake the computer"**.
    *   Repeat the same check under **Mice and other pointing devices**.
3.  *Note:* Always replug the USB cable after flashing so Windows refreshes its HID descriptor table.

---

## 📡 MQTT Interface

### Command Topic: `pc/command`
*   `wake` — Wakes the PC and unlocks the screen (Space + Enter).
*   `sleep` — Puts the PC into ACPI Standby (Sleep).
*   `power` — Sends ACPI Power Down (Shutdown).
*   `enter` — Presses the hardware Enter key.
*   `vol_up` / `vol_down` / `mute` — Hardware volume controls.
*   `play_pause` / `next` / `prev` — Media playback controls.

### Text Input Topic: `pc/type`
Send any text string (e.g., passwords, shell commands) to be typed directly into the active window.

### Status Topic: `pc/status`
Publishes feedback states: `online`, `woken`, `wake_failed`, `sleeping`, `power_down`, `entered`, `typed`, `vol_up`, `vol_down`, `muted`, `play_paused`, `next_track`, `prev_track`.

---

## ⚖️ License
Distributed under the MIT License.