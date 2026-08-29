#include "LedManager.h"

LedManager::LedManager() {}

void LedManager::begin()
{
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(50);
    steadyColor = CRGB::Black;
    leds[0] = CRGB::Black;
    lastShown = CRGB::Black;
    FastLED.show();
}

// 兼容旧调用：一次短暂的颜色闪烁
void LedManager::setColor(CRGB color)
{
    flash(color, 150);
}

// 瞬闪：覆盖稳态颜色一段时间，结束后回到稳态颜色
void LedManager::flash(CRGB color, unsigned long duration)
{
    flashColor = color;
    flashUntil = millis() + duration;
}

// 设置稳态颜色（比如正常运行显示蓝色）
void LedManager::setSteady(CRGB color)
{
    steadyColor = color;
    blinking = false;
}

void LedManager::blinkGreen()
{
    if (blinking && blinkColor == CRGB::Green) return;
    blinking = true; blinkColor = CRGB::Green; blinkState = true;
    lastBlinkTime = millis(); leds[0] = blinkColor; FastLED.show();
}

void LedManager::blinkBlue()
{
    if (blinking && blinkColor == CRGB::Blue) return;
    blinking = true; blinkColor = CRGB::Blue; blinkState = true;
    lastBlinkTime = millis(); leds[0] = blinkColor; FastLED.show();
}

void LedManager::blinkRed()
{
    if (blinking && blinkColor == CRGB::Red) return;
    blinking = true; blinkColor = CRGB::Red; blinkState = true;
    lastBlinkTime = millis(); leds[0] = blinkColor; FastLED.show();
}

void LedManager::blinkPurple()
{
    if (blinking && blinkColor == CRGB::Purple) return;
    blinking = true; blinkColor = CRGB::Purple; blinkState = true;
    lastBlinkTime = millis(); leds[0] = blinkColor; FastLED.show();
}

void LedManager::blinkWhite()
{
    if (blinking && blinkColor == CRGB::White) return;
    blinking = true; blinkColor = CRGB::White; blinkState = true;
    lastBlinkTime = millis(); leds[0] = blinkColor; FastLED.show();
}

void LedManager::stopBlink() { blinking = false; }

void LedManager::off()
{
    steadyColor = CRGB::Black;
    flashUntil = 0;
    blinking = false;
}

void LedManager::update()
{
    unsigned long now = millis();
    CRGB target;
    if (blinking)
    {
        if (now - lastBlinkTime >= blinkInterval)
        {
            lastBlinkTime = now;
            blinkState = !blinkState;
        }
        target = blinkState ? blinkColor : CRGB::Black;
    }
    else if (flashUntil && now < flashUntil)
    {
        target = flashColor;
    }
    else
    {
        flashUntil = 0;
        target = steadyColor;
    }
    if (target != lastShown)
    {
        leds[0] = target;
        lastShown = target;
        FastLED.show();
    }
}
