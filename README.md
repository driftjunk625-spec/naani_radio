# naani_radio

A deliberately dumb single-station internet radio. One toggle switch, one volume
slider, no display, no menus, no app. Flip it on and Radio Sharda 90.4 FM plays;
flip it off and the power is physically cut — no WiFi, no stream, no standby.

## Layout

| Path | What it is |
|---|---|
| `OneButtonRadioS3/` | **The build.** ESP32-S3-DevKitC-1 (N16R8) + MAX98357A. Start here. |
| `OneButtonRadio/` | Fallback for a plain ESP32-WROOM-32. Works, but no PSRAM. |
| `I2SToneTest/` | Hardware diagnostic. Synthesised tones straight to I2S — no WiFi, no decoding. Isolates amp, speaker and wiring from everything upstream. |
| `RadioMinimal/` | Bisection build kept from the stuttering investigation. WiFi, connect, `audio.loop()` and little else. |
| `patches/` | Local patches to ESP32-audioI2S. **Required** — see below. |

Each sketch folder has its own README. `OneButtonRadioS3/README.md` is the real
documentation: wiring, power, build flags, and the three non-obvious faults that
took the longest to find.

## Hardware

ESP32-S3-DevKitC-1 v1.0 (16 MB flash, 8 MB octal PSRAM) · MAX98357A I2S amplifier ·
HW-233 slide potentiometer · small speaker · 12 V supply through an SPST toggle into a
12 V→dual-USB buck module.

## Two things that will bite you

**The library is patched.** `patches/prefill_patch.py` adds `PREFILL_BYTES`,
`PREFILL_TIMEOUT_MS` and `REFERER` to ESP32-audioI2S. Re-run it after any reinstall or
update of that library, or the sketch will not compile:

```bash
python3 ~/Documents/coding_experiments/naani_radio/patches/prefill_patch.py
```

It is idempotent, and fails loudly rather than silently if a library update moves the
code it anchors on.

**The library lives outside this repo.** Arduino resolves libraries from the
sketchbook, so ESP32-audioI2S sits in `~/Documents/Arduino/libraries/` while the
project lives here. `.gitignore` excludes it either way. Confirm the sketchbook path
with `arduino-cli config get directories.user`.

## Changing station is constrained

Not every stream URL will work, and the reason is not obvious. The ESP32's TCP receive
window is fixed at 5760 bytes with window scaling compiled out, so throughput is
bounded by `window / RTT`. A far-away server simply cannot deliver fast enough, no
matter how good the WiFi is.

```
5760 / RTT_seconds  must exceed  bitrate_bytes_per_second,  with real margin
```

At 128 kbps (16 KB/s) that means RTT under roughly 300 ms. `ping` the host before
committing to a URL. This is why the station uses `radioindia.net` (266 ms, +35%
headroom) rather than the `voscast` mirror (358 ms, +1%) — the full measurements are in
`OneButtonRadioS3/README.md` §7.
