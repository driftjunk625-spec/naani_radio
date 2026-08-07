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

const char *STATION_URL = "http://ice1.somafm.com/groovesalad-128-mp3";

Audio audio;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nRadioMinimal");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);
  Serial.printf("wifi ok, rssi %d\n", WiFi.RSSI());

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.setVolume(12);  // fixed, mid-scale on the default 0-21 range

  Serial.println("connecting");
  audio.connecttohost(STATION_URL);
}

void loop() {
  audio.loop();
}
