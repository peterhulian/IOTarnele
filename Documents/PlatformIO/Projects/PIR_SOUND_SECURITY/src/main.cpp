#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// --- Wi-Fi Configuration ---
const char* ssid = "Benjithecat";
const char* password = "33333333";

// --- Supabase Configuration ---
const char* SUPABASE_URL = "https://eiilgfsmqizueqhvkptc.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_e6xBVMtOzOlmCeMxGjZVyw_kt5rg7hp";
const char* TABLE_NAME = "alerts";

// --- Input Pin Definitions ---
const int PIR_PIN = 27;        // Motion sensor (Green wire - HIGH when motion detected)
const int SOUND_DO_PIN = 14;   // Sound sensor DO pin (Violet wire - Digital threshold)
const int SOUND_AO_PIN = 34;   // Sound sensor AO pin (Brown wire - Analog waveform for shouts)

// --- Sound Analog Sensitivity Settings ---
// Lower value = more sensitive to shouts; Higher value = less sensitive (Range: 200 to 1500)
const int SOUND_PEAK_THRESHOLD = 600; 

// --- Output Pin Definitions ---
const int BUZZER_PIN = 25;     // Yellow wire
const int RED_LED_PIN = 26;    // Orange wire (Red LED for Motion)
const int BLUE_LED_PIN = 32;   // Orange wire (Blue LED for Sound)

// --- Timing & Cooldowns ---
unsigned long lastMotionTime = 0;
unsigned long lastSoundTime = 0;
const unsigned long COOLDOWN_MS = 10000; // Minimum 10s between separate alerts

// --- PIR Non-Looping State Machine ---
bool pirLatched = false;                 // Locks motion after 1 trigger until pin goes LOW
unsigned long pirHighStart = 0;
unsigned long pirLowStart = 0;
const unsigned long PIR_CONFIRM_MS = 500; // Must stay HIGH for 0.5s to count as real motion
const unsigned long PIR_RESET_MS = 3000;  // Must stay LOW for 3 full seconds before re-arming

// --- Sound Sensor Interrupt ---
volatile bool soundTriggered = false;
volatile unsigned long lastSoundIsrTime = 0;
volatile bool ignoreSensors = false;     // Blocks false triggers during alarm/Wi-Fi send

void IRAM_ATTR onSoundDetected() {
  if (ignoreSensors) return;
  unsigned long now = millis();
  if (now - lastSoundIsrTime > 100) {
    soundTriggered = true;
    lastSoundIsrTime = now;
  }
}

// --- Analog Peak-to-Peak Shout Detector (30ms sample window) ---
bool isLoudShoutDetected() {
  if (ignoreSensors) return false;

  unsigned long startWindow = millis();
  int signalMax = 0;
  int signalMin = 4095;

  // Sample the microphone waveform for 30ms to catch low-frequency human voice waves
  while (millis() - startWindow < 30) {
    int sample = analogRead(SOUND_AO_PIN);
    if (sample > signalMax) signalMax = sample;
    if (sample < signalMin) signalMin = sample;
  }

  int peakToPeak = signalMax - signalMin;

  // Uncomment the line below if you want to see live voice readings in the Serial Monitor to tune it:
  // if (peakToPeak > 200) Serial.println("Mic Peak-to-Peak: " + String(peakToPeak));

  return (peakToPeak >= SOUND_PEAK_THRESHOLD);
}

// --- Alarm Function ---
void triggerAlarm(int activeLedPin) {
  for (int i = 0; i < 3; i++) {
    digitalWrite(activeLedPin, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(activeLedPin, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
}

// --- Send Alert to Supabase over HTTPS ---
void sendAlertToSupabase(const char* alertType, const char* metadata) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot send alert");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_NAME;

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Prefer", "return=minimal");

    StaticJsonDocument<256> doc;
    doc["type"] = alertType;
    doc["metadata"] = metadata;

    String payload;
    serializeJson(doc, payload);

    Serial.println("Sending alert: " + payload);
    int httpCode = http.POST(payload);

    if (httpCode == 201 || httpCode == 200 || httpCode == 204) {
      Serial.println("✓ Alert sent successfully (HTTP " + String(httpCode) + ")");
    } else {
      Serial.println("✗ Failed to send alert. HTTP code: " + String(httpCode));
      Serial.println("Response: " + http.getString());
    }
    http.end();
  } else {
    Serial.println("✗ Unable to initialize HTTPS connection");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configure Input pins
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(SOUND_DO_PIN, INPUT_PULLUP);
  pinMode(SOUND_AO_PIN, INPUT);

  // Configure ADC resolution for ESP32 (0 - 4095)
  analogReadResolution(12);

  // Configure Output pins
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BLUE_LED_PIN, LOW);

  Serial.println("\n=== Security System Starting ===");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected! IP: " + WiFi.localIP().toString());
    sendAlertToSupabase("system", "Security System Online");
  } else {
    Serial.println("\n✗ Failed to connect to WiFi");
  }

  // If the PIR is already HIGH on boot, start it in the latched state so it doesn't false-fire
  if (digitalRead(PIR_PIN) == HIGH) {
    pirLatched = true;
  }

  // Attach Sound Interrupt after Wi-Fi setup finishes
  attachInterrupt(digitalPinToInterrupt(SOUND_DO_PIN), onSoundDetected, CHANGE);
  soundTriggered = false;

  Serial.println("=== Armed & Waiting for Motion or Sound ===\n");
}

void loop() {
  unsigned long now = millis();
  int pirState = digitalRead(PIR_PIN);

  // ===== 1. MOTION DETECTION (SINGLE-SHOT, NO LOOPING) =====
  if (pirState == HIGH) {
    pirLowStart = 0; // Reset LOW timer because pin is currently HIGH

    if (!pirLatched) {
      if (pirHighStart == 0) {
        pirHighStart = now; // Start timing how long PIR has been HIGH
      } else if (now - pirHighStart >= PIR_CONFIRM_MS) {
        // Confirmed real motion -> Lock immediately so it CANNOT loop!
        pirLatched = true;

        if (now - lastMotionTime > COOLDOWN_MS || lastMotionTime == 0) {
          ignoreSensors = true; // Block buzzer/Wi-Fi from triggering sound sensor
          Serial.println("\n🚨 MOTION DETECTED!");
          triggerAlarm(RED_LED_PIN);
          sendAlertToSupabase("motion", "Motion detected");
          lastMotionTime = millis();

          delay(3000); // Let power rail & Wi-Fi radio settle for 3s
          soundTriggered = false;
          ignoreSensors = false;
        }
      }
    }
  } else {
    // PIR pin is LOW (No motion)
    pirHighStart = 0;

    // Only unlock/re-arm the PIR after it stays completely LOW for 3 full seconds
    if (pirLatched) {
      if (pirLowStart == 0) {
        pirLowStart = now;
      } else if (now - pirLowStart >= PIR_RESET_MS) {
        pirLatched = false; // Re-armed and ready for next motion event
        pirLowStart = 0;
        Serial.println("✓ Motion sensor reset and re-armed.");
      }
    }
  }

  // ===== 2. SOUND DETECTION (DIGITAL INTERRUPT OR ANALOG SHOUT) =====
  bool analogShout = isLoudShoutDetected();

  if (soundTriggered || analogShout) {
    soundTriggered = false;

    if (now - lastSoundTime > COOLDOWN_MS || lastSoundTime == 0) {
      ignoreSensors = true; // Block buzzer from re-triggering sound interrupt
      Serial.println("\n🔊 SOUND DETECTED!");
      triggerAlarm(BLUE_LED_PIN);
      sendAlertToSupabase("sound", "Loud noise detected");
      lastSoundTime = millis();

      delay(3000); // Let Wi-Fi radio & room echo settle for 3s
      soundTriggered = false;
      ignoreSensors = false;
    }
  }
}