# OneButtonRadioS3 — build notes (recommended build)

ESP32-S3-DevKitC-1 v1.0 (N16R8) · MAX98357A · slide pot · 12 V → dual USB · hardware toggle

This is the preferred version. The ESP32-WROOM-32 build in `../OneButtonRadio/` still
works and is kept as a fallback, but this board is a better fit — see §0.

---

## 0. Why this board

The module is marked **ESP32-S3-32E N16R8**: 16 MB flash and **8 MB octal PSRAM**.

PSRAM is the thing that matters for an internet radio. It gives the library a large
stream buffer, which is what absorbs WiFi hiccups — dropouts are the most irritating
failure mode in this build, and this is the direct fix. It also means running
**upstream ESP32-audioI2S v4.0.0**, actively maintained, instead of a one-person fork
branch that exists solely to work around having no PSRAM.

Secondary wins: 16 MB flash (no partition juggling), a separate UART USB-C port so
flashing doesn't fight the audio, and an onboard RGB LED used here as a status light.

Trade-offs, honestly: the header pins need soldering if they aren't already, and three
GPIOs are off-limits (below).

---

## 1. Power — use both ports *on the 12 V module*

Unchanged from the other build. The two USB ports are on the buck module, not the
ESP32.

```
12V PSU (+) ──[ SPST toggle ]──┐
                               ├─ 12V→dual USB module
12V PSU (−) ───────────────────┘     ├── port 1 ─► USB-C cable ─► S3 "UART" port
                                     └── port 2 ─► USB cable with the far
                                                   end cut off:
                                                     red   ─► MAX98357A VIN
                                                     black ─► MAX98357A GND
```

Don't feed the amp from the board's `5V` pin. The MAX98357A pulls over 1 A on bass
peaks, and routing that through the dev board's USB connector and thin 5V trace is the
most common cause of this build browning out mid-song. Give the amp its own port, and
put a **1000 µF electrolytic plus a 0.1 µF ceramic across VIN/GND right at the amp**.

The toggle goes in the **12 V line, before the buck module**, rated for at least
12 V / 2 A DC. Switching 12 V rather than the 5 V output kills the converter's own idle
draw too, so the radio is truly at zero.

**Unplug the 12 V before connecting to your Mac** — the `5V` pin ties to USB VBUS, so
the module will back-feed your computer's USB port.

---

## 2. Wiring

### Forbidden pins on this board

- **GPIO 35, 36, 37** — wired to the octal PSRAM on an N16R8 module. Using them breaks
  PSRAM, which breaks audio. This is the one that catches people, because the pins are
  broken out on the header and look perfectly available.
- **GPIO 19, 20** — native USB D−/D+
- **GPIO 43, 44** — UART TX/RX
- **GPIO 0, 45, 46** — strapping pins, affect boot
- **GPIO 48** — onboard RGB LED (v1.0; moved to GPIO 38 on v1.1 boards)

### MAX98357A

GPIO 4/5/6/7 sit next to each other on the header, so this is one tidy run.

| Amp pin | Goes to | Notes |
|---|---|---|
| VIN | USB port 2, +5V | own port, see above |
| GND | USB port 2, GND | |
| BCLK | **GPIO 5** | |
| LRC | **GPIO 6** | word select |
| DIN | **GPIO 7** | |
| GAIN | leave floating | = 9 dB |
| SD | leave floating | see below |

**GAIN**, if too quiet: 100 kΩ to GND = 12 dB, straight to GND = 15 dB. Start floating.

**SD** is a mute *and* channel-select pin. Most breakouts have a 1 MΩ pullup that,
against the chip's internal 100 kΩ pulldown, sits near 0.45 V — enabled, output is the
(L+R)/2 mono mix. Leave it unconnected.

If you get silence with everything else right, your board lacks that pullup: add
**1 MΩ from SD to VIN**. Tying SD straight to 3V3 also un-mutes but selects *left
channel only*.

On this build it matters less than on the ESP32 one, because the sketch calls
`audio.forceMono(true)` and folds both channels together in software before I2S. Even
in left-only mode you'd hear the full mix.

### Slide pot

Your module is dual-gang: two wipers (OTA, OTB) sharing VCC and GND. Use one, ignore
the other.

| Pot pin | Goes to |
|---|---|
| VCC | **3V3** — *not* 5V |
| GND | GND |
| OTA | **GPIO 4** |
| OTB | leave unconnected |

3V3 matters: the ADC is not 5 V tolerant and the code assumes a 0–3.3 V swing.

GPIO 4 is on ADC1. **On the S3, ADC1 is GPIO 1–10 and ADC2 (GPIO 11–20) stops working
the moment WiFi is on** — if you relocate the pot, stay in 1–10.

### Speaker

To the amp's `+` and `−` terminals. **Do not ground either terminal** — the output is
bridged (BTL), and grounding one side shorts half the output stage. At 5 V you get
~3.2 W into 4 Ω or ~1.8 W into 8 Ω.

---

## 3. Library and the PSRAM flag

Installed at `~/Documents/Arduino/libraries/ESP32-audioI2S` — upstream
schreibfaul1 **v4.0.0**, straight from master. No fork, no patches.

> **The `PSRAM=opi` build flag is mandatory and is not the default.**
>
> The Arduino board option defaults to `PSRAM=disabled`. Build without it and
> `ESP.getPsramSize()` returns 0, the audio library fails to allocate, and it looks
> exactly like a board with no PSRAM. N16R8 is *octal* PSRAM, so it's `opi`, not
> `enabled` (that's QSPI).

The sketch prints its PSRAM figures at boot specifically so this is caught immediately.

---

## 4. Building and flashing

Fill in `secrets.h` with your 2.4 GHz SSID and password first. The S3 has no 5 GHz
radio.

Flash through the **UART** port (the right-hand USB-C, next to the silkscreen label),
not the `USB` one — it uses the onboard serial bridge and needs no mode juggling.

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default" ~/Documents/Arduino/OneButtonRadioS3
```

```bash
arduino-cli upload --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default" -p <PORT> ~/Documents/Arduino/OneButtonRadioS3
```

Current build: 1.95 MB, 61% of the 3 MB app partition.

### The station URL

Configured station is **Radio Sharda 90.4 FM, Jammu**:

```
http://s8.voscast.com:7738/stream
```

128 kbps MP3 over plain HTTP. Three things about how this was chosen:

**Why not `radioindia.net`.** The player on `onlineradiofm.in` uses
`https://radioindia.net/radio/sharda/icecast.audio`. That feed is HTTPS-only (port 80
is closed) *and* hotlink-protected — it returns 403 unless the request carries
`Referer: https://onlineradiofm.in/`. This library builds its own request headers with
no way to inject a custom Referer, so it cannot play that URL. The `api.instant.audio`
endpoint behind `radioindia.org` sits behind a Cloudflare challenge and is likewise a
dead end.

**Why this feed is the same station.** The voscast feed reports a generic
`icy-name: Default Stream`, so the name proves nothing. Verified by recording 30 s of
both feeds simultaneously and cross-correlating the audio: a single sharp peak of
**0.64 at a −11.4 s lag**. Unrelated audio correlates near zero — this is the same
broadcast through a different CDN with ~11 s more buffering. (Note that radio-browser's
only "sharda" hit is *Suno Sharda 90.8 FM*, a different station — don't use it.)

**Why the `/stream` path is load-bearing.** This library hardcodes a Chrome user-agent
at `Audio.cpp:1071` (the VLC one is commented out just below). Given a bare
`http://s8.voscast.com:7738/`, SHOUTcast DNAS v2 concludes a browser is asking and
302-redirects to its web admin page at `/index.html?sid=1`. The library follows the
redirect, receives HTML instead of audio, and reconnect-loops forever — which is
exactly what the first flash did. `/stream` (or the classic `/;`) forces the audio
endpoint. If you ever swap in another SHOUTcast station and it loops on connect, this
is the first thing to check.

A known-good fallback for testing, left commented in the sketch:
`http://ice1.somafm.com/groovesalad-128-mp3`

### Watching the serial log

`arduino-cli monitor` silently produces nothing when not attached to a TTY, and
`stty` + `cat` loses the baud rate because `cat` reopens the port. Use the bundled
helper instead:

```bash
python3 -m venv ~/.venv-serial && ~/.venv-serial/bin/pip -q install pyserial
```

```bash
~/.venv-serial/bin/python ~/Documents/Arduino/OneButtonRadioS3/mon.py 15 --reset
```

`mon.py <seconds> [--reset]` — `--reset` pulses DTR/RTS to force a clean boot so you
catch the startup lines. Edit the `port` variable at the top to match your port.

---

## 5. Status LED

The onboard RGB LED (GPIO 48) reports state, which is useful on a radio with no
display:

| Colour | Meaning |
|---|---|
| dim white | booting |
| amber | connecting to WiFi |
| blue | connecting to the stream |
| dim green | playing |
| red | WiFi or stream failed, will retry |

Set `#define STATUS_LED 0` at the top of the sketch to keep it dark inside the
enclosure.

---

## 6. Behaviour

Flip the toggle on: ~6–8 s to first audio, dominated by WiFi association. Flip it off:
everything dies instantly, stream included — no data flows while the radio is off,
which was the goal. Nothing is written to flash at runtime, so cutting power mid-song
can't corrupt anything.

The slider gives 100 volume steps (the library's default is 22), read at power-on too,
so the radio comes up at whatever level the slider physically sits at instead of
blasting.

If the stream or the router drops, the sketch reconnects on its own with backoff up to
30 s.
