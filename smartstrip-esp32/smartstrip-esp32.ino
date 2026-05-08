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

String stripId = "strip001";

// ── Pins ──────────────────────────────────────────────────────
int relayPins[8] = {13, 12, 14, 27, 26, 25, 33, 32};
const int voltPin = 34;   
const int currPin = 35;   

// ── Sensor calibration & Stability ────────────────────────────
float vCalibration    = 480.0; // تم تعديلها بناءً على الصورة لتقليل الخطأ
float currSensitivity = 0.066; 
float vOffset         = 1.65;  // سيتم تحديثها تلقائياً في setup
float iOffset         = 1.65;  // سيتم تحديثها تلقائياً في setup

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600 * 2);

unsigned long lastRelayCheck     = 0;
unsigned long lastRealtimeUpdate = 0;
unsigned long lastMonthlyUpdate  = 0;

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 8; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  // معايرة نقطة الصفر تلقائياً (Auto-Calibration)
  // تأكد من عدم وجود أحمال عند بدء التشغيل
  long vSum = 0;
  long iSum = 0;
  for(int i=0; i<1000; i++) {
    vSum += analogRead(voltPin);
    iSum += analogRead(currPin);
    delay(1);
  }
  vOffset = (vSum / 1000.0) * (3.3 / 4095.0);
  iOffset = (iSum / 1000.0) * (3.3 / 4095.0);
  
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASS;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  timeClient.begin();
  timeClient.update();
  Serial.println("\nSystem Ready");
}

// ── قراءة الجهد مع الحماية ───────────────────────────────────
float getVrms() {
  float vSum = 0;
  int samples = 500;
  for (int i = 0; i < samples; i++) {
    float v = (analogRead(voltPin) * (3.3 / 4095.0)) - vOffset;
    vSum += v * v;
    delayMicroseconds(500);
  }
  float vRMS = sqrt(vSum / samples) * vCalibration;
  return (vRMS < 15.0) ? 0 : vRMS; // إذا كان أقل من 15 فولت اعتبره 0
}

// ── قراءة التيار مع تصفية الضوضاء ──────────────────────────────
float getIrms() {
  float iSum = 0;
  int samples = 500;
  for (int i = 0; i < samples; i++) {
    float v = (analogRead(currPin) * (3.3 / 4095.0)) - iOffset;
    iSum += v * v;
    delayMicroseconds(500);
  }
  float iRMS = sqrt(iSum / samples) / currSensitivity;
  
  // أهم تعديل: لا يقرأ تيار إلا إذا تجاوز 0.12 أمبير (حوالي 26 وات)
  if (iRMS < 0.12) iRMS = 0.0; 
  return iRMS;
}

void loop() {
  timeClient.update();

  // تحديث حالة الريلاي (كل 2 ثانية لتجنب الضغط على السيرفر)
  if (millis() - lastRelayCheck > 2000) {
    lastRelayCheck = millis();
    for (int i = 0; i < 8; i++) {
      String path = "/powerstrips/" + stripId + "/outlets/" + String(i) + "/on";
      if (Firebase.getBool(fbdo, path)) {
        digitalWrite(relayPins[i], fbdo.boolData() ? HIGH : LOW);
      }
    }
  }

  // إرسال البيانات المباشرة (كل 5 ثوانٍ)
  if (millis() - lastRealtimeUpdate > 5000) {
    lastRealtimeUpdate = millis();

    float vRMS  = getVrms();
    float iRMS  = getIrms();
    float power = vRMS * iRMS;

    // في حال عدم وجود تيار، صفر القدرة تماماً
    if (iRMS <= 0.0) power = 0.0;

    unsigned long long ts = (unsigned long long)timeClient.getEpochTime() * 1000ULL;

    FirebaseJson json;
    json.set("voltage", vRMS);
    json.set("current", iRMS);
    json.set("power",   power);
    json.set("ts",      (double)ts);

    Firebase.setJSON(fbdo, "/powerstrips/" + stripId + "/realtime", json);
    Serial.printf("V: %.1fV | I: %.2fA | P: %.1fW\n", vRMS, iRMS, power);
  }

  // التحديث الشهري
  if (millis() - lastMonthlyUpdate > 3600000) {
    lastMonthlyUpdate = millis();
    updateMonthlyLog();
  }
}

void updateMonthlyLog() {
  time_t rawtime = timeClient.getEpochTime();
  struct tm* ti  = localtime(&rawtime);
  char yearMonth[8];
  sprintf(yearMonth, "%04d-%02d", ti->tm_year + 1900, ti->tm_mon + 1);
  String monthPath = "/powerstrips/" + stripId + "/monthly/" + String(yearMonth);

  float oldKwh = 0, rate = 0.127; // السعر كما في الصورة

  if (Firebase.getJSON(fbdo, monthPath)) {
    FirebaseJsonData res;
    FirebaseJson json;
    json.setJsonData(fbdo.stringData());
    json.get(res, "kwh_total");
    if (res.success) oldKwh = res.floatValue;
  }

  if (Firebase.getFloat(fbdo, "/powerstrips/" + stripId + "/config/rate")) {
    rate = fbdo.floatData();
  }

  float vRMS = getVrms();
  float iRMS = getIrms();
  float energyKwh = (vRMS * iRMS * 1.0) / 1000.0; // استهلاك ساعة واحدة

  float updatedKwh  = oldKwh + energyKwh;
  float updatedCost = updatedKwh * rate;

  FirebaseJson update;
  update.set("kwh_total", updatedKwh);
  update.set("cost",      updatedCost);

  Firebase.setJSON(fbdo, monthPath, update);
}