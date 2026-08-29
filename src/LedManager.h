#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "PinConfig.h"

#define NUM_LEDS 1

class LedManager
{
public:
    LedManager();
    void begin();
    void setColor(CRGB color);
    void flash(CRGB color, unsigned long duration);
    void setSteady(CRGB color);
    void blinkGreen();
    void blinkBlue();
    void blinkRed();
    void blinkPurple();
    void blinkWhite();
    void stopBlink();
    void off();
    void update();

private:
    CRGB leds[NUM_LEDS];
    CRGB steadyColor = CRGB::Black;
    CRGB flashColor = CRGB::Black;
    CRGB lastShown = CRGB::Black;
    unsigned long flashUntil = 0;
    bool blinking = false;
    CRGB blinkColor;
    bool blinkState = false;
    unsigned long lastBlinkTime = 0;
    const unsigned long blinkInterval = 500;
};
