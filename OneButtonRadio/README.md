# OneButtonRadio — build notes (fallback build)

ESP32 DevKit (WROOM-32) · MAX98357A · slide pot · 12 V → dual USB · hardware toggle

> **See `../OneButtonRadioS3/` first.** The ESP32-S3-DevKitC-1 (N16R8) has 8 MB of
> PSRAM, which lets it run the maintained upstream library and gives it a large stream
> buffer to ride out WiFi hiccups. This WROOM-32 build works and is fully tested up to
> the network step, but it is the second choice.

---

## 1. Power — use both ports *on the 12 V module*

The two USB ports are on the buck module, not on the ESP32. The DevKit has one
micro-USB and that's correct.

```
12V PSU (+) ──[ SPST toggle ]──┐
                               ├─ 12V→dual USB module
12V PSU (−) ───────────────────┘     ├── port 1 ─► normal USB cable ─► ESP32 micro-USB
                                     └── port 2 ─► USB cable with the far
                                                   end cut off:
                                                     red   ─► MAX98357A VIN
                                                     black ─► MAX98357A GND
```

Port 2's cable is a sacrificial one — cut the connector off the far end, strip the
red and black conductors, ignore the data lines (green/white).

Why bother: don't feed the amp from the ESP32's VIN pin. The MAX98357A pulls over
1 A on bass peaks, and routing that through the DevKit's micro-USB connector and
thin 5V trace is the single most common cause of this build browning out and
rebooting mid-song. Give the amp its own port.

The two ports share ground inside the module, so I2S signalling is already
referenced correctly. No extra ground wire needed, but one won't hurt.

**Add a 1000 µF electrolytic (6.3 V or higher) plus a 0.1 µF ceramic across VIN/GND
right at the amp**, leads as short as you can manage. This is not optional at 3 W.

The toggle goes in the **12 V line, before the buck module**, and needs to be rated
for at least 12 V / 2 A DC. Switching 12 V rather than the 5 V output means the buck
converter's own idle draw goes away too — the radio is truly at zero.

### While programming: unplug the 12 V

On a plain DevKitC, the USB VBUS and the 5V pin are directly connected. With both
your computer's USB *and* the 12 V module's USB attached, the module back-feeds your
computer's USB port. Pull the 12 V barrel jack before you plug into the Mac.

---

## 2. Wiring

Pin labels below match the silkscreen on the 30-pin DOIT DevKit v1. All four signal
pins are broken out: `D25`, `D26` and `D34` on the left header, `D22` on the right.

### MAX98357A

| Amp pin | Goes to | Notes |
|---|---|---|
| VIN | USB port B, +5V | own port, see above |
| GND | USB port B, GND | |
| BCLK | ESP32 **GPIO 26** | |
| LRC | ESP32 **GPIO 25** | word select |
| DIN | ESP32 **GPIO 22** | |
| GAIN | leave floating | = 9 dB |
| SD | leave floating | see below |

**GAIN**, if the speaker is too quiet: 100 kΩ to GND = 12 dB, wire straight to GND
= 15 dB. Floating is 9 dB. Start floating.

**SD** is a mute *and* channel-select pin, and it trips people up. Most breakouts
(Adafruit and the clones that copy it) have a 1 MΩ pullup which, against the chip's
internal 100 kΩ pulldown, sits at ~0.45 V — amp enabled, output is the (L+R)/2 mono
mix. That is exactly what you want with one speaker, so leave it unconnected.

If you get silence with everything else correct, your board lacks that pullup. Add
**1 MΩ from SD to VIN** to reproduce the mono mix. Tying SD straight to 3V3 will also
un-mute it, but that selects **left channel only** and you'll lose whatever is in the
right channel — avoid that shortcut.

### Slide pot

Your module is dual-gang: two wipers (OTA, OTB) sharing VCC and GND. Use one gang and
ignore the other.

| Pot pin | Goes to |
|---|---|
| VCC | ESP32 **3V3** — *not* 5V |
| GND | ESP32 GND |
| OTA | ESP32 **GPIO 34** |
| OTB | leave unconnected |

3V3 matters: GPIO34's ADC is not 5 V tolerant, and the code assumes a 0–3.3 V swing.

GPIO34 is on ADC1. **ADC2 pins (0, 2, 4, 12–15, 25–27) stop working the moment WiFi
is on** — if you relocate the pot, stay on GPIO 32–39.

### Speaker

Wire it to the amp's `+` and `−` screw terminals. **Do not ground either terminal** —
the MAX98357A output is bridged (BTL), and grounding one side shorts half the output
stage. At 5 V you get ~3.2 W into 4 Ω or ~1.8 W into 8 Ω, well past what a small
enclosure speaker wants anyway.

---

## 3. Library — already installed

Measured on your actual board rather than assumed:

```
chip model : ESP32-D0WD-V3 rev 300
cores      : 2
flash size : 4194304 bytes
PSRAM size : 0 bytes
max alloc  : 110580 bytes     <-- largest possible single allocation
```

Zero PSRAM, and the biggest contiguous block available is 110 KB. Upstream
ESP32-audioI2S v3.x asks for one 704 KB buffer at boot, so it cannot run here — it
would fail instantly. **Do not install ESP32-audioI2S from the Library Manager.**

What is installed instead, at
`~/Documents/Arduino/libraries/ESP32-audioI2S-nopsram`:

> PLSousa's **`v2.0.6-gcc14-nopsram`** branch — v2.0.6 plus 4 GCC 14 compile fixes and
> 5 stability patches backported from v3.2.1. Note this is a *branch*, not the fork's
> `master` (master is synced to upstream 3.4.4 and still needs PSRAM).

Its header is renamed, so the sketch uses `#include "Audio_nopsram.h"`. The class,
`setPinout`, `setVolume` (0–21), `isRunning`, and all the `audio_*` callbacks are
identical to the 2.x API.

To reinstall it from scratch:

```bash
curl -sL -o /tmp/nopsram.zip https://github.com/PLSousa/ESP32-audioI2S/archive/refs/heads/v2.0.6-gcc14-nopsram.zip && unzip -o -q /tmp/nopsram.zip -d /tmp && rm -rf ~/Documents/Arduino/libraries/ESP32-audioI2S-nopsram && cp -R /tmp/ESP32-audioI2S-2.0.6-gcc14-nopsram ~/Documents/Arduino/libraries/ESP32-audioI2S-nopsram
```

Fallback if it proves flaky in extended listening: **ESP8266Audio** by Earle
Philhower (targets ESP32 despite the name, no PSRAM, very well proven for 128 kbps
MP3 radio). Different API, so the sketch would need porting.

---

## 4. Building and flashing

Fill in `secrets.h` with your 2.4 GHz SSID and password first. Then:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" ~/Documents/Arduino/OneButtonRadio
```

```bash
arduino-cli upload --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=115200" -p /dev/cu.usbserial-56840042211 ~/Documents/Arduino/OneButtonRadio
```

Two flags that are doing real work:

- **`PartitionScheme=huge_app`** — the sketch is 1.19 MB. The default partition gives
  the app only 1.31 MB, so it fits at 90% with almost nothing spare. `huge_app` raises
  the ceiling to 3 MB, landing at 37%.
- **`UploadSpeed=115200`** — this board's USB-serial bridge fails at the default
  921600 (`A fatal error occurred: The chip stopped responding`, right after "Changing
  baud rate"). 115200 is reliable; a flash takes ~15 s.

`STATION_URL` in the sketch must be a **direct stream URL**, not a station's web page.
Prefer plain `http://`; TLS costs ~40 KB of heap and is a frequent cause of "plays for
five seconds, then dies." The default is SomaFM Groove Salad, 128 kbps MP3 — prove the
hardware on that before switching to your station.

### Watching the serial log

`arduino-cli monitor` silently produces nothing when it isn't attached to a TTY, and
`stty` + `cat` loses the baud rate because `cat` reopens the port. Use the pyserial
helper in the scratchpad instead — `mon.py <seconds> [--reset]`, where `--reset`
pulses DTR/RTS to force a clean boot so you catch the startup lines.

---

## 5. Behaviour

Flip the toggle on: ~6–8 s to first audio (WiFi association dominates this). Flip it
off: everything dies instantly, including the stream — no data flows while the radio
is off, which was the goal. Nothing is written to flash at runtime, so yanking power
mid-song can't corrupt anything.

The slider sets volume continuously while playing. It's read at power-on too, so the
radio comes up at whatever level the slider is physically at instead of blasting.

If the stream or the router drops, the sketch reconnects on its own with backoff up
to 30 s.

---

## Sources

- [schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S)
- [Solution for ESP32 without PSRAM (discussion #1262)](https://github.com/schreibfaul1/ESP32-audioI2S/discussions/1262)
- [ESP32 without PSRAM crashing (issue #1039)](https://github.com/schreibfaul1/ESP32-audioI2S/issues/1039)
