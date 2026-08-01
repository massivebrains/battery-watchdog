/*
 * BATTERY WATCHDOG v1.1
 * ESP32 + 2x LM393 LDR modules + SG90 servo
 *
 * Watches the Growatt battery panel LEDs. When the battery shuts off
 * (RUN off AND battery level lights off), the servo presses the power button once and
 * verifies the battery came back. Alerts go to your server as:
 *   POST https://vast-comet-94.webhook.cool/  {"event":"<code>","message":"<text>"}
 *
 * Board: ESP32 Dev Module | Library: ESP32Servo
 *
 * Modes (set MODE below):
 *   MODE_CALIBRATE - prints live sensor values, lets you jog the servo. No pressing.
 *   MODE_DRY_RUN   - full logic + alerts, servo never moves. Run 2-3 days first.
 *   MODE_ACTIVE    - the real thing.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP32Servo.h>

#define MODE_CALIBRATE 0
#define MODE_DRY_RUN 1
#define MODE_ACTIVE 2

#define MODE MODE_ACTIVE // <-- change as you progress

// ---------- WiFi + alerts ----------
const char* WIFI_SSID = "Flat_2";
const char* WIFI_PASSWORD = "08175020329";
const char* ALERT_HOST = "brass.trymassive.ai";
const char* ALERT_PATH = "/api/inverter";

// ---------- Pins (ADC1 only: 32-39 work with WiFi on) ----------
const int PIN_RUN = 34;
const int PIN_BATTERY = 35;
const int PIN_SERVO = 18;

// ---------- Sensor calibration ----------
// These modules read LOWER with more light.
// Calibrated 2026-07: LED on reads < 2000, LED off reads 3600-4000.
const int LED_THRESHOLD = 3100;

// ---------- Servo calibration ----------
int SERVO_REST_ANGLE = 0;
int SERVO_PRESS_ANGLE = 32;
const unsigned long PRESS_HOLD_MS = 600;

// ---------- Timing & safety ----------
const unsigned long CHECK_INTERVAL_MS = 2000;
const int STABLE_CHECKS_REQUIRED = 3; // 3 x 2s = 6s of "all off" before pressing
const unsigned long POST_PRESS_WAIT_MS = 15000;
const int MAX_PRESSES_PER_HOUR = 3;
const unsigned long FAILED_WAKE_COOLDOWN_MS = 5UL * 60 * 1000;

// ============================================================

Servo servo;

enum State { MONITORING, POST_PRESS, COOLDOWN, LOCKED_OUT };
State state = MONITORING;

const char* stateName(State value) {
  switch (value) {
    case MONITORING: return "MONITORING";
    case POST_PRESS: return "POST_PRESS";
    case COOLDOWN: return "COOLDOWN";
    case LOCKED_OUT: return "LOCKED_OUT";
  }

  return "UNKNOWN";
}

unsigned long lastCheckTime = 0;
unsigned long stateEnteredAt = 0;
unsigned long hourWindowStart = 0;
int pressesThisHour = 0;
int allOffStreak = 0;
unsigned long allOffSince = 0;
bool lastRunState = true;
bool announcedShutdownPending = false;

int readAverage(int pin) {
  long total = 0;

  for (int sample = 0; sample < 10; sample++) {
    total += analogRead(pin);
    delay(3);
  }

  return total / 10;
}

bool ledOn(int pin) {
  return LED_THRESHOLD > readAverage(pin);
}

void sendAlert(const char* event, const char* message) {
  Serial.printf("[ALERT] %s - %s\n", event, message);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ALERT] no wifi, skipped");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(ALERT_HOST, 443)) {
    Serial.println("[ALERT] connect failed");
    return;
  }

  char body[600];
  snprintf(body, sizeof(body), "{\"event\":\"%s\",\"message\":\"%s\"}", event, message);

  char request[768];
  snprintf(request, sizeof(request), "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", ALERT_PATH, ALERT_HOST, (int)strlen(body), body);
  client.print(request);

  unsigned long startTime = millis();

  while (client.connected() && millis() - startTime < 5000) {
    if (client.available()) {
      client.readStringUntil('\n');
      break;
    }
  }

  client.stop();
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.println("[WiFi] connecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] failed, will retry");
  }
}

void pressButton() {
#if MODE == MODE_DRY_RUN
  Serial.println("[SERVO] DRY RUN - would press button now");
  return;
#endif

  Serial.println("[SERVO] pressing button");
  servo.attach(PIN_SERVO, 500, 2400);
  servo.write(SERVO_REST_ANGLE);
  delay(300);
  servo.write(SERVO_PRESS_ANGLE);
  delay(PRESS_HOLD_MS);
  servo.write(SERVO_REST_ANGLE);
  delay(500);
  servo.detach(); // detached servo can't jam or hold the button
}

bool pressBudgetAvailable() {
  if (millis() - hourWindowStart > 3600000UL) {
    hourWindowStart = millis();
    pressesThisHour = 0;
  }

  return pressesThisHour < MAX_PRESSES_PER_HOUR;
}

void enterState(State newState) {
  state = newState;
  stateEnteredAt = millis();
  allOffSince = 0;
}

long remainingSeconds(long totalMs, long elapsedMs) {
  long remaining = (totalMs - elapsedMs) / 1000;
  return remaining < 0 ? 0 : remaining;
}

void describeCountdown(char* out, size_t len, bool runOn, bool batteryOn) {
  unsigned long now = millis();
  long elapsed = (long)(now - stateEnteredAt);

  switch (state) {
    case MONITORING:
      if (runOn || batteryOn) {
        allOffSince = 0;
        snprintf(out, len, "waiting for shutdown");
      } else {
        if (allOffSince == 0) {
          allOffSince = now;
        }
        snprintf(out, len, "press in %lds", remainingSeconds((long)STABLE_CHECKS_REQUIRED * CHECK_INTERVAL_MS, (long)(now - allOffSince)));
      }
      break;
    case POST_PRESS:
      snprintf(out, len, "verify wake in %lds", remainingSeconds((long)POST_PRESS_WAIT_MS, elapsed));
      break;
    case COOLDOWN:
      snprintf(out, len, "retry in %lds", remainingSeconds((long)FAILED_WAKE_COOLDOWN_MS, elapsed));
      break;
    case LOCKED_OUT:
      snprintf(out, len, "waiting for RUN to return");
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);
  pinMode(PIN_RUN, INPUT);
  pinMode(PIN_BATTERY, INPUT);

#if MODE == MODE_CALIBRATE
  Serial.println("\n=== CALIBRATION MODE ===");
  Serial.println("Watch the sensor values. Type an angle 0-180 + Enter to move the servo.\n");
  servo.attach(PIN_SERVO, 500, 2400);
  servo.write(SERVO_REST_ANGLE);
#else
  ensureWifi();

  char message[64];
  snprintf(message, sizeof(message), "Battery watchdog started (%s mode)", MODE == MODE_DRY_RUN ? "DRY RUN" : "ACTIVE");
  sendAlert("started", message);

  hourWindowStart = millis();
  enterState(MONITORING);
#endif
}

void loop() {
#if MODE == MODE_CALIBRATE
  int runValue = readAverage(PIN_RUN);
  int batteryValue = readAverage(PIN_BATTERY);
  bool runOn = LED_THRESHOLD > runValue;
  bool batteryOn = LED_THRESHOLD > batteryValue;

  char countdown[64];
  describeCountdown(countdown, sizeof(countdown), runOn, batteryOn);
  Serial.printf("[%8lu] state=%s RUN=%d(%d) BATTERY=%d(%d) -> %s\n", millis(), stateName(state), runOn, runValue, batteryOn, batteryValue, countdown);

  if (Serial.available()) {
    int angle = Serial.parseInt();

    while (Serial.available()) {
      Serial.read();
    }

    if (angle >= 0 && angle <= 180) {
      Serial.printf("[SERVO] moving to %d\n", angle);
      servo.write(angle);
    }
  }

  delay(500);
#else

  if (millis() - lastCheckTime < CHECK_INTERVAL_MS) {
    return;
  }

  lastCheckTime = millis();
  ensureWifi();

  bool runOn = ledOn(PIN_RUN);
  bool batteryOn = ledOn(PIN_BATTERY);
  unsigned long now = millis();

  char countdown[64];
  describeCountdown(countdown, sizeof(countdown), runOn, batteryOn);
  Serial.printf("[%8lu] state=%s RUN=%d BATTERY=%d -> %s\n", now, stateName(state), runOn, batteryOn, countdown);

  if (runOn && !lastRunState && state == MONITORING) {
    sendAlert("run_restored", "Battery RUN light is back on.");
  }

  lastRunState = runOn;

  switch (state) {

    case MONITORING:
      if (runOn) {
        allOffStreak = 0;
        announcedShutdownPending = false;
        break;
      }

      if (batteryOn) {
        // RUN off but battery lights still lit: shutdown in progress, wait for all off
        allOffStreak = 0;

        if (!announcedShutdownPending) {
          sendAlert("shutdown_pending", "Battery RUN light went OFF (battery level lights still on). Waiting for full shutdown before pressing.");
          announcedShutdownPending = true;
        }

        break;
      }

      allOffStreak++;

      if (allOffStreak >= STABLE_CHECKS_REQUIRED) {
        allOffStreak = 0;
        announcedShutdownPending = false;

        if (!pressBudgetAvailable()) {
          char message[128];
          snprintf(message, sizeof(message), "Battery is OFF but press limit reached (%d/hour). LOCKED OUT for safety - please check manually!", MAX_PRESSES_PER_HOUR);
          sendAlert("locked_out", message);
          enterState(LOCKED_OUT);
          break;
        }

        sendAlert("pressing", "Battery is fully OFF. Pressing the power button now...");
        pressesThisHour++;
        pressButton();
        enterState(POST_PRESS);
      }

      break;

    case POST_PRESS:
      if (now - stateEnteredAt < POST_PRESS_WAIT_MS) {
        break;
      }

      if (runOn) {
        sendAlert("wake_success", "SUCCESS: battery is running again. House power should be restored.");
        enterState(MONITORING);
      } else {
        sendAlert("wake_failed", "FAILED: pressed the button but RUN did not come back. Waiting 5 minutes before retrying.");
        enterState(COOLDOWN);
      }

      break;

    case COOLDOWN:
      if (runOn || now - stateEnteredAt >= FAILED_WAKE_COOLDOWN_MS) {
        enterState(MONITORING);
      }

      break;

    case LOCKED_OUT:
      // stays locked until the battery is seen running again
      if (runOn) {
        sendAlert("rearmed", "Battery is running again. Watchdog re-armed.");
        pressesThisHour = 0;
        hourWindowStart = now;
        enterState(MONITORING);
      }

      break;
  }
#endif
}
