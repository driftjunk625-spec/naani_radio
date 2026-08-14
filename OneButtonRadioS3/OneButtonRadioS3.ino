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
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include "Audio.h"

#include "secrets.h"  // compile-time fallback credentials
#include "webui.h"    // the two served pages

// Reachable at http://naani.local once joined to a network.
static const char *MDNS_NAME = "naani";

// SoftAP raised when no network can be joined. Open on purpose: it exists only
// to hand over credentials, and a password you would have to look up defeats
// the point of a recovery portal. It is only up while unconfigured.
static const char *AP_SSID = "naani-radio-setup";

// ---------------------------------------------------------------- config

// Radio Sharda 90.4 FM, Jammu - 128 kbps MP3.
//
// Use the station's own feed, NOT the voscast mirror. This is a throughput
// decision, not a preference.
//
// The ESP32's TCP receive window is a hard 5760 bytes: sdkconfig has
// CONFIG_LWIP_TCP_WND_DEFAULT=5760 and CONFIG_LWIP_WND_SCALE unset, so it
// cannot grow. TCP throughput is therefore capped at window/RTT, and both
// servers are far away:
//
//   voscast     358ms RTT -> 16.1 KB/s  vs 16.0 KB/s needed = +1%  headroom
//   radioindia  266ms RTT -> 21.6 KB/s  vs 16.0 KB/s needed = +35% headroom
//
// With +1% the buffer can never refill: measured draining from 98KB to 17KB
// (1.1s of audio) and then stuttering permanently. With +35% it climbed to
// 453KB (28s) in 85s and kept going. That is the whole difference.
//
// This feed is HTTPS-only (port 80 closed) and hotlink-protected - it 403s
// without the Referer below, which needs patches/prefill_patch.py applied.
const char *STATION_URL = "https://radioindia.net/radio/sharda/icecast.audio";
const char *STATION_REFERER = "https://onlineradiofm.in/";

// Same station via voscast. Plain HTTP, no Referer needed, but only 1%
// throughput headroom - it stutters. Kept for reference only. Note the
// "/stream" path is load-bearing there: the library sends a Chrome
// user-agent, so a bare "http://host:7738/" makes SHOUTcast DNAS v2 assume a
// browser and 302 to its web admin page, and the stream reconnect-loops.
// Confirmed the same broadcast by cross-correlating 30s of each feed: a
// single sharp peak of 0.64 at -11.4s lag.
// const char *STATION_URL = "http://s8.voscast.com:7738/stream";

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

// Tone control, in dB, -12.0 to +12.0 each. The library runs a 3-band IIR
// (low shelf 500 Hz, peaking EQ 1800 Hz, high shelf 6000 Hz) and it is enabled
// by default, so the biquads are already in the signal path at flat gain -
// changing these costs no extra CPU at all, only a one-off coefficient recalc.
//
// Corner frequencies are tunable too, via audio.settings.FREQ_LS_HZ,
// FREQ_PEAK_HZ and FREQ_HS_HZ, if 500/1800/6000 don't suit the speaker.
//
// CAUTION: a positive gain eats headroom. Boosting bass by +6 dB means the
// signal clips 6 dB earlier, and on a small driver that arrives as distortion
// long before it gets loud. On little speakers it is usually better to CUT the
// band you dislike than to boost the one you want - cutting mids to tame
// boxiness beats boosting bass the driver cannot physically reproduce.
static const float TONE_BASS   = 0.0f;   // low shelf,  500 Hz
static const float TONE_MID    = 0.0f;   // peaking EQ, 1800 Hz
static const float TONE_TREBLE = 0.0f;   // high shelf, 6000 Hz

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

enum Status { ST_BOOT, ST_WIFI, ST_STREAM, ST_PLAYING, ST_ERROR, ST_AP };

// --------------------------------------------------------- persisted config
//
// Written to NVS only when a form is submitted, never during playback. That
// keeps the "nothing is written to flash at runtime" property that makes
// cutting power mid-song safe. NVS is power-fail safe by design anyway, but
// there is no reason to be writing at all while the radio is just playing.
Preferences prefs;

static String cfgSsid, cfgPass, cfgUrl;
static float  cfgBass, cfgMid, cfgTreble;

static AsyncWebServer server(80);
static bool   apMode = false;

// Networks found by the single scan taken before the SoftAP is raised. Held in
// a fixed array rather than re-scanning on demand, because scanning while the
// AP is up breaks the AP - see startAP().
static const int MAX_SCAN = 20;
static struct { char ssid[33]; int32_t rssi; bool open; } scanList[MAX_SCAN];
static int scanCount = 0;
static uint32_t lastApRetry = 0;
static String nowPlaying;          // latest ICY stream title
static uint32_t nowBitrate = 0;
static volatile bool pendingReboot = false;
static volatile bool pendingRetune = false;

static void loadConfig() {
  prefs.begin("naani", true);                       // read-only
  cfgSsid   = prefs.getString("ssid", WIFI_SSID);   // secrets.h is the fallback
  cfgPass   = prefs.getString("pass", WIFI_PASS);
  cfgUrl    = prefs.getString("url",  STATION_URL);
  cfgBass   = prefs.getFloat("bass",   TONE_BASS);
  cfgMid    = prefs.getFloat("mid",    TONE_MID);
  cfgTreble = prefs.getFloat("treble", TONE_TREBLE);
  prefs.end();
}

static void saveWiFi(const String &s, const String &p) {
  prefs.begin("naani", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}

static void saveTone() {
  prefs.begin("naani", false);
  prefs.putFloat("bass", cfgBass);
  prefs.putFloat("mid", cfgMid);
  prefs.putFloat("treble", cfgTreble);
  prefs.end();
}

static void saveUrl(const String &u) {
  prefs.begin("naani", false);
  prefs.putString("url", u);
  prefs.end();
}

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
    case ST_AP:      neopixelWrite(PIN_RGB, 26, 0, 26); break;  // magenta: setup
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

// --------------------------------------------------------- tone + loudness
//
// Boosting a band raises the overall level, so without compensation the EQ
// doubles as a second volume control - +6 dB of bass is audibly louder, not
// just bassier. This estimates the broadband level change the three filters
// cause and returns its negative, to be applied as makeup gain.
//
// The weights are how much each band contributes to *perceived* loudness, not
// how much spectrum it covers. Mids carry the most because the ear is most
// sensitive around 1-4 kHz; the high shelf sits at 6 kHz where there is little
// programme energy. They sum to 1.0, so a uniform +6 dB across all three
// returns exactly -6 dB and cancels.
static const float W_BASS = 0.30f, W_MID = 0.45f, W_TREBLE = 0.25f;

// Eight presets. Defined here rather than in the page so there is one source
// of truth; the browser fetches them from /presets and renders the buttons.
//
// They lean on cuts more than boosts, because loudness compensation means a
// boosted preset just pulls the volume down to match - you get the tonal shape
// either way, and cutting keeps more headroom on a small driver.
struct Preset { const char *name; float bass, mid, treble; };

static const Preset PRESETS[] = {
  { "Flat",         0,   0,   0 },
  { "Speech",      -4,  +4,  +1 },   // talk radio: clear, no boom
  { "Warm",        +3,   0,  -3 },   // soften a harsh or sibilant stream
  { "Bright",      -2,   0,  +4 },   // lift detail on a dull recording
  { "Bass boost",  +6,  -1,   0 },   // as much low end as the driver allows
  { "Small spkr",  -3,  +2,  +3 },   // cut what a tiny cone cannot make anyway
  { "Late night",  -2,  +3,  -2 },   // intelligible at very low volume
  { "Loudness",    +4,  -2,  +3 },   // smiley curve for quiet listening
};
static const size_t N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

static float toneMakeupDb() {
  return -(W_BASS * cfgBass + W_MID * cfgMid + W_TREBLE * cfgTreble);
}

// Map the smoothed ADC reading onto the 0..VOLUME_STEPS scale.
static uint8_t adcToStep(float adc) {
  if (adc <= ADC_LOW) return 0;
  if (adc >= ADC_HIGH) return VOLUME_STEPS;
  float span = (adc - ADC_LOW) / (float)(ADC_HIGH - ADC_LOW);
  return (uint8_t)lroundf(span * VOLUME_STEPS);
}

// Send the slider position to the library with the tone makeup folded in.
//
// The volume curve is dB = 33.22*log10(t) for t = step/steps, so shifting the
// output by C dB means scaling t by 10^(C/33.22) - no need to touch the curve
// itself. Clamping at 1.0 means a large cut cannot be fully made up when the
// slider is already at maximum; that limit is real, you cannot amplify past
// full scale. The other direction always works, and conveniently a boost
// pulls the volume down, which is exactly the headroom you need to not clip.
static void applyVolume() {
  if (volumeStep == 0) { audio.setVolume(0); return; }

  float t = (float)volumeStep / VOLUME_STEPS;
  t *= powf(10.0f, toneMakeupDb() / 33.22f);
  t = constrain(t, 0.0f, 1.0f);

  audio.setVolume((uint8_t)lroundf(t * VOLUME_STEPS));
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
    applyVolume();
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
  if (cfgSsid.isEmpty()) return false;

  setStatus(ST_WIFI);
  Serial.printf("wifi: connecting to %s\n", cfgSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep causes audible stream dropouts
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());

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
  Serial.printf("stream: connecting to %s\n", cfgUrl.c_str());
  if (audio.connecttohost(cfgUrl.c_str())) {
    reconnectDelay = 2000;
    setStatus(ST_PLAYING);
  } else {
    Serial.println("stream: connect failed");
    setStatus(ST_ERROR);
  }
}

// -------------------------------------------------------------- web server
//
// Handlers run in the AsyncTCP task, NOT in loop(). That is the whole reason
// for using the async server: the synchronous one needs handleClient() inside
// loop(), which would block audio.loop() - and audio.loop() is what reads the
// network. Anything slow or blocking is therefore deferred to loop() via the
// pendingReboot / pendingRetune flags rather than done inside a handler.

static String jsonEscape(const String &in) {
  String o;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n' || c == '\r') o += ' ';
    else if ((uint8_t)c < 0x20) continue;
    else o += c;
  }
  return o;
}

static void startWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send_P(200, "text/html", apMode ? PAGE_SETUP : PAGE_MAIN);
  });

  // Serves the list captured BEFORE the SoftAP came up. Never scans while the
  // portal is live: a STA scan makes the radio hop channels, which knocks the
  // SoftAP off its own channel and leaves it visible but unjoinable. That is
  // exactly the bug this replaces.
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
    String j = "{\"done\":true,\"nets\":[";
    for (int i = 0; i < scanCount; i++) {
      if (i) j += ',';
      j += "{\"ssid\":\"" + jsonEscape(String(scanList[i].ssid)) +
           "\",\"rssi\":" + String(scanList[i].rssi) +
           ",\"open\":" + (scanList[i].open ? "true" : "false") + "}";
    }
    j += "]}";
    r->send(200, "application/json", j);
  });

  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("ssid", true)) { r->send(400, "text/plain", "ssid required"); return; }
    String s = r->getParam("ssid", true)->value();
    String p = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
    saveWiFi(s, p);
    Serial.printf("wifi: saved '%s', rebooting\n", s.c_str());
    r->send(200, "application/json", "{\"ok\":true}");
    pendingReboot = true;      // actually reboot from loop(), not here
  });

  server.on("/tone", HTTP_POST, [](AsyncWebServerRequest *r) {
    auto g = [&](const char *k, float d) {
      return r->hasParam(k, true) ? r->getParam(k, true)->value().toFloat() : d;
    };
    cfgBass   = constrain(g("bass",   cfgBass),   -12.0f, 12.0f);
    cfgMid    = constrain(g("mid",    cfgMid),    -12.0f, 12.0f);
    cfgTreble = constrain(g("treble", cfgTreble), -12.0f, 12.0f);
    audio.setTone(cfgBass, cfgMid, cfgTreble);   // cheap: recomputes coefficients
    applyVolume();                               // re-apply makeup for the new curve
    saveTone();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/station", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("url", true)) { r->send(400, "text/plain", "url required"); return; }
    cfgUrl = r->getParam("url", true)->value();
    saveUrl(cfgUrl);
    r->send(200, "application/json", "{\"ok\":true}");
    pendingRetune = true;      // reconnect from loop()
  });

  server.on("/presets", HTTP_GET, [](AsyncWebServerRequest *r) {
    String j = "[";
    for (size_t i = 0; i < N_PRESETS; i++) {
      if (i) j += ',';
      j += "{\"n\":\"" + String(PRESETS[i].name) + "\",\"b\":" + String(PRESETS[i].bass, 0) +
           ",\"m\":" + String(PRESETS[i].mid, 0) + ",\"t\":" + String(PRESETS[i].treble, 0) + "}";
    }
    j += "]";
    r->send(200, "application/json", j);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t sz = audio.getInBufferSize();
    uint32_t f  = sz ? audio.inBufferFilled() : 0;
    char buf[512];
    snprintf(buf, sizeof(buf),
      "{\"title\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"vol\":%u,\"br\":%lu,"
      "\"bufpct\":%lu,\"bufsec\":%.1f,\"url\":\"%s\","
      "\"bass\":%.0f,\"mid\":%.0f,\"treble\":%.0f,\"makeup\":%.1f}",
      jsonEscape(nowPlaying).c_str(),
      WiFi.localIP().toString().c_str(), WiFi.RSSI(), volumeStep,
      nowBitrate / 1000, sz ? f * 100 / sz : 0, f / 16000.0,
      jsonEscape(cfgUrl).c_str(), cfgBass, cfgMid, cfgTreble, toneMakeupDb());
    r->send(200, "application/json", buf);
  });

  server.onNotFound([](AsyncWebServerRequest *r) {
    // In AP mode bounce everything to the portal, so a captive-portal probe
    // or a mistyped path still lands somewhere useful.
    if (apMode) r->send_P(200, "text/html", PAGE_SETUP);
    else        r->send(404, "text/plain", "not found");
  });

  server.begin();
}

static void startMDNS() {
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mdns: http://%s.local\n", MDNS_NAME);
  } else {
    Serial.println("mdns: failed to start");
  }
}

// One blocking scan, taken while the AP is NOT running, cached for the portal.
static void scanOnce() {
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, false);
  scanCount = 0;
  for (int i = 0; i < n && scanCount < MAX_SCAN; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    strncpy(scanList[scanCount].ssid, s.c_str(), 32);
    scanList[scanCount].ssid[32] = '\0';
    scanList[scanCount].rssi = WiFi.RSSI(i);
    scanList[scanCount].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    scanCount++;
  }
  WiFi.scanDelete();
  Serial.printf("scan: %d networks cached\n", scanCount);
}

// No network could be joined - raise the setup portal instead of sitting dark.
//
// Deliberately AP-only, with STA fully disconnected first. Two things break a
// SoftAP: a STA scan, and a STA still retrying association to credentials that
// do not work. Both make the radio hop channels, and the AP then shows up in
// the network list but refuses to accept clients. Scan first, then go AP-only
// and leave the radio alone.
static void startAP() {
  apMode = true;
  setStatus(ST_AP);

  WiFi.disconnect(true);   // stop STA retrying the failed credentials
  scanOnce();              // scan while there is no AP to disturb

  WiFi.mode(WIFI_AP);      // AP only - nothing else touching the radio
  WiFi.softAP(AP_SSID);
  lastApRetry = millis();
  Serial.printf("ap: '%s' at http://%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
}

// Periodically re-try the saved network while the portal is up, so a router
// that was simply slow to boot does not strand the radio in setup mode
// forever. Skipped while someone is actually connected to the portal, since
// the STA attempt would disrupt the AP they are using.
static bool retrySavedNetwork() {
  if (cfgSsid.isEmpty()) return false;
  if (WiFi.softAPgetStationNum() > 0) return false;   // portal in use, leave it alone

  Serial.printf("ap: retrying '%s'\n", cfgSsid.c_str());
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(200);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("ap: joined, ip %s - dropping portal\n",
                  WiFi.localIP().toString().c_str());
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    apMode = false;
    startMDNS();
    startStream();
    return true;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);     // back to AP-only so the portal stays joinable
  return false;
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

  loadConfig();
  Serial.printf("config: ssid='%s' url='%s' tone %.0f/%.0f/%.0f\n",
                cfgSsid.c_str(), cfgUrl.c_str(), cfgBass, cfgMid, cfgTreble);

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
    // Capture what the web UI wants to show, rather than re-deriving it.
    // Substring, not equality: the library labels these "streamtitle" but
    // "bitrate (b/s)", so an exact match on the latter silently never fires.
    if (m.s && m.msg) {
      if (strstr(m.s, "streamtitle")) nowPlaying = m.msg;
      else if (strstr(m.s, "bitrate")) nowBitrate = atol(m.msg);
    }
    Serial.printf("%s: %s\n", m.s ? m.s : "?", m.msg ? m.msg : "");
  };

  // These three need patches/prefill_patch.py applied to the library.
  //
  // The Referer is required by the hotlink-protected feed. The prefill banks
  // a starting cushion instead of upstream's behaviour of beginning playback
  // 1.5KB in: 64KB is ~4s of audio and costs ~3s of startup. It stays modest
  // deliberately, because on this feed the buffer keeps growing during
  // playback anyway - it does not need to be filled up front.
  audio.settings.REFERER = STATION_REFERER;
  audio.settings.PREFILL_BYTES = 64000;
  audio.settings.PREFILL_TIMEOUT_MS = 8000;

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.setVolumeSteps(VOLUME_STEPS);
  audio.setVolumeCurve(volumeCurve);
  audio.setTone(cfgBass, cfgMid, cfgTreble);

  // One speaker, so fold both channels together in software. This makes the
  // amp's SD channel-select pin irrelevant - nothing is lost either way.
  audio.forceMono(true);

  // Set volume before connecting, so the radio comes up at whatever the
  // slider is physically set to rather than blasting at full.
  applyVolume();
  Serial.printf("volume: %u/%u at power-on (tone makeup %+.1f dB)\n",
                volumeStep, VOLUME_STEPS, toneMakeupDb());

  if (connectWiFi(20000)) {
    startMDNS();
    startWebServer();
    startStream();
  } else {
    // No usable credentials, or the saved network is gone. Rather than sit
    // dark, raise a setup portal so the radio can be reconfigured without a
    // laptop and a USB cable.
    startAP();
    startMDNS();        // naani.local resolves on the AP too
    startWebServer();
  }
}

void loop() {
  // Deferred work from web handlers. Doing these inside an AsyncTCP handler
  // would reboot or reconnect out from under the HTTP response.
  if (pendingReboot) {
    delay(400);            // let the response actually reach the browser
    ESP.restart();
  }
  if (pendingRetune) {
    pendingRetune = false;
    Serial.printf("station: switching to %s\n", cfgUrl.c_str());
    audio.stopSong();
    nowPlaying = "";
    startStream();
  }

  // In setup mode there is no stream to service - keep the portal alive, and
  // every 30 s have another go at the saved network so a router that was slow
  // to boot recovers on its own instead of stranding the radio here.
  if (apMode) {
    if (millis() - lastApRetry > 30000) {
      lastApRetry = millis();
      retrySavedNetwork();
    }
    delay(10);
    return;
  }

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
