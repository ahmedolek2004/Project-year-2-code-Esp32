#include <WiFi.h>
#include <FirebaseESP32.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ── WiFi ──────────────────────────────────────────────────────
#define WIFI_SSID     "Redmi10"
#define WIFI_PASSWORD "11111111"

// ── Firebase ──────────────────────────────────────────────────
#define API_KEY      "AIzaSyBZ6fgNfFzkT22KWoXeYWkj_lmOB25MEFs"
#define DATABASE_URL "https://planb-ib-default-rtdb.firebaseio.com"
#define USER_EMAIL   "strip001@smartstrip.local"
#define USER_PASS    "123456"

// ── Strip ID ──────────────────────────────────────────────────
String stripId = "strip001";

// ── Pins ──────────────────────────────────────────────────────
int relayPins[8] = {13, 12, 14, 27, 26, 25, 33, 32};
const int voltPin = 34;   // ZMPT101B
const int currPin = 35;   // ACS712

// ── Sensor calibration ────────────────────────────────────────
float vCalibration    = 530.0;
float currSensitivity = 0.066;
float vOffset         = 1.65;

// ── Firebase objects ──────────────────────────────────────────
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ── NTP (time) ────────────────────────────────────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600 * 2);

// ── Timers ────────────────────────────────────────────────────
unsigned long lastRelayCheck    = 0;
unsigned long lastRealtimeUpdate = 0;
unsigned long lastMonthlyUpdate  = 0;

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Set all relays OFF on boot
  for (int i = 0; i < 8; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  // Firebase config
  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;

  // FIX 1: Authenticate with email + password
  // Without this, Firebase rejects every read and write
  auth.user.email    = USER_EMAIL;
  auth.user.password = USER_PASS;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Wait until Firebase is ready and authenticated
  Serial.print("Authenticating with Firebase");
  while (!Firebase.ready()) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nFirebase ready");

  // Start NTP time client
  timeClient.begin();
  timeClient.update();
}

// ─────────────────────────────────────────────────────────────
// Read RMS voltage from ZMPT101B
float getVrms() {
  float vSum = 0;
  int samples = 500;
  for (int i = 0; i < samples; i++) {
    float v = (analogRead(voltPin) * (3.3 / 4095.0)) - vOffset;
    vSum += v * v;
    delayMicroseconds(500);
  }
  return sqrt(vSum / samples) * vCalibration;
}

// Read RMS current from ACS712
float getIrms() {
  float iSum = 0;
  int samples = 500;
  for (int i = 0; i < samples; i++) {
    float v = (analogRead(currPin) * (3.3 / 4095.0)) - vOffset;
    iSum += v * v;
    delayMicroseconds(500);
  }
  return sqrt(iSum / samples) / currSensitivity;
}

// ─────────────────────────────────────────────────────────────
void loop() {
  timeClient.update();

  // FIX 3: Check relays every 2 seconds — not every loop tick
  // Reading 8 paths per loop was hammering Firebase and causing timeouts
  if (millis() - lastRelayCheck > 2000) {
    lastRelayCheck = millis();

    for (int i = 0; i < 8; i++) {
      String path = "/powerstrips/" + stripId + "/outlets/" + String(i) + "/on";
      if (Firebase.getBool(fbdo, path)) {
        bool shouldBeOn = fbdo.boolData();
        digitalWrite(relayPins[i], shouldBeOn ? HIGH : LOW);
      } else {
        Serial.println("Relay read failed [" + String(i) + "]: " + fbdo.errorReason());
      }
    }
  }

  // Send live sensor readings every 5 seconds
  if (millis() - lastRealtimeUpdate > 5000) {
    lastRealtimeUpdate = millis();

    float vRMS  = getVrms();
    float iRMS  = getIrms();
    float power = vRMS * iRMS;

    // FIX 2: ts must be a NUMBER (milliseconds), not a string
    // The website does new Date(ts) which only works with a number
    unsigned long long ts = (unsigned long long)timeClient.getEpochTime() * 1000ULL;

    FirebaseJson json;
    json.set("voltage", vRMS);
    json.set("current", iRMS);
    json.set("power",   power);
    json.set("ts",      (double)ts);   // double keeps full precision in FirebaseESP32

    if (Firebase.setJSON(fbdo, "/powerstrips/" + stripId + "/realtime", json)) {
      Serial.printf("Realtime sent — V:%.1f  I:%.2f  W:%.0f\n", vRMS, iRMS, power);
    } else {
      Serial.println("Realtime send failed: " + fbdo.errorReason());
    }
  }

  // Update monthly energy log every hour
  if (millis() - lastMonthlyUpdate > 3600000) {
    lastMonthlyUpdate = millis();
    updateMonthlyLog();
  }
}

// ─────────────────────────────────────────────────────────────
void updateMonthlyLog() {

  // Build "YYYY-MM" key from current time
  time_t rawtime = timeClient.getEpochTime();
  struct tm* ti  = localtime(&rawtime);
  char yearMonth[8];
  sprintf(yearMonth, "%04d-%02d", ti->tm_year + 1900, ti->tm_mon + 1);
  String monthPath = "/powerstrips/" + stripId + "/monthly/" + String(yearMonth);

  float oldKwh = 0, oldCost = 0, rate = 0.12;

  // FIX 4: Read the existing monthly JSON FIRST, copy data out before any other Firebase call
  // In the old code, a second Firebase call overwrote fbdo making the first JSON unreadable
  if (Firebase.getJSON(fbdo, monthPath)) {
    FirebaseJsonData res;
    FirebaseJson json;
    json.setJsonData(fbdo.stringData());          // copy data out of fbdo safely

    json.get(res, "kwh_total");
    if (res.success) oldKwh = res.floatValue;

    json.get(res, "cost");
    if (res.success) oldCost = res.floatValue;
  }

  // THEN read the rate (this overwrites fbdo — safe now because we already copied above)
  if (Firebase.getFloat(fbdo, "/powerstrips/" + stripId + "/config/rate")) {
    rate = fbdo.floatData();
  }

  // Measure power right now for the hourly energy estimate
  float vRMS         = getVrms();
  float iRMS         = getIrms();
  float energyKwh    = (vRMS * iRMS) / 1000.0;   // watts → kilowatts × 1 hour

  float updatedKwh   = oldKwh  + energyKwh;
  float updatedCost  = updatedKwh * rate;

  FirebaseJson update;
  update.set("kwh_total", updatedKwh);
  update.set("cost",      updatedCost);

  if (Firebase.setJSON(fbdo, monthPath, update)) {
    Serial.printf("Monthly updated — %s  kWh:%.4f  Cost:$%.4f\n", yearMonth, updatedKwh, updatedCost);
  } else {
    Serial.println("Monthly update failed: " + fbdo.errorReason());
  }
}
