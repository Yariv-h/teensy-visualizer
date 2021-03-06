#define DEBUG
//#define DEBUG_SERIAL_WAIT
#include "bs_debug.h"

// select one of these CONTROL_* defines
// TODO: error if one and only one of these is not set
#define CONTROL_LED
//#define CONTROL_EL

#include <stdlib.h>

#include <Audio.h>
#include <FastLED.h>
#include <SD.h>
#include <SPI.h>
#include <SerialFlash.h>
#include <Wire.h>

#define VOLUME_KNOB A2
#define LED_CLOCK_PIN 0  // yellow wire on my dotstars
#define LED_DATA_PIN 1  // green wire on my dotstars
#define SDCARD_CS_PIN 10
#define SPI_MOSI_PIN 7  // alt pin for use with audio board
#define RED_LED 13
#define SPI_SCK_PIN 14  // alt pin for use with audio board

#define LED_CHIPSET APA102
#define LED_MODE BGR

#define DEFAULT_BRIGHTNESS 52 // TODO: read from SD

AudioInputI2S i2s1;  // xy=139,91
AudioOutputI2S i2s2; // xy=392,32
AudioAnalyzeFFT1024 fft1024;
AudioConnection patchCord1(i2s1, 0, i2s2, 0);
AudioConnection patchCord2(i2s1, 0, fft1024, 0);
AudioControlSGTL5000 audioShield; // xy=366,225

// each frequencyBin = ~43Hz
const int minBin = 1;   // skip 0-43Hz. it's too noisy
const int maxBin = 373; // skip over 16kHz

// this looks best when they are even multiples of each other and numLEDs, but it should work if they aren't
#ifdef CONTROL_EL
// TOODO: put this back to 8 and 1
const int numFreqBands = 8;
const int numOutputs = 1;

// we don't want all the lights to be on at once
const int maxOn = 5;

// the shortest amount of time to leave an output on
// TODO: tune this!
const unsigned int minOnMs = 184; // 118? 150? 184? 200? 250?

#endif

#ifdef CONTROL_LED
const int numFreqBands = 11;
const int numOutputs = 11;

// we don't want all the lights to be on at once
const int maxOn = numOutputs * 3 / 4;

// the shortest amount of time to leave an output on
// TODO: tune this!
const unsigned int minOnMs = 337; // 118? 150? 184? 200? 250?

#endif

// we could put many of these variables under a ifdef CONTROL_EL, but its fine having extra globals around for now
const int ledsPerSpreadOutput = 1;
const int numSpreadOutputs = numOutputs * ledsPerSpreadOutput;

// 30 leds per meter
// 3 meters (90) works without a level shifter
// 4 meters (120) works with a level shifter
// we had 5 meters (150), but we need more power
const int numLEDs = 120;

int freqBands[numFreqBands];
CHSV frequencyColors[numFreqBands];

// frequencyColors are stretched/squished to fit this (squishing being what you probably want)
CHSV outputs[numOutputs];

// outputs are stretched to fit this
CHSV outputsStretched[numSpreadOutputs];

// outputs repeats across this
CRGB leds[numLEDs];

// TODO: support multiple el sequencers
unsigned char el_output[1] = {0};

int numOn = 0;

// slide the leds over 1 every X frames
const int frames_per_shift = 174;  // 174 frames * 11.5 ms/frame = 2001ms

// how close a sound has to be to the loudest sound in order to activate
const float activate_difference = 0.98;
// simple % decrease
const float decayMax = 0.98;
// set a floor so that decayMax doesn't go too low
const float minMaxLevel = 0.15 / activate_difference;

// how much of the neighbor's max to consider when deciding when to turn on
const float scale_neighbor_max = 0.9;
// how much of all the other bin's max to consider when deciding when to turn on
const float scale_overall_max = 0.4;
// how much of the neighbor's max to consider when deciding how bright to be
const float scale_neighbor_brightness = 1.1;

// keep track of the max volume for each frequency band (slowly decays)
float maxLevel[numFreqBands];
// keep track of the current levels. this is a sum of multiple frequency bins.
float currentLevel[numFreqBands];

// going through the levels loudest to quietest makes it so we can ensure the loudest get turned on ASAP
int sortedLevelIndex[numFreqBands];

// keep track of when to turn lights off so they don't flicker
unsigned long turnOffMsArray[numFreqBands];

// used to keep track of framerate // TODO: remove this if debug mode is disabled
unsigned long lastUpdate = 0;

/* sort the levels normalized against their max
 *
 * with help from https://phoxis.org/2012/07/12/get-sorted-index-orderting-of-an-array/
 */
static int compare_levels(const void *a, const void *b) {
  int aa = *((int *)a), bb = *((int *)b);
  return (currentLevel[bb] / maxLevel[bb]) - (currentLevel[aa] / maxLevel[aa]);
}

float FindE(int bands, int minBin, int maxBin) {
  // https://forum.pjrc.com/threads/32677-Is-there-a-logarithmic-function-for-FFT-bin-selection-for-any-given-of-bands?p=133842&viewfull=1#post133842
  float increment = 0.1, eTest, n;
  int b, count, d;

  for (eTest = 1; eTest < maxBin; eTest += increment) { // Find E through brute force calculations
    count = minBin;
    for (b = 0; b < bands; b++) { // Calculate full log values
      n = pow(eTest, b);
      d = int(n + 0.5);
      count += d;
    }

    if (count > maxBin) { // We calculated over our last bin
      eTest -= increment; // Revert back to previous calculation increment
      increment /= 10.0;  // Get a finer detailed calculation & increment a decimal point lower

      if (increment < 0.0000001) { // Ran out of calculations. Return previous E. Last bin will be lower than (bins-1)
        return (eTest - increment);
      }
    } else if (count == maxBin) { // We found the correct E
      return eTest;               // Return calculated E
    }
  }

  return 0; // Return error 0
}

void setupFFTBins() {
  // https://forum.pjrc.com/threads/32677-Is-there-a-logarithmic-function-for-FFT-bin-selection-for-any-given-of-bands?p=133842&viewfull=1#post133842
  float e, n;
  int count = minBin, d;

  e = FindE(numFreqBands, minBin, maxBin); // Find calculated E value

  if (e) {                           // If a value was returned continue
    Serial.printf("E = %4.4f\n", e); // Print calculated E value
    Serial.printf("  i  low high\n");
    for (int b = 0; b < numFreqBands; b++) { // Test and print the bins from the calculated E
      n = pow(e, b);
      d = int(n + 0.5);

      Serial.printf("%3d ", b);

      Serial.printf("%4d ", count); // Print low bin
      freqBands[b] = count;

      count += d - 1;
      Serial.printf("%4d\n", count); // Print high bin

      count++;
    }
  } else {
    Serial.println("Error calculating E"); // Error, something happened
    while (1)
      ;
  }
}

void setupSD() {
  // slave select pin for SPI
  pinMode(SDCARD_CS_PIN, OUTPUT);

  SPI.begin(); // should this be here?

  // read values from the SD card using IniFile
}

void setupLights() {
  // TODO: clock select pin for FastLED to OUTPUT like we do for the SDCARD?
  FastLED.addLeds<LED_CHIPSET, LED_DATA_PIN, LED_CLOCK_PIN, LED_MODE>(leds, numLEDs).setCorrection(TypicalSMD5050);

  // TODO: what should this be set to?
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 450);

  FastLED.setBrightness(DEFAULT_BRIGHTNESS); // TODO: read this from the SD card

  FastLED.clear();
  FastLED.show();

  // TODO: turn off onboard LED
}

void setupAudio() {
  // Audio requires memory to work. I haven't seen this go over 11
  AudioMemory(12);

  // Enable the audio shield and set the output volume.
  audioShield.enable();
  audioShield.muteHeadphone(); // to avoid any clicks
  audioShield.inputSelect(AUDIO_INPUT_MIC);
  audioShield.volume(0.5);
  audioShield.micGain(60); // was 63, then 40  // 0-63 // TODO: tune this

  //audioShield.audioPreProcessorEnable(); // todo: pre or post?

  // bass, mid_bass, midrange, mid_treble, treble
  // TODO: tune this. maybe read from SD card
  //audioShield.eqSelect(GRAPHIC_EQUALIZER);
  // audioShield.eqBands(-0.80, -0.75, -0.50, 0.50, 0.80);  // the great northern
  // audioShield.eqBands(-0.5, -.2, 0, .2, .5);  // todo: tune this
  // audioShield.eqBands(-0.80, -0.10, 0, 0.10, 0.33);  // todo: tune this
  //audioShield.eqBands(0.0, 0.0, 0.0, 0.1, 0.33); // todo: tune this

  audioShield.unmuteHeadphone(); // for debugging

  // setup array for sorting
  for (int i = 0; i < numFreqBands; i++) {
    sortedLevelIndex[i] = i;
  }
}

void setup() {
  debug_serial(115200, 2000);

  Serial.println("Setting up...");

  // setup SPI for the Audio board (which has an SD card reader)
  SPI.setMOSI(SPI_MOSI_PIN);
  SPI.setSCK(SPI_SCK_PIN);

  setupSD();

  // TODO: read SD card here to configure things

  // TODO: read numOutputs from the SD card

#ifdef CONTROL_LED
  setupLights();
#endif

  setupAudio();

  setupFFTBins();

#ifdef CONTROL_EL
  // https://www.pjrc.com/teensy/td_uart.html
  Serial1.begin(9600);  // RX=0 (yellow), TX=1 (green)
  // TODO: support multiple serial connections
  //Serial2.begin(9600);  // RX=9, TX=10
  //Serial3.begin(9600);  // RX=7, TX=8

  Serial.print("Waiting for Serial1... ");
  while (!Serial1) {
    ; // wait for serial port to connect
  }
  Serial.println("DONE");
#endif

  Serial.println("Starting...");
}

// we could/should pass fft and level as args
float updateLevelsFromFFT() {
  // https://forum.pjrc.com/threads/32677-Is-there-a-logarithmic-function-for-FFT-bin-selection-for-any-given-of-bands

  // read the FFT frequencies into numOutputs levels
  // music is heard in octaves, but the FFT data
  // is linear, so for the higher octaves, read
  // many FFT bins together.

  float overall_max = 0;

  for (int i = 0; i < numFreqBands - 1; i++) {
    currentLevel[i] = fft1024.read(freqBands[i], freqBands[i + 1] - 1);

    if (currentLevel[i] > overall_max) {
      overall_max = currentLevel[i];
    }
  }

  // the last level always goes to maxBin
  currentLevel[numFreqBands - 1] = fft1024.read(freqBands[numFreqBands - 1], maxBin);

  return overall_max;
}

float getLocalMaxLevel(int i, float scale_neighbor, float overall_max, float scale_overall_max) {
  float localMaxLevel = maxLevel[i];

  // check previous level if we aren't the first level
  if (i != 0) {
    localMaxLevel = max(localMaxLevel, maxLevel[i - 1] * scale_neighbor);
  }

  // check the next level if we aren't the last level
  if (i != numFreqBands) {
    localMaxLevel = max(localMaxLevel, maxLevel[i + 1] * scale_neighbor);
  }
  
  // check all the other bins, too
  if (overall_max and scale_overall_max) {
    localMaxLevel = max(localMaxLevel, overall_max * scale_overall_max);
  }

  return localMaxLevel;
}

void updateFrequencyColors() {
  // read FFT frequency data into a bunch of levels. assign each level a color and a brightness
  float overall_max = updateLevelsFromFFT();

  float local_max = 0;

  // turn off any quiet levels. we do this before turning any lights on so that our loudest frequencies are most
  // responsive
  for (int i = 0; i < numFreqBands; i++) {
    // update maxLevel
    // TODO: don't just track max. track the % change. then do something with stddev of neighbors?
    if (currentLevel[i] > maxLevel[i]) {
      maxLevel[i] = currentLevel[i];
    }

    if (! frequencyColors[i].value) {
      // this light is already off
      continue;
    }

    local_max = getLocalMaxLevel(i, scale_neighbor_max, overall_max, scale_overall_max);

    // turn off if current level is less than the activation threshold
    // TODO: i'm not sure i like this method anymore. its too arbitrary
    if (currentLevel[i] < local_max * activate_difference) {
      // the output should be off
      if (millis() < turnOffMsArray[i]) {
        // the output has not been on for long enough to prevent flicker
        // leave it on but reduce brightness at 2x the rate we reduce maxLevel
        // TODO: should the brightness be tied to the currentLevel somehow? that might make it too random looking
        // we were using "video" scaling to fade (meaning: never fading to full black), but CHSV doesn't have a fadeLightBy method
        // frequencyColors[i].fadeLightBy(int((1.0 - decayMax) * 4.0 * 255));

        frequencyColors[i].value *= decayMax;
        frequencyColors[i].value *= decayMax;

        // make sure we don't turn off while fading
        // TODO: tune this minimum?
        if (frequencyColors[i].value < 1) {
          frequencyColors[i].value = 1;
        }
      } else {
        // the output has been on for at least minOnMs and is quiet now
        // if it is on, dim it quickly to off
        // TODO: tune this.
        if (frequencyColors[i].value > 16) {
          frequencyColors[i].value -= 16;
        } else {
          frequencyColors[i].value = 0;
          numOn -= 1;
        }
      }
    }
  }

  // sort the levels normalized against their max
  // this allows us to prioritize turning on for the loudest sounds
  qsort(sortedLevelIndex, numFreqBands, sizeof(float), compare_levels);

  // turn on up to maxOn loud levels in order of loudest to quietest
  for (int j = 0; j < numFreqBands; j++) {
    int i = sortedLevelIndex[j];

    local_max = getLocalMaxLevel(i, scale_neighbor_max, overall_max, scale_overall_max);

    // check if current is close to the last max (also check the neighbor maxLevels)
    if (currentLevel[i] >= local_max * activate_difference) {
      // this light should be on!
      if (numOn >= maxOn) {
        // except we already have too many lights on! don't do anything since this light is already off
        // don't break the loop because we still want to decay max level and process other lights
      } else {
        // we have room for the light! turn it on

        // if it isn't already on, increment numOn
        if (!frequencyColors[i].value) {
          // track to make sure we don't turn too many lights on. some configurations max out at 6.
          // we don't do this every time because it could have already been on, but now we made it brighter
          numOn += 1;
        }

        // map(value, fromLow, fromHigh, toLow, toHigh)
        int color_hue = map(i, 0, numFreqBands, 0, 255);
        // TODO: what should saturation be? maybe not 255
        // use 255 as the max brightness. if that is too bright, FastLED.setBrightness can be changed in setup to reduce
        // what 255 does

        // look at neighbors and use their max for brightness if they are louder (but don't be less than 10% on!)
        // TODO: s-curve? i think FastLED actually does a curve for us
        // TODO: what should the min be? should we limit how fast it moves around by including frequencyColors[i].value here?
        // notie how we getLocalMax but exclude the overall volume. we only want that for on/off. if we include it here everything flickers too much
        int color_value = constrain(int(currentLevel[i] / getLocalMaxLevel(i, scale_neighbor_max, 0, 0) * 255), 25, 255);

        // https://github.com/FastLED/FastLED/wiki/FastLED-HSV-Colors#color-map-rainbow-vs-spectrum
        // HSV makes it easy to cycle through the rainbow
        // TODO: color from a pallet instead.
        // TODO: what saturation?
        frequencyColors[i] = CHSV(color_hue, 255, color_value);

        // make sure we stay on for a minimum amount of time
        // if we were already on, extend the time that we stay on
        turnOffMsArray[i] = millis() + minOnMs;
      }
    }

    // decay maxLevel
    // TODO: tune this. not sure a simple % decay is a good idea. might be better to subtract a fixed amount instead
    maxLevel[i] = maxLevel[i] * decayMax;

    // don't let the max ever go to zero otherwise so that it turns off when its quiet instead of activating at a whisper
    if (maxLevel[i] < minMaxLevel) {
      maxLevel[i] = minMaxLevel;
    }
  }

  // debug print
  // TODO: wrap this in an ifdef DEBUG
  for (int i = 0; i < numFreqBands; i++) {
    Serial.print("| ");

    // TODO: maybe do something with parity here? i think i don't have enough lights for that to matter at this point.
    // do some research

    if (frequencyColors[i].value) {
      // Serial.print(leds[i].getLuma() / 255.0);
      // Serial.print(currentLevel[i]);
      // Serial.print(currentLevel[i] / getLocalMaxLevel(i, scale_neighbor_brightness));
      Serial.print(frequencyColors[i].value / 255.0, 2);
    } else {
      Serial.print("    ");
    }
  }
  Serial.print("| ");
  Serial.print(AudioMemoryUsageMax());
  Serial.print(" blocks | Num On=");
  Serial.print(numOn);
  Serial.print(" | ");

  // finish debug print
  Serial.print(millis() - lastUpdate);
  Serial.println("ms");
  lastUpdate = millis();
  Serial.flush();
}

void mapFrequencyColorsToOutputs() {
  for (int i = 0; i < numOutputs; i++) {
    // numFreqBands can be bigger or smaller than numOutputs. a simple map like this works fine if numOutputs >
    // numFreqBands, but if not it skips some
    if (numOutputs == numFreqBands) {
      outputs[i] = frequencyColors[i];
    } else if (numOutputs > numFreqBands) {
      // spread the frequency bands out; multiple LEDs for one frequency
      outputs[i] = frequencyColors[map(i, 0, numOutputs, 0, numFreqBands)];
    } else {
      // shrink frequency bands down. pick the brightest color
      // TODO: I don't think this is working

      // start by setting it to the first available band.
      int bottomFreqId = map(i, 0, numOutputs, 0, numFreqBands);

      outputs[i] = frequencyColors[bottomFreqId];

      int topFreqId = map(i + 1, 0, numOutputs, 0, numFreqBands);
      for (int f = bottomFreqId + 1; f < topFreqId; f++) {
        if (!frequencyColors[f].value) {
          // TODO: dim it some to represent neighbor being off?
          continue;
        }

        if (!outputs[i].value) {
          // output is off, simply set the color as is
          outputs[i] = frequencyColors[f];
        } else {
          // output has multiple frequencies to show
          // TODO: don't just replace with the brighter. instead increase the brightness and shift the color or
          // something to combine outputs[i] and frequencyColors[f]
          if (outputs[i].value < frequencyColors[f].value) {
            outputs[i] = frequencyColors[f];
          }
        }
      }
    }
  }
}

void mapOutputsToSpreadOutputs() {
  for (int i = 0; i < numSpreadOutputs; i++) {
    outputsStretched[i] = outputs[map(i, 0, numSpreadOutputs, 0, numOutputs)];
  }
}

void mapSpreadOutputsToLEDs() {
  // shift increments each frame and is used to slowly modify the pattern
  static unsigned int shift = 0;

  CHSV new_color;

  for (int i = 0; i < numLEDs; i++) {
    int shifted_i = (shift / frames_per_shift + i) % numLEDs;

    if (numSpreadOutputs == numLEDs) {
      new_color = outputsStretched[shifted_i];
    } else {
      // numFreqBands can be bigger or smaller than numOutputs
      // TODO: test this with large and small values of numSpreadOutputs vs numLEDs
      if (numSpreadOutputs < numLEDs) {
        // simple repeat of the pattern
        new_color = outputsStretched[shifted_i % numSpreadOutputs];
      } else {
        // pattern is larger than numLEDs
        new_color = outputsStretched[shifted_i % numLEDs];
      }
    }

    leds[i] = new_color;
  }

  shift++;
}

void mapFrequencyColorsToElOutputs() {
  for (int i = 0; i < numFreqBands; i++) {
    // TODO: better support multiple sequencers
    if (frequencyColors[i].value) {
      bitSet(el_output[i / 8], i % 8);
    } else {
      bitClear(el_output[i / 8], i % 8);
    }
  }
}

void loop() {
  if (fft1024.available()) {
    updateFrequencyColors();

#ifdef CONTROL_LED
    // I'm sure this could be a lot more efficient
    // TODO: get rid of this and just hard code our current repeating code to keep it simpler?
    mapFrequencyColorsToOutputs();
    mapOutputsToSpreadOutputs();
    mapSpreadOutputsToLEDs();

    // using FastLED's delay allows for dithering
    // we sleep for a while inside the loop since we know we don't need to process anything for 11 or 12 ms
    FastLED.delay(9);

    FastLED.show();
#endif

#ifdef CONTROL_EL
    mapFrequencyColorsToElOutputs();

    // send the bytes to their devices
    Serial1.write(el_output[0]);
    //Serial2.write(el_output[1]);
    //Serial3.write(el_output[2]);

    delay(9);

    // make sure we finished writing
    Serial1.flush();
    //Serial2.flush();
    //Serial3.flush();
#endif
  }

  // using FastLED's delay allows for dithering
  // a longer sleep is inside the fft available loop
  FastLED.delay(1);
}
