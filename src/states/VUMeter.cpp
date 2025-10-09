// MIT License
//
// Copyright 2024 Eurofurence e.V.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the “Software”),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

/**
 * @author 32
 */

#include <EFLed.h>
#include <EFLogging.h>

#include "FSMState.h"

#define AUDIO_PIN 4
#define DRAGON_LED_NUM 6
#define DRAGON_SLOW 3
#define BAR_LED_NUM 11
#define LEDS_NUM DRAGON_LED_NUM + BAR_LED_NUM
#define MIN_LEVEL 40
#define LEVEL_DIVISOR 4

uint8_t dragon_hue = 130;
const int samples = 500;


const char *VUMeter::getName()
{
    return "VUMeter";
}

bool VUMeter::shouldBeRemembered()
{
    return true;
}
void VUMeter::entry()
{
    this->tick = 0;
}

int sMin = 4096; // make shure to be outside the range from 0-4095
int sMax = -1;
int sMaxLevel = MIN_LEVEL; // provide minimal level not to
int nl = 32;

CRGB leds[LEDS_NUM];

void VUMeter::run()
{
    sMin = 4096; // make shure to be outside the range from 0-4095
    sMax = -1;

    // take "samples" Samples and determin min and max
    for (int s = 0; s < samples; s++)
    {
        int sample = analogRead(AUDIO_PIN);

        // Update max and min values
        if (sample < sMin)
        {
            sMin = sample;
        }
        if (sample > sMax)
        {
            sMax = sample;
        }
    }

    // Calculate peak-to-peak range (this is the signal strength)
    int peakToPeak = sMax - sMin;

    // Determin Maxlevel recorded
//    if (sMaxLevel < peakToPeak)
//        sMaxLevel = peakToPeak;

    // Map the peak-to-peak value to a range of 0 to NUM_LEDS * 256 - 1
    uint16_t n = map(peakToPeak, 0, sMaxLevel, 0, 256 * BAR_LED_NUM - 1);

//    LOGF_DEBUG("(VUMeter) Min: %d Max: %d peakToPeak: %d Maxlevel: %d Mapped: %d\r\n", sMin, sMax, peakToPeak, sMaxLevel, n);

    // VU Meter
    for (int i = 0; i < BAR_LED_NUM; ++i)
    {
        leds[LEDS_NUM - 1 - i] = CHSV((uint8_t)(this->tick + i * (256 / (BAR_LED_NUM + 1)) % 256), 255,
            (uint8_t)(n >= 256 * (i + 1) ? 255 : n >= 256 * (i) ? n % 256 : 0));
        // LOGF_DEBUG("(VUMeter) LED: %d H: %d S: %d V: %d\r\n", LEDS_NUM - 1 - i, (this->tick + i * 21) % 256, 255, (uint8_t)(n >= 256 * (i + 1) ? 255 : n >= 256 * (i) ? n % 256 : 0));

    }

    // Dragon Face
    for (int i = 0; i < DRAGON_LED_NUM; ++i) {
         leds[i] = CHSV(dragon_hue, 255, (uint8_t)( 255 - (((i * DRAGON_SLOW + tick) * (256 / (DRAGON_LED_NUM * DRAGON_SLOW)) ) % 256 )));
    }

    EFLed.setAll(leds);

    this->tick++;

    sMaxLevel = sMaxLevel + ((peakToPeak * LEVEL_DIVISOR) - sMaxLevel) * 0.01;
    if (sMaxLevel < MIN_LEVEL ) sMaxLevel = MIN_LEVEL; // Enshure staying at or above minlevel

}

std::unique_ptr<FSMState> VUMeter::touchEventFingerprintShortpress()
{
    if (this->isLocked())
    {
        return nullptr;
    }

    return std::make_unique<MenuMain>();
}

std::unique_ptr<FSMState> VUMeter::touchEventFingerprintLongpress()
{
    return this->touchEventFingerprintShortpress();
}

std::unique_ptr<FSMState> VUMeter::touchEventFingerprintRelease()
{
    if (this->isLocked())
    {
        return nullptr;
    }

    dragon_hue = (dragon_hue + 31) % 256;

    return nullptr;
}

std::unique_ptr<FSMState> VUMeter::touchEventAllLongpress()
{
    this->toggleLock();
    return nullptr;
}

long map_l(long x, long in_min, long in_max, long out_min, long out_max) {
    const long run = in_max - in_min;
    if(run == 0){
        log_e("map(): Invalid input range, min == max");
        return -1; // AVR returns -1, SAM returns 0
    }
    const long rise = out_max - out_min;
    const long delta = x - in_min;
    return (delta * rise) / run + out_min;
}
