/*
 * OneButtonRadioS3 - single-station internet radio (ESP32-S3 version)
 *
 * Hardware:
 *   ESP32-S3-DevKitC-1 v1.0, module ESP32-S3-WROOM-1 N16R8
 *     -> 16MB flash, 8MB OCTAL PSRAM
 *   MAX98357A I2S amplifier
 *   Dual-gang linear slide pot (OTA wiper used for volume)
 *   12V -> dual-USB buck module, with an SPST toggle in the 12V line
 *
 * There is no software power control. The toggle switch cuts 12V, so the
 * whole radio is genuinely off: no WiFi, no stream, no data. Losing power
 * mid-stream is safe - nothing is written to flash at runtime.
 *
 * Requires: ESP32-audioI2S (upstream schreibfaul1, v4.0.0). This needs PSRAM,
 * which this board has. It MUST be compiled with PSRAM=opi - see README.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "Audio.h"

#include "secrets.h"  // WIFI_SSID and WIFI_PASS live here

// ---------------------------------------------------------------- config

// Radio Sharda 90.4 FM, Jammu - 128 kbps MP3 over plain HTTP.
//
// This is the Shoutcast feed listed by OnlineRadioBox. Verified to be the same
// broadcast as the radioindia.net feed by recording 30s of each simultaneously
// and cross-correlating: peak 0.64 at a -11.4s lag (one sharp peak, so same
// audio through a different CDN with ~11s more buffering).
//
// Preferred over https://radioindia.net/radio/sharda/icecast.audio because that
// one is HTTPS-only (port 80 closed) *and* hotlink-protected - it 403s unless
// the request carries "Referer: https://onlineradiofm.in/", and this library
// has no way to set a custom Referer.
// The "/stream" path is load-bearing. This library hardcodes a Chrome
// user-agent (Audio.cpp:1071), so a bare "http://host:7738/" makes SHOUTcast
// DNAS v2 think a browser is asking and 302-redirect to its web admin page
// (/index.html?sid=1). The library follows that, gets HTML instead of audio,
// and reconnect-loops forever. "/stream" (or "/;") forces the audio endpoint.
const char *STATION_URL = "http://s8.voscast.com:7738/stream";

// Known-good reference stream, same 128 kbps MP3. Useful for telling "this
// station is misbehaving" apart from "this build is misbehaving".
// const char *STATION_URL = "http://ice1.somafm.com/groovesalad-128-mp3";

// I2S pins to the MAX98357A. GPIO 4/5/6/7 are adjacent on the header, so
// the amp wiring is one tidy run.
//
// Do NOT move these onto GPIO 35, 36 or 37: on an N16R8 module those three
// are wired to the octal PSRAM and are not available. Also avoid 19/20
// (native USB), 43/44 (UART), 0/45/46 (strapping), 48 (onboard RGB LED).
static const int PIN_I2S_BCLK = 5;   // -> BCLK
static const int PIN_I2S_LRC  = 6;   // -> LRC  (word select)
static const int PIN_I2S_DOUT = 7;   // -> DIN

// Volume pot wiper. On the S3, ADC1 is GPIO1-10; ADC2 (GPIO11-20) is
// unusable while WiFi is on, so keep this in the 1-10 range.
static const int PIN_VOLUME = 4;

// Onboard WS2812 status LED. This is GPIO48 on DevKitC-1 v1.0 (the silk on
// the back of your board reads v1.0). On v1.1 boards it moved to GPIO38.
static const int PIN_RGB = 48;
#define STATUS_LED 1  // set to 0 to keep the LED dark inside the enclosure

// 100 volume steps instead of the default 22, so the slider feels smooth.
static const uint8_t VOLUME_STEPS = 100;

// How often the pot is sampled. This is deliberately slow, and it is the fix
// for the stuttering: analogRead() costs only ~135us of CPU, but
// adc_oneshot_read() acquires the SAR ADC power domain, which is RTC/analog
// circuitry shared with the RF front end. Every read briefly disturbs WiFi,
// the network read in audio.loop() stalls, and because the library keeps only
// about a second of buffer that stall is audible. At 50ms this stuttered
// constantly; at 500ms it is clean and the slider still feels responsive.
//
// A rotary encoder would remove the ADC from the picture entirely and is the
// proper fix if this ever proves marginal - see README section 7.
static const uint32_t POLL_MS = 500;

// Periodic diagnostics, off by default. Set to 1 to get a stats line every 2s
// (buffer fill, RSSI, heap, loop rate, decoder resyncs, setVolume calls).
#define DIAG 0

// ADC endpoints. The ADC does not reach a clean 0 or 4095, so the travel is
// clipped slightly at both ends to guarantee true silence at the bottom and
// full volume at the top.
static const int ADC_LOW  = 120;
static const int ADC_HIGH = 3900;

// --------------------------------------------------------------- globals

Audio audio;

static float    volumeFiltered  = 0.0f;
static float    volumeAnchor    = -9999.0f;  // ADC value when volumeStep was last set
static uint8_t  volumeStep      = 0;
static uint32_t lastVolumePoll  = 0;
static uint32_t lastStreamCheck = 0;
static uint32_t reconnectDelay  = 2000;  // grows on repeated failure

#if DIAG
static uint32_t diagLoops    = 0;   // loop() iterations since the last report
static uint32_t diagResyncs  = 0;   // decoder lost sync and had to re-find it
static uint32_t diagVolWrites = 0;  // setVolume() calls, to catch ADC chatter
static uint32_t lastDiag     = 0;
static uint32_t bufMinPct    = 100; // low-water mark between reports
#endif

enum Status { ST_BOOT, ST_WIFI, ST_STREAM, ST_PLAYING, ST_ERROR };

// ------------------------------------------------------------- functions

// Only touches the LED when the state actually changes. neopixelWrite() drives
// the RMT peripheral and blocks until the transfer completes, so there is no
// reason to repeat it every couple of seconds for a state that has not moved.
static void setStatus(Status s) {
#if STATUS_LED
  static Status last = ST_ERROR;
  static bool   first = true;
  if (!first && s == last) return;
  first = false;
  last = s;

  switch (s) {
    case ST_BOOT:    neopixelWrite(PIN_RGB, 8, 8, 8);  break;  // dim white
    case ST_WIFI:    neopixelWrite(PIN_RGB, 20, 8, 0);  break;  // amber
    case ST_STREAM:  neopixelWrite(PIN_RGB, 0, 6, 24);  break;  // blue
    case ST_PLAYING: neopixelWrite(PIN_RGB, 0, 10, 0);  break;  // dim green
    case ST_ERROR:   neopixelWrite(PIN_RGB, 30, 0, 0);  break;  // red
  }
#endif
}

// Volume taper, in dB, for a position t of 0.0-1.0.
//
// The library's default is dB = -112t^3 + 172t^2 - 60, which puts half travel
// at -31 dB - roughly 3% amplitude. That is why the slider felt dead until it
// was near the top: nearly all of the useful range was crammed into the last
// third of the stroke.
//
// Perceived loudness roughly doubles for every +10 dB, so making dB
// proportional to log2(t) gives a control where half way sounds about half as
// loud: 33.22*log10(t) == 10*log2(t). Half travel lands at -10 dB, a quarter
// at -20 dB. The library clamps the result to -60 dB, so t=0 is true silence.
static float volumeCurve(float t) {
  return 33.22f * log10f(t);
}

// Map the smoothed ADC reading onto the 0..VOLUME_STEPS scale.
static uint8_t adcToStep(float adc) {
  if (adc <= ADC_LOW) return 0;
  if (adc >= ADC_HIGH) return VOLUME_STEPS;
  float span = (adc - ADC_LOW) / (float)(ADC_HIGH - ADC_LOW);
  return (uint8_t)lroundf(span * VOLUME_STEPS);
}

static void pollVolume() {
  if (millis() - lastVolumePoll < POLL_MS) return;
  lastVolumePoll = millis();

  int raw = analogRead(PIN_VOLUME);

  // Exponential moving average. The S3 ADC jitters by tens of counts sitting
  // still, which would otherwise make the volume chatter between steps.
  volumeFiltered += 0.2f * (raw - volumeFiltered);

  // Hysteresis. One volume step spans (ADC_HIGH-ADC_LOW)/VOLUME_STEPS = ~38
  // ADC counts, and the residual noise after smoothing is comparable, so a
  // stationary slider still chattered across step boundaries - measured at
  // 5-18 setVolume() calls per second with the slider untouched. Requiring
  // three quarters of a step of real movement before acting silences that
  // without any perceptible loss of resolution.
  const float stepSpan = (float)(ADC_HIGH - ADC_LOW) / VOLUME_STEPS;
  if (fabsf(volumeFiltered - volumeAnchor) < stepSpan * 0.75f) return;

  uint8_t step = adcToStep(volumeFiltered);
  if (step != volumeStep) {
    volumeAnchor = volumeFiltered;
    volumeStep = step;
    audio.setVolume(volumeStep);
#if DIAG
    diagVolWrites++;
#endif
  }
}

#if DIAG
// One line every 2 s. Read it as: is the buffer draining (network), or is it
// full while the audio still breaks up (consumer side)?
static void reportDiag() {
  if (millis() - lastDiag < 2000) return;
  uint32_t elapsed = millis() - lastDiag;
  lastDiag = millis();

  uint32_t size = audio.getInBufferSize();
  uint32_t pct  = size ? (audio.inBufferFilled() * 100 / size) : 0;

  Serial.printf("diag buf=%3lu%% (min %3lu%%)  rssi=%4d  heap=%6u  psram=%7u  "
                "loops/s=%5lu  resync=%lu  volwr=%lu\n",
                pct, bufMinPct, WiFi.RSSI(), ESP.getFreeHeap(),
                ESP.getFreePsram(), diagLoops * 1000 / max(elapsed, 1UL),
                diagResyncs, diagVolWrites);

  diagLoops = 0;
  diagVolWrites = 0;
  bufMinPct = 100;
}
#endif

static bool connectWiFi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  setStatus(ST_WIFI);
  Serial.printf("wifi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep causes audible stream dropouts
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi: ok, ip %s, rssi %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("wifi: failed");
  setStatus(ST_ERROR);
  return false;
}

static void startStream() {
  setStatus(ST_STREAM);
  Serial.printf("stream: connecting to %s\n", STATION_URL);
  if (audio.connecttohost(STATION_URL)) {
    reconnectDelay = 2000;
    setStatus(ST_PLAYING);
  } else {
    Serial.println("stream: connect failed");
    setStatus(ST_ERROR);
  }
}

// ------------------------------------------------------------------ main

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nOneButtonRadioS3 starting");

  setStatus(ST_BOOT);

  // If this prints 0, the sketch was built without PSRAM=opi and the audio
  // library will not work. See README section 3.
  Serial.printf("PSRAM: %u bytes free of %u\n",
                ESP.getFreePsram(), ESP.getPsramSize());

  // 11dB attenuation gives the ADC its full ~0-3.3V range, which is what the
  // pot swings across when its top rail is on 3V3.
  analogSetPinAttenuation(PIN_VOLUME, ADC_11db);

  // Seed the filter so the first reading is already correct - otherwise the
  // volume ramps up from zero over the first second.
  volumeFiltered = analogRead(PIN_VOLUME);
  for (int i = 0; i < 16; i++) {
    volumeFiltered += 0.2f * (analogRead(PIN_VOLUME) - volumeFiltered);
    delay(5);
  }
  volumeStep = adcToStep(volumeFiltered);

  // Log everything the library reports - station name, bitrate, track titles.
  Audio::audio_info_callback = [](Audio::msg_t m) {
#if DIAG
    // "syncword found at pos N" means the decoder lost the bitstream and had
    // to hunt for the next frame header. Each one is an audible glitch.
    if (m.msg && strstr(m.msg, "syncword")) diagResyncs++;
#endif
    Serial.printf("%s: %s\n", m.s ? m.s : "?", m.msg ? m.msg : "");
  };

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.setVolumeSteps(VOLUME_STEPS);
  audio.setVolumeCurve(volumeCurve);

  // One speaker, so fold both channels together in software. This makes the
  // amp's SD channel-select pin irrelevant - nothing is lost either way.
  audio.forceMono(true);

  // Set volume before connecting, so the radio comes up at whatever the
  // slider is physically set to rather than blasting at full.
  audio.setVolume(volumeStep);
  Serial.printf("volume: %u/%u at power-on\n", volumeStep, VOLUME_STEPS);

  if (connectWiFi(20000)) startStream();
}

void loop() {
  audio.loop();  // must be called constantly - this is what feeds I2S
  pollVolume();

#if DIAG
  // Sampled at 50ms, never per-iteration: inBufferFilled() takes a mutex the
  // audio task also needs, so polling it every loop was real lock contention.
  diagLoops++;
  static uint32_t lastBufSample = 0;
  if (millis() - lastBufSample >= 50) {
    lastBufSample = millis();
    uint32_t sz = audio.getInBufferSize();
    if (sz) {
      uint32_t p = audio.inBufferFilled() * 100 / sz;
      if (p < bufMinPct) bufMinPct = p;
    }
  }
  reportDiag();
#endif

  // Recover from a dropped stream or a dropped router, checked at a slow
  // cadence with backoff so a dead station does not spin the CPU.
  if (millis() - lastStreamCheck > reconnectDelay) {
    lastStreamCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("wifi: lost, retrying");
      WiFi.disconnect();
      if (connectWiFi(15000)) startStream();
      reconnectDelay = min(reconnectDelay * 2, (uint32_t)30000);
    } else if (!audio.isRunning()) {
      Serial.println("stream: not running, reconnecting");
      startStream();
      reconnectDelay = min(reconnectDelay * 2, (uint32_t)30000);
    } else {
      setStatus(ST_PLAYING);
    }
  }
}
