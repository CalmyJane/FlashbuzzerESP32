#include <map>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Arduino.h>
#include <Preferences.h>  // For non-volatile memory storage (NVS)
#include <FastLED.h>

#include <vector>
#include <FastLED.h>

#define MAX_LEDS 700          // Compile-time ceiling: sizes the pixel buffer, 3 bytes each
#define MAX_TIP_LEDS 200      // Ceiling for the tip section, sizes TipAnimator's scratch buffer
#define DEFAULT_LED_COUNT 600 // Runtime-configurable via the "LedCount" web parameter
#define DEFAULT_TIP_LENGTH 50 // Runtime-configurable via the "Tip_Length" web parameter
#define LED_PIN 16        // Define your LED strip pin
#define DEFAULT_BRIGHTNESS 100  // Default brightness
#define DEFAULT_COLOR CRGB::Red // Default color for the running dots
#define BUTTON_PIN 13  // Pin where the button is connected
#define EXTERNAL_PIN 17  // Pin on the 3pin outside connector (red-, white is GND) to connect an external switch
#define TOUCH_PIN 4  // Pin on the 3pin connector , green cable can be used as touch sensor and connected to some metal thing
#define BUTTON2_PIN 19 // Second pushbutton (D19), cycles the tip animation mode
#define BUTTON3_PIN 21  // Third pushbutton on RX0. Serial is started TX-only so this pin is free as GPIO.
                       // Keep it released while flashing: it shorts the bootloader's RX line.
                       // The USB-serial chip's TX is also hardwired to this pin on most dev boards
                       // and keeps driving it, hence the generous debounce in ButtonHandler.
#define DEFAULT_WIDTH 1

// The whole strip lives in one buffer, split into two regions:
//   leds[0 .. trailLength())         - the running-dot trail
//   leds[trailLength() .. gLedCount) - the tip, wrapped around the balloon
CRGB leds[MAX_LEDS];
uint16_t gLedCount = DEFAULT_LED_COUNT;
uint16_t gTipLength = DEFAULT_TIP_LENGTH;
bool gReverse = false;

// Set by TipAnimator::setMode. FillUp is driven by dots landing on the balloon, so it
// overrides Tip_DotMode to make them stop at the tip, and draws the balloon in the dot's
// colour rather than its own.
bool gTipFillUpActive = false;
CRGB gDotColor = DEFAULT_COLOR;
uint8_t gDotHue = 0; // where Button3's random colour walk currently sits

inline int trailLength() { return (int)gLedCount - (int)gTipLength; }

// Indices are always physical: 0 is the far end from the balloon, gLedCount-1 is the far
// end of the tip. "Reverse" does not remap anything - the balloon stays at the end of the
// strip - it only flips where a dot spawns and which way it travels.

class RunningDot {
public:
    // Constructor: Initialize variables with default color and brightness
    // dotMode: 0 = stop before the tip, 1 = run on through it,
    //          2 = run through it mirrored, meeting itself at the balloon's top point
    RunningDot() : lastUpdateTime(0), speed(30.0f), currentColor(DEFAULT_COLOR), currentBrightness(DEFAULT_BRIGHTNESS), dotWidth(DEFAULT_WIDTH), decayFactor(3.0f), dotMode(0) {}

    // FastLED itself is registered in setup(); this only applies our own defaults
    void begin()
    {
        FastLED.setBrightness(currentBrightness);
    }

    // Trigger a new dot. Normally it starts at the far end and runs towards the balloon;
    // with Reverse on it starts at the balloon and runs back towards you.
    // The colour is captured here, so changing it only affects dots fired afterwards and
    // several dots of different colours can be in flight at once.
    void trigger()
    {
        Dot d;
        d.position = gReverse ? travelSpan() - 1.0f : 0.0f;
        d.color = currentColor;
        activeDots.push_back(d);
    }

    // Update the position of all dots and render them into the trail region. The caller
    // clears the buffer beforehand and calls FastLED.show() afterwards, so this renders
    // on every call even when no time has passed.
    void update()
    {
        // Get the current time
        unsigned long currentMillis = millis();

        // Calculate time passed in seconds since the last update
        float deltaTime = (currentMillis - lastUpdateTime) / 1000.0f;
        lastUpdateTime = currentMillis;

        const int trail = trailLength();
        const float span = travelSpan();
        const float step = (gReverse ? -speed : speed) * deltaTime;

        // Update positions of all active dots based on time passed and speed
        for (int i = 0; i < activeDots.size(); i++)
        {
            // Render the dot at its current floating position with exponential brightness falloff
            renderDot(activeDots[i].position, activeDots[i].color);

            // Update the dot's position based on the speed (pixels/second)
            float previous = activeDots[i].position;
            activeDots[i].position += step;

            // Only meaningful running towards the balloon; in reverse the dot starts there.
            // Reported per dot so the tip learns each arriving dot's own colour.
            if (!gReverse && gTipLength > 0 && previous < trail && activeDots[i].position >= trail
                && tipReachedCallback)
            {
                tipReachedCallback(activeDots[i].color);
            }
        }

        // Remove dots that have run off either end of the region they may travel
        activeDots.erase(
            std::remove_if(activeDots.begin(), activeDots.end(),
                           [span](const Dot& d) { return d.position >= span || d.position < -1.0f; }),
            activeDots.end());
    }

    // Set the speed of the running dots (pixels per second)
    void setSpeed(float newSpeed)
    {
        speed = newSpeed; // Speed is now in pixels/second
    }

    // Set the color of the running dots
    void setColor(CRGB newColor)
    {
        currentColor = newColor;
    }

    // Set the brightness of the LED strip
    void setBrightness(uint8_t newBrightness)
    {
        currentBrightness = newBrightness;
        FastLED.setBrightness(currentBrightness); // Update FastLED brightness setting
    }

    // Set the width of the dot (affects brightness falloff)
    void setWidth(float newWidth)
    {
        dotWidth = newWidth;
    }

    // Set the decay factor for exponential fade
    void setDecayFactor(float newDecayFactor)
    {
        decayFactor = newDecayFactor;
    }

    void setDotMode(int mode)
    {
        dotMode = constrain(mode, 0, 2);
    }

    // Fired once per dot, the moment it reaches the start of the tip. Carries that dot's
    // own colour, so the balloon can fill in the colours that actually arrived.
    void onTipReached(std::function<void(CRGB)> callback)
    {
        tipReachedCallback = callback;
    }

private:
    struct Dot {
        float position;
        CRGB color;   // captured at trigger time, not read live
    };

    std::vector<Dot> activeDots;   // positions and colours of the dots in flight
    unsigned long lastUpdateTime;  // To track when the dots were last updated
    float speed;                   // Speed of the dots in pixels per second
    CRGB currentColor;             // Current color of the running dots
    uint8_t currentBrightness;     // Current brightness of the LED strip
    float dotWidth;                // Width of the dot (affects how quickly the brightness falls off)
    float decayFactor;             // Decay factor for exponential fade
    int dotMode;                   // How dots interact with the tip, see the constructor
    std::function<void(CRGB)> tipReachedCallback;

    // The FillUp tip animation needs dots to land on the balloon, so it forces mode 0
    int effectiveDotMode() const
    {
        return gTipFillUpActive ? 0 : dotMode;
    }

    // How far a dot travels before it is retired. In mode 2 it stops at the balloon's top
    // point - the middle of the tip - because its mirror arrives there at the same moment
    // and the two meet.
    float travelSpan() const
    {
        int m = effectiveDotMode();
        if (m == 0) return (float)trailLength();
        if (m == 2 && gTipLength > 0) {
            return trailLength() + (gTipLength - 1) * 0.5f + 1.0f;
        }
        return (float)gLedCount;
    }

    // How far a dot may draw. Distinct from travelSpan() because a mode 2 dot stops
    // halfway into the tip but its mirror is drawn on the far side of it.
    int renderLimit() const
    {
        return (effectiveDotMode() == 0) ? trailLength() : (int)gLedCount;
    }

    // Render a dot, plus its mirror image in dot mode 2. Mirroring inside the tip means
    // the dot rounds the balloon up both sides at once and the two halves meet at the top
    // point, since the tip's midpoint is the balloon's top.
    void renderDot(float position, const CRGB& color)
    {
        renderDotAt(position, color);

        if (effectiveDotMode() == 2 && gTipLength > 0)
        {
            int trail = trailLength();
            if (position >= trail)
            {
                float intoTip = position - trail;
                renderDotAt(trail + (gTipLength - 1) - intoTip, color);
            }
        }
    }

    // Draw a single dot with exponential brightness fading
    void renderDotAt(float position, const CRGB& color)
    {
        // Determine the range of LEDs to update based on dotWidth
        int start = max(0, (int)(position - dotWidth));
        int end = min(renderLimit() - 1, (int)(position + dotWidth));

        for (int i = start; i <= end; i++)
        {
            float distance = fabs(i - position);

            if (distance < dotWidth)
            {
                // Exponential brightness falloff
                // Using decayFactor to control the rate of decay
                float brightness = expf(-distance / decayFactor);

                // Ensure brightness is within [0,1]
                brightness = constrain(brightness, 0.0f, 1.0f);

                // Apply the brightness to the color and add it to the existing LED state
                leds[i] += CRGB(
                    (uint8_t)(color.r * brightness),
                    (uint8_t)(color.g * brightness),
                    (uint8_t)(color.b * brightness));
            }
        }
    }
};

#define TIP_MODE_COUNT 11
#define TIP_MODE_FILLUP 10

// FillUp tuning, all in "height" units where 0 is the balloon's bottom and 1 its top
#define TIP_SWEEP_RATE 2.2f     // how fast the balloon empties or refills in one go
#define TIP_FLASH_DECAY 2.5f    // how quickly the burst glow fades, per second
#define TIP_MAX_FILL_STEPS 64   // matches the setFillSteps() clamp, sizes the band colours

// FillUp is either sitting at its current level, or sweeping the whole balloon to
// empty/full after it has topped out.
enum TipFillPhase { FILL_IDLE = 0, FILL_SWEEP };

// Animates the last gTipLength LEDs, which are wrapped around a balloon starting at the
// bottom, up one side, over the top point and back down to the bottom. So tip index 0 and
// index gTipLength-1 are both at the balloon's bottom and the middle index sits at the top
// point. heightAt() maps an index onto that shape: 0.0 = bottom, 1.0 = top point.
// Animations are written in terms of height, which makes them automatically symmetric
// across both sides of the balloon.
class TipAnimator {
public:
    TipAnimator()
        : mode(0), phase(0.0f), hueAccum(0.0f), lastUpdateTime(0),
          color(CRGB::Aqua), speed(1.0f), brightness(255), hueShift(0.0f),
          fillLevel(0.0f), fillTarget(0.0f), fillStep(0), fillSteps(8),
          fillPhase(FILL_IDLE), flash(0.0f) {
        memset(scratch, 0, sizeof(scratch));
        setAllBands(DEFAULT_COLOR);
    }

    void setMode(int m) {
        mode = ((m % TIP_MODE_COUNT) + TIP_MODE_COUNT) % TIP_MODE_COUNT;
        gTipFillUpActive = (mode == TIP_MODE_FILLUP);
        phase = 0.0f;
        resetFill();
        memset(scratch, 0, sizeof(scratch)); // modes share this buffer, don't inherit stale state
    }

    void nextMode() { setMode(mode + 1); }
    int getMode() const { return mode; }
    const char* getModeName() const { return modeNames[mode]; }

    void setColor(CRGB c) { color = c; }
    void setSpeed(float s) { speed = s; }
    void setBrightness(uint8_t b) { brightness = b; }
    void setHueShift(float h) { hueShift = h; }  // hue steps per second, 0 = use fixed color

    void setFillSteps(int steps) {
        fillSteps = constrain(steps, 1, 64);
        resetFill();
    }

    // Which event moves the fill depends on the direction the dots run: normally the
    // balloon inflates as each dot lands on it, in reverse each dot fired drains it.
    // Both carry the colour of the dot in question.
    void onDotFired(const CRGB& c)   { if (gReverse)  stepFill(c); }
    void onDotArrived(const CRGB& c) { if (!gReverse) stepFill(c); }

    // While the balloon is draining it is a single colour, so a colour change should show
    // straight away rather than waiting for the next dot to be fired. Filling forwards the
    // bands keep the colours that arrived, so a change there only affects later dots.
    void onDotColorChanged(const CRGB& c) {
        if (gReverse) setAllBands(c);
    }

    // Empty to begin with when dots run towards the balloon, full when they run away
    void resetFill() {
        fillStep = gReverse ? fillSteps : 0;
        fillTarget = gReverse ? 1.0f : 0.0f;
        fillLevel = fillTarget;
        fillPhase = FILL_IDLE;
        flash = 0.0f;
        setAllBands(gDotColor);
    }

    void update()
    {
        unsigned long now = millis();
        float dt = (now - lastUpdateTime) / 1000.0f;
        lastUpdateTime = now;

        const int n = (int)gTipLength;
        if (n <= 0) return; // tip disabled, leave those pixels black

        if (dt > 0.25f) dt = 0.25f; // guard against the first call and any long stall

        phase += speed * dt;
        if (phase > 4096.0f) phase -= 4096.0f;
        hueAccum += hueShift * dt;
        while (hueAccum >= 256.0f) hueAccum -= 256.0f;

        // --- FillUp state machine ---------------------------------------------------
        if (flash > 0.0f) {
            flash -= TIP_FLASH_DECAY * dt;
            if (flash < 0.0f) flash = 0.0f;
        }

        // A sweep empties or refills the whole balloon in one quick pass; otherwise the
        // surface just eases towards the level the last dot asked for.
        float levelRate = (fillPhase == FILL_SWEEP)
            ? TIP_SWEEP_RATE
            : 1.5f * (speed > 0.05f ? speed : 1.0f);

        if (fillLevel < fillTarget) {
            fillLevel += levelRate * dt;
            if (fillLevel >= fillTarget) {
                fillLevel = fillTarget;
                fillPhase = FILL_IDLE;
            }
        } else if (fillLevel > fillTarget) {
            fillLevel -= levelRate * dt;
            if (fillLevel <= fillTarget) {
                fillLevel = fillTarget;
                fillPhase = FILL_IDLE;
            }
        } else {
            fillPhase = FILL_IDLE;
        }

        // The tip is always the last gTipLength pixels, Reverse does not move it
        CRGB* tip = &leds[trailLength()];

        switch (mode) {
            case 0: travel(tip, n, -1.0f); break; // RiseUp
            case 1: travel(tip, n, +1.0f); break; // FallDown
            case 2: modeBreathe(tip, n);   break;
            case 3: modeRainbow(tip, n);   break;
            case 4: modeComet(tip, n);     break;
            case 5: modeFire(tip, n);      break;
            case 6: modeSparkle(tip, n);   break;
            case 7: modeWaterline(tip, n); break;
            case 8: modeAurora(tip, n);    break;
            case 9: modeStrobe(tip, n);    break;
            case TIP_MODE_FILLUP: modeFillUp(tip, n); break;
        }

        if (brightness < 255) {
            for (int i = 0; i < n; i++) tip[i].nscale8(brightness);
        }
    }

private:
    static const char* const modeNames[TIP_MODE_COUNT];

    int mode;
    float phase;       // animation clock, advanced by speed
    float hueAccum;    // rotating hue when hueShift > 0
    unsigned long lastUpdateTime;
    CRGB color;
    float speed;
    uint8_t brightness;
    float hueShift;
    float fillLevel;   // current surface height, 0..1 (FillUp mode)
    float fillTarget;  // height the current step corresponds to, eased towards
    int fillStep;      // 0 = empty .. fillSteps = full
    int fillSteps;     // dots needed to go from empty to full
    uint8_t fillPhase; // TipFillPhase
    float flash;       // 0..1 whole-balloon glow, decays; the burst leading a reset sweep
    CRGB fillColors[TIP_MAX_FILL_STEPS]; // colour of the dot that delivered each band
    uint8_t scratch[MAX_TIP_LEDS];       // per-pixel state for the fire and sparkle modes

    // One dot's worth of liquid, in that dot's colour. Once the balloon has topped out,
    // the next dot sweeps it back to the other end rather than snapping.
    void setAllBands(const CRGB& c) {
        for (int i = 0; i < TIP_MAX_FILL_STEPS; i++) fillColors[i] = c;
    }

    void stepFill(const CRGB& c) {
        if (!gReverse) {
            // Topped out. Drain it, keeping the colours it was filled with, so the
            // emptying animation shows the same stack of colours that went in.
            if (fillStep >= fillSteps) { fillStep = 0; beginSweep(0.0f); return; }
            // Band fillStep is the one this dot is adding, so it keeps this dot's colour
            // and earlier bands keep theirs: the balloon fills in the colours that arrived.
            if (fillStep < TIP_MAX_FILL_STEPS) fillColors[fillStep] = c;
            fillStep++;
        } else {
            // Emptied out: refill in one colour, the current one
            if (fillStep <= 0) { fillStep = fillSteps; setAllBands(c); beginSweep(1.0f); return; }
            fillStep--;
            setAllBands(c); // draining, the balloon is one colour and follows the current one
        }
        fillTarget = (float)fillStep / (float)fillSteps;
    }

    // The whole balloon empties (or refills) in one quick pass, led by a bright burst.
    // Band colours are left alone; the caller decides whether they should change.
    void beginSweep(float target) {
        fillPhase = FILL_SWEEP;
        fillTarget = target;
        flash = 1.0f;
    }

    // 0.0 at the balloon's bottom (both ends of the tip), 1.0 at the top point
    static float heightAt(int i, int n) {
        if (n <= 1) return 1.0f;
        return 1.0f - fabsf((2.0f * i) / (n - 1) - 1.0f);
    }

    static CRGB scaleColor(const CRGB& c, float v) {
        if (v <= 0.0f) return CRGB::Black;
        if (v > 1.0f) v = 1.0f;
        return CRGB((uint8_t)(c.r * v), (uint8_t)(c.g * v), (uint8_t)(c.b * v));
    }

    // The colour the fixed-colour modes draw with: either the configured one, or a
    // continuously rotating hue when Tip_HueShift is turned up.
    CRGB baseColor() const {
        if (hueShift > 0.001f) return CRGB(CHSV((uint8_t)hueAccum, 255, 255));
        return color;
    }

    // Modes 0/1: repeating dots with a soft trailing tail, running along the balloon.
    // dir -1 sends them bottom -> top point, dir +1 sends them top point -> bottom.
    void travel(CRGB* tip, int n, float dir) {
        CRGB c = baseColor();
        const float waves = 3.0f; // dots visible along one side at a time
        for (int i = 0; i < n; i++) {
            float f = heightAt(i, n) * waves + dir * phase * 2.0f;
            f -= floorf(f); // 0..1 sawtooth
            tip[i] = scaleColor(c, powf(1.0f - f, 6.0f));
        }
    }

    // Mode 2: the whole balloon swells and fades
    void modeBreathe(CRGB* tip, int n) {
        float f = phase - floorf(phase);
        float v = 0.5f - 0.5f * cosf(f * 2.0f * PI);
        v *= v; // lingers longer at the dim end
        CRGB c = scaleColor(baseColor(), 0.08f + 0.92f * v);
        for (int i = 0; i < n; i++) tip[i] = c;
    }

    // Mode 3: rainbow banded by height, scrolling up toward the top point
    void modeRainbow(CRGB* tip, int n) {
        for (int i = 0; i < n; i++) {
            uint8_t hue = (uint8_t)(heightAt(i, n) * 170.0f - phase * 120.0f);
            tip[i] = CHSV(hue, 240, 255);
        }
    }

    // Mode 4: a comet circling the balloon, tail wrapping around the loop
    void modeComet(CRGB* tip, int n) {
        CRGB c = baseColor();
        float head = (phase * 0.5f - floorf(phase * 0.5f)) * n;
        float tail = n * 0.25f;
        if (tail < 3.0f) tail = 3.0f;
        for (int i = 0; i < n; i++) {
            float d = head - i;
            if (d < 0.0f) d += n; // the tail wraps past the bottom seam
            float v = (d < tail) ? (1.0f - d / tail) : 0.0f;
            tip[i] = scaleColor(c, v * v);
        }
    }

    // Mode 5: flames climbing both sides toward the top point
    void modeFire(CRGB* tip, int n) {
        int half = (n + 1) / 2; // scratch[0] = bottom, scratch[half-1] = top point
        if (half < 2) half = 2;

        for (int i = 0; i < half; i++) {
            scratch[i] = qsub8(scratch[i], random8(0, 3 + 90 / half));
        }
        for (int i = half - 1; i >= 2; i--) {
            scratch[i] = (scratch[i - 1] + scratch[i - 2] + scratch[i - 2]) / 3;
        }
        if (random8() < 120) {
            uint8_t y = random8(2);
            scratch[y] = qadd8(scratch[y], random8(160, 255));
        }
        for (int i = 0; i < n; i++) {
            int hi = (int)(heightAt(i, n) * (half - 1) + 0.5f);
            tip[i] = HeatColor(scratch[hi]);
        }
    }

    // Mode 6: random twinkles fading out
    void modeSparkle(CRGB* tip, int n) {
        uint8_t fade = (uint8_t)constrain((int)(speed * 45.0f), 8, 200);
        for (int i = 0; i < n; i++) scratch[i] = qsub8(scratch[i], fade);

        int spawns = 1 + n / 40;
        for (int s = 0; s < spawns; s++) {
            if (random8() < 90) scratch[random16(n)] = 255;
        }
        CRGB c = baseColor();
        for (int i = 0; i < n; i++) {
            tip[i] = c;
            tip[i].nscale8(scratch[i]);
        }
    }

    // Mode 7: a bright waterline that fills the balloon to the top point, then drains
    void modeWaterline(CRGB* tip, int n) {
        float f = phase * 0.4f;
        f -= floorf(f);
        float level = 1.0f - fabsf(2.0f * f - 1.0f);
        CRGB c = baseColor();
        const float edge = 0.12f;
        for (int i = 0; i < n; i++) {
            float h = heightAt(i, n);
            float v = 0.0f;
            if (h <= level - edge)      v = 0.35f; // submerged, dim glow
            else if (h <= level + edge) v = 1.0f;  // the surface itself
            tip[i] = scaleColor(c, v);
        }
    }

    // Mode 8: slow drifting colour clouds
    void modeAurora(CRGB* tip, int n) {
        uint16_t t = (uint16_t)(phase * 600.0f);
        for (int i = 0; i < n; i++) {
            uint8_t hue = inoise8(i * 40, t);
            uint8_t val = inoise8(i * 40 + 5000, t + 12000);
            val = scale8(val, val); // deepen the gaps between the bright bands
            tip[i] = CHSV(hue, 200, qadd8(val, 30));
        }
    }

    // Mode 9: sharp double-flash, hopping to a new colour each burst
    void modeStrobe(CRGB* tip, int n) {
        float cycle = phase * 0.8f;
        float f = cycle - floorf(cycle);
        bool on = (f < 0.05f) || (f > 0.11f && f < 0.16f);
        CRGB c = (hueShift > 0.001f)
            ? baseColor()
            : CRGB(CHSV((uint8_t)((uint32_t)floorf(cycle) * 53), 255, 255));
        for (int i = 0; i < n; i++) tip[i] = on ? c : CRGB::Black;
    }

    // Mode 10: the balloon gains one step of fill per dot until it is full, then the next
    // dot resets it. Filling by height fills both sides at once, bottom up, meeting at the
    // top. The filled area shimmers gently so it does not look flat. Draws in the dot's
    // colour rather than Tip_Color, and forces dots to stop at the tip.
    void modeFillUp(CRGB* tip, int n) {
        const float surface = 0.06f;
        bool empty = fillLevel <= 0.001f;
        uint16_t t = (uint16_t)(millis() / 6);

        for (int i = 0; i < n; i++) {
            float h = heightAt(i, n);

            if (empty || h > fillLevel) {
                tip[i] = CRGB::Black;
                continue;
            }

            // Which dot delivered the liquid at this height, and therefore its colour
            int band = (int)(h * fillSteps);
            if (band >= fillSteps) band = fillSteps - 1;
            if (band < 0) band = 0;
            CRGB c = fillColors[band];

            // Dimmer body, brighter meniscus, gentle shimmer
            float v = (fillLevel - h < surface) ? 1.0f : 0.7f;
            v *= 0.80f + 0.20f * (inoise8(i * 70, t) / 255.0f);

            // The burst that leads an empty/refill sweep
            if (flash > 0.0f) {
                v += flash * flash * 0.55f;
            }

            tip[i] = scaleColor(c, v);
        }
    }
};

const char* const TipAnimator::modeNames[TIP_MODE_COUNT] = {
    "RiseUp", "FallDown", "Breathe", "Rainbow", "Comet",
    "Fire", "Sparkle", "Waterline", "Aurora", "Strobe", "FillUp"
};

class ConfigParameter {
public:
    enum Type { STRING, FLOAT, BOOLEAN, ARRAY_STRING, COLOR }; // Added COLOR type

    // Default constructor
    ConfigParameter() : type(FLOAT), floatValue(0.0f) {}

    // Constructor for String parameter
    ConfigParameter(const String& name, const String& value)
        : name(name), type(STRING) {
        stringValue = new String(value);
    }

    // Constructor for Float parameter
    ConfigParameter(const String& name, float value)
        : name(name), type(FLOAT), floatValue(value) {}

    // Constructor for Boolean parameter
    ConfigParameter(const String& name, bool value)
        : name(name), type(BOOLEAN), boolValue(value) {}

    // Constructor for ArrayString parameter
    ConfigParameter(const String& name, const std::vector<String>& value)
        : name(name), type(ARRAY_STRING) {
        arrayStringValue = new std::vector<String>(value);
    }

    // Constructor for Color parameter
    ConfigParameter(const String& name, const CRGB& value)
        : name(name), type(COLOR), colorValue(value) {}

    // Copy constructor
    ConfigParameter(const ConfigParameter& other)
        : name(other.name), type(other.type) {
        if (type == STRING) {
            stringValue = new String(*other.stringValue);
        } else if (type == FLOAT) {
            floatValue = other.floatValue;
        } else if (type == BOOLEAN) {
            boolValue = other.boolValue;
        } else if (type == ARRAY_STRING) {
            arrayStringValue = new std::vector<String>(*other.arrayStringValue);
        } else if (type == COLOR) {
            colorValue = other.colorValue;
        }
    }

    // Move constructor
    ConfigParameter(ConfigParameter&& other) noexcept
        : name(std::move(other.name)), type(other.type) {
        if (type == STRING) {
            stringValue = other.stringValue;
            other.stringValue = nullptr;
        } else if (type == FLOAT) {
            floatValue = other.floatValue;
        } else if (type == BOOLEAN) {
            boolValue = other.boolValue;
        } else if (type == ARRAY_STRING) {
            arrayStringValue = other.arrayStringValue;
            other.arrayStringValue = nullptr;
        } else if (type == COLOR) {
            colorValue = other.colorValue;
        }
    }

    // Destructor to properly handle the dynamic memory
    ~ConfigParameter() {
        if (type == STRING && stringValue) {
            delete stringValue;
        } else if (type == ARRAY_STRING && arrayStringValue) {
            delete arrayStringValue;
        }
    }

    // Copy assignment operator
    ConfigParameter& operator=(const ConfigParameter& other) {
        if (this == &other) return *this;

        // Clean up existing data
        if (type == STRING && stringValue) {
            delete stringValue;
        } else if (type == ARRAY_STRING && arrayStringValue) {
            delete arrayStringValue;
        }

        name = other.name;
        type = other.type;

        if (type == STRING) {
            stringValue = new String(*other.stringValue);
        } else if (type == FLOAT) {
            floatValue = other.floatValue;
        } else if (type == BOOLEAN) {
            boolValue = other.boolValue;
        } else if (type == ARRAY_STRING) {
            arrayStringValue = new std::vector<String>(*other.arrayStringValue);
        } else if (type == COLOR) {
            colorValue = other.colorValue;
        }

        return *this;
    }

    // Move assignment operator
    ConfigParameter& operator=(ConfigParameter&& other) noexcept {
        if (this == &other) return *this;

        // Clean up existing data
        if (type == STRING && stringValue) {
            delete stringValue;
        } else if (type == ARRAY_STRING && arrayStringValue) {
            delete arrayStringValue;
        }

        name = std::move(other.name);
        type = other.type;

        if (type == STRING) {
            stringValue = other.stringValue;
            other.stringValue = nullptr;
        } else if (type == FLOAT) {
            floatValue = other.floatValue;
        } else if (type == BOOLEAN) {
            boolValue = other.boolValue;
        } else if (type == ARRAY_STRING) {
            arrayStringValue = other.arrayStringValue;
            other.arrayStringValue = nullptr;
        } else if (type == COLOR) {
            colorValue = other.colorValue;
        }

        return *this;
    }

    // Getter for parameter name
    String getName() const { return name; }

    // Getter for type
    Type getType() const { return type; }

    // Get the String value (only call if type is STRING)
    String getStringValue() const {
        if (type == STRING && stringValue) return *stringValue;
        return "";
    }

    // Get the float value (only call if type is FLOAT)
    float getFloatValue() const {
        if (type == FLOAT) return floatValue;
        return 0.0f;
    }

    // Get the boolean value (only call if type is BOOLEAN)
    bool getBoolValue() const {
        if (type == BOOLEAN) return boolValue;
        return false;
    }

    // Get the ArrayString value (only call if type is ARRAY_STRING)
    const std::vector<String>& getArrayStringValue() const {
        if (type == ARRAY_STRING && arrayStringValue) return *arrayStringValue;
        static std::vector<String> emptyVector;
        return emptyVector;
    }

    // Get the Color value (only call if type is COLOR)
    CRGB getColorValue() const {
        if (type == COLOR) return colorValue;
        return CRGB(0, 0, 0);
    }

    // Setter for String value
    void setValue(const String& newValue) {
        if (type == STRING) {
            if (stringValue) {
                *stringValue = newValue;
            } else {
                stringValue = new String(newValue);
            }
        }
    }

    // Setter for float value
    void setValue(float newValue) {
        if (type == FLOAT) {
            floatValue = newValue;
        }
    }

    // Setter for boolean value
    void setValue(bool newValue) {
        if (type == BOOLEAN) {
            boolValue = newValue;
        }
    }

    // Setter for ArrayString value
    void setValue(const std::vector<String>& newValue) {
        if (type == ARRAY_STRING) {
            if (arrayStringValue) {
                *arrayStringValue = newValue;
            } else {
                arrayStringValue = new std::vector<String>(newValue);
            }
        }
    }

    // Setter for Color value
    void setValue(const CRGB& newValue) {
        if (type == COLOR) {
            colorValue = newValue;
        }
    }

private:
    String name;
    Type type;

    union {
        String* stringValue;
        float floatValue;
        bool boolValue;
        std::vector<String>* arrayStringValue; // For ARRAY_STRING
        CRGB colorValue; // Added for COLOR
    };
};

class WebConfig {
public:
    WebConfig(const char* ssid, const char* password)
        : softAP_ssid(ssid), softAP_password(password), server(80), title("Configuration Page") {}

    void begin() {
        preferences.begin("webconfig", false);  // Open NVS with namespace 'webconfig'
        loadParameters();  // Load parameters from NVS on startup
        configureAccessPoint();
        setupDNS();
        setupWebServer();
    }

    void handleClient() {
        dnsServer.processNextRequest();
        server.handleClient();
    }

    void addParamString(const String& name, const String& defaultValue) {
        String storedValue = defaultValue;
        if (preferences.isKey(name.c_str())) {
            storedValue = preferences.getString(name.c_str(), defaultValue);
        }
        configParams[name] = ConfigParameter(name, storedValue);
        saveParameter(name);
    }

    void addParamArrayString(const String& name, const std::vector<String>& defaultValue) {
        std::vector<String> storedValue = defaultValue;
        if (preferences.isKey(name.c_str())) {
            String serializedArray = preferences.getString(name.c_str(), "");
            storedValue = deserializeArrayString(serializedArray);
        }
        configParams[name] = ConfigParameter(name, storedValue);
        saveParameter(name);
    }

    void addParamFloat(const String& name, float defaultValue) {
        float storedValue = defaultValue;
        if (preferences.isKey(name.c_str())) {
            storedValue = preferences.getFloat(name.c_str(), defaultValue);
        }
        configParams[name] = ConfigParameter(name, storedValue);
        saveParameter(name);
    }

    void addParamBoolean(const String& name, bool defaultValue) {
        bool storedValue = defaultValue;
        if (preferences.isKey(name.c_str())) {
            storedValue = preferences.getBool(name.c_str(), defaultValue);
        }
        configParams[name] = ConfigParameter(name, storedValue);
        saveParameter(name);
    }

    void addParamColor(const String& name, const CRGB& defaultValue) {
        CRGB storedValue = defaultValue;
        if (preferences.isKey(name.c_str())) {
            String hexColor = preferences.getString(name.c_str(), "");
            storedValue = hexToCRGB(hexColor);
        }
        configParams[name] = ConfigParameter(name, storedValue);
        saveParameter(name);
    }

    String getParamString(const String& name) {
        return configParams[name].getStringValue();
    }

    std::vector<String> getParamArrayString(const String& name) {
        return configParams[name].getArrayStringValue();
    }

    float getParamFloat(const String& name) {
        return configParams[name].getFloatValue();
    }

    bool getParamBool(const String& name) {
        return configParams[name].getBoolValue();
    }

    CRGB getParamColor(const String& name) {
        return configParams[name].getColorValue();
    }

    void setParam(const String& name, const String& value) {
        if (configParams[name].getType() == ConfigParameter::STRING) {
            configParams[name].setValue(value);
            saveParameter(name);
        }
    }

    void setParam(const String& name, float value) {
        if (configParams[name].getType() == ConfigParameter::FLOAT) {
            configParams[name].setValue(value);
            saveParameter(name);
        }
    }

    void setParam(const String& name, const std::vector<String>& value) {
        if (configParams[name].getType() == ConfigParameter::ARRAY_STRING) {
            configParams[name].setValue(value);
            saveParameter(name);
        }
    }

    void setParam(const String& name, bool value) {
        if (configParams[name].getType() == ConfigParameter::BOOLEAN) {
            configParams[name].setValue(value);
            saveParameter(name);
        }
    }

    void setParam(const String& name, const CRGB& value) {
        if (configParams[name].getType() == ConfigParameter::COLOR) {
            configParams[name].setValue(value);
            saveParameter(name);
        }
    }

    void setTitle(const String& newTitle) {
        title = newTitle;
    }

    // Method to set custom HTML content
    void setCustomHTML(const String& htmlContent) {
        customHTML = htmlContent;
    }

    void onWebButtonPressed(std::function<void(String)> callback) {
        webButtonCallback = callback;
    }

    void onPropertiesModified(std::function<void(void)> callback) {
        propertiesModifiedCallback = callback;
    }

    
    String getHtmlButton(const String& buttonName) {
        // Define inline CSS styles for the button
        String buttonStyle = ""
            "padding: 15px 30px; "              // Adds space inside the button
            "font-size: 1.5em; "                // Increases the font size
            "font-weight: bold; "                // Makes the text bold
            "color: #ffffff; "                   // Sets the text color to white
            "background-color: #cccccc; "        // Sets the button background color (green)
            "border: none; "                      // Removes the default border
            "border-radius: 8px; "                // Rounds the corners
            "cursor: pointer; "                   // Changes cursor on hover
            "box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1); " // Adds a subtle shadow
            "transition: background-color 0.3s, transform 0.2s;"; // Smooth transitions

        // Define JavaScript for hover effect (since inline styles can't handle :hover)
        String onHoverEffect = ""
            "onmouseover=\"this.style.backgroundColor='#218838'; this.style.transform='scale(1.05)';\" "
            "onmouseout=\"this.style.backgroundColor='#28a745'; this.style.transform='scale(1)';\"";

        // Construct and return the styled button HTML
        return "<button style=\"" + buttonStyle + "\" "
            + onHoverEffect +
            "onclick=\"window.location.href='/button?name=" + buttonName + "';\">"
            + buttonName +
            "</button>";
    }


private:
    const char* softAP_ssid;
    const char* softAP_password;
    IPAddress apIP = IPAddress(8, 8, 8, 8); // Access Point IP Address
    IPAddress netMsk = IPAddress(255, 255, 255, 0); // Netmask
    const byte DNS_PORT = 53;
    DNSServer dnsServer;
    WebServer server;
    String title;  // Dynamic title for the configuration page
    String customHTML; // Custom HTML content to include in the webpage
    std::function<void(String)> webButtonCallback;
    std::function<void(void)> propertiesModifiedCallback; // Callback for property modifications

    Preferences preferences;  // NVS Preferences for storing parameters

    std::map<String, ConfigParameter> configParams; // Configuration storage

    void configureAccessPoint() {
        WiFi.softAPConfig(apIP, apIP, netMsk);
        WiFi.softAP(softAP_ssid, softAP_password);
        delay(1000);
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
    }

    void setupDNS() {
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(DNS_PORT, "*", apIP);
    }

    void setupWebServer() {
        server.on("/", [this]() { handleRoot(); });
        server.on("/generate_204", [this]() { handleRoot(); }); // Handle Android captive portal request
        server.on("/submit", [this]() { handleSubmit(); });     // Form submission
        server.on("/button", [this]() { handleButton(); });     // Button click handler
        server.onNotFound([this]() { handleNotFound(); });
        server.begin();
        Serial.println("HTTP server started");
    }

    // Save parameter to NVS
    void saveParameter(const String& paramName) {
        ConfigParameter& param = configParams[paramName];
        if (param.getType() == ConfigParameter::ARRAY_STRING) {
            String serializedArray = serializeArrayString(param.getArrayStringValue());
            preferences.putString(paramName.c_str(), serializedArray);
        } else if (param.getType() == ConfigParameter::STRING) {
            preferences.putString(paramName.c_str(), param.getStringValue());
        } else if (param.getType() == ConfigParameter::FLOAT) {
            preferences.putFloat(paramName.c_str(), param.getFloatValue());
        } else if (param.getType() == ConfigParameter::BOOLEAN) {
            preferences.putBool(paramName.c_str(), param.getBoolValue());
        } else if (param.getType() == ConfigParameter::COLOR) {
            String hexColor = CRGBtoHex(param.getColorValue());
            preferences.putString(paramName.c_str(), hexColor);
        }
    }

    void loadParameters() {
        for (auto& param : configParams) {
            String paramName = param.first;
            if (param.second.getType() == ConfigParameter::ARRAY_STRING) {
                String serializedArray = preferences.getString(param.first.c_str(), "");
                std::vector<String> value = deserializeArrayString(serializedArray);
                param.second.setValue(value);
            } else if (param.second.getType() == ConfigParameter::STRING) {
                String value = preferences.getString(paramName.c_str(), param.second.getStringValue());
                param.second.setValue(value);
            } else if (param.second.getType() == ConfigParameter::FLOAT) {
                float value = preferences.getFloat(paramName.c_str(), param.second.getFloatValue());
                param.second.setValue(value);
            } else if (param.second.getType() == ConfigParameter::BOOLEAN) {
                bool value = preferences.getBool(paramName.c_str(), param.second.getBoolValue());
                param.second.setValue(value);
            } else if (param.second.getType() == ConfigParameter::COLOR) {
                String hexColor = preferences.getString(paramName.c_str(), CRGBtoHex(param.second.getColorValue()));
                CRGB value = hexToCRGB(hexColor);
                param.second.setValue(value);
            }
        }
    }

    String serializeArrayString(const std::vector<String>& array) {
        String result = "[";
        for (size_t i = 0; i < array.size(); ++i) {
            result += "\"" + escapeString(array[i]) + "\"";
            if (i < array.size() - 1) {
                result += ",";
            }
        }
        result += "]";
        return result;
    }

    std::vector<String> deserializeArrayString(const String& serializedArray) {
        std::vector<String> result;
        String s = serializedArray;
        s.trim();
        if (s.startsWith("[") && s.endsWith("]")) {
            s = s.substring(1, s.length() - 1);
            int start = 0;
            bool inString = false;
            String currentString = "";
            for (size_t i = 0; i < s.length(); ++i) {
                char c = s.charAt(i);
                if (c == '\\') {
                    if (i + 1 < s.length()) {
                        currentString += s.charAt(i + 1);
                        ++i;
                    }
                } else if (c == '"') {
                    inString = !inString;
                    if (!inString) {
                        result.push_back(currentString);
                        currentString = "";
                    }
                } else if (inString) {
                    currentString += c;
                }
            }
        }
        return result;
    }

    String escapeString(const String& str) {
        String escaped = "";
        for (size_t i = 0; i < str.length(); ++i) {
            char c = str.charAt(i);
            if (c == '\\' || c == '\"') {
                escaped += '\\';
            }
            escaped += c;
        }
        return escaped;
    }

    String CRGBtoHex(const CRGB& color) {
        char hexCol[8];
        sprintf(hexCol, "#%02X%02X%02X", color.r, color.g, color.b);
        return String(hexCol);
    }

    CRGB hexToCRGB(const String& hexColor) {
        uint8_t r = 0, g = 0, b = 0;
        if (hexColor.startsWith("#") && hexColor.length() == 7) {
            String rs = hexColor.substring(1, 3);
            String gs = hexColor.substring(3, 5);
            String bs = hexColor.substring(5, 7);
            r = strtoul(rs.c_str(), NULL, 16);
            g = strtoul(gs.c_str(), NULL, 16);
            b = strtoul(bs.c_str(), NULL, 16);
        }
        return CRGB(r, g, b);
    }

    void handleButton() {
        if (server.hasArg("name")) {
            String buttonName = server.arg("name");
            // Call the webButtonCallback if it's set
            if (webButtonCallback) {
                webButtonCallback(buttonName);
            }
            // Redirect back to the main page
            server.send(200, "text/html", "<html><body><script>window.location.href = '/';</script></body></html>");
        } else {
            server.send(400, "text/plain", "Bad Request: Missing 'name' parameter");
        }
    }

    void handleRoot() {
        if (captivePortal()) {
            return;
        }
        server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        server.sendHeader("Pragma", "no-cache");
        server.sendHeader("Expires", "-1");

        // Create an HTML page with a dynamic title and tabs for each group
        String p = F("<html><head>"
                    "<style>"
                    /* Global Box Sizing */
                    "*, *::before, *::after {"
                    "  box-sizing: border-box;"
                    "}"
                    /* General Styles */
                    "body {"
                    "  margin: 0;"
                    "  font-family: Arial, sans-serif;"
                    "  background-color: #f0f0f0;"
                    "  height: 100vh;"
                    "  overflow-x: hidden;" /* Prevent horizontal scroll */
                    "}"
                    ".header {"
                    "  width: 100%;"
                    "  background-color: #fff;"
                    "  padding: 10px 0;"
                    "  position: sticky;" /* Keep the header at the top when scrolling */ 
                    "  top: 0;"
                    "  z-index: 1000;"
                    "  box-shadow: 0 2px 4px rgba(0,0,0,0.1);"
                    "  text-align: center;"
                    "}"
                    ".header h1 {"
                    "  font-size: 3em;" /* Adjust as needed */
                    "  color: #333;"
                    "  margin: 10px 0;"
                    "  -webkit-text-stroke: 3px transparent;"
                    "  text-shadow: 0 0 12px rgba(0, 0, 0, 0.5);"
                    "  animation: textOutlineAnimation 3s infinite ease-in-out;"
                    "}"
                    "@keyframes textOutlineAnimation {"
                    "  0%, 100% { -webkit-text-stroke: 2px transparent; text-shadow: 0 0 6px rgba(0, 0, 0, 0.5); }"
                    "  50% { -webkit-text-stroke: 2px #4CAF50; text-shadow: none; }"
                    "}"
                    ".svg-container {"
                    "  width: 100%;"
                    "  display: flex;"
                    "  justify-content: center;"
                    "  margin-bottom: 10px;"
                    "  padding: 15px;"
                    "}"
                    "svg {"
                    "  width: 60%;"
                    "  max-width: 1080px;"
                    "}"
                    ".svg-outline {"
                    "  fill: none;"
                    "  stroke: black;"
                    "  stroke-width: 2;"
                    "  stroke-dasharray: 10, 5;"
                    "  animation: dash 5s linear infinite;"
                    "}"
                    "@keyframes dash {"
                    "  to { stroke-dashoffset: -50; }"
                    "}"
                    /* Tab Styles */
                    ".tab-container {"
                    "  width: 100%;"
                    "  display: flex;"
                    "  justify-content: center;"
                    "  margin-top: 20px;"
                    "}"
                    "ul {"
                    "  list-style-type: none;"
                    "  padding: 0;"
                    "  margin: 0;"
                    "  width: 95%;" /* Full width of the tab container */
                    "  display: flex;"
                    "  justify-content: center;" /* Center the tabs */
                    "  overflow-x: auto;" /* Allow horizontal scrolling for smaller screens */ 
                    "}"
                    "li {"
                    "  flex: 1;"
                    "  text-align: center;"
                    "  margin-right: 10px;"
                    "}"
                    "a {"
                    "  font-size: 1.5em;" /* Adjust as needed */
                    "  text-decoration: none;"
                    "  color: #333;"
                    "  padding: 10px;"
                    "  background-color: #f0f0f0;"
                    "  border: 1px solid #ccc;"
                    "  border-radius: 5px;"
                    "  display: block;"
                    "  width: 100%;"
                    "}"
                    "a:hover {"
                    "  background-color: #ddd;"
                    "}"
                    ".tab-content {"
                    "  display: none;"
                    "  width: 95%;"
                    "  padding: 0px;"
                    "  margin: 20px auto;"
                    "}"
                    ".active-tab {"
                    "  display: block;"
                    "}"
                    /* Form Styles */
                    "form {"
                    "  background: white;"
                    "  padding: 20px;"
                    "  border-radius: 10px;"
                    "  box-shadow: 0 4px 8px rgba(0,0,0,0.1);"
                    "  width: 100%;"
                    "  box-sizing: border-box;"
                    "  margin: 0 auto;"
                    "}"
                    /* Form Row Styles */
                    ".form-row {"
                    "  display: flex;"
                    "  justify-content: space-between;"
                    "  align-items: flex-start;" /* Align items to the top */
                    "  margin-bottom: 20px;" /* Increased margin for better spacing */
                    "}"
                    ".form-row label {"
                    "  flex: 1;"
                    "  font-size: 1.5em;" /* Adjust as needed */
                    "  font-weight: bold;"
                    "  color: #333;"
                    "  margin-top: 10px;" /* Align label vertically */
                    "}"
                    ".form-row input[type='text'],"
                    ".form-row input[type='number'] {"
                    "  flex: 2;"
                    "  padding: 10px;"
                    "  border: 1px solid #ccc;"
                    "  border-radius: 5px;"
                    "  font-size: 1.5em;" /* Consistent font size */
                    "  box-sizing: border-box;"
                    "}"
                    ".form-row input[type='checkbox'] {"
                    "  transform: scale(1.5);" /* Adjusted scale */
                    "  margin-left: auto;" /* Push checkbox to the right */
                    "}"
                    ".form-row .checkbox-label {"
                    "  flex: 1;"
                    "  font-size: 1.5em;" /* Adjust as needed */
                    "  font-weight: bold;"
                    "  color: #333;"
                    "}"
                    /* Submit Button Styles */
                    "input[type='submit'] {"
                    "  background-color: #333333;"
                    "  color: white;"
                    "  border: none;"
                    "  cursor: pointer;"
                    "  padding: 15px;"
                    "  transition: background-color 0.3s ease;"
                    "  font-size: 1.5em;" /* Adjust as needed */
                    "  font-weight: bold;" /* Made text bold */
                    "  border-radius: 5px;" /* Rounded corners */
                    "  width: 100%;" /* Full width submit button */
                    "}"
                    "input[type='submit']:hover {"
                    "  background-color: #45a049;"
                    "}"
                    "input[type='color'] {"
                    "  width: 50px;"
                    "  height: 50px;"
                    "}"
                    /* Array Container Styles */
                    ".array-container {"
                    "  display: flex;"
                    "  flex: 1;" /* Take up 1 part of the flex */
                    "}"
                    ".array-label-container {"
                    "  display: flex;"
                    "  flex-direction: column;"
                    "  align-items: flex-start;"
                    "  margin-right: 20px;" /* Space between label and inputs */
                    "  flex: 1;" /* Same as other labels */
                    "}"
                    ".array-label-container label {"
                    "  margin-bottom: 10px;" /* Space between label and add button */
                    "}"
                    ".add-button {"
                    "  background-color: #4CAF50;"
                    "  color: white;"
                    "  border: none;"
                    "  cursor: pointer;"
                    "  padding: 15px 20px;"
                    "  transition: background-color 0.3s ease;"
                    "  font-size: 1.2em;" /* Adjust as needed */
                    "  border-radius: 5px;"
                    "  display: flex;"
                    "  align-items: center;"
                    "}"
                    ".add-button:hover {"
                    "  background-color: #45a049;"
                    "}"
                    ".add-button svg {"
                    "  width: 28px;"
                    "  height: 28px;"
                    "  margin-right: 5px;"
                    "}"
                    /* Scrollable Container for Array Items */
                    ".array-input-container {"
                    "  flex: 2.1;" /* Take up 2 parts of the flex */
                    "}"
                    ".scrollable-container {"
                    "  max-height: 40vh;" /* 40% of the viewport height */
                    "  overflow-y: auto;"
                    "  border: 1px solid #ccc;"
                    "  padding: 10px;"
                    "  margin-bottom: 20px;"
                    "  width: 100%;" /* Ensure it fills the container */
                    "  box-sizing: border-box;"
                    "}"
                    /* Array Item Styles */
                    ".array-item {"
                    "  display: flex;"
                    "  align-items: center;"
                    "  margin-bottom: 10px;"
                    "}"
                    ".array-item input[type='text'] {"
                    "  flex: 1;"
                    "  margin-right: 10px;"
                    "  font-size: 1.2em;" /* Smaller font size */
                    "  padding: 8px;" /* Smaller padding */
                    "}"
                    ".remove-button {"
                    "  background: none;"
                    "  border: none;"
                    "  cursor: pointer;"
                    "  padding: 0;"
                    "  display: flex;"
                    "  align-items: center;"
                    "}"
                    ".remove-button svg {"
                    "  width: 28px;"
                    "  height: 28px;"
                    "  fill: #ff4d4d;" /* Red color for the trash icon */
                    "}"
                    "</style>"

                    /* JavaScript to handle the tab switching and dynamic array fields */
                    "<script>"
                    "function openTab(tabName) {"
                    "  var i, tabcontent;"
                    "  tabcontent = document.getElementsByClassName('tab-content');"
                    "  for (i = 0; i < tabcontent.length; i++) {"
                    "    tabcontent[i].style.display = 'none';"
                    "  }"
                    "  document.getElementById(tabName).style.display = 'block';"
                    "}"
                    "function addElement(containerId, inputName) {"
                    "  var container = document.getElementById(containerId);"
                    "  var newDiv = document.createElement('div');"
                    "  newDiv.className = 'array-item';"
                    "  newDiv.innerHTML = \"<input type='text' name='\" + inputName + \"' value=''>"
                    "                      <button type='button' class='remove-button' onclick='removeElement(this)'>"
                    "                        <svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
                    "                          <path d='M3 6h18v2H3V6zm2 3h14l-1.5 12.5c-0.1 0.8-0.9 1.5-1.7 1.5H7.2c-0.8 0-1.6-0.7-1.7-1.5L5 9zM8 11v9h2v-9H8zm6 0v9h2v-9h-2z'/>"
                    "                        </svg>"
                    "                      </button>\";"
                    "  container.appendChild(newDiv);"
                    "}"
                    "function removeElement(button) {"
                    "  var div = button.parentNode;"
                    "  div.parentNode.removeChild(div);"
                    "}"
                    "</script>"

                    "</head><body>");


        // Insert SVG animation and title in a fixed header container
        p += "<div class='header'><div class='svg-container'>";
        p += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 150 126\">";
        p += "<g transform=\"translate(-28.34617, -67.34671)\">";
        p += "<path class=\"svg-outline\" d=\"M46.648479 131.26477v13.51339h-0.003v17.82217H159.91391V144.77816H64.562629V131.26477ZM126.77385 99.98706h18.61959v18.63263h-18.61959zm-62.580031 0h18.61959v18.63263H64.193819ZM28.346749 67.346711c-0.002 41.819719 0.002 84.474009 0 126.000059h0.0486 149.900931V72.846631h0.0501l-0.0501 -5.49992zm5.49992 5.49992H172.79633V187.84685H33.846669Z\" />";
        p += "</g></svg></div>";
        p += "<h1>" + title + "</h1></div>";

        // Collect group names and parameters
        std::map<String, std::vector<String>> groupedParams;
        std::vector<String> noGroupParams;

        // Organize parameters by group or no group
        for (const auto& param : configParams) {
            String paramName = param.first;
            int underscoreIndex = paramName.indexOf('_');
            if (underscoreIndex != -1) {
                // Grouped parameter (group_name_parameter_name)
                String groupName = paramName.substring(0, underscoreIndex);
                String subParamName = paramName.substring(underscoreIndex + 1);
                groupedParams[groupName].push_back(subParamName);
            } else {
                // Parameter without a group
                noGroupParams.push_back(paramName);
            }
        }

        // Display the tabs for each group and the Home tab for non-grouped parameters
        p += "<div class='tab-container'><ul>";
        p += "<li><a onclick=\"openTab('home')\">Home</a></li>";
        for (const auto& group : groupedParams) {
            p += "<li><a onclick=\"openTab('" + group.first + "')\">" + group.first + "</a></li>";
        }
        p += "</ul></div>";

        // Display non-grouped parameters (Home Tab)
        p += "<div id='home' class='tab-content active-tab'><form action=\"/submit\" method=\"POST\">";
        if (!noGroupParams.empty()) {
            for (const String& paramName : noGroupParams) {
                const ConfigParameter& param = configParams[paramName];
                if (param.getType() == ConfigParameter::BOOLEAN) {
                    // Render checkbox for Boolean parameters with label on the left
                    p += "<div class='form-row'>";
                    p += "<span class='checkbox-label'>" + paramName + "</span>";
                    p += "<input type='hidden' name='" + paramName + "' value='false'>";
                    p += "<input type='checkbox' name='" + paramName + "' value='true'" + (param.getBoolValue() ? " checked" : "") + ">";
                    p += "</div>";

                } else if (param.getType() == ConfigParameter::ARRAY_STRING) {
                    // Render ArrayString parameters
                    // [Your existing code to render ArrayString parameters]
                } else if (param.getType() == ConfigParameter::COLOR) {
                    // Render color picker for Color parameters
                    p += "<div class='form-row'>";
                    p += "<label for='" + paramName + "'>" + paramName + ":</label>";
                    p += "<input type='color' name='" + paramName + "' value='" + CRGBtoHex(param.getColorValue()) + "'>";
                    p += "</div>";
                } else {
                    // Render input for String and Float parameters
                    p += "<div class='form-row'>";
                    p += "<label for='" + paramName + "'>" + paramName + ":</label>";
                    if (param.getType() == ConfigParameter::STRING) {
                        p += "<input type='text' name='" + paramName + "' value='" + param.getStringValue() + "'>";
                    } else if (param.getType() == ConfigParameter::FLOAT) {
                        p += "<input type='number' step='any' name='" + paramName + "' value='" + String(param.getFloatValue()) + "' min='-1000000'>";                      }
                    p += "</div>";
                }
            }
            p += "<input type='submit' value='Submit'>";

        } else {
            p += "<p>No parameters available on this page.</p>";
        }
        p += "</form></div>";

        // Display grouped parameters (Each group in its own tab)
        for (const auto& group : groupedParams) {
            p += "<div id='" + group.first + "' class='tab-content'><form action=\"/submit\" method=\"POST\">";
            for (const String& subParamName : group.second) {
                String fullParamName = group.first + "_" + subParamName;
                const ConfigParameter& param = configParams[fullParamName];
                if (param.getType() == ConfigParameter::BOOLEAN) {
                    // Render checkbox for Boolean parameters with label on the left
                    p += "<div class='form-row'>";
                    p += "<span class='checkbox-label'>" + subParamName + "</span>";
                    p += "<input type='hidden' name='" + fullParamName  + "' value='false'>";
                    p += "<input type='checkbox' name='" + fullParamName  + "' value='true'" + (param.getBoolValue() ? " checked" : "") + ">";
                    p += "</div>";

                } else if (param.getType() == ConfigParameter::ARRAY_STRING) {
                    // Render ArrayString parameters
                    // [Your existing code to render ArrayString parameters]
                } else if (param.getType() == ConfigParameter::COLOR) {
                    // Render color picker for Color parameters
                    p += "<div class='form-row'>";
                    p += "<label for='" + fullParamName + "'>" + subParamName + ":</label>";
                    p += "<input type='color' name='" + fullParamName + "' value='" + CRGBtoHex(param.getColorValue()) + "'>";
                    p += "</div>";
                } else {
                    // Render input for String and Float parameters
                    p += "<div class='form-row'>";
                    p += "<label for='" + fullParamName + "'>" + subParamName + ":</label>";
                    if (param.getType() == ConfigParameter::STRING) {
                        p += "<input type='text' name='" + fullParamName + "' value='" + param.getStringValue() + "'>";
                    } else if (param.getType() == ConfigParameter::FLOAT) {
                        p += "<input type='number' step='any' name='" + fullParamName + "' value='" + String(param.getFloatValue()) + "' min='-1000000'>";                    }
                    p += "</div>";
                }
            }
            p += "<input type='submit' value='Submit'></form></div>";
        }

        // Insert custom HTML if any
        p += customHTML;

        p += "</body></html>";

        server.send(200, "text/html", p);
    }

    void handleSubmit() {
        // Map to store parameter values
        std::map<String, std::vector<String>> paramValues;

        // Collect values from the form
        for (uint8_t i = 0; i < server.args(); i++) {
            String paramName = server.argName(i);
            String paramValue = server.arg(i);

            // Remove '[]' from parameter name if present
            if (paramName.endsWith("[]")) {
                paramName = paramName.substring(0, paramName.length() - 2);
            }

            paramValues[paramName].push_back(paramValue);
        }

        // Process each parameter
        for (const auto& param : paramValues) {
            String paramName = param.first;
            const std::vector<String>& values = param.second;

            if (configParams.find(paramName) != configParams.end()) {
                ConfigParameter::Type type = configParams[paramName].getType();

                if (type == ConfigParameter::STRING) {
                    setParam(paramName, values[0]);
                } else if (type == ConfigParameter::FLOAT) {
                    setParam(paramName, values[0].toFloat());
                } else if (type == ConfigParameter::BOOLEAN) {
                    // Check if any of the values is 'true'
                    bool boolValue = false;
                    for (const auto& v : values) {
                        if (v == "true") {
                            boolValue = true;
                            break;
                        }
                    }
                    setParam(paramName, boolValue);
                } else if (type == ConfigParameter::ARRAY_STRING) {
                    setParam(paramName, values);
                } else if (type == ConfigParameter::COLOR) {
                    setParam(paramName, hexToCRGB(values[0]));
                }
            }
        }

        // Call the propertiesModifiedCallback if it's set
        if (propertiesModifiedCallback) {
            propertiesModifiedCallback();
        }

        // Reload the current page after submission
        server.send(200, "text/html", "<html><body><script>window.location.href = '/';</script></body></html>");
    }

    void handleNotFound() {
        if (captivePortal()) {
            return;
        }
        String message = "404 Not Found\n\n";
        message += "URI: ";
        message += server.uri();
        message += "\nMethod: ";
        message += (server.method() == HTTP_GET) ? "GET" : "POST";
        message += "\nArguments: ";
        message += server.args();
        message += "\n";
        for (uint8_t i = 0; i < server.args(); i++) {
            message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
        }
        server.send(404, "text/plain", message);
    }

    boolean captivePortal() {
        if (!isIp(server.hostHeader())) {
            Serial.println("Request redirected to captive portal");
            server.sendHeader("Location", String("http://") + toStringIp(server.client().localIP()), true);
            server.send(302, "text/plain", "");
            server.client().stop();
            return true;
        }
        return false;
    }

    bool isIp(String str) {
        for (size_t i = 0; i < str.length(); i++) {
            int c = str.charAt(i);
            if ((c != '.') && (c < '0' || c > '9')) {
                return false;
            }
        }
        return true;
    }

    String toStringIp(IPAddress ip) {
        return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
    }
};

class ButtonHandler {
public:
    // Method to add a digital button with a name and pin number
    void addButton(const String& name, uint8_t pin) {
        // Configure the pin as input with internal pull-up
        pinMode(pin, INPUT_PULLUP);
        ButtonState state;
        state.pin = pin;
        state.isTouch = false; // Digital button
        state.inverted = false;
        state.lastStableState = digitalRead(pin) == LOW;
        state.lastReading = state.lastStableState;
        state.lastDebounceTime = millis();
        buttons[name] = state;
    }

    // Method to add a touch-button with a name and touch pin number
    void addTouchButton(const String& name, uint8_t touchPin, int threshold) {
        // No need to set pinMode for touch pins on ESP32
        ButtonState state;
        state.pin = touchPin;
        state.isTouch = true; // Touch button
        state.inverted = false;
        state.threshold = threshold;
        state.lastStableState = isTouchPressed(state);
        state.lastReading = state.lastStableState;
        state.lastDebounceTime = millis();
        buttons[name] = state;
    }

    // Method to set the button state changed callback
    void onButtonStateChanged(std::function<void(String, bool)> callback) {
        buttonCallback = callback;
    }

    // Method to update the button states; should be called in the loop()
    void update() {
        unsigned long currentTime = millis();
        for (auto& pair : buttons) {
            const String& name = pair.first;
            ButtonState& state = pair.second;
            bool reading;

            if (state.isTouch) {
                reading = isTouchPressed(state);
            } else {
                reading = digitalRead(state.pin) == LOW; // Digital buttons are active LOW
            }

            reading = reading ^ state.inverted; //XOR makes conditional inversion

            if (reading != state.lastReading) {
                // Reset the debouncing timer
                state.lastDebounceTime = currentTime;
            }

            if ((currentTime - state.lastDebounceTime) > debounceDelay) {
                // If the reading has been stable longer than debounceDelay
                if (reading != state.lastStableState) {
                    state.lastStableState = reading;

                    // Button state changed, call the callback
                    if (buttonCallback) {
                        buttonCallback(name, reading); // Pass the name and the new state (pressed/released)
                    }
                }
            }

            state.lastReading = reading;
        }
    }

    void setInverted(const String& name, bool inv){
        auto it = buttons.find(name);
        if (it != buttons.end()) {
            it->second.inverted = inv;
        }
    }

    // Method to get the current state of a button
    bool isButtonPressed(const String& name) const {
        auto it = buttons.find(name);
        if (it != buttons.end()) {
            return it->second.lastStableState;
        }
        return false; // Button not found
    }

    // Method to update the threshold of a touch-button
    void setTouchThreshold(const String& name, int threshold) {
        auto it = buttons.find(name);
        if (it != buttons.end() && it->second.isTouch) {
            it->second.threshold = threshold;
        }
    }

private:
    struct ButtonState {
        uint8_t pin;
        bool isTouch; // false for digital buttons, true for touch-buttons
        int threshold; // Relevant only for touch-buttons
        bool lastStableState; // The last stable state
        bool lastReading;     // The last reading from the pin
        unsigned long lastDebounceTime;
        bool inverted;
    };
    
    std::map<String, ButtonState> buttons;
    std::function<void(String, bool)> buttonCallback;
    // 25ms, not 5: the USB-serial chip drives GPIO3 (Button3) whenever the monitor is
    // open, and a shorter window lets that register as phantom presses.
    const unsigned long debounceDelay = 25; // Debounce delay in milliseconds

    // Helper method to determine if a touch-button is pressed
    bool isTouchPressed(const ButtonState& state) const {
        // touchRead returns higher values when not touched and lower when touched
        // Typically, a touch threshold might be around 40-50, but calibrate as needed
        int touchValue = touchRead(state.pin);
        return touchValue < state.threshold;
    }
};

// Global WebConfig object
WebConfig webConfig("CJ_FB_Red", "Flash1234");
RunningDot dot;
TipAnimator tip;
ButtonHandler buttonHandler;

// Single place the dot colour is set, so the FillUp balloon always tracks it
void setDotColor(const CRGB& c) {
    gDotColor = c;
    dot.setColor(c);
    tip.onDotColorChanged(c); // a draining balloon recolours immediately
}

// Button handlers never write NVS directly. Each write is a flash operation that suspends
// the instruction cache, and a noisy input pin can produce them far faster than intended -
// GPIO3 in particular. Mark the value dirty here and flush it later, at a bounded rate.
#define SAVE_INTERVAL_MS 5000

bool gColorDirty = false;
bool gTipModeDirty = false;
unsigned long gLastSaveTime = 0;

void flushPendingSaves() {
    if (!gColorDirty && !gTipModeDirty) return;

    unsigned long now = millis();
    if (now - gLastSaveTime < SAVE_INTERVAL_MS) return;
    gLastSaveTime = now;

    if (gColorDirty) {
        webConfig.setParam(String("Color"), gDotColor);
        gColorDirty = false;
    }
    if (gTipModeDirty) {
        webConfig.setParam(String("Tip_Mode"), (float)tip.getMode());
        gTipModeDirty = false;
    }
}

// Point FastLED at the configured strip length. The buffer is always MAX_LEDS, but the
// controller only clocks out gLedCount pixels, so shortening the strip costs no extra
// frame time and needs no reboot.
void applyGeometry(float rawCount, float rawTip) {
    int count = constrain((int)rawCount, 1, MAX_LEDS);
    int maxTip = count < MAX_TIP_LEDS ? count : MAX_TIP_LEDS;
    int tipLen = constrain((int)rawTip, 0, maxTip);

    gLedCount = (uint16_t)count;
    gTipLength = (uint16_t)tipLen;

    fill_solid(leds, MAX_LEDS, CRGB::Black); // drop anything left past the new end
    FastLED[0].setLeds(leds, gLedCount);

    Serial.print("Strip: ");
    Serial.print(gLedCount);
    Serial.print(" LEDs, trail 0..");
    Serial.print(trailLength() - 1);
    Serial.print(", tip ");
    Serial.println(gTipLength);
}

void handlePropertiesModified() {
    Serial.println("Properties were modified.");
    // Update dot parameters based on the new configuration
    dot.setBrightness(webConfig.getParamFloat("Brightness"));
    dot.setSpeed(webConfig.getParamFloat("Speed"));
    dot.setWidth(webConfig.getParamFloat("Width"));
    dot.setDecayFactor(webConfig.getParamFloat("DecayFactor"));
    // Update the color if color parameters have changed
    setDotColor(webConfig.getParamColor("Color"));
    buttonHandler.setTouchThreshold("Touch", webConfig.getParamFloat("TouchSens"));

    // gReverse first: setMode() and setFillSteps() reset the balloon to empty or full
    // depending on which way the dots are running.
    gReverse = webConfig.getParamBool("Reverse");
    dot.setDotMode((int)webConfig.getParamFloat("Tip_DotMode"));
    applyGeometry(webConfig.getParamFloat("LedCount"), webConfig.getParamFloat("Tip_Length"));
    tip.setColor(webConfig.getParamColor("Tip_Color"));
    tip.setSpeed(webConfig.getParamFloat("Tip_Speed"));
    tip.setBrightness((uint8_t)constrain((int)webConfig.getParamFloat("Tip_Brightness"), 0, 255));
    tip.setHueShift(webConfig.getParamFloat("Tip_HueShift"));
    tip.setMode((int)webConfig.getParamFloat("Tip_Mode"));
    tip.setFillSteps((int)webConfig.getParamFloat("Tip_FillSteps"));

    buttonHandler.setInverted("Trigger", webConfig.getParamBool("Invert_Trigger"));
    buttonHandler.setInverted("External", webConfig.getParamBool("Invert_External"));
    buttonHandler.setInverted("Touch", webConfig.getParamBool("Invert_Touch"));
}

//called if a physical button, trigger, external or touch is pressed/activated
void handleButtonPressed(String name, bool pressed) {
    Serial.print("Button ");
    Serial.print(name);
    Serial.print(" ");
    Serial.println(pressed ? "pressed" : "released");

    // Button2 cycles the tip animation and remembers it across reboots
    if (name == "Button2") {
        if (pressed) {
            tip.nextMode();
            gTipModeDirty = true; // saved later by flushPendingSaves()
            Serial.print("Tip mode ");
            Serial.print(tip.getMode());
            Serial.print(": ");
            Serial.print(tip.getModeName());
            Serial.println(gTipLength == 0 ? " (tip length is 0, nothing to show)" : "");
        }
        return;
    }

    // Button3 jumps the dot colour to a random hue. The step is bounded away from 0 and
    // 256 so consecutive presses are always visibly different.
    if (name == "Button3") {
        if (pressed) {
            gDotHue += random8(40, 216);
            setDotColor(CRGB(CHSV(gDotHue, 255, 255)));
            gColorDirty = true; // saved later by flushPendingSaves()
            Serial.print("Dot color -> hue ");
            Serial.println(gDotHue);
        }
        return;
    }

    bool trig = (name == "Trigger" && webConfig.getParamBool("Use_Trigger"));
    trig = trig || (name == "External" && webConfig.getParamBool("Use_External"));
    trig = trig|| (name == "Touch" && webConfig.getParamBool("Use_Touch"));
    trig = trig|| (name == "Web" && webConfig.getParamBool("Use_Web"));
    if (trig) {
        if (pressed) {
            // Handle the action for the 'Trigger' button pressed
            Serial.println("Trigger pressed.");
            dot.trigger();
            tip.onDotFired(gDotColor); // steps the balloon in FillUp mode when in reverse
        } else {
            // Handle the action for the 'Trigger' button released
            Serial.println("Trigger  released.");
            // Add any action needed on button release
        }
    }
    // Add cases for other buttons if needed
}

// Called when a button on the website is pressed
void handleWebButtonPressed(String name) {
    Serial.println("Web Button pressed: " + name);

    handleButtonPressed(name, true);
    Serial.println(name);
}

void setup() {
    // TX only (rx pin = -1) so GPIO3/RX0 is free for BUTTON3_PIN
    Serial.begin(115200, SERIAL_8N1, -1, 1);
    delay(1000);
    Serial.println("Configuring access point...");
    random16_set_seed((uint16_t)micros()); // otherwise Button3 picks the same colours every boot
    
    // Set dynamic title for the configuration page
    webConfig.setTitle("Flash Buzzer Configuration");

    // Add configuration parameters
    webConfig.addParamColor("Color", CRGB::White);
    webConfig.addParamFloat("Speed", 30);
    webConfig.addParamFloat("Brightness", 30);
    webConfig.addParamFloat("Width", 30);
    webConfig.addParamFloat("TouchSens", 20);
    webConfig.addParamBoolean("Use_Trigger", true);
    webConfig.addParamBoolean("Use_External", false);
    webConfig.addParamBoolean("Use_Touch", false);
    webConfig.addParamBoolean("Use_Web", true);
    webConfig.addParamBoolean("Invert_Trigger", false);
    webConfig.addParamBoolean("Invert_External", false);
    webConfig.addParamBoolean("Invert_Touch", false);
    webConfig.addParamFloat("DecayFactor", 3.0f);
    webConfig.addParamFloat("LedCount", DEFAULT_LED_COUNT);
    webConfig.addParamBoolean("Reverse", false);  // shoot towards the balloon, or away from it

    // Tip section, shown on its own "Tip" tab (WebConfig groups by the name prefix)
    webConfig.addParamFloat("Tip_Length", DEFAULT_TIP_LENGTH); // 0 disables the tip entirely
    webConfig.addParamColor("Tip_Color", CRGB::Aqua);
    webConfig.addParamFloat("Tip_Speed", 1.0f);
    webConfig.addParamFloat("Tip_Brightness", 255);
    webConfig.addParamFloat("Tip_HueShift", 0);   // >0 overrides Tip_Color with a rotating hue
    webConfig.addParamFloat("Tip_Mode", 0);       // also cycled by Button2
    webConfig.addParamFloat("Tip_DotMode", 0);    // 0 stop at tip, 1 run through, 2 mirrored
    webConfig.addParamFloat("Tip_FillSteps", 8);  // dots needed to fill the balloon in FillUp mode

    // Set the web button pressed callback
    webConfig.onWebButtonPressed(handleWebButtonPressed);
    webConfig.onPropertiesModified(handlePropertiesModified);

    // Set the properties modified callback
    webConfig.onPropertiesModified(handlePropertiesModified);// Construct the custom HTML with styled button and information
    String customHTML = "<div style='"
                "  display: flex; "
                "  flex-direction: column; "
                "  align-items: center; "
                "  background-color: #f9f9f9; "
                "  padding: 20px; "
                "  border-radius: 10px; "
                "  box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);"
                "  font-size: 3em !important;"
                "'>"
                
                // Add the styled button
                + webConfig.getHtmlButton("Web") +
                "  </div>"
                // Add the informational text with specific styles
                "<div style='"
                "  text-align: center; "
                "  font-size: 1.5em; "
                "  margin-top: 20px; "
                "  color: #333;"
                "'>"
                "  <p><strong style='color: #555;'>White</strong> = GND</p>"
                "  <p><strong style='color: #555;'>Red</strong> = EXTERNAL</p>"
                "  <p><strong style='color: #555;'>Green</strong> = TOUCH</p>"
                "</div>";

    
    webConfig.setCustomHTML(customHTML);

    
    webConfig.begin(); // Start the AP and web server

    // Register the full buffer once; applyGeometry() narrows it to the configured length
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, MAX_LEDS);

    // Initialize dot with initial parameters
    dot.setBrightness(webConfig.getParamFloat("Brightness"));
    dot.setSpeed(webConfig.getParamFloat("Speed"));
    setDotColor(webConfig.getParamColor("Color"));
    dot.setWidth(webConfig.getParamFloat("Width"));
    dot.onTipReached([](CRGB c) { tip.onDotArrived(c); }); // fills the balloon in that dot's colour
    dot.begin();

    // Initialize button handler and add buttons
    buttonHandler.addButton("Trigger", BUTTON_PIN);
    buttonHandler.addButton("External", EXTERNAL_PIN);
    buttonHandler.addTouchButton("Touch", TOUCH_PIN, webConfig.getParamFloat("TouchSens"));
    buttonHandler.addButton("Button2", BUTTON2_PIN); // cycles the tip animation mode
    buttonHandler.addButton("Button3", BUTTON3_PIN); // randomises the dot colour

    // Set the button state changed callback
    buttonHandler.onButtonStateChanged(handleButtonPressed);

    // Optionally, call handlePropertiesModified to initialize any dependent states
    handlePropertiesModified();
}

void loop() {
    webConfig.handleClient(); // Handle client requests
    buttonHandler.update();   // Update button states and handle presses
    flushPendingSaves();      // Rate-limited NVS writes, never from a button handler

    // One clear/show per frame for the whole strip. Tip first because it assigns its
    // pixels, dots second because they add on top and may run over the tip.
    fill_solid(leds, gLedCount, CRGB::Black);
    tip.update();  // balloon
    dot.update();  // trail
    FastLED.show();
}
