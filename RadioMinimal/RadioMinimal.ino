/*
 * RadioMinimal - bisection build for the stuttering investigation.
 *
 * The absolute minimum: WiFi, connect, audio.loop(). Nothing else.
 *
 * Deliberately removed compared with OneButtonRadioS3:
 *   - the volume pot (no analogRead, no setVolume calls at all)
 *   - the RGB status LED (no neopixelWrite, which drives the RMT peripheral)
 *   - the periodic diagnostics (no Serial traffic during playback)
 *   - the reconnect watchdog
 *   - forceMono
 *
 * If this plays cleanly, the fault is in one of those and I add them back one
 * at a time. If it still stutters at the same ~1s period, the fault is in the
 * library, the I2S path, or the hardware, and the application code is
 * exonerated.
 *
 * Volume is fixed at a moderate level so nothing touches setVolume() mid-play.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "Audio.h"

#include "secrets.h"

static const int PIN_I2S_BCLK = 5;
static const int PIN_I2S_LRC  = 6;
static const int PIN_I2S_DOUT = 7;
static const int PIN_VOLUME   = 4;

// radioindia.net is the station's own feed and is 90ms closer than voscast:
// 266ms RTT instead of 358ms. That matters because the ESP32's TCP receive
// window is a hard 5760 bytes (CONFIG_LWIP_WND_SCALE is off), so throughput
// is capped at window/RTT. voscast gives 16.1 KB/s against a 16.0 KB/s
// stream - 1% headroom, no way to ever refill the buffer. radioindia gives
// 21.6 KB/s, a 35% surplus, so the buffer can actually recover after a stall.
// It is HTTPS-only and hotlink-protected, hence the Referer.
const char *STATION_URL = "https://radioindia.net/radio/sharda/icecast.audio";

// Bisection step 3. Step 2 was a botched test: it introduced BOTH pot polling
// and setVolumeSteps(100) at once, so "the stutter is back" did not say which
// of the two caused it.
//
//   POLL_POT 0 -> setVolumeSteps(100) with a fixed volume, no analogRead at
//                 all. Stutter here means the 100-step setting is at fault.
//   POLL_POT 1 -> the same plus pot polling. Stutter only here means
//                 analogRead() is at fault.
#define POLL_POT 1
#define MEASURE  0

// Step 4. analogRead() measured at 135us avg / 223us worst - far too fast to
// stall audio - so the ADC is probably not the culprit. The other thing
// POLL_POT=1 introduced was setVolume() calls DURING playback. With the pot
// noise crossing step boundaries a few times a second, the library's volume
// ramp may be producing audible amplitude wobble.
//
//   APPLY_VOLUME 0 -> read the pot, compute the step, but never call
//                     setVolume(). Clean here means setVolume-during-playback
//                     is the cause, not the ADC.
//   APPLY_VOLUME 1 -> also apply it (this is what stuttered).
#define APPLY_VOLUME 1

// Step 5 - candidate fix. analogRead() costs only ~135us of CPU, so the
// problem is not time: adc_oneshot_read() acquires the SAR ADC power domain,
// which is RTC/analog circuitry shared with the RF front end. Each read
// briefly disturbs WiFi, the network read stalls, and with only ~1s of buffer
// that is audible. Polling 5x less often should cut the disturbance
// proportionally while keeping the knob perfectly usable at 4 Hz.
#define POLL_MS 500

static const uint8_t VOLUME_STEPS = 100;
static const uint8_t FIXED_VOLUME = 76;  // matches what the slider read
static const int ADC_LOW  = 120;
static const int ADC_HIGH = 3900;

Audio audio;

static float   volumeFiltered = 0.0f;
static float   volumeAnchor   = -9999.0f;
static uint8_t volumeStep     = 0;
static uint32_t lastPoll      = 0;
#if MEASURE
static volatile uint32_t adcMax = 0, adcSum = 0, adcN = 0;
static uint32_t loopGapMax = 0, lastLoopUs = 0, lastRep = 0;
#endif

static uint8_t adcToStep(float adc) {
  if (adc <= ADC_LOW) return 0;
  if (adc >= ADC_HIGH) return VOLUME_STEPS;
  return (uint8_t)lroundf((adc - ADC_LOW) / (float)(ADC_HIGH - ADC_LOW) * VOLUME_STEPS);
}

#if POLL_POT
static void pollVolume() {
  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();

#if MEASURE
  uint32_t t0 = micros();
  int raw = analogRead(PIN_VOLUME);
  uint32_t dt = micros() - t0;
  if (dt > adcMax) adcMax = dt;
  adcSum += dt; adcN++;
#else
  int raw = analogRead(PIN_VOLUME);
#endif
  volumeFiltered += 0.2f * (raw - volumeFiltered);

  const float stepSpan = (float)(ADC_HIGH - ADC_LOW) / VOLUME_STEPS;
  if (fabsf(volumeFiltered - volumeAnchor) < stepSpan * 0.75f) return;

  uint8_t step = adcToStep(volumeFiltered);
  if (step != volumeStep) {
    volumeAnchor = volumeFiltered;
    volumeStep = step;
#if APPLY_VOLUME
    audio.setVolume(volumeStep);
#endif
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nRadioMinimal");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);
  Serial.printf("wifi ok, rssi %d\n", WiFi.RSSI());

#if POLL_POT
  analogSetPinAttenuation(PIN_VOLUME, ADC_11db);
  volumeFiltered = analogRead(PIN_VOLUME);
  for (int i = 0; i < 16; i++) {
  #if MEASURE
  uint32_t t0 = micros();
  int raw = analogRead(PIN_VOLUME);
  uint32_t dt = micros() - t0;
  if (dt > adcMax) adcMax = dt;
  adcSum += dt; adcN++;
#else
  int raw = analogRead(PIN_VOLUME);
#endif
  volumeFiltered += 0.2f * (raw - volumeFiltered);
    delay(5);
  }
  volumeStep = adcToStep(volumeFiltered);
#else
  volumeStep = FIXED_VOLUME;
#endif

  // Bank the server's connect burst instead of spending it immediately.
  // Measured: it hands out ~234 KB of surplus over the first ~4.6 s, then
  // throttles to exactly real time. At 16 KB/s that surplus is ~14 s of
  // cushion. Upstream starts decoding 1.5 KB in and throws it all away.
  // Requires patches/prefill_patch.py to have been applied to the library.
  audio.settings.REFERER = "https://onlineradiofm.in/";
  audio.settings.PREFILL_BYTES = 200000;      // ~12.5 s of audio
  audio.settings.PREFILL_TIMEOUT_MS = 8000;   // play anyway if it never fills

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.setVolumeSteps(VOLUME_STEPS);
  audio.setVolume(volumeStep);
  Serial.printf("volume %u/%u  (POLL_POT=%d)\n", volumeStep, VOLUME_STEPS, POLL_POT);

  Serial.println("connecting");
  audio.connecttohost(STATION_URL);
}

void loop() {
#if MEASURE
  uint32_t nowUs = micros();
  if (lastLoopUs && (nowUs - lastLoopUs) > loopGapMax) loopGapMax = nowUs - lastLoopUs;
  lastLoopUs = nowUs;
  if (millis() - lastRep > 2000) {
    lastRep = millis();
    Serial.printf("adc: max=%luus avg=%luus n=%lu | worst loop gap=%luus\n",
                  adcMax, adcN ? adcSum/adcN : 0, adcN, loopGapMax);
    adcMax = 0; adcSum = 0; adcN = 0; loopGapMax = 0;
  }
#endif

  // Buffer trend. A steady decline means we consume faster than the server
  // supplies; sudden drops mean discrete network stalls. Different fixes.
  static uint32_t lastBufRep = 0;
  if (millis() - lastBufRep > 2000) {
    lastBufRep = millis();
    uint32_t sz = audio.getInBufferSize();
    uint32_t f  = audio.inBufferFilled();
    Serial.printf("t=%4lus  buf=%7lu B (%2lu%%)  %5.1fs audio  rssi=%d\n",
                  millis() / 1000, f, sz ? f * 100 / sz : 0, f / 16000.0, WiFi.RSSI());
  }

  audio.loop();
#if POLL_POT
  pollVolume();
#endif
}
