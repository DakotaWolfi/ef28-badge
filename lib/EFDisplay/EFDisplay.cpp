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
 * @author Irah / DarkRat
 */

//#include <EFConfig.h>
#include <GlitchLine.h>
#include <EFLogging.h>
#include <EFBoard.h>
#include <EFLed.h>

#include <U8g2lib.h>
#include <SPI.h>

#include <vector>
#include <array>
#include <string>
#include <sstream>

#include "EFDisplay.h"


#define OLED_CS    5   // Chip Select
#define OLED_DC    6   // Data/Command
#define OLED_RESET 7   // Reset
#define OLED_MOSI  17  // MOSI
#define OLED_SCLK  18  // SCLK

// --- AUDIO / NOISE CONFIG ---
#define AUDIO_PIN   4
#define NOISE_PIN   12   // <- use exactly one floating pin


#include <math.h>   // for fabsf
#include <esp_system.h>   // for esp_random()

float g_audio_level = 0.0f;

static float audio_dc  = 2048.0f;   // DC estimate (midpoint for 12-bit ADC)
static float audio_env = 0.0f;      // smoothed envelope
// g_audio_level is already defined above by you: float g_audio_level = 0.0f;

static uint16_t counter = 0;
//static bool glitch_anim = false;
//static int thick_line = -1;
//static int thin_line = -1;
int battery_update_counter = 0;
int battery_percentage = 0;
float battery_voltage = 0;

// define these near the top of EFDisplay.cpp to avoid magic numbers
// Your coordinate system with U8G2_R3 ends up X: 0..63, Y: 0..127
static constexpr int SCR_W = 64;
static constexpr int SCR_H = 128;

// ensure font is set consistently
static constexpr uint8_t HUD_LINE_H = 8;   // 5x8 font height
static constexpr uint8_t HUD_Y0     = 30;  // first baseline y
//static constexpr uint8_t HUD_MARGIN = 4;   // space below last HUD line
static constexpr uint8_t HUD_LINE5_Y = 85;  // pick what clears the eye outline

bool inMenu = false;


std::vector<GlitchLine*> lines = {};

U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, OLED_CS, OLED_DC, OLED_RESET);

void EFDisplayClass::init() {
    SPI.begin(OLED_SCLK, -1, OLED_MOSI, OLED_CS);
    u8g2.begin();
    u8g2.setDisplayRotation(U8G2_R3);
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.clearBuffer();
    LOG_INFO("Display setup!");

    audioInit();         // <— optional now; enable when you want
    bootupAnimation();
}

void EFDisplayClass::loop() {
    if(inMenu)return;//show the display Loop only when not in the main menu
    u8g2.clearBuffer();
    updatePowerInfo();
    //EFLed.setDragonEye(CRGB(60, 60, 100));
    //EFLed.setDragonMuzzle(CRGB(40, 40, 80));

    if (!hudEnabled) {
        //audioTick();         // <— optional now; enable when you want
        animationTick();
        /*
        if(random(0, 1000) == 0) {
            lines.insert(lines.end(), new GlitchLine());
        }
        */
        // New: small steady chance + occasional bursts
        if (random(0, 180) == 0) {
            lines.push_back(new GlitchLine());
        }
        // rare burst: spawn 3–6 lines at once
        if (random(0, 1200) == 0) {
            int n = random(3, 7);
            while (n--) lines.push_back(new GlitchLine());
        }
        animateGlitchLines();
    }else {
        // HUD mode: lighter background instead of glitch lines
        drawHUD();
        drawHUDStatic(0);
    }
    eyeOutline();
    drawTraces();
    u8g2.sendBuffer();
}


void EFDisplayClass::updatePowerInfo() const {
    battery_update_counter++;
    if(battery_update_counter % 1000 == 0) {
        battery_percentage = EFBoard.getBatteryCapacityPercent();
        battery_voltage = EFBoard.getBatteryVoltage();
        battery_update_counter = 0;
    }
    String batt = "BAT:" + String(battery_percentage) + "%";

    if(!EFBoard.isBatteryPowered()) {
        batt = "USB POWER";
    }else{
        u8g2.drawStr(10, 20, ("PWR:" + String(battery_voltage) + "V").c_str());
    }
    u8g2.drawStr(10, 10, batt.c_str());
}

void EFDisplayClass::animateGlitchLines() const {
    std::vector<GlitchLine*> alive;
    alive.reserve(lines.size());

    for (auto *line : lines) {
        if (!line->isFinished()) {
            line->tick();

            if ((line->getTick() ^ 0x5A) % 5 == 0) {
                alive.push_back(line);
                continue;
            }

            for (int t = 0; t < line->getThickness(); ++t) {
                int baseY = line->getPosition() + t;

                // CLAMP TO 0..127 (not 0..63)
                int y = baseY + (int)random(-1, 2);
                if (y < 0) y = 0;
                if (y >= SCR_H) y = SCR_H - 1;

                int x = 0;
                while (x < SCR_W) {                 // width is 64
                    int seg = random(4, 16);
                    int gap = random(2, 10);
                    int jitterX = random(-1, 2);

                    int sx = x + jitterX;
                    if (sx < 0) sx = 0;
                    if (sx + seg > SCR_W) seg = SCR_W - sx;

                    if (seg > 0) {
                        u8g2.drawHLine(sx, y, seg);

                        if (random(0, 10) < 3) {
                            int ty = y + ((random(0, 2) == 0) ? -1 : +1);
                            // CLAMP TO 0..127
                            if (ty >= 0 && ty < SCR_H) {
                                int seg2 = seg - random(1, 4);
                                if (seg2 > 0) u8g2.drawHLine(sx, ty, seg2);
                            }
                        }
                    }

                    x += seg + gap;
                }

                // noise around the line — CLAMP TO 0..127
                int noiseN = random(2, 6);
                while (noiseN--) {
                    int nx = random(0, SCR_W);
                    int ny = y + random(-2, 3);
                    if (ny >= 0 && ny < SCR_H) {
                        u8g2.drawPixel(nx, ny);
                    }
                }
            }

            alive.push_back(line);
        } else {
            delete line;
        }
    }

    lines.swap(alive);
}

void EFDisplayClass::drawTraces() const {
    const int offset[] = {7, 88};
    const std::vector<std::array<int, 2>> points = {
            {0, 0},
            {7, 7},
            {47, 7},
            {54, 0},
    };
    drawShape(offset, points);

    const std::vector<std::array<int, 2>> points2 = {
            {18, 7},
            {5, 20},
            {5, 34},
            {10, 39},
            {16, 39},
            {24, 31},
    };
    drawShape(offset, points2);

    const std::vector<std::array<int, 2>> points3 = {
            {32, 7},
            {47, 22},
            {47, 34},
            {52, 39},
    };
    drawShape(offset, points3);

    const std::vector<std::array<int, 2>> points4 = {
            {5, 34},
            {5, 44},
    };
    drawShape(offset, points4);
}

void EFDisplayClass::drawShape(const int *offset, const std::vector<std::array<int, 2>> &points) const {
    for(int i = 0; i < points.size() - 1; i++){
        auto p = points[i];
        auto in = points[i + 1];
        u8g2.drawLine(p[0] + offset[0], p[1] + offset[1], in[0] + offset[0], in[1] + offset[1]);
    }
}

void EFDisplayClass::eyeOutline() const {
    int point_size = 5;
    int8_t points[5][2] = {
            {0, 0},
            {17, 17},
            {3, 31},
            {-25, 31},
            {-25, 21},
    };

    int x_offset = 44;
    int y_offset = 43;
    for(int p=0; p < point_size; p++){
        int in = (p + 1) % point_size;
        u8g2.drawLine(points[p][0] + x_offset, points[p][1] + y_offset, points[in][0] + x_offset, points[in][1] + y_offset);
    }
}

void EFDisplayClass::DisplayMenu(const String& text,bool showMenu){
    if(showMenu){
        inMenu = true;
        u8g2.clearBuffer(); //clear screen
        updatePowerInfo();  //print power status
        drawMultiline(0, 30, text.c_str());
        u8g2.sendBuffer();
    }else{
        inMenu = false;
    }
 }

 void EFDisplayClass::drawMultiline(int x, int y, const char *text) {
  int lineHeight = u8g2.getMaxCharHeight() + 1;
  std::string s(text);
  std::stringstream ss(s);
  std::string line;
  int i = 0;
  while (std::getline(ss, line, '\n')) {
    u8g2.drawStr(x, y + i * lineHeight, line.c_str());
    i++;
  }
}

void EFDisplayClass::animationTick() const {
    counter++;
    if(counter > 1000) counter = 0;
}

void EFDisplayClass::bootupAnimation() {
    int binary_pattern = {};

    const int offset[] = {7, 88};
    const std::vector<std::array<int, 2>> points = {
            {0, 0},
            {7, 7},
            {47, 7},
            {54, 0},
    };
    drawShape(offset, points);
    u8g2.sendBuffer();
    delay(500);


    const std::vector<std::array<int, 2>> points2 = {
            {18, 7},
            {5, 20},
            {5, 34},
            {10, 39},
            {16, 39},
            {24, 31},
    };
    drawShape(offset, points2);
    u8g2.sendBuffer();
    delay(500);

    const std::vector<std::array<int, 2>> points3 = {
            {32, 7},
            {47, 22},
            {47, 34},
            {52, 39},
    };
    drawShape(offset, points3);
    u8g2.sendBuffer();
    delay(500);

    const std::vector<std::array<int, 2>> points4 = {
            {5, 34},
            {5, 44},
    };
    drawShape(offset, points4);
    u8g2.sendBuffer();
    delay(500);

    eyeOutline();
    u8g2.sendBuffer();
    delay(200);

    for(int i = 0; i < 255; i++) {
        EFLed.setDragonEye(CRGB(i, 0, 0));
        delay(5);
    }

    EFLed.setDragonEye(CRGB(60, 60, 120));
    delay(2000);
/*
    for(int i = 0; i< 12; i++) {
        u8g2.drawStr(0, 10, "000001111100000");
    }
    u8g2.drawStr(0, 20, "111110000011111");
    */
    u8g2.sendBuffer();
}

void EFDisplayClass::audioInit() {
    pinMode(AUDIO_PIN, INPUT);
    pinMode(NOISE_PIN, INPUT);   // leave floating
}

void EFDisplayClass::audioTick() {
    int s = analogRead(AUDIO_PIN);
    audio_dc  = 0.995f * audio_dc + 0.005f * (float)s;
    float ac  = (float)s - audio_dc;
    float mag = fabsf(ac);

    const float ATTACK  = 0.20f;
    const float RELEASE = 0.01f;
    if (mag > audio_env) audio_env = ATTACK  * mag + (1.0f - ATTACK)  * audio_env;
    else                 audio_env = RELEASE * mag + (1.0f - RELEASE) * audio_env;

    // ---- single-pin EM hiss (or hardware RNG fallback) ----
    float hiss = 0.0f;

    // Try ADC read from the chosen floating pin
    int nv = analogRead(NOISE_PIN);          // if not ADC-capable, may be 0/4095 or noisy anyway
    hiss = (nv & 1023) * (1.0f / 1023.0f);   // normalize to ~0..1

    // If that read looks “stuck”, blend in ESP32 hardware RNG (cheap & good entropy)
    #ifdef ARDUINO_ARCH_ESP32
    if (nv == 0 || nv == 4095) {
    uint32_t r = esp_random();
    hiss = (float)(r & 1023) * (1.0f / 1023.0f);
}
    #endif

    float envNorm = audio_env * (1.0f / 1024.0f);
    if (envNorm < 0) envNorm = 0; else if (envNorm > 1) envNorm = 1;

    // Keep hiss subtle; it just prevents dead-flat silence
    const float HISS_MIX = 0.15f;            // tune 0.05..0.25 to taste
    float lvl = (1.0f - HISS_MIX) * envNorm + HISS_MIX * hiss;
    if (lvl < 0) lvl = 0; else if (lvl > 1) lvl = 1;

    g_audio_level = lvl;
}

void EFDisplayClass::setHUDEnabled(bool on) {
    hudEnabled = on;
}

void EFDisplayClass::setHUDLine(uint8_t idx, const String& text) {
    if (idx < 5) hudLines[idx] = text;
}

void EFDisplayClass::clearHUD() {
    for (auto &l : hudLines) l = "";
}

String EFDisplayClass::truncateToWidth(const String& s, uint8_t maxW) {
    // ensure font is selected for width calc
    u8g2.setFont(u8g2_font_5x8_tr);
    if (u8g2.getStrWidth(s.c_str()) <= maxW) return s;

    String out = s;
    // leave room for ellipsis "…"
    const char* ell = "\xE2\x80\xA6"; // UTF-8 ellipsis
    while (out.length() > 0 && u8g2.getStrWidth((out + ell).c_str()) > maxW) {
        out.remove(out.length()-1);
    }
    return out + ell;
}

void EFDisplayClass::drawHUD() const {
    if (!hudEnabled) return;

    u8g2.setFont(u8g2_font_5x8_tr);
    uint8_t y = HUD_Y0;

    // draw lines 0..3 with uniform spacing
    for (int i = 0; i < 4; ++i) {
        String line = truncateToWidth(hudLines[i], SCR_W);
        if (hudLines[i].length() > 0) {
            u8g2.drawStr(0, y, line.c_str());
        }
        y += HUD_LINE_H;
    }

    // draw line 4 (the 5th line) at a custom Y to avoid the eye
    if (hudLines[4].length() > 0) {
        // clamp just in case
        uint8_t y5 = (HUD_LINE5_Y < SCR_H) ? HUD_LINE5_Y : (SCR_H - 1);
        String line = truncateToWidth(hudLines[4], SCR_W);
        u8g2.drawStr(0, y5, line.c_str());
    }
}

void EFDisplayClass::drawHUDStatic(uint8_t yStart) const {
    if (yStart >= SCR_H) return;
    // Lightweight “TV static”: a handful of random pixels + short dashes per frame.
    // Tuned to be cheap enough to run every loop.

    // Seed varies per frame, but we just use Arduino random()
    // DOTS: ~150 sparse pixels
    const int DOTS = 150;
    for (int i = 0; i < DOTS; ++i) {
        int x = random(0, SCR_W);
        int y = random(yStart, SCR_H);
        u8g2.drawPixel(x, y);
    }

    // Dashes: a few short horizontal jitter lines
    const int DASHES = 10;
    for (int i = 0; i < DASHES; ++i) {
        int y  = random(yStart, SCR_H);
        int x  = random(0, SCR_W - 2);
        int w  = random(2, 8);
        u8g2.drawHLine(x, y, w);
        // occasional echo line one pixel up/down
        if (random(0, 4) == 0) {
            int y2 = y + (random(0, 2) ? 1 : -1);
            if (y2 >= (int)yStart && y2 < SCR_H) {
                int w2 = max(1, (int)(w - random(1, 3)));

                u8g2.drawHLine(x, y2, w2);
            }
        }
    }
}


#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_EFDISPLAY)
EFDisplayClass EFDisplay;
#endif
