/*
 * I2SToneTest - hardware diagnostic for the OneButtonRadio build
 *
 * Generates test tones straight into I2S. No WiFi, no network, no MP3
 * decoding, no audio library - just synthesised samples. That is the point:
 * it isolates the amp, the speaker, the wiring and the I2S clocking from
 * everything upstream of them.
 *
 * How to read the result:
 *
 *   Every test clean          -> hardware and I2S are fine. The garbling is
 *                                upstream: stream buffering, WiFi, or decode.
 *   Quiet steps clean,        -> clipping. Lower GAIN, or the speaker/supply
 *   loud steps buzzy             is being pushed past its limit. This is the
 *                                most common cause of "garbled".
 *   Everything buzzy at all   -> I2S wiring or clocking. Check BCLK/LRC are
 *   amplitudes                   not swapped, wires are short, connections
 *                                are seated.
 *   Rhythmic stutter/gaps     -> power. The rail is sagging; add the 1000uF
 *                                cap right beside the amp.
 *   Sweep has warbles/beats   -> sample-rate or clock integrity problem.
 *
 * Same pins as the radio sketch: BCLK=5, LRC=6, DIN=7, pot=4.
 */

#include <Arduino.h>
#include <ESP_I2S.h>

static const int PIN_I2S_BCLK = 5;
static const int PIN_I2S_LRC  = 6;
static const int PIN_I2S_DOUT = 7;
static const int PIN_VOLUME   = 4;

static const uint32_t SAMPLE_RATE = 44100;

// 256 stereo frames per write. Small enough to stay responsive, large enough
// that the write overhead does not itself cause gaps.
static const size_t FRAMES = 256;

I2SClass i2s;
static int16_t buf[FRAMES * 2];
static double  phase = 0.0;

// Render a tone. Sweeps logarithmically if f1 != f0 (log matches how pitch is
// actually perceived, so a linear sweep would spend most of its time in the
// treble). amp is 0.0-1.0 of full scale. left/right gate each channel.
static void tone(float f0, float f1, uint32_t ms, float amp, bool left, bool right) {
  const uint32_t total = (uint32_t)((uint64_t)SAMPLE_RATE * ms / 1000);
  const float    ratio = (f1 > 0 && f0 > 0) ? (f1 / f0) : 1.0f;
  uint32_t done = 0;

  while (done < total) {
    size_t n = min((uint32_t)FRAMES, total - done);

    for (size_t i = 0; i < n; i++) {
      float t = (float)(done + i) / (float)total;
      float f = f0 * powf(ratio, t);

      phase += 2.0 * PI * f / SAMPLE_RATE;
      if (phase > 2.0 * PI) phase -= 2.0 * PI;

      int16_t s = (int16_t)(amp * 32000.0f * sinf(phase));
      buf[i * 2 + 0] = left  ? s : 0;
      buf[i * 2 + 1] = right ? s : 0;
    }

    i2s.write((uint8_t *)buf, n * 2 * sizeof(int16_t));
    done += n;
  }
}

static void silence(uint32_t ms) {
  memset(buf, 0, sizeof(buf));
  const uint32_t total = (uint32_t)((uint64_t)SAMPLE_RATE * ms / 1000);
  uint32_t done = 0;
  while (done < total) {
    size_t n = min((uint32_t)FRAMES, total - done);
    i2s.write((uint8_t *)buf, n * 2 * sizeof(int16_t));
    done += n;
  }
}

static void say(const char *what) {
  Serial.printf("[%6lu ms] %-34s pot=%4d\n", millis(), what, analogRead(PIN_VOLUME));
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nI2SToneTest");
  Serial.printf("pins: BCLK=%d LRC=%d DIN=%d  |  %lu Hz, 16-bit stereo\n",
                PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT, SAMPLE_RATE);

  analogSetPinAttenuation(PIN_VOLUME, ADC_11db);

  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("i2s.begin() FAILED - check the pin numbers above");
    while (true) delay(1000);
  }
  Serial.println("i2s ready\n");
}

void loop() {
  Serial.println("--- pass start ---");

  // 1. Steady reference tone. Should be a pure, boring 1 kHz. Any rasp,
  //    buzz or warble here means the problem is in hardware, not the stream.
  say("1 kHz sine, 25%");
  tone(1000, 1000, 3000, 0.25f, true, true);
  silence(400);

  // 2. Amplitude staircase. This is the single most diagnostic test: it finds
  //    the level at which clean turns to distorted. If 6% and 12% are clean
  //    but 50% and 100% buzz, you are clipping - lower GAIN or accept less
  //    volume. Small speakers distort well before the amp does.
  say("staircase 6%");   tone(1000, 1000, 1200, 0.06f, true, true); silence(200);
  say("staircase 12%");  tone(1000, 1000, 1200, 0.12f, true, true); silence(200);
  say("staircase 25%");  tone(1000, 1000, 1200, 0.25f, true, true); silence(200);
  say("staircase 50%");  tone(1000, 1000, 1200, 0.50f, true, true); silence(200);
  say("staircase 100%"); tone(1000, 1000, 1200, 1.00f, true, true); silence(600);

  // 3. Sweep. Reveals clocking and sample-rate problems as warbling or
  //    beating, and shows where the little speaker rolls off at each end.
  say("sweep 100 Hz - 8 kHz");
  tone(100, 8000, 5000, 0.25f, true, true);
  silence(600);

  // 4. Channel check. The amp mixes (L+R)/2, so BOTH bursts should be
  //    audible and equally loud. If one is silent, SD is selecting a single
  //    channel instead of the mono mix.
  say("left channel only");
  tone(660, 660, 1500, 0.25f, true, false);
  silence(300);
  say("right channel only");
  tone(660, 660, 1500, 0.25f, false, true);
  silence(300);

  // 5. Low tone. Bass draws the most current, so this is where a sagging
  //    breadboard rail shows up as stutter or reset.
  say("120 Hz, 50% (current draw)");
  tone(120, 120, 2500, 0.50f, true, true);

  Serial.println("--- pass end ---\n");
  silence(1500);
}
