// SPDX-FileCopyrightText: 2023 Phil B. for Adafruit Industries
// SPDX-License-Identifier: MIT

#include <Adafruit_seesaw.h>
#include <seesaw_neopixel.h>

// NeoPixel configuration
#define PIN 15
#define TOTAL_PIXELS 36

// Tiki Face Sections
#define BOTTOM_TEETH_START 0
#define BOTTOM_TEETH_END 11
#define TOP_TEETH_START 12
#define TOP_TEETH_END 23
#define LEFT_EYE_START 24
#define LEFT_EYE_END 29
#define RIGHT_EYE_START 30
#define RIGHT_EYE_END 35

// ANO Rotary Encoder
#define ANO_ADDR 0x36
#define ANO_SWITCH 24 // center switch
#define ANO_UP 2      // up button
#define ANO_DOWN 1    // down button
#define ANO_LEFT 3    // left button
#define ANO_RIGHT 0   // right button

// Define patterns
#define NUM_PATTERNS 6

seesaw_NeoPixel strip =
    seesaw_NeoPixel(TOTAL_PIXELS, PIN, NEO_GRB + NEO_KHZ800);
Adafruit_seesaw ano = Adafruit_seesaw();

// Global variables
int currentPattern = 0;
int brightness = 100; // 0-255
uint32_t lastPatternChange = 0;
uint32_t lastButtonCheck = 0;
uint32_t lastUpdate = 0;
uint32_t animationStep = 0;
bool anoAvailable = false;

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial a moment to initialize

  Serial.println("Starting tiki LED sculpture");

  // Initialize NeoPixels first
  Serial.println("Initializing NeoPixel seesaw...");
  if (!strip.begin(0x60)) {
    Serial.println("NeoPixel seesaw not found! Check connections.");
    // Blink the built-in LED to indicate error
    pinMode(LED_BUILTIN, OUTPUT);
    while (1) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(300);
      digitalWrite(LED_BUILTIN, LOW);
      delay(300);
    }
  }

  Serial.println("NeoPixel seesaw started OK!");
  strip.setBrightness(brightness);
  strip.show(); // Initialize all pixels to 'off'

  // Simple test pattern to verify LEDs are working
  Serial.println("Testing LEDs with simple patterns...");

  // All red
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(50, 0, 0));
  }
  strip.show();
  delay(500);

  // All green
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(0, 50, 0));
  }
  strip.show();
  delay(500);

  // All blue
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(0, 0, 50));
  }
  strip.show();
  delay(500);

  allOff();
  delay(500);

  // Initialize ANO encoder (optional)
  Serial.println("Attempting to connect to ANO encoder...");

  // Try to connect to ANO
  for (int attempt = 0; attempt < 3; attempt++) {
    delay(100);
    anoAvailable = ano.begin(ANO_ADDR);
    if (anoAvailable)
      break;
    Serial.println("ANO connection attempt failed, retrying...");
  }

  if (!anoAvailable) {
    Serial.println("ANO seesaw not found! Will run without input controls.");
    Serial.println("Cycling through patterns automatically.");
  } else {
    Serial.println("ANO seesaw started OK!");

    // Configure ANO buttons
    ano.pinMode(ANO_SWITCH, INPUT_PULLUP);
    ano.pinMode(ANO_UP, INPUT_PULLUP);
    ano.pinMode(ANO_DOWN, INPUT_PULLUP);
    ano.pinMode(ANO_LEFT, INPUT_PULLUP);
    ano.pinMode(ANO_RIGHT, INPUT_PULLUP);

    // Initialize encoder
    ano.setEncoderPosition(0);

    Serial.println("ANO controls ready!");
  }

  // Initialize the pattern change timer
  lastPatternChange = millis();
  lastUpdate = millis();

  Serial.println("Setup complete - starting animation");
}

void loop() {
  uint32_t currentMillis = millis();

  // Check buttons if ANO is available
  if (anoAvailable && currentMillis - lastButtonCheck >= 50) {
    checkInputs();
    lastButtonCheck = currentMillis;
  }

  // Auto-cycle patterns if ANO is not available
  if (!anoAvailable &&
      currentMillis - lastPatternChange >= 15000) { // Change every 15 seconds
    currentPattern = (currentPattern + 1) % NUM_PATTERNS;
    Serial.print("Auto-changing to pattern: ");
    Serial.println(currentPattern);
    animationStep = 0;
    lastPatternChange = currentMillis;
  }

  // Print heartbeat once per second
  static uint32_t lastHeartbeat = 0;
  if (currentMillis - lastHeartbeat >= 1000) {
    Serial.print("Running pattern: ");
    Serial.println(currentPattern);
    lastHeartbeat = currentMillis;
  }

  // Run current pattern
  updatePattern(currentMillis);
}

void updatePattern(uint32_t currentMillis) {
  // Update animation based on the current pattern
  switch (currentPattern) {
  case 0:
    rainbowTiki(currentMillis, 20);
    break;
  case 1:
    fireEyesPattern(currentMillis, 50);
    break;
  case 2:
    chatteringTeethPattern(currentMillis, 250);
    break;
  case 3:
    blinkingEyesPattern(currentMillis, 100);
    break;
  case 4:
    alternatingTeethPattern(currentMillis, 150);
    break;
  case 5:
    breathingPattern(currentMillis, 30);
    break;
  default:
    // Default to rainbow
    rainbowTiki(currentMillis, 20);
    break;
  }
}

void checkInputs() {
  if (!anoAvailable)
    return;

  // Read encoder for brightness control
  int32_t encoderPosition = ano.getEncoderPosition();
  static int32_t lastPosition = 0;

  if (encoderPosition != lastPosition) {
    int change = encoderPosition - lastPosition;
    brightness = constrain(brightness + change * 5, 10, 255);
    strip.setBrightness(brightness);
    Serial.print("Brightness: ");
    Serial.println(brightness);
    lastPosition = encoderPosition;
  }

  // Read buttons with debouncing
  static bool lastButtons[5] = {false, false, false, false, false};

  // Center button - next pattern
  bool centerButton = !ano.digitalRead(ANO_SWITCH);
  if (centerButton && !lastButtons[0]) {
    changePattern(1);
  }
  lastButtons[0] = centerButton;

  // Up button - increase brightness
  bool upButton = !ano.digitalRead(ANO_UP);
  if (upButton && !lastButtons[1]) {
    brightness = constrain(brightness + 25, 10, 255);
    strip.setBrightness(brightness);
    Serial.print("Brightness: ");
    Serial.println(brightness);
  }
  lastButtons[1] = upButton;

  // Down button - decrease brightness
  bool downButton = !ano.digitalRead(ANO_DOWN);
  if (downButton && !lastButtons[2]) {
    brightness = constrain(brightness - 25, 10, 255);
    strip.setBrightness(brightness);
    Serial.print("Brightness: ");
    Serial.println(brightness);
  }
  lastButtons[2] = downButton;

  // Left button - previous pattern
  bool leftButton = !ano.digitalRead(ANO_LEFT);
  if (leftButton && !lastButtons[3]) {
    changePattern(-1);
  }
  lastButtons[3] = leftButton;

  // Right button - next pattern
  bool rightButton = !ano.digitalRead(ANO_RIGHT);
  if (rightButton && !lastButtons[4]) {
    changePattern(1);
  }
  lastButtons[4] = rightButton;
}

void changePattern(int direction) {
  animationStep = 0;
  lastUpdate = millis();

  currentPattern = (currentPattern + NUM_PATTERNS + direction) % NUM_PATTERNS;
  Serial.print("Pattern: ");
  Serial.println(currentPattern);

  // Clear LEDs for clean transition
  allOff();

  lastPatternChange = millis();
}

// Pattern 1: Rainbow cycle across tiki sections
void rainbowTiki(uint32_t currentMillis, uint8_t wait) {
  static uint16_t j = 0;

  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  // Bottom teeth - base color
  for (int i = BOTTOM_TEETH_START; i <= BOTTOM_TEETH_END; i++) {
    strip.setPixelColor(i, Wheel(((i * 256 / 12) + j) & 255));
  }

  // Top teeth - offset color
  for (int i = TOP_TEETH_START; i <= TOP_TEETH_END; i++) {
    strip.setPixelColor(i, Wheel(((i * 256 / 12) + j + 85) & 255));
  }

  // Left eye
  for (int i = LEFT_EYE_START; i <= LEFT_EYE_END; i++) {
    strip.setPixelColor(i, Wheel(((i * 256 / 6) + j + 170) & 255));
  }

  // Right eye (matching left)
  for (int i = RIGHT_EYE_START; i <= RIGHT_EYE_END; i++) {
    strip.setPixelColor(i, Wheel(((i * 256 / 6) + j + 170) & 255));
  }

  strip.show();

  j = (j + 1) % 256;
}

// Pattern 2: Fire eyes with glowing teeth
void fireEyesPattern(uint32_t currentMillis, uint8_t wait) {
  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  // Teeth glow red
  for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
    strip.setPixelColor(i, strip.Color(30, 0, 0)); // Dim red for teeth
  }

  // Eyes flicker orange/yellow/red
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    uint8_t flicker = random(80, 255);
    // More red than green for orange/yellow fire color
    strip.setPixelColor(i, strip.Color(flicker, flicker / 3, 0));
  }

  strip.show();
}

// Pattern 3: Chattering teeth
void chatteringTeethPattern(uint32_t currentMillis, uint8_t wait) {
  static bool teethOpen = false;

  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  teethOpen = !teethOpen; // Toggle teeth state

  if (teethOpen) {
    // Open mouth - bottom teeth glow orange, top teeth light pink
    for (int i = BOTTOM_TEETH_START; i <= BOTTOM_TEETH_END; i++) {
      strip.setPixelColor(i, strip.Color(255, 100, 0)); // Orange-yellow
    }
    for (int i = TOP_TEETH_START; i <= TOP_TEETH_END; i++) {
      strip.setPixelColor(i, strip.Color(255, 150, 150)); // Light pink
    }
  } else {
    // Closed mouth - all teeth white
    for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
      strip.setPixelColor(i, strip.Color(255, 255, 255)); // White
    }
  }

  // Eyes glow red
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    strip.setPixelColor(i, strip.Color(255, 0, 0)); // Red
  }

  strip.show();
}

// Pattern 4: Blinking eyes
void blinkingEyesPattern(uint32_t currentMillis, uint8_t wait) {
  static uint8_t blinkState = 0;
  static uint32_t nextStateChange = 0;

  if (currentMillis < nextStateChange)
    return;

  switch (blinkState) {
  case 0: // Eyes open
    // Green glowing eyes
    for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
      strip.setPixelColor(i, strip.Color(0, 255, 0));
    }
    // Purple teeth
    for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
      strip.setPixelColor(i, strip.Color(50, 0, 50));
    }
    blinkState = 1;
    nextStateChange =
        currentMillis + random(2000, 5000); // Random time until blink
    break;

  case 1: // Start blinking - eyes half closed
    for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
      strip.setPixelColor(i, strip.Color(0, 100, 0)); // Dimmer green
    }
    blinkState = 2;
    nextStateChange = currentMillis + 80; // Quick transition
    break;

  case 2: // Eyes closed
    for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
      strip.setPixelColor(i, strip.Color(0, 0, 0)); // Off
    }
    blinkState = 3;
    nextStateChange = currentMillis + 150; // Closed for a moment
    break;

  case 3: // Eyes half open
    for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
      strip.setPixelColor(i, strip.Color(0, 100, 0)); // Dimmer green
    }
    blinkState = 0;
    nextStateChange = currentMillis + 80; // Quick transition back
    break;
  }

  strip.show();
  lastUpdate = currentMillis;
}

// Pattern 5: Alternating red and blue teeth
void alternatingTeethPattern(uint32_t currentMillis, uint8_t wait) {
  static bool alternate = false;

  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  alternate = !alternate;

  for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
    bool isEven = (i % 2 == 0);
    if (isEven == alternate) {
      strip.setPixelColor(i, strip.Color(255, 0, 0)); // Red
    } else {
      strip.setPixelColor(i, strip.Color(0, 0, 255)); // Blue
    }
  }

  // Eyes match alternating pattern
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    if (alternate) {
      strip.setPixelColor(i, strip.Color(255, 0, 255)); // Purple
    } else {
      strip.setPixelColor(i, strip.Color(255, 255, 0)); // Yellow
    }
  }

  strip.show();
}

// Pattern 6: Breathing effect (gradually fading in and out)
void breathingPattern(uint32_t currentMillis, uint8_t wait) {
  static uint8_t breathLevel = 0;
  static bool increasing = true;

  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  // Update breath level
  if (increasing) {
    breathLevel += 5;
    if (breathLevel >= 250) {
      breathLevel = 250;
      increasing = false;
    }
  } else {
    breathLevel -= 5;
    if (breathLevel <= 5) {
      breathLevel = 5;
      increasing = true;
    }
  }

  // Apply to all LEDs with different colors
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    if (i >= LEFT_EYE_START && i <= RIGHT_EYE_END) {
      // Eyes - blue
      strip.setPixelColor(i, strip.Color(0, 0, breathLevel));
    } else {
      // Teeth - purple-ish
      strip.setPixelColor(i, strip.Color(breathLevel, 0, breathLevel / 2));
    }
  }

  strip.show();
}

// Turn all LEDs off
void allOff() {
  for (int i = 0; i < strip.numPixels(); i++) {
    strip.setPixelColor(i, 0);
  }
  strip.show();
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if (WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}
