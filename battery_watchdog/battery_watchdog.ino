/*
 * ============================================================
 *  BATTERY WATCHDOG  v1.0
 *  ESP32 + 3x LM393 LDR modules + SG90 servo
 *
 *  Watches the Growatt battery front panel LEDs.
 *  When the battery shuts itself off (RUN off AND SOC off),
 *  the servo presses the power button once (press & release)
 *  and verifies the battery came back. Sends Telegram alerts.
 *
 *  Board:   ESP32 Dev Module (Arduino IDE)
 *  Library: ESP32Servo  (install via Library Manager)
 *
 *  HOW TO USE — 3 modes, set MODE below:
 *   1. MODE_CALIBRATE : prints live sensor values to Serial.
 *                       Use it to find thresholds. No pressing.
 *   2. MODE_DRY_RUN   : full logic + Telegram alerts, but the
 *                       servo NEVER moves. Run 2-3 days first.
 *   3. MODE_ACTIVE    : the real thing.
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP32Servo.h>

/* ================= 1. MODE ================= */
#define MODE_CALIBRATE 0
#define MODE_DRY_RUN   1
#define MODE_ACTIVE    2

#define MODE MODE_CALIBRATE          // <-- change this as you progress

/* ================= 2. WIFI + TELEGRAM ================= */
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Create a bot with @BotFather on Telegram, paste the token here.
// Get your chat id from @userinfobot.
const char* BOT_TOKEN = "123456789:AAAA-your-bot-token";
const char* CHAT_ID   = "123456789";

/* ================= 3. PINS ================= */
// ADC1 pins only (32-39). ADC2 pins do NOT work while Wi-Fi is on.
const int PIN_RUN   = 34;   // LDR over RUN LED  (input-only pin)
const int PIN_SOC   = 35;   // LDR over leftmost SOC LED (input-only pin)
const int PIN_ALM   = 32;   // LDR over ALM LED
const int PIN_SERVO = 18;

/* ================= 4. SENSOR CALIBRATION ================= */
// Run MODE_CALIBRATE and note the analog values (0-4095) with the
// LED on vs off. Set each threshold halfway between the two.
//
// On most LM393 modules, MORE light = LOWER value on AO.
// If your module reads HIGHER with light, set LIGHT_IS_LOW to false.
const bool LIGHT_IS_LOW  = true;

int THRESHOLD_RUN = 2000;    // <-- replace after calibration
int THRESHOLD_SOC = 2000;    // <-- replace after calibration
int THRESHOLD_ALM = 2000;    // <-- replace after calibration

/* ================= 5. SERVO CALIBRATION ================= */
// Find these by trial: REST must not touch the button,
// PRESS must fully press it.
int SERVO_REST_ANGLE  = 20;    // <-- adjust
int SERVO_PRESS_ANGLE = 70;    // <-- adjust
const unsigned long PRESS_HOLD_MS = 600;   // press & release (~0.6 s)

/* ================= 6. TIMING & SAFETY ================= */
const unsigned long CHECK_INTERVAL_MS   = 2000;      // sensor check rate
const int  STABLE_CHECKS_REQUIRED       = 8;         // 8 x 2 s = 16 s of
                                                     // "all off" before pressing
const unsigned long POST_PRESS_WAIT_MS  = 45000;     // wait 45 s after press
                                                     // before judging success
const int  MAX_PRESSES_PER_HOUR         = 3;
const unsigned long FAILED_WAKE_COOLDOWN_MS = 5UL * 60 * 1000;   // 5 min
const unsigned long ALM_STUCK_MS        = 60000;     // ALM on >60 s = abnormal
const unsigned long ALM_ALERT_REPEAT_MS = 30UL * 60 * 1000;      // re-alert 30 min

/* ============================================================
 *  Internals — nothing below needs editing
 * ============================================================ */
Servo servo;

enum State { MONITORING, SHUTDOWN_PENDING, POST_PRESS, COOLDOWN, LOCKED_OUT };
State state = MONITORING;

unsigned long lastCheck        = 0;
unsigned long stateEnteredAt   = 0;
unsigned long almOnSince       = 0;
unsigned long lastAlmAlert     = 0;
unsigned long hourWindowStart  = 0;
int  pressesThisHour           = 0;
int  allOffStreak              = 0;
bool lastRunState              = true;
bool announcedShutdownPending  = false;

/* ---------- sensor reading (averaged) ---------- */
int readAvg(int pin) {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(pin); delay(3); }
  return sum / 10;
}

bool ledOn(int pin, int threshold) {
  int v = readAvg(pin);
  return LIGHT_IS_LOW ? (v < threshold) : (v > threshold);
}

/* ---------- Telegram ---------- */
String urlEncode(const String& s) {
  String out = "";
  const char* hex = "0123456789ABCDEF";
  for (char c : s) {
    if (isalnum(c)) out += c;
    else if (c == ' ') out += "%20";
    else { out += '%'; out += hex[(c >> 4) & 0xF]; out += hex[c & 0xF]; }
  }
  return out;
}

void sendTelegram(const String& msg) {
  Serial.println("[TG] " + msg);
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[TG] no wifi, skipped"); return; }
  WiFiClientSecure client;
  client.setInsecure();                       // skip cert check (fine for alerts)
  client.setTimeout(10);
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("[TG] connect failed"); return;
  }
  String url = "/bot" + String(BOT_TOKEN) + "/sendMessage?chat_id=" +
               String(CHAT_ID) + "&text=" + urlEncode(msg);
  client.print("GET " + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
  unsigned long t = millis();
  while (client.connected() && millis() - t < 5000) {
    if (client.available()) { client.readStringUntil('\n'); break; }
  }
  client.stop();
}

/* ---------- Wi-Fi ---------- */
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WiFi] connecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
  Serial.println(WiFi.status() == WL_CONNECTED
                 ? "[WiFi] connected: " + WiFi.localIP().toString()
                 : "[WiFi] FAILED (will retry)");
}

/* ---------- servo press ---------- */
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
  servo.detach();               // detach = no jitter, no strain, no hum
}

/* ---------- press budget (max N per hour) ---------- */
bool pressBudgetAvailable() {
  unsigned long now = millis();
  if (now - hourWindowStart > 3600000UL) { hourWindowStart = now; pressesThisHour = 0; }
  return pressesThisHour < MAX_PRESSES_PER_HOUR;
}

/* ---------- state helper ---------- */
void enterState(State s) {
  state = s;
  stateEnteredAt = millis();
}

/* ============================================================ */
void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);     // 0-4095
  pinMode(PIN_RUN, INPUT);
  pinMode(PIN_SOC, INPUT);
  pinMode(PIN_ALM, INPUT);

#if MODE == MODE_CALIBRATE
  Serial.println("\n=== CALIBRATION MODE ===");
  Serial.println("Cover/uncover each sensor, note values, set thresholds.");
  Serial.println("Also test servo angles: type an angle 0-180 + Enter.\n");
  servo.attach(PIN_SERVO, 500, 2400);
  servo.write(SERVO_REST_ANGLE);
#else
  ensureWifi();
  sendTelegram("Battery watchdog started ("
               + String(MODE == MODE_DRY_RUN ? "DRY RUN" : "ACTIVE") + " mode)");
  hourWindowStart = millis();
  enterState(MONITORING);
#endif
}

/* ============================================================ */
void loop() {

#if MODE == MODE_CALIBRATE
  /* ---- calibration: stream values, allow servo angle tests ---- */
  Serial.printf("RUN=%4d  SOC=%4d  ALM=%4d\n",
                readAvg(PIN_RUN), readAvg(PIN_SOC), readAvg(PIN_ALM));
  if (Serial.available()) {
    int angle = Serial.parseInt();
    while (Serial.available()) Serial.read();
    if (angle >= 0 && angle <= 180) {
      Serial.printf("[SERVO] moving to %d\n", angle);
      servo.write(angle);
    }
  }
  delay(500);
  return;
#else

  if (millis() - lastCheck < CHECK_INTERVAL_MS) return;
  lastCheck = millis();
  ensureWifi();

  bool runOn = ledOn(PIN_RUN, THRESHOLD_RUN);
  bool socOn = ledOn(PIN_SOC, THRESHOLD_SOC);
  bool almOn = ledOn(PIN_ALM, THRESHOLD_ALM);
  unsigned long now = millis();

  Serial.printf("[%8lu] state=%d RUN=%d SOC=%d ALM=%d\n", now, state, runOn, socOn, almOn);

  /* ---- ALM stuck-on watchdog (ALM is normal ~3 s at startup only) ---- */
  if (almOn) {
    if (almOnSince == 0) almOnSince = now;
    else if (now - almOnSince > ALM_STUCK_MS &&
             now - lastAlmAlert > ALM_ALERT_REPEAT_MS) {
      sendTelegram("WARNING: battery ALM light has stayed on for over a minute. "
                   "Please check the battery.");
      lastAlmAlert = now;
    }
  } else almOnSince = 0;

  /* ---- notice battery coming back on its own / after our press ---- */
  if (runOn && !lastRunState && state == MONITORING) {
    sendTelegram("Battery RUN light is back on.");
  }
  lastRunState = runOn;

  switch (state) {

    case MONITORING:
      if (runOn) {                          // normal — reset counters
        allOffStreak = 0;
        announcedShutdownPending = false;
        break;
      }
      if (socOn) {                          // RUN off but SOC still lit:
        allOffStreak = 0;                   // battery is mid-shutdown. WAIT.
        if (!announcedShutdownPending) {
          sendTelegram("Battery RUN light went OFF (SOC lights still on). "
                       "Waiting for full shutdown before pressing.");
          announcedShutdownPending = true;
        }
        break;
      }
      // RUN off AND SOC off — count stable readings before acting
      allOffStreak++;
      if (allOffStreak >= STABLE_CHECKS_REQUIRED) {
        allOffStreak = 0;
        announcedShutdownPending = false;
        if (!pressBudgetAvailable()) {
          sendTelegram("Battery is OFF but press limit reached ("
                       + String(MAX_PRESSES_PER_HOUR) +
                       "/hour). LOCKED OUT for safety - please check manually!");
          enterState(LOCKED_OUT);
          break;
        }
        sendTelegram("Battery is fully OFF. Pressing the power button now...");
        pressesThisHour++;
        pressButton();
        enterState(POST_PRESS);
      }
      break;

    case POST_PRESS:
      // Give the battery time to boot (ALM+RUN light, ALM drops after ~3 s,
      // then inverter reconnects). Judge after POST_PRESS_WAIT_MS.
      if (now - stateEnteredAt < POST_PRESS_WAIT_MS) break;
      if (runOn) {
        sendTelegram("SUCCESS: battery is running again. House power should be restored.");
        enterState(MONITORING);
      } else {
        sendTelegram("FAILED: pressed the button but RUN did not come back. "
                     "Waiting 5 minutes before retrying.");
        enterState(COOLDOWN);
      }
      break;

    case COOLDOWN:
      if (runOn) { enterState(MONITORING); break; }   // recovered by itself
      if (now - stateEnteredAt >= FAILED_WAKE_COOLDOWN_MS) enterState(MONITORING);
      break;

    case LOCKED_OUT:
      // Only leaves lockout when the battery is seen running again
      // (i.e., someone fixed it manually or it recovered).
      if (runOn) {
        sendTelegram("Battery is running again. Watchdog re-armed.");
        pressesThisHour = 0;
        hourWindowStart = now;
        enterState(MONITORING);
      }
      break;

    default: break;
  }
#endif
}
