// SPDX-FileCopyrightText: 2023 Phil B. for Adafruit Industries
// SPDX-License-Identifier: MIT

#include <Adafruit_seesaw.h>
#include <seesaw_neopixel.h>
#include <esp_now.h>
#include <WiFi.h>
#include <driver/rtc_io.h>

// NeoPixel configuration
#define PIN 15
#define TOTAL_PIXELS 36

// Sleep mode configuration
#define BUTTON_HOLD_TIME 2000      // 2 seconds to trigger sleep/wake

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
#define ANO_ADDR 0x49  // Corrected address per Adafruit docs
#define ANO_SWITCH 1   // center switch
#define ANO_UP 2       // up button
#define ANO_DOWN 4     // down button
#define ANO_LEFT 3     // left button
#define ANO_RIGHT 5    // right button

// Define patterns
#define NUM_PATTERNS 5

// ESP-Now broadcast configuration
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Broadcast to all ESP devices
#define SYNC_INTERVAL 1000 // Sync every second
#define ESP_NOW_MAX_RETRIES 3 // Number of retries for ESP-NOW initialization

// Data structure for ESP-Now synchronization
typedef struct sync_message {
  uint32_t timestamp;    // Seconds since boot
  uint8_t pattern;       // Current pattern number
  uint8_t brightness;    // Current brightness
  uint8_t colorOffset;   // Current color offset
} sync_message;

// Create a sync structure for sending and receiving data
sync_message syncData;

// Flag to track if ESP-NOW is properly initialized
bool espNowInitialized = false;

seesaw_NeoPixel strip =
    seesaw_NeoPixel(TOTAL_PIXELS, PIN, NEO_GRB + NEO_KHZ800);
Adafruit_seesaw ano = Adafruit_seesaw();

// Global variables
int currentPattern = 0;
int brightness = 255; // 0-255 (start at maximum brightness)
int colorPosition = 0; // 0-255 for color wheel position
int baseColorOffset = 0; // Starting color offset for the current pattern
int targetColorOffset = 0; // Target color offset when rotating the encoder
bool useCustomColor = false; // Flag to indicate custom color should be used
uint32_t lastPatternChange = 0;
uint32_t lastButtonCheck = 0;
uint32_t lastUpdate = 0;
uint32_t animationStep = 0;
uint32_t lastEyeBlink = 0; // For occasional random eye blinks
uint32_t nextBlinkTime = 0; // When to do the next blink
bool isBlinking = false; // Currently in a blink animation
uint8_t blinkState = 0; // State of the blink animation
uint32_t startBlinkMs_ = 0; // Start time for the current blink (for easing calculation)
uint32_t endBlinkMs_ = 0; // End time for the current blink (for easing calculation)
float blinkFadeBrightness_ = 1.0; // Brightness factor for eye blinks (1.0 = full brightness, 0.0 = off)
bool anoAvailable = false;

// ESP-Now synchronization variables
uint32_t lastSyncTime = 0;
uint32_t bootTime = 0;
uint32_t lastChangeTime = 0; // Track when last change happened
bool syncPending = false;
bool fastSyncMode = false; // Track if we're in fast sync mode

// Sleep mode variables
uint32_t buttonPressStartTime = 0;
bool buttonLongPressInProgress = false;
bool inSleepMode = false;
bool longPressHandled = false; // Track if the long press was already handled

// Callback when data is sent
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

// Callback when data is received
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  sync_message *incomingSync = (sync_message *)incomingData;
  Serial.print("Received sync: Time=");
  Serial.print(incomingSync->timestamp);
  Serial.print(", Pattern=");
  Serial.print(incomingSync->pattern);
  Serial.print(", Brightness=");
  Serial.print(incomingSync->brightness);
  Serial.print(", ColorOffset=");
  Serial.println(incomingSync->colorOffset);
  
  // Compare received timestamp with local time
  uint32_t localTimestamp = (millis() - bootTime) / 1000;
  
  // If received timestamp is ahead of ours, adopt their settings
  if (incomingSync->timestamp > localTimestamp) {
    Serial.println("Adopting received settings (newer timestamp)");
    // Set sync flag to true so we won't broadcast immediately
    syncPending = true;
    
    // Update our local timestamp to match received one
    // Add a small millisecond offset to account for transmission delay
    bootTime = millis() - (incomingSync->timestamp * 1000 + 50);
    
    bool stateChanged = false;
    
    // Update pattern
    if (currentPattern != incomingSync->pattern) {
      currentPattern = incomingSync->pattern;
      animationStep = 0;
      lastPatternChange = millis();
      stateChanged = true;
    }
    
    // Update brightness
    if (brightness != incomingSync->brightness) {
      brightness = incomingSync->brightness;
      strip.setBrightness(brightness);
      stateChanged = true;
    }
    
    // Update color offset - immediately set both base and target to received value
    if (colorPosition != incomingSync->colorOffset) {
      Serial.print("Updating color from ");
      Serial.print(colorPosition);
      Serial.print(" to ");
      Serial.println(incomingSync->colorOffset);
      
      // Set both base and target color to the exact same value to avoid transition
      baseColorOffset = incomingSync->colorOffset;
      targetColorOffset = incomingSync->colorOffset;
      colorPosition = incomingSync->colorOffset;
      useCustomColor = true;
      stateChanged = true;
    }
    
    // If state changed, enter fast sync mode
    if (stateChanged) {
      lastChangeTime = millis();
      fastSyncMode = true;
      Serial.println("Entering fast sync mode");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial a moment to initialize

  Serial.println("Starting tiki LED sculpture");
  
  // Set boot time
  bootTime = millis();

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

  // Initialize ANO encoder (optional)
  Serial.println("Attempting to connect to ANO encoder...");

  // Try to connect to ANO
  for (int attempt = 0; attempt < 3; attempt++) {
    delay(100);
    anoAvailable = ano.begin(ANO_ADDR);
    if (anoAvailable) {
      uint16_t pid = ano.getVersion();
      Serial.print("Seesaw found Product ID: ");
      Serial.println(pid, HEX);
      
      // Accept the actual product ID (0x4A97) we're seeing
      if (pid != 0x4A97) {
        Serial.println("Unknown product ID for seesaw device");
        anoAvailable = false;
      } else {
        Serial.println("Recognized compatible seesaw device");
        break;
      }
    }
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
    
    // Enable encoder interrupt
    ano.enableEncoderInterrupt();

    Serial.println("ANO controls ready!");
  }

  // Initialize the pattern change timer
  lastPatternChange = millis();
  lastUpdate = millis();

  // Initialize ESP-Now with retry logic
  WiFi.mode(WIFI_STA);
  
  // Get MAC for debugging
  Serial.print("ESP32 MAC Address: ");
  Serial.println(WiFi.macAddress());

  // Try to initialize ESP-Now with multiple attempts
  espNowInitialized = false;
  for (int attempt = 0; attempt < ESP_NOW_MAX_RETRIES; attempt++) {
    Serial.print("ESP-NOW initialization attempt ");
    Serial.print(attempt + 1);
    Serial.print(" of ");
    Serial.println(ESP_NOW_MAX_RETRIES);
    
    // Reset WiFi first
    WiFi.disconnect();
    delay(10);
    WiFi.mode(WIFI_STA);
    delay(10);
    
    // Try to initialize ESP-Now
    esp_err_t result = esp_now_init();
    if (result == ESP_OK) {
      Serial.println("ESP-Now initialized successfully");
      espNowInitialized = true;
      
      // Register the send callback
      esp_now_register_send_cb(OnDataSent);
      
      // Register peer (broadcast address)
      esp_now_peer_info_t peerInfo;
      memset(&peerInfo, 0, sizeof(peerInfo)); // Zero out the structure first
      memcpy(peerInfo.peer_addr, broadcastAddress, 6);
      peerInfo.channel = 0;  
      peerInfo.encrypt = false;
      
      // Delete any existing peer first
      esp_now_del_peer(broadcastAddress);
      
      // Add peer        
      if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add peer");
        espNowInitialized = false;
      } else {
        // Register for a callback function that will be called when data is received
        esp_now_register_recv_cb(OnDataRecv);
        Serial.println("ESP-Now peer added and callbacks registered");
        break; // Success - exit retry loop
      }
    } else {
      Serial.print("ESP-Now init failed with error: ");
      Serial.println(result);
      
      // Try de-initializing before retrying
      esp_now_deinit();
      delay(200); // Brief pause before retry
    }
  }
  
  if (espNowInitialized) {
    Serial.println("ESP-Now initialized and ready");
  } else {
    Serial.println("Failed to initialize ESP-Now after multiple attempts");
    Serial.println("Will continue without sync capabilities");
  }

  // Initialize blink timing
  nextBlinkTime = millis() + random(2000, 30000); // Every 2-30 seconds
  blinkFadeBrightness_ = 1.0; // Start with eyes fully visible
  
  // Make sure blink state is initialized correctly
  blinkState = 0;
  isBlinking = false;

  Serial.println("Setup complete - starting animation");
}

// Function to enter sleep mode (just turn off LEDs)
void enterSleepMode() {
  Serial.println("Entering sleep mode...");
  Serial.println("anoAvailable=" + String(anoAvailable));
  
  // Turn off all LEDs
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    strip.setPixelColor(i, 0);
  }
  strip.show();
  
  // Set sleep mode flag
  inSleepMode = true;
  
  Serial.println("LEDs turned off, device in sleep mode");
}

// Function to wake from sleep mode
void wakeFromSleepMode() {
  Serial.println("Waking from sleep mode...");
  Serial.println("anoAvailable=" + String(anoAvailable));
  
  // Clear sleep mode flag
  inSleepMode = false;
  
  // Reset animation timers
  lastUpdate = millis();
  animationStep = 0;
  
  // Randomize the pattern on wake (optional - gives visual feedback that device is awake)
  currentPattern = random(NUM_PATTERNS);
  
  // Randomize color for immediate visual feedback
  colorPosition = random(256);
  targetColorOffset = colorPosition;
  baseColorOffset = colorPosition;
  useCustomColor = true;
  
  // Show a quick flash of light to confirm wake
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 255));
  }
  strip.show();
  delay(50);
  
  Serial.println("Device awake, resuming normal operation");
}

// Function to broadcast the current state to all other tikis
void broadcastSync() {
  // Skip if ESP-NOW was not successfully initialized
  if (!espNowInitialized) {
    // Only log this periodically to avoid spam
    static uint32_t lastLogTime = 0;
    uint32_t currentMillis = millis();
    if (currentMillis - lastLogTime > 10000) { // Log every 10 seconds
      Serial.println("Broadcast skipped - ESP-NOW not initialized");
      lastLogTime = currentMillis;
    }
    return;
  }
  
  // Prepare the data packet
  memset(&syncData, 0, sizeof(syncData)); // Clear any stale data
  
  // Calculate seconds since boot
  syncData.timestamp = (millis() - bootTime) / 1000;
  syncData.pattern = currentPattern;
  syncData.brightness = brightness;
  
  // Send the colorPosition/targetOffset rather than baseColorOffset
  // This ensures we sync to the target color immediately
  syncData.colorOffset = useCustomColor ? colorPosition : baseColorOffset;
  
  Serial.print("Broadcasting color: ");
  Serial.println(syncData.colorOffset);
  
  // Try to re-initialize ESP-NOW if not working
  static uint8_t errorCount = 0;
  
  // Send message via ESP-Now with retry on failure
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&syncData, sizeof(syncData));
  
  if (result == ESP_OK) {
    Serial.println("Broadcast sent successfully");
    errorCount = 0; // Reset error count on success
  } else {
    Serial.print("Error sending broadcast: ");
    Serial.println(result);
    
    // Count errors and attempt to reinitialize if persistent
    errorCount++;
    if (errorCount >= 5) {
      Serial.println("Multiple broadcast failures - attempting to reinitialize ESP-NOW");
      
      // Try to reinitialize ESP-NOW
      esp_now_deinit();
      delay(100);
      
      if (esp_now_init() == ESP_OK) {
        // Re-register callbacks and peer
        esp_now_register_send_cb(OnDataSent);
        esp_now_register_recv_cb(OnDataRecv);
        
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, broadcastAddress, 6);
        peerInfo.channel = 0;  
        peerInfo.encrypt = false;
        
        esp_now_del_peer(broadcastAddress);
        esp_now_add_peer(&peerInfo);
        
        Serial.println("ESP-NOW reinitialized");
        errorCount = 0;
      } else {
        Serial.println("ESP-NOW reinitialization failed");
      }
    }
  }
}

void loop() {
  uint32_t currentMillis = millis();
  
  // Always check buttons even in sleep mode (to detect wake button press)
  if (anoAvailable && currentMillis - lastButtonCheck >= 50) {
    // Debug log button checking (only every 3 seconds to avoid spamming)
    static uint32_t lastButtonDebug = 0;
    if (inSleepMode && currentMillis - lastButtonDebug > 3000) {
      Serial.println("Still checking buttons in sleep mode (anoAvailable=" + 
                      String(anoAvailable) + ", inSleepMode=" + String(inSleepMode) + ")");
      lastButtonDebug = currentMillis;
    }
    
    checkInputs();
    lastButtonCheck = currentMillis;
  }
  
  // Calculate these values regardless of sleep state
  uint32_t secondsSinceBoot = (currentMillis - bootTime) / 1000;
  uint32_t millisInCurrentSecond = (currentMillis - bootTime) % 1000;
  uint32_t syncInterval = fastSyncMode ? 100 : SYNC_INTERVAL;
  
  // If in sleep mode, skip animation and patterns but still process buttons and ESP-Now
  if (inSleepMode) {
    // Check if we should still be in fast sync mode
    if (fastSyncMode && currentMillis - lastChangeTime > 30000) {
      fastSyncMode = false;
      Serial.println("Exiting fast sync mode");
    }
    
    // Reset syncPending flag if we've moved to the next opportunity
    if (syncPending && espNowInitialized) {
      if (fastSyncMode && currentMillis - lastSyncTime >= syncInterval) {
        syncPending = false;
      } else if (!fastSyncMode && millisInCurrentSecond >= 100) {
        syncPending = false;
      }
    }
    
    // Return early - we've processed button inputs and ESP-Now sync flags
    return;
  }
  
  // Only non-sleeping devices continue here
  
  // Check if it's time to sync with other devices
  
  // Check if we should still be in fast sync mode
  if (fastSyncMode && currentMillis - lastChangeTime > 30000) {
    fastSyncMode = false;
    Serial.println("Exiting fast sync mode");
  }
  
  // We already calculated syncInterval above
  
  // Only try to sync if ESP-NOW is initialized
  if (espNowInitialized) {
    // If in fast mode, sync frequently
    if (fastSyncMode) {
      if (currentMillis - lastSyncTime >= syncInterval && !syncPending) {
        Serial.print("Fast sync broadcasting at second: ");
        Serial.print(secondsSinceBoot);
        Serial.print(".");
        Serial.println(millisInCurrentSecond);
        broadcastSync();
        lastSyncTime = currentMillis;
      }
    } 
    // In normal mode, sync on the second mark for better synchronization
    else if (millisInCurrentSecond < 100 && currentMillis - lastSyncTime >= syncInterval && !syncPending) {
      Serial.print("Normal sync broadcasting at second: ");
      Serial.println(secondsSinceBoot);
      broadcastSync();
      lastSyncTime = currentMillis;
    }
  } else {
    // ESP-NOW not available, still update the sync time to avoid spam
    if (currentMillis - lastSyncTime >= 10000) { // Only check every 10 seconds
      lastSyncTime = currentMillis;
    }
  }
  
  // Reset syncPending flag if we've moved to the next opportunity
  if (syncPending) {
    if (fastSyncMode && currentMillis - lastSyncTime >= syncInterval) {
      syncPending = false;
    } else if (!fastSyncMode && millisInCurrentSecond >= 100) {
      syncPending = false;
    }
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
  
  // Update the base color offset smoothly if needed, but only for local changes
  // Skip smooth transitions for synchronized values to maintain perfect color sync
  if (baseColorOffset != targetColorOffset && !syncPending) {
    // Gradually move towards target (faster when rotating encoder)
    static uint32_t lastColorTransition = 0;
    
    // Calculate distance between current and target
    int distance = abs(targetColorOffset - baseColorOffset);
    if (distance > 128) {
      // If we need to go more than halfway around the wheel, use the shorter path
      distance = 256 - distance;
    }
    
    // For very small distances, just snap to the target value
    if (distance <= 3) {
      baseColorOffset = targetColorOffset;
      Serial.println("Snapping to target color (close enough)");
    }
    // Otherwise use smooth transitions for user experience
    else {
      // Use variable speed based on distance - faster when further from target
      int transitionDelay = 25; // Much faster than before (was 80ms)
      
      if (currentMillis - lastColorTransition >= transitionDelay) {
        // Step size also varies with distance
        int stepSize = 1;
        if (distance > 60) stepSize = 3; // Faster movement when far away
        else if (distance > 30) stepSize = 2;
        
        // Make the move using shortest path
        if (baseColorOffset < targetColorOffset) {
          if (targetColorOffset - baseColorOffset > 128) {
            // Go backward (shorter)
            baseColorOffset = (baseColorOffset - stepSize) % 256;
          } else {
            // Go forward
            baseColorOffset = (baseColorOffset + stepSize) % 256;
          }
        } else if (baseColorOffset > targetColorOffset) {
          if (baseColorOffset - targetColorOffset > 128) {
            // Go forward (shorter)
            baseColorOffset = (baseColorOffset + stepSize) % 256;
          } else {
            // Go backward
            baseColorOffset = (baseColorOffset - stepSize) % 256;
          }
        }
        
        // Wrap around color wheel
        if (baseColorOffset >= 256) baseColorOffset = 0;
        if (baseColorOffset < 0) baseColorOffset += 256;
        
        lastColorTransition = currentMillis;
      }
    }
  }
  
  // Check if we need to do a random eye blink in smooth patterns
  if (!isBlinking && currentMillis >= nextBlinkTime && 
      (currentPattern != 1)) { // Don't blink during fire eyes pattern
    // Start a blink
    isBlinking = true;
    blinkState = 0;
    lastEyeBlink = currentMillis;
  }

  // Run current pattern
  updatePattern(currentMillis);
  
  // Handle eye blinking if needed
  if (isBlinking && currentPattern != 1) { // Skip during fire eyes
    handleEyeBlink(currentMillis);
  }
  
  // End of main loop
}

void updatePattern(uint32_t currentMillis) {
  // Update animation based on the current pattern
  switch (currentPattern) {
  case 0:
    // Gentle rainbow pattern with color subset (slowed down)
    gentleRainbowTikiCustom(currentMillis, 60); // Was 30, now 60ms
    break;
  case 1:
    // Fire eyes with custom color influence
    fireEyesPatternCustom(currentMillis, 50);
    break;
  case 2:
    // Smooth breathing with custom colors
    breathingPatternCustom(currentMillis, 30);
    break;
  case 3:
    // Gradient teeth pattern
    gradientTeethPattern(currentMillis, 40);
    break;
  case 4:
    // Color wave pattern
    colorWavePattern(currentMillis, 30);
    break;
  default:
    // Default to gentle rainbow
    gentleRainbowTikiCustom(currentMillis, 30);
    break;
  }
}

void checkInputs() {
  if (!anoAvailable) {
    Serial.println("ANO not available, skipping input check");
    return;
  }
  
  uint32_t now = millis();
  
  // If in sleep mode, print debug info about button state
  if (inSleepMode) {
    // Only debug every 2 seconds to avoid spamming logs
    static uint32_t lastDebugTime = 0;
    if (now - lastDebugTime > 2000) {
      // Read all button states for debugging
      bool centerBtn = !ano.digitalRead(ANO_SWITCH);
      bool upBtn = !ano.digitalRead(ANO_UP);
      bool downBtn = !ano.digitalRead(ANO_DOWN);
      bool leftBtn = !ano.digitalRead(ANO_LEFT);
      bool rightBtn = !ano.digitalRead(ANO_RIGHT);
      
      Serial.print("Button states while sleeping - Center:");
      Serial.print(centerBtn);
      Serial.print(" Up:");
      Serial.print(upBtn);
      Serial.print(" Down:");
      Serial.print(downBtn);
      Serial.print(" Left:");
      Serial.print(leftBtn);
      Serial.print(" Right:");
      Serial.println(rightBtn);
      
      lastDebugTime = now;
    }
  }

  // Read encoder for color control
  int32_t encoderPosition = ano.getEncoderPosition();
  static int32_t lastPosition = 0;

  if (encoderPosition != lastPosition) {
    int change = encoderPosition - lastPosition;
    colorPosition = (colorPosition + change) % 256;
    if (colorPosition < 0) colorPosition += 256;
    
    Serial.print("Color position: ");
    Serial.println(colorPosition);
    lastPosition = encoderPosition;
    
    // Set target color offset for smooth transition
    targetColorOffset = colorPosition;
    // Update base color offset immediately to avoid color sync issues
    baseColorOffset = colorPosition;
    
    // Set flag to use custom color
    useCustomColor = true;
    
    // Force our timestamp to advance so we become the master
    // But only update once per 500ms to avoid flooding with updates during rapid rotation
    static uint32_t lastEncoderUpdate = 0;
    if (now - lastEncoderUpdate > 500) {
      // Advance our time by 2 seconds
      uint32_t currentTime = (now - bootTime) / 1000;
      bootTime = now - ((currentTime + 2) * 1000);
      lastEncoderUpdate = now;
      
      // Enter fast sync mode
      fastSyncMode = true;
      lastChangeTime = now;
      Serial.println("Entering fast sync mode");
      
      // Broadcast soon but not immediately (allow for more encoder turns)
      lastSyncTime = now - 900; // Will broadcast in ~100ms
    }
  }

  // Read buttons with debouncing
  static bool lastButtons[5] = {false, false, false, false, false};

  // Center button - randomize or sleep/wake
  bool centerButton = !ano.digitalRead(ANO_SWITCH);
  
  // Button just pressed (transition from not pressed to pressed)
  if (centerButton && !lastButtons[0]) {
    // Start tracking for potential long press for sleep/wake
    buttonLongPressInProgress = true;
    buttonPressStartTime = now;
    Serial.println("Starting sleep/wake timer (inSleepMode=" + String(inSleepMode) + ")");
    
    // If in sleep mode, wake up IMMEDIATELY on press, don't wait for release
    if (inSleepMode) {
      Serial.println("Center button pressed while sleeping - WAKING UP IMMEDIATELY");
      wakeFromSleepMode();
      // Reset long press handling
      longPressHandled = false;
    } else {
      Serial.println("Center button pressed - randomizing immediately");
      
      // Randomize pattern
      currentPattern = random(NUM_PATTERNS);
      
      // Randomize color - set all color values to the same value
      colorPosition = random(256);
      targetColorOffset = colorPosition;
      baseColorOffset = colorPosition; // Set directly to the same value to avoid transition
      
      // Enable custom color mode
      useCustomColor = true;
     
      // Reset animation
      animationStep = 0;
      lastUpdate = now;
      
      // Schedule next random eye blink
      nextBlinkTime = now + random(2000, 30000);
      isBlinking = false;
      
      Serial.print("Random pattern: ");
      Serial.println(currentPattern);
      Serial.print("Random color: ");
      Serial.println(colorPosition);
      Serial.print("Random brightness: ");
      Serial.println(brightness);
      
      lastPatternChange = now;
      
      // Force our timestamp to advance so we become the master
      uint32_t currentTime = (now - bootTime) / 1000;
      bootTime = now - ((currentTime + 2) * 1000);
      
      // Enter fast sync mode
      fastSyncMode = true;
      lastChangeTime = now;
      Serial.println("Entering fast sync mode");
      
      // Reset sync time to broadcast the change immediately
      lastSyncTime = 0;
    }
  }
  
  // Track button hold time for sleep/wake functionality
  // longPressHandled is now a global variable
  
  if (centerButton) {
    // Button pressed - new press or continued hold
    if (!buttonLongPressInProgress) {
      // This is a new button press (wasn't previously pressed)
      buttonLongPressInProgress = true;
      buttonPressStartTime = now;
      longPressHandled = false; // Reset for new press
      
      Serial.println("New button press detected, longPressHandled reset to false");
    }
    
    // Check if we've held long enough for sleep
    if (buttonLongPressInProgress && !longPressHandled && (now - buttonPressStartTime >= BUTTON_HOLD_TIME)) {
      Serial.println("Long press detected! (inSleepMode=" + String(inSleepMode) + 
                    ", buttonPressStartTime=" + String(buttonPressStartTime) + 
                    ", now=" + String(now) + 
                    ", delta=" + String(now - buttonPressStartTime) + ")");
      
      // Mark this long press as handled (so we don't toggle again on release)
      longPressHandled = true;
      
      if (inSleepMode) {
        // If already in sleep mode, wake up
        wakeFromSleepMode();
      } else {
        // Enter sleep mode
        enterSleepMode();
      }
    }
  }
  else if (buttonLongPressInProgress) {
    // Button was released
    Serial.println("Button released after " + String(now - buttonPressStartTime) + 
                  "ms (longPressHandled=" + String(longPressHandled) + 
                  ", inSleepMode=" + String(inSleepMode) + ")");
    
    buttonLongPressInProgress = false;
    
    // Note: We no longer handle wake on release since we do it on press
    // But reset the flag anyway
    longPressHandled = false;
  }
  
  lastButtons[0] = centerButton;

  // Up button - increase brightness
  bool upButton = !ano.digitalRead(ANO_UP);
  if (upButton && !lastButtons[1]) {
    Serial.println("Up button pressed - increasing brightness");
    brightness = constrain(brightness + 25, 10, 255);
    strip.setBrightness(brightness);
    Serial.print("Brightness: ");
    Serial.println(brightness);
    
    // Force our timestamp to advance so we become the master
    uint32_t currentTime = (now - bootTime) / 1000;
    bootTime = now - ((currentTime + 2) * 1000);
    
    // Enter fast sync mode
    fastSyncMode = true;
    lastChangeTime = now;
    Serial.println("Entering fast sync mode");
    
    // Reset sync time to broadcast the change immediately
    lastSyncTime = 0;
  }
  lastButtons[1] = upButton;

  // Down button - decrease brightness
  bool downButton = !ano.digitalRead(ANO_DOWN);
  if (downButton && !lastButtons[2]) {
    Serial.println("Down button pressed - decreasing brightness");
    brightness = constrain(brightness - 25, 10, 255);
    strip.setBrightness(brightness);
    Serial.print("Brightness: ");
    Serial.println(brightness);
    
    // Force our timestamp to advance so we become the master
    uint32_t currentTime = (now - bootTime) / 1000;
    bootTime = now - ((currentTime + 2) * 1000);
    
    // Enter fast sync mode
    fastSyncMode = true;
    lastChangeTime = now;
    Serial.println("Entering fast sync mode");
    
    // Reset sync time to broadcast the change immediately
    lastSyncTime = 0;
  }
  lastButtons[2] = downButton;

  // Left button - previous pattern
  bool leftButton = !ano.digitalRead(ANO_LEFT);
  if (leftButton && !lastButtons[3]) {
    Serial.println("Left button pressed - previous pattern");
    changePattern(-1);
  }
  lastButtons[3] = leftButton;

  // Right button - next pattern
  bool rightButton = !ano.digitalRead(ANO_RIGHT);
  if (rightButton && !lastButtons[4]) {
    Serial.println("Right button pressed - next pattern");
    changePattern(1);
  }
  lastButtons[4] = rightButton;
}

void changePattern(int direction) {
  uint32_t now = millis();
  animationStep = 0;
  lastUpdate = now;

  currentPattern = (currentPattern + NUM_PATTERNS + direction) % NUM_PATTERNS;
  Serial.print("Pattern: ");
  Serial.println(currentPattern);

  // We should NOT reset the useCustomColor flag
  // This ensures that if we've set a color with the encoder, it's preserved when changing patterns
  
  // Always set color positions and targets to match the current colorPosition
  // This ensures the pattern uses the same color we've set with the dial
  baseColorOffset = colorPosition;
  targetColorOffset = colorPosition;
  
  // Schedule next random eye blink in 2-30 seconds
  nextBlinkTime = now + random(2000, 30000);
  isBlinking = false;
  blinkState = 0;

  lastPatternChange = now;
  
  // Force our timestamp to advance so we become the master
  // Adjust bootTime to make our timestamp 2 seconds ahead
  uint32_t currentTime = (now - bootTime) / 1000;
  bootTime = now - ((currentTime + 2) * 1000);
  
  // Enter fast sync mode
  fastSyncMode = true;
  lastChangeTime = now;
  Serial.println("Entering fast sync mode");
  
  // Reset sync time to broadcast the change immediately on next loop iteration
  lastSyncTime = 0;
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

  // Get color from wheel based on current colorPosition
  uint32_t baseColor = Wheel(colorPosition);
  uint8_t r = (baseColor >> 16) & 0xFF;
  uint8_t g = (baseColor >> 8) & 0xFF;
  uint8_t b = baseColor & 0xFF;
  
  // Apply to all LEDs with custom color scaled by breath level
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    uint8_t scaledR = (r * breathLevel) / 255;
    uint8_t scaledG = (g * breathLevel) / 255;
    uint8_t scaledB = (b * breathLevel) / 255;
    
    if (i >= LEFT_EYE_START && i <= RIGHT_EYE_END) {
      // Eyes - use base color
      strip.setPixelColor(i, strip.Color(scaledR, scaledG, scaledB));
    } else {
      // Teeth - use base color with slight variation
      strip.setPixelColor(i, strip.Color(scaledR, scaledG/2, scaledB));
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

// Apply custom color to mouth and eyes based on colorPosition (used on encoder change)
void applyCustomColor() {
  // When encoder changes, we want to ensure custom colors are used
  useCustomColor = true;
}

// Various easing functions for different blink effects
// Quadratic easing functions
float quadraticEaseIn(float t) {
  return t * t;
}

float quadraticEaseOut(float t) {
  return t * (2.0 - t);
}

float quadraticEaseInOut(float t) {
  if (t < 0.5) return 2.0 * t * t;
  return (-2.0 * t * t) + (4.0 * t) - 1.0;
}

// Cubic easing functions
float cubicEaseIn(float t) {
  return t * t * t;
}

float cubicEaseOut(float t) {
  float f = t - 1.0;
  return f * f * f + 1.0;
}

float cubicEaseInOut(float t) {
  if (t < 0.5) return 4.0 * t * t * t;
  float f = (2.0 * t) - 2.0;
  return 0.5 * f * f * f + 1.0;
}

// Quartic easing functions
float quarticEaseIn(float t) {
  return t * t * t * t;
}

float quarticEaseOut(float t) {
  float f = t - 1.0;
  return f * f * f * (1.0 - t) + 1.0;
}

float quarticEaseInOut(float t) {
  if (t < 0.5) return 8.0 * t * t * t * t;
  float f = t - 1.0;
  return -8.0 * f * f * f * f + 1.0;
}

// Quintic easing functions
float quinticEaseIn(float t) {
  return t * t * t * t * t;
}

float quinticEaseOut(float t) {
  float f = t - 1.0;
  return f * f * f * f * f + 1.0;
}

float quinticEaseInOut(float t) {
  if (t < 0.5) return 16.0 * t * t * t * t * t;
  float f = (2.0 * t) - 2.0;
  return 0.5 * f * f * f * f * f + 1.0;
}

// Simplified blinking approach - no dynamic function pointers
typedef float (*EasingFunction)(float);

// Handle eye blink animations by updating the global blinkFadeBrightness_ value
// Completely rewritten with defensive programming
void handleEyeBlink(uint32_t currentMillis) {
  // Ensure times are valid
  if (endBlinkMs_ < startBlinkMs_) {
    // Invalid timing - reset and try again
    startBlinkMs_ = currentMillis;
    endBlinkMs_ = currentMillis + 200;
  }
  
  switch (blinkState) {
    case 0: // Initialize blink and set up timing
      // Set up timing for blink
      startBlinkMs_ = currentMillis;
      endBlinkMs_ = currentMillis + 200; // 200ms for the closing fade
      
      // Start at full brightness
      blinkFadeBrightness_ = 1.0f;
      blinkState = 1; // Move to the closing phase
      break;
      
    case 1: // Fading eyes closed with simple linear interpolation
      if (currentMillis < endBlinkMs_) {
        // Make sure we don't divide by zero
        uint32_t blinkDuration = endBlinkMs_ - startBlinkMs_;
        if (blinkDuration == 0) blinkDuration = 1; // Prevent division by zero
        
        // Calculate progress ratio with bounds checking (0.0 to 1.0)
        uint32_t elapsed = (currentMillis > startBlinkMs_) ? currentMillis - startBlinkMs_ : 0;
        float progress = (float)elapsed / blinkDuration;
        
        // Clamp progress to valid range
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        
        // Simple linear fade down - most reliable approach
        blinkFadeBrightness_ = 1.0f - (0.8f * progress);
      } else {
        // Eyes at minimum brightness (20%)
        blinkFadeBrightness_ = 0.2f;
        
        // Small hold time - immediately start opening
        startBlinkMs_ = currentMillis;
        endBlinkMs_ = currentMillis + 10; // 10ms hold time
        blinkState = 2;
      }
      break;
      
    case 2: // Hold eyes mostly closed briefly 
      if (currentMillis >= endBlinkMs_) {
        // Time to start opening
        startBlinkMs_ = currentMillis;
        endBlinkMs_ = currentMillis + 200; // 200ms for the opening fade
        blinkState = 3;
      }
      break;
      
    case 3: // Fading eyes open with simple linear interpolation
      if (currentMillis < endBlinkMs_) {
        // Make sure we don't divide by zero
        uint32_t blinkDuration = endBlinkMs_ - startBlinkMs_;
        if (blinkDuration == 0) blinkDuration = 1; // Prevent division by zero
        
        // Calculate progress with bounds checking
        uint32_t elapsed = (currentMillis > startBlinkMs_) ? currentMillis - startBlinkMs_ : 0;
        float progress = (float)elapsed / blinkDuration;
        
        // Clamp progress to valid range
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        
        // Simple linear fade up - most reliable approach
        blinkFadeBrightness_ = 0.2f + (0.8f * progress);
      } else {
        // Eyes fully open - safely complete the blink cycle
        blinkFadeBrightness_ = 1.0f;
        
        // Reset blink state and schedule next blink
        isBlinking = false;
        nextBlinkTime = currentMillis + random(2000, 30000); // Every 2-30 seconds
      }
      break;
      
    default:
      // Defensive code for invalid state
      blinkState = 0;
      blinkFadeBrightness_ = 1.0f;
      isBlinking = false;
      nextBlinkTime = currentMillis + random(2000, 30000);
      break;
  }
  
  // Final safety check - make sure we always have a valid brightness value
  if (blinkFadeBrightness_ < 0.2f) blinkFadeBrightness_ = 0.2f;
  if (blinkFadeBrightness_ > 1.0f) blinkFadeBrightness_ = 1.0f;
}

// Gentle rainbow pattern that uses a small color segment
void gentleRainbowTikiCustom(uint32_t currentMillis, uint8_t wait) {
  static uint16_t j = 0;

  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  // Calculate base position based on smoothly transitioning color offset
  int baseColor = useCustomColor ? baseColorOffset : 0;
  
  // Only use a 60-degree slice of the color wheel (instead of full 256 values)
  // This creates a more cohesive, less chaotic rainbow
  int colorRange = 60; 
  
  // Bottom teeth - gentle gradient from base color
  for (int i = BOTTOM_TEETH_START; i <= BOTTOM_TEETH_END; i++) {
    int pixelPos = i - BOTTOM_TEETH_START;
    int colorPos = (baseColor + (pixelPos * colorRange / 12) + j) % 256;
    strip.setPixelColor(i, Wheel(colorPos));
  }

  // Top teeth - offset from bottom
  for (int i = TOP_TEETH_START; i <= TOP_TEETH_END; i++) {
    int pixelPos = i - TOP_TEETH_START;
    int colorPos = (baseColor + 30 + (pixelPos * colorRange / 12) + j) % 256;
    strip.setPixelColor(i, Wheel(colorPos));
  }

  // Eyes - use a complementary color from the base, applying blink brightness factor
  uint32_t eyeColor = Wheel((baseColor + 128) % 256);
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    // Apply the blink fade brightness to the eye colors
    uint8_t r = ((eyeColor >> 16) & 0xFF) * blinkFadeBrightness_;
    uint8_t g = ((eyeColor >> 8) & 0xFF) * blinkFadeBrightness_;
    uint8_t b = (eyeColor & 0xFF) * blinkFadeBrightness_;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }

  strip.show();

  // Even slower movement (moving only every other cycle)
  static int slowCounter = 0;
  slowCounter++;
  if (slowCounter >= 2) {
    j = (j + 1) % 256;
    slowCounter = 0;
  }
}

// Gradient teeth pattern - smooth gradient across all teeth
void gradientTeethPattern(uint32_t currentMillis, uint8_t wait) {
  static uint16_t j = 0;
  
  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;
  
  // Calculate base position based on smoothly transitioning color offset
  int baseColor = useCustomColor ? baseColorOffset : 0;
  
  // Create a smooth gradient across all teeth
  for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
    // Map pixel position to a smooth gradient
    int pixelPos = map(i, BOTTOM_TEETH_START, TOP_TEETH_END, 0, 120);
    int colorPos = (baseColor + pixelPos + j) % 256;
    strip.setPixelColor(i, Wheel(colorPos));
  }
  
  // Eyes pulse gently in complementary color, applying blink brightness factor
  int sinOffset = (int)(sin(j * 0.1) * 20);  // Cast the result to int
  uint32_t eyeColor = Wheel((baseColor + 128 + sinOffset) % 256);
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    // Apply the blink fade brightness to the eye colors
    uint8_t r = ((eyeColor >> 16) & 0xFF) * blinkFadeBrightness_;
    uint8_t g = ((eyeColor >> 8) & 0xFF) * blinkFadeBrightness_;
    uint8_t b = (eyeColor & 0xFF) * blinkFadeBrightness_;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  
  strip.show();
  
  // Very slow movement
  j = (j + 1) % 256;
}

// Color wave pattern - sine wave of color through the teeth
void colorWavePattern(uint32_t currentMillis, uint8_t wait) {
  static uint16_t j = 0;
  
  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;
  
  // Calculate base position based on smoothly transitioning color offset
  int baseColor = useCustomColor ? baseColorOffset : 0;
  
  // Create a sine wave pattern through the teeth
  for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
    int pixelPos = i - BOTTOM_TEETH_START;
    // Create sine wave effect
    float sinVal = sin((pixelPos / 8.0) + (j / 20.0)) * 0.5 + 0.5;
    int colorOffset = sinVal * 60; // 60-degree color shift based on sine wave
    
    int colorPos = (baseColor + colorOffset) % 256;
    strip.setPixelColor(i, Wheel(colorPos));
  }
  
  // Eyes glow with main color
  uint32_t eyeColor = Wheel(baseColor);
  
  // Apply a breathing effect to the eyes and also apply blink brightness factor
  float breathFactor = (sin(j * 0.05) * 0.3) + 0.7; // 0.4 to 1.0 range for breathing
  // Combine the breathing effect with the blink fade
  float combinedFactor = breathFactor * blinkFadeBrightness_;
  uint8_t eyeR = (uint8_t)(((eyeColor >> 16) & 0xFF) * combinedFactor);
  uint8_t eyeG = (uint8_t)(((eyeColor >> 8) & 0xFF) * combinedFactor);
  uint8_t eyeB = (uint8_t)((eyeColor & 0xFF) * combinedFactor);
  
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    strip.setPixelColor(i, strip.Color(eyeR, eyeG, eyeB));
  }
  
  strip.show();
  
  // Gentle progression
  j = (j + 1) % 256;
}

// Fire eyes with custom color influence
void fireEyesPatternCustom(uint32_t currentMillis, uint8_t wait) {
  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  // For custom color, get colors from wheel
  uint32_t teethColor, eyeBaseColor;
  
  if (useCustomColor) {
    // Use custom color for teeth with smooth transition
    teethColor = Wheel(baseColorOffset);
    
    // Use complementary color for eyes
    eyeBaseColor = Wheel((baseColorOffset + 128) % 256);
  } else {
    // Default: dim red teeth
    teethColor = strip.Color(30, 0, 0);
    
    // Default: orange/yellow base for fire
    eyeBaseColor = strip.Color(255, 100, 0);
  }
  
  // Extract RGB components for eye color (for flickering)
  uint8_t eyeR = (eyeBaseColor >> 16) & 0xFF;
  uint8_t eyeG = (eyeBaseColor >> 8) & 0xFF;
  uint8_t eyeB = eyeBaseColor & 0xFF;

  // Teeth color
  for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
    strip.setPixelColor(i, teethColor);
  }

  // Eyes flicker with custom color base and apply blink brightness
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    uint8_t flicker = random(80, 255);
    
    // Scale RGB values by flicker factor and blink brightness
    float combinedFactor = (flicker / 255.0) * blinkFadeBrightness_;
    uint8_t r = eyeR * combinedFactor;
    uint8_t g = eyeG * combinedFactor;
    uint8_t b = eyeB * combinedFactor;
    
    strip.setPixelColor(i, strip.Color(r, g, b));
  }

  strip.show();
}

// Chattering teeth with custom colors
void chatteringTeethPatternCustom(uint32_t currentMillis, uint8_t wait) {
  static bool teethOpen = false;

  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  teethOpen = !teethOpen; // Toggle teeth state

  // Colors to use based on custom setting
  uint32_t bottomTeethColor, topTeethColor, closedTeethColor, eyeColor;
  
  if (useCustomColor) {
    // Base color
    uint32_t baseColor = Wheel(colorPosition);
    
    // Complementary color
    uint32_t compColor = Wheel((colorPosition + 128) % 256);
    
    bottomTeethColor = baseColor;
    topTeethColor = compColor;
    closedTeethColor = strip.Color(200, 200, 200); // Still white when closed
    eyeColor = baseColor;
  } else {
    // Default colors
    bottomTeethColor = strip.Color(255, 100, 0); // Orange-yellow
    topTeethColor = strip.Color(255, 150, 150);  // Light pink
    closedTeethColor = strip.Color(255, 255, 255); // White
    eyeColor = strip.Color(255, 0, 0); // Red
  }
  
  if (teethOpen) {
    // Open mouth - different colors for top and bottom
    for (int i = BOTTOM_TEETH_START; i <= BOTTOM_TEETH_END; i++) {
      strip.setPixelColor(i, bottomTeethColor);
    }
    for (int i = TOP_TEETH_START; i <= TOP_TEETH_END; i++) {
      strip.setPixelColor(i, topTeethColor);
    }
  } else {
    // Closed mouth - same color for all teeth
    for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
      strip.setPixelColor(i, closedTeethColor);
    }
  }

  // Eyes
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    strip.setPixelColor(i, eyeColor);
  }

  strip.show();
}

// Blinking eyes with custom colors
void blinkingEyesPatternCustom(uint32_t currentMillis, uint8_t wait) {
  static uint8_t blinkState = 0;
  static uint32_t nextStateChange = 0;

  if (currentMillis < nextStateChange)
    return;
    
  // Colors to use
  uint32_t eyeOpenColor, eyeHalfColor, teethColor;
  
  if (useCustomColor) {
    // Use custom color for eyes
    eyeOpenColor = Wheel(colorPosition);
    
    // Half brightness version of eye color
    uint8_t r = (eyeOpenColor >> 16) & 0xFF;
    uint8_t g = (eyeOpenColor >> 8) & 0xFF;
    uint8_t b = eyeOpenColor & 0xFF;
    eyeHalfColor = strip.Color(r/3, g/3, b/3);
    
    // Use complementary color for teeth
    teethColor = Wheel((colorPosition + 128) % 256);
  } else {
    // Default colors
    eyeOpenColor = strip.Color(0, 255, 0);  // Green
    eyeHalfColor = strip.Color(0, 100, 0);   // Dim green
    teethColor = strip.Color(50, 0, 50);     // Purple
  }

  switch (blinkState) {
  case 0: // Eyes open
    // Eyes color
    for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
      strip.setPixelColor(i, eyeOpenColor);
    }
    // Teeth color
    for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
      strip.setPixelColor(i, teethColor);
    }
    blinkState = 1;
    nextStateChange = currentMillis + random(2000, 5000); // Random time until blink
    break;

  case 1: // Start blinking - eyes half closed
    for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
      strip.setPixelColor(i, eyeHalfColor);
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
      strip.setPixelColor(i, eyeHalfColor);
    }
    blinkState = 0;
    nextStateChange = currentMillis + 80; // Quick transition back
    break;
  }

  strip.show();
  lastUpdate = currentMillis;
}

// Alternating teeth with custom colors
void alternatingTeethPatternCustom(uint32_t currentMillis, uint8_t wait) {
  static bool alternate = false;

  if (currentMillis - lastUpdate < wait)
    return;
  lastUpdate = currentMillis;

  alternate = !alternate;
  
  // Colors to use
  uint32_t color1, color2, eyeColor1, eyeColor2;
  
  if (useCustomColor) {
    // Base colors from custom position
    color1 = Wheel(colorPosition);
    color2 = Wheel((colorPosition + 128) % 256); // Complementary
    
    // Eye colors slightly different
    eyeColor1 = Wheel((colorPosition + 64) % 256);
    eyeColor2 = Wheel((colorPosition + 192) % 256);
  } else {
    // Default colors
    color1 = strip.Color(255, 0, 0); // Red
    color2 = strip.Color(0, 0, 255); // Blue
    eyeColor1 = strip.Color(255, 0, 255); // Purple
    eyeColor2 = strip.Color(255, 255, 0); // Yellow
  }

  for (int i = BOTTOM_TEETH_START; i <= TOP_TEETH_END; i++) {
    bool isEven = (i % 2 == 0);
    if (isEven == alternate) {
      strip.setPixelColor(i, color1);
    } else {
      strip.setPixelColor(i, color2);
    }
  }

  // Eyes match alternating pattern
  for (int i = LEFT_EYE_START; i <= RIGHT_EYE_END; i++) {
    if (alternate) {
      strip.setPixelColor(i, eyeColor1);
    } else {
      strip.setPixelColor(i, eyeColor2);
    }
  }

  strip.show();
}

// Breathing pattern with custom color
void breathingPatternCustom(uint32_t currentMillis, uint8_t wait) {
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

  // Colors to use
  uint32_t eyeColor, teethColor;
  
  if (useCustomColor) {
    // Use custom colors with smooth transition
    eyeColor = Wheel(baseColorOffset);
    teethColor = Wheel((baseColorOffset + 64) % 256); // Slightly offset
  } else {
    // Default colors
    eyeColor = strip.Color(0, 0, 255); // Blue
    teethColor = strip.Color(255, 0, 128); // Purple-ish
  }
  
  // Extract RGB components
  uint8_t eyeR = (eyeColor >> 16) & 0xFF;
  uint8_t eyeG = (eyeColor >> 8) & 0xFF;
  uint8_t eyeB = eyeColor & 0xFF;
  
  uint8_t teethR = (teethColor >> 16) & 0xFF;
  uint8_t teethG = (teethColor >> 8) & 0xFF;
  uint8_t teethB = teethColor & 0xFF;
  
  // Apply scaled by breath level
  for (int i = 0; i < TOTAL_PIXELS; i++) {
    if (i >= LEFT_EYE_START && i <= RIGHT_EYE_END) {
      // Eyes - apply both breathing and blinking effects
      float combinedFactor = (breathLevel / 255.0) * blinkFadeBrightness_;
      uint8_t scaledR = eyeR * combinedFactor;
      uint8_t scaledG = eyeG * combinedFactor;
      uint8_t scaledB = eyeB * combinedFactor;
      strip.setPixelColor(i, strip.Color(scaledR, scaledG, scaledB));
    } else {
      // Teeth - just apply breathing effect
      float breathFactor = breathLevel / 255.0;
      uint8_t scaledR = teethR * breathFactor;
      uint8_t scaledG = teethG * breathFactor;
      uint8_t scaledB = teethB * breathFactor;
      strip.setPixelColor(i, strip.Color(scaledR, scaledG, scaledB));
    }
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
