# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based LED tiki sculpture with 36 NeoPixels arranged as a tiki face. Multiple tikis synchronize patterns and colors wirelessly via ESP-Now.

## Build & Upload

Arduino project using arduino-cli (v1.3.1+).

### First-Time Setup

```bash
# Add ESP32 board index (if not already configured)
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

# Install ESP32 core
arduino-cli core install esp32:esp32

# Install required libraries
arduino-cli lib install "Adafruit seesaw Library"
```

### Detect Board

```bash
# List connected boards (shows port and FQBN)
arduino-cli board list
```

Current hardware: **Adafruit Feather ESP32-S3 No PSRAM**
- FQBN: `esp32:esp32:adafruit_feather_esp32s3_nopsram`
- Port: `/dev/cu.usbmodem2101` (varies)

### Compile & Upload

```bash
# Compile only
arduino-cli compile -b esp32:esp32:adafruit_feather_esp32s3_nopsram tikiv3/tikiv3.ino

# Compile and upload (auto-detects port if only one board connected)
arduino-cli compile -b esp32:esp32:adafruit_feather_esp32s3_nopsram -u -p /dev/cu.usbmodem2101 tikiv3/tikiv3.ino

# Upload pre-compiled binary
arduino-cli upload -b esp32:esp32:adafruit_feather_esp32s3_nopsram -p /dev/cu.usbmodem2101 tikiv3/tikiv3.ino
```

### Serial Monitor

```bash
# Open serial monitor (default 115200 baud for ESP32)
arduino-cli monitor -p /dev/cu.usbmodem2101 -c baudrate=115200

# With timestamps
arduino-cli monitor -p /dev/cu.usbmodem2101 -c baudrate=115200 --timestamp
```

### Useful Commands

```bash
arduino-cli board listall | grep -i esp32    # List all ESP32 board variants
arduino-cli lib list                          # Show installed libraries
arduino-cli core list                         # Show installed cores
arduino-cli compile --clean ...               # Force full recompile
```

## Hardware Configuration

- **NeoPixels**: 36 LEDs on pin 15 via seesaw controller at I2C 0x60
- **ANO Encoder**: Adafruit ANO rotary encoder at I2C 0x49 (optional)
- **Sleep Wake Pin**: GPIO 13 for deep sleep interrupt

LED sections (pixel indices):
- Bottom teeth: 0-11
- Top teeth: 12-23
- Left eye: 24-29
- Right eye: 30-35

## Architecture

**Single-file Arduino sketch** (`tikiv3/tikiv3.ino`) with these main components:

1. **ESP-Now Sync**: Broadcasts state every second (or 100ms in fast-sync mode). Devices adopt settings from whichever has the higher timestamp, creating leader-based synchronization. Sync messages include `animTime` for animation phase sync and `customColor` flag.

2. **Pattern System**: 5 patterns controlled by `currentPattern` (0-4). Each pattern has a "Custom" variant that responds to color wheel position. Add new patterns by creating a function and adding to `updatePattern()` switch.

3. **Input Handling**: Rotary encoder changes color, buttons change pattern/brightness. Hold center button 2s for sleep mode.

4. **Color System**: Uses `Wheel()` function (0-255 maps to RGB color wheel). `baseColorOffset` transitions smoothly toward `targetColorOffset` for smooth color changes.

## Animation Synchronization

Animations stay synchronized across all tikis using a shared time mechanism:

### Key Variables
- `bootTime`: Adjusted timestamp used for leader election (higher = leader)
- `sharedTime`: `millis() - bootTime - animationOffset` — the synchronized animation clock
- `animationOffset`: Offset that preserves animation phase when `bootTime` changes

### How It Works
1. **Leader Election**: Device with highest `(millis() - bootTime) / 1000` becomes leader
2. **Animation Sync**: All patterns use `sharedTime` instead of local counters, so animations stay in phase
3. **Phase Preservation**: When a device becomes leader (via `advanceTimestamp()`), it adjusts `animationOffset` to keep `sharedTime` continuous, preventing animation jumps
4. **Receiver Sync**: When receiving sync messages, followers set their `animationOffset` to match the sender's `animTime`, aligning animation phases

### Sync Message Structure
```c
typedef struct sync_message {
  uint32_t timestamp;    // Seconds since boot (leader election)
  uint32_t animTime;     // Animation time in ms (phase sync)
  uint8_t pattern;
  uint8_t brightness;
  uint8_t colorOffset;
  uint8_t customColor;   // Whether custom color is active
} sync_message;
```

### Adding New Synced Patterns
To ensure a new pattern stays synchronized across tikis:
1. Use `sharedTime` instead of a static counter for animation position
2. Calculate animation frame: `uint16_t j = (sharedTime / frameDelayMs) % 256;`
3. Do NOT increment `j` at the end of the function — it's derived from `sharedTime`

## Controls

- **Encoder rotation**: Change color
- **Up/Down buttons**: Brightness
- **Left/Right buttons**: Previous/next pattern
- **Center button (short)**: Randomize all settings
- **Center button (hold 2s)**: Enter deep sleep

## Pattern Analysis

All animated patterns use `sharedTime` for synchronized animation across tikis.

### Pattern 0: Fire Eyes (`fireEyesPatternCustom`)
- **Frame delay**: 50ms
- **Behavior**: Teeth solid color, eyes flicker randomly. When custom color enabled, eyes use complementary color.
- **Sync**: Not time-synced (random flicker is intentionally independent)
- **Tuning**: Adjust flicker min (80) for more/less dim moments

### Pattern 1: Gentle Rainbow (`gentleRainbowTikiCustom`)
- **Frame delay**: 60ms
- **Behavior**: Uses only 60-degree slice of color wheel for cohesive look. Teeth display gradient, eyes show complementary color (+128 on wheel).
- **Sync**: `j = (sharedTime / 120) % 256` — advances every 120ms
- **Tuning**: Adjust `colorRange` (default 60) for wider/narrower palette spread

### Pattern 2: Breathing (`breathingPatternCustom`)
- **Frame delay**: 30ms
- **Behavior**: All pixels fade in/out together. Eyes use base color, teeth use +64 offset.
- **Sync**: `breathPhase = (sharedTime % 3000) / 3000.0 * 2π` — 3-second breath cycle
- **Tuning**: Change 3000 for faster/slower breathing

### Pattern 3: Gradient Teeth (`gradientTeethPattern`)
- **Frame delay**: 40ms
- **Behavior**: Smooth color gradient across all 24 teeth (maps pixel position to 0-120 color range). Eyes pulse using sine wave.
- **Sync**: `j = (sharedTime / 40) % 256` — advances every 40ms
- **Tuning**: Adjust the 120 mapping range for wider gradient

### Pattern 4: Color Wave (`colorWavePattern`)
- **Frame delay**: 30ms
- **Behavior**: Sine wave travels through teeth (top and bottom in opposite phase). Eyes have breathing overlay.
- **Sync**: `j = (sharedTime / 30) % 256` — advances every 30ms
- **Tuning**: Change wave divisors for tighter/looser waves

## Global Tunable Parameters

| Parameter | Range | Default | Location |
|-----------|-------|---------|----------|
| `brightness` | 10-255 | 100 | Button adjusts by ±25 |
| `colorPosition` | 0-255 | 0 | Encoder controlled |
| `BUTTON_HOLD_TIME` | ms | 2000 | Sleep trigger threshold |
| `SYNC_INTERVAL` | ms | 1000 | Normal sync broadcast rate |
| Fast sync interval | ms | 100 | Used for 30s after any change |
| `nextBlinkTime` | ms | 10000-60000 | Random eye blink interval |

## Eye Blink System

Overlays on patterns 0, 2, 3, 4 (skips fire eyes). Four-stage animation:
1. Save current eye colors
2. Dim to 30% (150ms)
3. Off (100ms)
4. Dim to 30% (200ms)
5. Restore (150ms)

Total blink duration: ~600ms. Next blink scheduled randomly 10-60 seconds later.
