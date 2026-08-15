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

## 1a. Power — breadboard prototype (USB-C power bank)

For bench work, skip the 12 V side entirely: feed the S3 from a USB-C power bank and
run the amp off the board's own `5V` pin. Both hang off the breadboard rails.

```
power bank ──USB-C──► S3 board
                        ├─ 5V  ──► red rail  ──► amp Vin
                        ├─ GND ──► blue rail ──► amp GND, pot GND
                        └─ 3V3 ─────────────────► pot VCC
```

The power bank is not the constraint — it supplies 2–3 A. The constraint is
**breadboard contact resistance**. Each spring contact is tens of milliohms and the
current crosses several, so a 1 A bass transient sags the rail and brownout-resets the
ESP32. This is the classic breadboard audio failure.

Two fixes make it a non-issue at sane volume on a small speaker:

- **1000 µF electrolytic across the amp's Vin/GND, in the holes right beside the amp** —
  not out on the rail. It supplies the transient locally so it never crosses the
  contacts. Stripe to GND.
- **A short, thick USB-C cable.** Thin charge-only cables sag the rail by themselves.

If a 4 Ω speaker at full volume still resets the board, that isn't a fault — that is
precisely what the 12 V build below exists to solve.

**Never plug the power bank and the Mac in at the same time.** Both USB-C ports feed
the same 5V rail, so you would be tying two supplies together. Use the Mac's UART port
while testing (you get serial output), then switch to the bank.

Amp `Vin` goes to **5V, never 3V3** — at 3.3 V you get a fraction of the output power
and it clips early.

---

## 1b. Power — final build, 12 V

Use both ports on the buck module. The two USB ports are on the buck module, not the
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

In the *final* build, don't feed the amp from the board's `5V` pin. The MAX98357A pulls
over 1 A on bass peaks, and routing that through the dev board's USB connector and thin
5V trace is the most common cause of this build browning out mid-song. (On the
breadboard it's fine at moderate volume — see §1a.) Give the amp its own port, and
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

**SD** is a mute *and* channel-select pin. Confirmed on your specific board: the SMD
resistor beside the SD pin is marked **`105`** = 1 MΩ. Against the chip's internal
100 kΩ pulldown that puts SD near 0.45 V — amp enabled, output is the (L+R)/2 mono mix.
**Leave SD unconnected.**

(If you ever get silence with everything else right, measure SD against GND: it should
read roughly 0.4–0.5 V. Tying SD straight to 3V3 also un-mutes, but selects *left
channel only*.)

Pin order on this board, left to right: `LRC · BCLK · DIN · GAIN · SD · GND · Vin`.
Note `LRC` and `BCLK` are adjacent but in that order — easy to swap by accident. The
7-pin header needs soldering; the pads ship bare.

On this build it matters less than on the ESP32 one, because the sketch calls
`audio.forceMono(true)` and folds both channels together in software before I2S. Even
in left-only mode you'd hear the full mix.

### Slide pot

Your module is an **HW-233** with two separate 3-pin headers: `OTA/VCC/GND` and
`OTB/VCC/GND`. Use the first, leave the OTB header entirely unconnected.

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
schreibfaul1 **v4.0.0**, straight from master. No fork, but it **does** carry three
small local patches applied by `patches/prefill_patch.py` — see §7d. Re-run that
script after any library reinstall or the sketch will not compile.

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
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default" ~/Documents/coding_experiments/naani_radio/OneButtonRadioS3
```

```bash
arduino-cli upload --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default" -p <PORT> ~/Documents/coding_experiments/naani_radio/OneButtonRadioS3
```

Current build: 1.95 MB, 61% of the 3 MB app partition.

### The station URL

Configured station is **Radio Sharda 90.4 FM, Jammu**:

```
https://radioindia.net/radio/sharda/icecast.audio     (Referer required)
```

**This choice is about throughput, not preference — see §7.** The voscast mirror
below is plain HTTP and needs no Referer, but it stutters permanently; this feed does
not. Both are the same 128 kbps broadcast.

The rest of this section is the history of how the station was tracked down, and why
the voscast URL looked right at first:

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

### Flashing from the Arduino IDE

`File → Open` the `.ino`, then `Tools → Board → esp32 → ESP32S3 Dev Module`, and
set these. The first is not optional - build without OPI PSRAM and the audio
library fails to allocate, which looks exactly like a board with no PSRAM:

| Setting | Value |
|---|---|
| **PSRAM** | **OPI PSRAM** |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB CDC On Boot | Disabled |
| Upload Speed | 921600 |

Port is the **UART** USB-C socket (`/dev/cu.usbserial-*`), not the one marked
`USB`. Serial Monitor at 115200.

Three things that trip people up:

- **"Port busy" on upload** - the Serial Monitor is holding the port. Close it.
- **Errors about `PREFILL_BYTES` or `REFERER`** - a library reinstall wiped the
  patches. Re-run `patches/prefill_patch.py`.
- **Editing `secrets.h` appears to do nothing** - it cannot any more, since
  compiled networks are tried first, but the serial log always prints which
  SSID it is attempting.

### Watching the serial log

`arduino-cli monitor` silently produces nothing when not attached to a TTY, and
`stty` + `cat` loses the baud rate because `cat` reopens the port. Use the bundled
helper instead:

```bash
python3 -m venv ~/.venv-serial && ~/.venv-serial/bin/pip -q install pyserial
```

```bash
~/.venv-serial/bin/python ~/Documents/coding_experiments/naani_radio/OneButtonRadioS3/mon.py 15 --reset
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

---

## 7. The stuttering investigation

Three separate faults, found in this order. Recorded because none of them are
guessable and all three will bite again if the code is changed.

### 7a. Periodic ~1 s stutter — the ADC

Bisecting down to a bare "WiFi + connect + audio.loop()" build and adding pieces back
identified `analogRead()` on the volume pot. **Not because it is slow** — measured at
135 µs average, 223 µs worst, which is 0.3% of the loop. The cost is that
`adc_oneshot_read()` acquires the SAR ADC power domain, RTC/analog circuitry the RF
front end shares. Each read briefly disturbs WiFi.

Fix: poll at 500 ms instead of 50 ms (`POLL_MS`). A rotary encoder would remove the
ADC entirely and is the proper fix if this ever proves marginal; the tradeoff is that
an encoder has no absolute position, so volume would reset on every power cycle.

### 7b. Slider felt dead until the top — the volume curve

The library's default taper is `dB = -112t³ + 172t² - 60`, which puts half travel at
**−31 dB**, about 3% amplitude. Replaced via `setVolumeCurve()` with `33.22·log10(t)`,
i.e. dB proportional to log2(position). Loudness roughly doubles per +10 dB, so half
travel now sounds about half as loud.

### 7c. Sporadic stutter that no amount of buffering fixed — TCP window vs RTT

This was the real one, and it is a hard platform limit.

```
CONFIG_LWIP_TCP_WND_DEFAULT=5760
# CONFIG_LWIP_WND_SCALE is not set        <- cannot grow at runtime
```

TCP throughput is bounded by `window / RTT`. With a fixed 5760-byte window:

| Server | RTT | Max throughput | vs 16.0 KB/s needed |
|---|---|---|---|
| voscast | 358 ms | 16.1 KB/s | **+1%** |
| SomaFM | 322 ms | 17.8 KB/s | +11% |
| radioindia.net | 266 ms | 21.6 KB/s | **+35%** |

At +1% headroom the buffer can never refill. Measured: it drained from 98 KB to
17.7 KB (1.1 s of audio) and stayed there, glitching permanently — that floor is not
equilibrium, it is starvation, because a decoder can only consume what arrives. On
radioindia it climbed to 453 KB (28 s) in 85 seconds and kept going, a measured
surplus of +4.25 KB/s.

A MacBook on the same WiFi is smooth because macOS auto-tunes its window to hundreds
of KB. The ESP32 cannot.

**Consequences for changing station:** any stream must satisfy
`5760 / RTT_seconds > bitrate_bytes_per_sec`, with real margin. Check with
`ping <host>` before committing to a URL. A 64 kbps stream needs only 8 KB/s and would
tolerate RTT up to ~700 ms; a 128 kbps stream needs RTT under ~300 ms.

If you ever need a far-away high-bitrate station, the options are: rebuild the core
with a larger window (PlatformIO with a custom sdkconfig, or ESP-IDF directly), or run
a relay on the LAN so the RTT the ESP32 sees is ~1 ms.

### 7d. Required library patches

`patches/prefill_patch.py` modifies the installed ESP32-audioI2S. **Re-run it after
reinstalling or updating the library**, or the sketch will not compile:

```bash
python3 ~/Documents/coding_experiments/naani_radio/patches/prefill_patch.py
```

It adds three fields to the public `settings` struct:

- `PREFILL_BYTES` — bank this much input before playback starts. Upstream begins
  decoding 1.5 KB in, so a live stream never accumulates a cushion.
- `PREFILL_TIMEOUT_MS` — start anyway if a server does not burst.
- `REFERER` — the radioindia feed is hotlink-protected and 403s without
  `Referer: https://onlineradiofm.in/`. Upstream has no way to set one.

The script is idempotent and fails loudly if a library update moves the code it
anchors on.

---

## 8. Web UI and setup portal

Reachable at **http://naani.local** once the radio has joined a network. If mDNS
doesn't resolve (Windows needs Bonjour; Android historically doesn't do `.local` at
all), the IP is printed to serial at boot and shown on the page itself.

**Measured cost of adding all of this:** 93 KB flash (61% → 64%) and 2.7 KB of static
RAM. Requests serve in ~20 ms. There is no measurable effect on audio.

### Why the async server specifically

`ESPAsyncWebServer` handles requests in the AsyncTCP task, **not** in `loop()`. The
synchronous `WebServer` library needs `server.handleClient()` inside `loop()`, which
would block `audio.loop()` — and `audio.loop()` is what reads the network. That is the
same class of bug as §7a, so the async server is not a preference here, it is the
requirement.

Handlers therefore never do anything slow. Rebooting and stream-switching set a flag
that `loop()` acts on, so the HTTP response is actually delivered before the radio
reboots or drops the connection.

### Normal mode

- **Tone** — bass/mid/treble sliders, −12…+12 dB, applied live and saved, plus
  eight presets (Flat, Speech, Warm, Bright, Bass boost, Small spkr, Late night,
  Loudness). The active preset highlights itself when the sliders match it.
- **Loudness is held constant across tone changes.** Boosting a band raises the
  overall level, so without compensation the EQ doubles as a second volume control.
  The sketch estimates the broadband level change as a perceptual weighted sum
  (`0.30·bass + 0.45·mid + 0.25·treble` — mids weighted highest because the ear is
  most sensitive around 1–4 kHz) and offsets the volume by its negative. The weights
  sum to 1.0, so a uniform +6 dB across all three returns exactly −6 dB and cancels.
  The page shows the compensation currently applied.

  Because the volume curve is `dB = 33.22·log10(t)`, a C dB offset is just scaling
  `t` by `10^(C/33.22)` — the curve itself is untouched. One real limit: a large cut
  cannot be fully made up when the slider is already at maximum, since you cannot
  amplify past full scale. The other direction always works, and a boost pulling the
  volume down is exactly the headroom you need in order not to clip.
- **Buffer** — fill percentage and seconds of audio banked. This is the number that
  matters if audio ever misbehaves again; see §7c
- **Station** — change the stream URL. Applied immediately, no reboot
- Live stream title, IP, RSSI, volume and bitrate, polled every 2 s

### Connection policy — this is an appliance

The radio is for an elderly user with nobody around to reconfigure it, so the
defaults favour self-healing over flexibility.

- **Networks are compiled in** (`secrets.h`), tried in order, and always win. A
  wiped or corrupted NVS still comes up on the right access point. Changing
  network means editing two lines and reflashing - deliberately.
- **A second network is optional** (`WIFI_SSID2`). Leave it empty to disable.
  Useful as a phone-hotspot backup if the main AP dies while nobody is home.
- **Anything saved through the portal is ADDITIVE**, never an override - it is
  tried after the compiled networks. A stale entry there can no longer stop the
  radio reaching its real access point. That override was the trap that made
  editing `secrets.h` appear to do nothing.
- **It retries forever.** Boot no longer gives up after 20 s. A router reboot
  takes 60-90 s, which used to strand the radio in setup mode during an outage
  that would have healed itself.
- **The portal is a last resort**, raised only after 5 minutes offline, and the
  real networks keep being retried underneath it. In an ordinary outage nobody
  ever sees it.
- **A watchdog reboots** if nothing has played for 15 minutes, covering wedged
  states that are not crashes and that nobody is present to power-cycle.
- **Amber means reconnecting** - a calm, expected state. Magenta is now rare.

### Setup portal (AP mode)

If no network can be joined at boot — no saved credentials, or the saved network is
gone — the radio raises an **open** SoftAP called `naani-radio-setup` instead of
sitting dark. The status LED goes **magenta** so this state is visible without a
serial cable.

Join it, open any address, pick a network from the scanned list (or type it), enter
the password, and it saves and reboots.

**The scan happens before the AP is raised, and never again while it is up.** A STA
scan makes the radio hop channels, which knocks the SoftAP off its own channel - the
AP stays visible but refuses clients ("could not be joined"). A STA still retrying
failed credentials does the same. So `startAP()` disconnects STA, scans once, caches
the result, and then goes AP-only; `/scan` just serves that cache.

**It retries the saved network every 30 s while the portal is up**, so a router that
was merely slow to boot recovers on its own rather than stranding the radio in setup
mode forever. The retry is skipped whenever a client is connected to the portal, since
the STA attempt would disrupt the very AP being used. One consequence: if no one is
connected, there is a ~10 s window every 30 s where the AP is briefly busy - if a join
fails, simply try again. The AP is open on purpose: it exists only to
hand over credentials, and a password you'd have to look up defeats the point of a
recovery portal. It is only up while the radio is unconfigured.

### Where settings live

NVS, under the namespace `naani`, via `Preferences`. `secrets.h` remains the
compile-time fallback: NVS wins if set, so a fresh board with credentials compiled in
still works, and anything entered through the portal overrides it thereafter.

Writes happen **only on form submit**, never during playback, which preserves the
"nothing is written to flash at runtime" property that makes cutting power mid-song
safe.

To forget saved credentials and force the portal, erase NVS:

```bash
esptool.py --port /dev/cu.usbserial-0001 erase_flash
```

(That erases the sketch too — reflash afterwards.)
