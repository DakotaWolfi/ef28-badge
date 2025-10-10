#include "GlitchLine.h"
#include <Arduino.h>


GlitchLine::GlitchLine() {
    frame = 0;
    position = 0;
    direction = (random(10)%2==0)? 1 : -1;
    speed = random(1,8);
    thickness = random(1,5);
}

void GlitchLine::tick() {
    frame++;

    // occasionally change effective speed on the fly
    // (every ~16 frames, with 50% chance, re-roll speed 1..8)
    if ((frame & 0x0F) == 0 && random(0, 2) == 0) {
        speed = random(1, 9); // 1..8 inclusive
    }

    if (frame % speed == 0) {
        // sometimes jump 2 rows at once to create jolts
        int step = 1;
        if (random(0, 10) < 2) { // 20% chance
            step = 1 + (frame & 0x01); // either 1 or 2
        }
        position += step;
    }

    // very rare micro-stutter (no movement this tick) – already covered by modulo,
    // but we could add a forced stutter:
    // if (random(0, 60) == 0) { /* do nothing extra */ }
}


bool GlitchLine::isFinished() {
    if(position > 127) {
        return true;
    }
    return false;
}

int GlitchLine::getPosition() {
    if(direction > 0) {
        return position;
    }else{
        return 127 - position;
    }
}

int GlitchLine::getTick() {
    return frame;
}

int GlitchLine::getThickness() {
    return thickness;
}
