/*
 * OneButtonRadio - single-station internet radio
 *
 * Hardware:
 *   ESP32 DevKit (WROOM-32)
 *   MAX98357A I2S amplifier
 *   Dual-gang linear slide pot (OTA wiper used for volume)
 *   12V -> dual-USB buck module, with an SPST toggle in the 12V line
 *
 * There is no software power control. The toggle switch cuts 12V, so the
 * whole radio is genuinely off: no WiFi, no stream, no data. Losing power
 * mid-stream is safe - nothing is written to flash at runtime.
 *
 * Requires: ESP32-audioI2S-nopsram (PLSousa's v2.0.6 fork). This board is an
 * ESP32-D0WD-V3 with 0 bytes of PSRAM and a 110KB max single allocation, so
 * upstream v3.x - which wants one contiguous 704KB buffer - cannot run here.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "Audio_nopsram.h"

#include "secrets.h"  // WIFI_SSID and WIFI_PASS live here

// ---------------------------------------------------------------- config

// Direct stream URL, not a station homepage. Plain http:// is strongly
// preferred - https costs ~40KB of heap for TLS and is a common cause of
// "connects, then dies after a few seconds".
const char *STATION_URL = "http://ice1.somafm.com/groovesalad-128-mp3";

// I2S pins to the MAX98357A
static const int PIN_I2S_BCLK = 26;  // -> BCLK
static const int PIN_I2S_LRC  = 25;  // -> LRC  (word select)
static const int PIN_I2S_DOUT = 22;  // -> DIN

// Volume pot wiper. GPIO34 is input-only and on ADC1.
// ADC2 pins (0,2,4,12-15,25-27) do NOT work while WiFi is on - do not move
// this to one of those.
static const int PIN_VOLUME = 34;

static const uint8_t VOLUME_MAX_STEP = 21;  // library scale is 0..21

// ADC endpoints. The ESP32 ADC does not reach a clean 0 or 4095, so the
// travel is clipped slightly at both ends to guarantee true silence and
// true full volume at the physical limits of the slider.
static const int ADC_LOW  = 120;
static const int ADC_HIGH = 3900;

// --------------------------------------------------------------- globals

Audio audio;

static float    volumeFiltered = 0.0f;
static uint8_t  volumeStep     = 0;
static uint32_t lastVolumePoll = 0;
static uint32_t lastStreamCheck = 0;
static uint32_t reconnectDelay = 2000;  // grows on repeated failure

// ------------------------------------------------------------- functions

// Map the smoothed ADC reading onto the library's 0..21 volume scale.
static uint8_t adcToStep(float adc) {
  if (adc <= ADC_LOW) return 0;
  if (adc >= ADC_HIGH) return VOLUME_MAX_STEP;
  float span = (adc - ADC_LOW) / (float)(ADC_HIGH - ADC_LOW);
  return (uint8_t)lroundf(span * VOLUME_MAX_STEP);
}

static void pollVolume() {
  if (millis() - lastVolumePoll < 50) return;
  lastVolumePoll = millis();

  int raw = analogRead(PIN_VOLUME);

  // Exponential moving average. The ESP32 ADC is noisy enough that a raw
  // read jitters by 20-40 counts sitting still, which would otherwise make
  // the volume chatter between two steps.
  volumeFiltered += 0.2f * (raw - volumeFiltered);

  uint8_t step = adcToStep(volumeFiltered);
  if (step != volumeStep) {
    volumeStep = step;
    audio.setVolume(volumeStep);
    Serial.printf("volume: %u/%u\n", volumeStep, VOLUME_MAX_STEP);
  }
}

static bool connectWiFi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("wifi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep causes audible stream dropouts
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi: ok, ip %s, rssi %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("wifi: failed");
  return false;
}

static void startStream() {
  Serial.printf("stream: connecting to %s\n", STATION_URL);
  if (audio.connecttohost(STATION_URL)) {
    reconnectDelay = 2000;
  } else {
    Serial.println("stream: connect failed");
  }
}

// ------------------------------------------------------------------ main

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nOneButtonRadio starting");

  // 11dB attenuation gives the ADC the full ~0-3.3V input range, which is
  // what the pot swings across when its top rail is on 3V3.
  analogSetPinAttenuation(PIN_VOLUME, ADC_11db);

  // Seed the filter so the first reading is already correct - otherwise
  // the volume ramps up from zero over the first second.
  volumeFiltered = analogRead(PIN_VOLUME);
  for (int i = 0; i < 16; i++) {
    volumeFiltered += 0.2f * (analogRead(PIN_VOLUME) - volumeFiltered);
    delay(5);
  }
  volumeStep = adcToStep(volumeFiltered);

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);

  // Set volume before connecting, so the radio comes up at whatever the
  // slider is physically set to rather than blasting at full.
  audio.setVolume(volumeStep);
  Serial.printf("volume: %u/%u at power-on\n", volumeStep, VOLUME_MAX_STEP);

  if (connectWiFi(20000)) {
    startStream();
  }
}

void loop() {
  audio.loop();  // must be called constantly - this is what feeds I2S
  pollVolume();

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
    }
  }
}

// -------------------------------------------------- library callbacks

void audio_info(const char *info)        { Serial.printf("info: %s\n", info); }
void audio_showstation(const char *info) { Serial.printf("station: %s\n", info); }
void audio_showstreamtitle(const char *info) { Serial.printf("track: %s\n", info); }
void audio_eof_stream(const char *info)  { Serial.printf("eof: %s\n", info); }
