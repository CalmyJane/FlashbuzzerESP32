# Flashbuzzer ESP32

An ESP32 built into a **Grobhandtaster** (palm-button buzzer — the big mushroom-head
pushbutton you slam with your hand) driving a WS2812B LED strip. Hit the button and a dot
of light shoots away down the strip. It is made for parties and events, where people can
"shoot" light along a strip and race or duel each other.

Everything is configured over WiFi: the device opens its own access point and serves a
configuration page, so there is no app to install and no network to join.

## Balloon mode

The newest feature. The last stretch of the strip — the **tip** — is wrapped around a
helium balloon and floats upwards, with the rest of the strip trailing down to the buzzer.

The tip is wrapped starting at the bottom of the balloon, up one side, over the top point
and back down to the bottom. The firmware knows this shape: internally every tip pixel is
addressed by its *height*, `0.0` at the balloon's bottom and `1.0` at its top point, with
both ends of the tip meeting at the bottom. Every tip animation is written in terms of
height, so all of them are automatically symmetric across both sides of the balloon.

The strip is therefore two regions:

```
  [ ------------- trail ------------- ][ --- tip --- ]
   0                                    LedCount - TipLength ... LedCount-1
   dots spawn here                      wrapped around the balloon
```

Set `Tip_Length` to `0` to disable the tip entirely and get a plain strip.

`Tip_Length` is **not** limited by `LedCount`. Setting it longer than the strip is allowed
and means the wrap is only partly lit: the animations still run over the full balloon shape,
but only the part that physically exists is shown. Since the tip is anchored to the far end
of the strip, what you see is the *end* of the wrap. A 600-LED tip on a 300-LED strip
therefore lights the top point down one side to the bottom — half a balloon — with no trail
left over. Useful for a balloon that is only half covered, or for wrapping a larger one.

## Hardware

### The strip

Typically a flexible **LED wire** (a pixel string rather than a rigid strip) with one LED
every 2 cm and around **300 LEDs**, so roughly **6 m** end to end. The flexibility is what
lets the last stretch wrap neatly around the balloon.

Set `LedCount` to match the string. With the default `Tip_Length` of 50, a 300-LED build
gives a 250-LED trail (5 m) and a 50-LED tip (1 m) around the balloon.

### Power

Usually a **USB power bank**. That works because the strip is almost never fully lit — a
dot in flight plus the tip animation is a small fraction of the string at any moment.

Do not treat the full-white figure as the budget: 300 LEDs at full white would draw about
18 A, far beyond any power bank. What keeps it comfortable is the master `Brightness`, which
defaults to a low value, and how few LEDs are on at once. A filled balloon at default
brightness is well under an amp.

Two practical consequences:

- **Turning `Brightness` right up on a long string can brown out the power bank.** The
  symptom is the ESP32 resetting, or WiFi becoming unreliable, when the strip lights up.
- **Voltage drops along 6 m of thin wire.** The balloon is at the far end, so it is the
  worst-affected part: colours there can shift warm and whites look yellowish. Injecting
  power at the far end as well as the buzzer end fixes it if it bothers you.

### Pins

| Pin | Function |
| --- | --- |
| GPIO16 | WS2812B data |
| GPIO13 | Trigger — the palm button itself |
| GPIO17 | External trigger, 3-pin connector, **red** wire |
| GPIO4  | Touch input, 3-pin connector, **green** wire (attach to something metal) |
| GPIO19 | Button 2 — cycles the tip animation |
| GPIO3  | Button 3 — randomises the dot colour |

The 3-pin connector's **white** wire is GND.

**GPIO3 is the UART RX pin.** `Serial` is started TX-only so it can be used as a GPIO, but
the USB-serial chip still drives that line physically. Two consequences: keep Button 3
released while flashing, or the upload fails; and it can register phantom presses while the
serial monitor is open, which is why the debounce is 25 ms rather than 5 ms.

## Configuration

Join the access point and the configuration page opens as a captive portal.

| | |
| --- | --- |
| SSID | `CJ_FB_Red` |
| Password | `Flash1234` |
| Address | `http://8.8.8.8` |

Settings are stored in the ESP32's NVS and survive a reboot. Parameters are grouped into
tabs by their name prefix — `Tip_Length` appears as *Length* on the *Tip* tab.

### Home

| Parameter | Meaning |
| --- | --- |
| `Color` | Colour of the next dot fired |
| `Speed` | Dot travel speed, pixels per second |
| `Brightness` | Master brightness for the whole strip |
| `Width` | Dot width — how far its glow spreads |
| `DecayFactor` | How sharply a dot's glow falls off |
| `TouchSens` | Touch threshold; lower is less sensitive |
| `LedCount` | Number of LEDs on the strip |
| `Reverse` | Shoot *from* the balloon back towards you instead of towards it |
| `AutoPlay` | Run the strip by itself, no triggers needed — see below |

### Use / Invert

`Use_Trigger`, `Use_External`, `Use_Touch`, `Use_Web` enable each trigger source.
`Invert_Trigger`, `Invert_External`, `Invert_Touch` flip the logic for normally-closed
switches.

### Tip

| Parameter | Meaning |
| --- | --- |
| `Tip_Length` | LEDs wrapped around the balloon; `0` disables the tip. May exceed `LedCount` |
| `Tip_Color` | Colour for the tip animations |
| `Tip_Speed` | Animation speed multiplier |
| `Tip_Brightness` | Tip brightness, on top of the master `Brightness` |
| `Tip_HueShift` | Hue steps per second; above `0` this overrides `Tip_Color` and makes any fixed-colour mode cycle |
| `Tip_Mode` | Animation, `0`–`10`; also cycled by Button 2 |
| `Tip_DotMode` | How dots interact with the tip, see below |
| `Tip_FillSteps` | Dots needed to fill the balloon in FillUp mode |

## Tip animations

| # | Name | |
| --- | --- | --- |
| 0 | RiseUp | Dots climb both sides to the top point |
| 1 | FallDown | Dots pour from the top point down both sides |
| 2 | Breathe | The whole balloon swells and fades |
| 3 | Rainbow | Rainbow banded by height, scrolling upwards *(multicolour)* |
| 4 | Comet | A bright head orbiting the balloon, tail wrapping the seam |
| 5 | Fire | Flames climbing to the top point *(multicolour)* |
| 6 | Sparkle | Random twinkles fading out |
| 7 | Waterline | A bright surface fills to the top, then drains |
| 8 | Aurora | Slow drifting colour clouds *(multicolour)* |
| 9 | Strobe | Sharp double-flash, new colour each burst *(multicolour)* |
| 10 | FillUp | The balloon inflates as dots land on it, see below |

Modes not marked *multicolour* use `Tip_Color`, or cycle continuously if `Tip_HueShift` is
above `0`.

### Dot modes

`Tip_DotMode` controls what a dot does when it reaches the balloon:

- **0** — stops at the tip.
- **1** — carries on through it, drawn over the tip animation.
- **2** — carries on *mirrored*, so the dot rounds the balloon up both sides at once and
  the two halves meet at the top point.

### FillUp

A game mode rather than an ambient one. Each dot that lands adds one step of "liquid" to
the balloon, filling it from the bottom up on both sides at once. After `Tip_FillSteps`
dots it is full, and the next dot triggers a bright burst and a quick drain back to empty.

FillUp overrides `Tip_DotMode` and `Tip_Color`: dots always stop at the tip so they can
land, and the balloon is drawn in the **dots' own colours**. Each arriving dot's colour goes
in at the **bottom** of the balloon and pushes what is already in there upwards, the way
liquid poured in from below would. So firing red, then blue, then green leaves green at the
bottom, blue above it and red at the top, and the drain shows that same stack.

With `Reverse` on it works the other way round: the balloon starts full and each dot fired
drains one step out of it. Draining, the balloon is a single colour and follows the current
colour immediately, so changing colour recolours it on the spot.

## Buttons

Each dot carries the colour it was fired with, so several differently coloured dots can be
travelling down the strip at once and changing the colour only affects later shots.

Both buttons have a short press and a press-and-hold action. The short action fires on
**release**; the hold action fires **after one second, while still held**, so you get
feedback without waiting, and the release afterwards does nothing.

| Button | Tap | Hold ≥ 1 s |
| --- | --- | --- |
| **Button 2** (GPIO19) | Next tip animation | Random new **tip** colour, mode unchanged |
| **Button 3** (GPIO3) | Random new **dot** colour | Step back through previous dot colours |

New colours are stepped far enough round the hue wheel that consecutive presses always look
clearly different. The last **10** dot colours are remembered, so holding Button 3 walks
backwards through them one at a time. Once it reaches the oldest it simply stays there
rather than wrapping around. Only Button 3's own changes are recorded — setting a colour on
the web page does not add to the history.

Two cases where holding Button 2 has no visible effect: if `Tip_HueShift` is above `0` it
overrides the tip colour with a continuous cycle, and FillUp draws the balloon in the dots'
colours rather than the tip colour.

### Chords

Hold a button down and hit the buzzer for a third action. The held button's own action is
skipped when it is used this way, so a chord never also changes the colour or mode.

| Chord | Result |
| --- | --- |
| Hold **Button 3** + buzzer | Shoots a **rainbow dot** whose hue shifts as it travels |
| Hold **Button 2** + buzzer | Cycles the **dot mode**, `Tip_DotMode` — no dot is fired |
| Hold **both** + tap buzzer | Toggles **`Reverse`**, the shooting direction |
| Hold **both** + hold buzzer | Toggles **AutoPlay** on or off |

The both-buttons chord resolves when you let the buzzer go, so a quick hit flips the
direction and holding it for a second toggles AutoPlay. Cycling the dot mode while the tip
is in FillUp has no visible effect: that mode forces dots to stop at the tip so they can
land on the balloon.

All settings are saved, but written to flash at a bounded rate rather than on every press —
a flash write stalls the CPU cache, and doing that from a button handler can disturb the
WiFi radio.

## AutoPlay

The strip runs itself, for when nobody is at the buzzer. Toggle it by holding both buttons and
holding the buzzer, or with the `AutoPlay` checkbox on the web page; the setting is saved.

Dots fire at random intervals in both directions, colours change, the dot parameters drift,
a dim shimmer keeps the trail alive between shots and a soft band sweeps along now and then.
All of it is adjustable on the **Auto** tab:

| Parameter | Meaning | Default |
| --- | --- | --- |
| `Auto_MinGap` | Shortest wait between shots, ms | 350 |
| `Auto_MaxGap` | Longest wait between shots, ms | 1400 |
| `Auto_NewColor` | New colour on every shot; off keeps the configured `Color` | on |
| `Auto_Rainbow` | Percent of shots that come out as rainbow dots | 27 |
| `Auto_Reverse` | Percent of shots running *away* from the balloon | 43 |
| `Auto_Drift` | Let `Speed`, `Width` and `DecayFactor` wander every 4–9 s | on |
| `Auto_Ambient` | Background shimmer brightness, `0`–`255`; `0` turns it off | 45 |
| `Auto_SweepGap` | Average seconds between sweeps; `0` turns them off | 10 |

For a calmer look turn `Auto_Ambient` down and `Auto_MinGap` up; for a single-colour show
turn `Auto_NewColor` off and `Auto_Rainbow` to `0`.

The **tip is left alone** — whatever tip animation and colour are configured keep running
exactly as in manual mode. The buzzer still works while AutoPlay is on.

Turning AutoPlay off restores `Speed`, `Width`, `DecayFactor` and `Color` from the saved
configuration, since AutoPlay drifts those while it runs.

## Building

```
pio run -t upload      # build and flash
pio device monitor     # serial log at 115200
```

The ESP32 platform is pinned in `platformio.ini`. Leaving it unpinned lets PlatformIO
resolve to a different Arduino core, which changes the WiFi stack underneath identical
source — worth avoiding.

## Notes for future changes

- **Web parameter names must be 15 characters or fewer.** That is the ESP32's NVS key
  length limit; a longer name cannot be stored and the setting silently fails to persist.
- `MAX_LEDS` (700) sizes the pixel buffer at compile time and is the ceiling for both
  `LedCount` and `Tip_Length`. The tip is rendered into a staging buffer of that size and
  only its visible window is blitted onto the strip, which is what lets the wrap be longer
  than the strip.
- `LedCount` is applied through `FastLED[0].setLeds()`, so strip length changes take effect
  on submit without a reboot.
- Rendering order in `loop()` is tip first (it assigns pixels), dots second (they add on
  top), then one `FastLED.show()`.
- If a tip animation looks vertically mirrored, the balloon is wrapped the other way round:
  flip `heightAt()`.
