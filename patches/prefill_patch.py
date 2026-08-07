#!/usr/bin/env python3
"""
Adds a configurable stream prefill to ESP32-audioI2S.

Why this exists
---------------
Upstream starts decoding as soon as the input buffer holds one frame
(Audio.cpp, "start audio decoding"). For a live stream that means playback
begins ~1.5 KB in and then tracks real time forever, so the 655 KB input
buffer never holds more than about a second. Any network stall longer than
that is audible - which is exactly the sporadic stuttering this project hit
on a small SHOUTcast server.

Most stream servers burst on connect (this one sends ~250 KB before
throttling to real time). Banking that burst instead of spending it
immediately buys ~15 s of cushion, which rides out jitter. The cushion
persists, because after the burst supply and demand are both real time.

What it changes
---------------
  Audio.h   adds PREFILL_BYTES and PREFILL_TIMEOUT_MS to the public
            `settings` struct (default 0 = upstream behaviour)
  Audio.cpp waits for PREFILL_BYTES before setting m_f_stream, with a
            timeout so a server that does not burst still plays

Idempotent: running it twice is harmless. Re-run after reinstalling or
updating the library, because that overwrites these files.

    python3 patches/prefill_patch.py
"""

import pathlib
import sys

LIB = pathlib.Path.home() / "Documents/Arduino/libraries/ESP32-audioI2S/src"

H_ANCHOR = """        uint32_t BUFFER_TRESHOLD_HLS = UINT16_MAX; // Level at which the HLS-TS stream starts and is reloaded
    } settings;"""

H_PATCHED = """        uint32_t BUFFER_TRESHOLD_HLS = UINT16_MAX; // Level at which the HLS-TS stream starts and is reloaded
        uint32_t PREFILL_BYTES = 0;                // bank this many input bytes before playback starts (0 = upstream)
        uint32_t PREFILL_TIMEOUT_MS = 8000;        // start anyway if PREFILL_BYTES is not reached in time
    } settings;"""

# Separate anchor: the header may already carry the prefill fields from an
# earlier run, so the Referer field needs its own marker or it gets skipped.
HREF_ANCHOR = """        uint32_t PREFILL_TIMEOUT_MS = 8000;        // start anyway if PREFILL_BYTES is not reached in time"""

HREF_PATCHED = """        uint32_t PREFILL_TIMEOUT_MS = 8000;        // start anyway if PREFILL_BYTES is not reached in time
        const char* REFERER = nullptr;             // sent as the Referer header; hotlink-protected streams need it"""

REF_ANCHOR = """    rqh.appendf("User-Agent: {}\\r\\n", user_agent);
    if (authLen > 0) {"""

REF_PATCHED = """    rqh.appendf("User-Agent: {}\\r\\n", user_agent);
    if (settings.REFERER && settings.REFERER[0]) rqh.appendf("Referer: {}\\r\\n", settings.REFERER);
    if (authLen > 0) {"""

CPP_ANCHOR = """    if (((InBuff.bufferFilled() > m_pwst.maxFrameSize) || (m_f_allDataReceived)) && !m_f_stream) { // waiting for buffer filled
        info(*this, evt_info, "stream ready");
        m_f_stream = true; // ready to play the audio data
    }"""

CPP_PATCHED = """    if (!m_f_stream) { // waiting for buffer filled
        static uint32_t s_prefillStart = 0;
        if (s_prefillStart == 0) s_prefillStart = millis();

        uint32_t want = settings.PREFILL_BYTES > m_pwst.maxFrameSize ? settings.PREFILL_BYTES : m_pwst.maxFrameSize;
        bool timedOut = (millis() - s_prefillStart) > settings.PREFILL_TIMEOUT_MS;
        bool haveFrame = InBuff.bufferFilled() > m_pwst.maxFrameSize;

        if ((InBuff.bufferFilled() > want) || m_f_allDataReceived || (timedOut && haveFrame)) {
            info(*this, evt_info, "stream ready, buffered {} bytes", (uint32_t)InBuff.bufferFilled());
            m_f_stream = true; // ready to play the audio data
            s_prefillStart = 0;
        }
    }"""


def patch(path, anchor, patched, marker):
    src = path.read_text()
    if marker in src:
        print(f"  {path.name}: already patched")
        return True
    if anchor not in src:
        print(f"  {path.name}: ANCHOR NOT FOUND - library version changed, patch by hand")
        return False
    path.write_text(src.replace(anchor, patched, 1))
    print(f"  {path.name}: patched")
    return True


def main():
    if not LIB.is_dir():
        sys.exit(f"library not found at {LIB}")
    print(f"patching {LIB}")
    ok = patch(LIB / "Audio.h", H_ANCHOR, H_PATCHED, "PREFILL_BYTES")
    ok &= patch(LIB / "Audio.cpp", CPP_ANCHOR, CPP_PATCHED, "s_prefillStart")
    ok &= patch(LIB / "Audio.h", HREF_ANCHOR, HREF_PATCHED, "REFERER")
    ok &= patch(LIB / "Audio.cpp", REF_ANCHOR, REF_PATCHED, "settings.REFERER")
    if not ok:
        sys.exit(1)
    print("done - set audio.settings.PREFILL_BYTES in the sketch before connecttohost()")


if __name__ == "__main__":
    main()
