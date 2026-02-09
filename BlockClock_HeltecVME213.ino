#include <WiFi.h>
#include <PubSubClient.h>
#include <heltec-eink-modules.h>
#include "esp_sleep.h"
#include <cstring>
#include "time.h"
#include "secrets.h"

// =========================
// User config
// =========================

const char* TOPIC_HEIGHT   = "home/bitcoin/height";
const char* TOPIC_HALVING  = "home/bitcoin/halving/blocks_remaining";
const char* TOPIC_HASHRATE = "home/bitcoin/hashrate_ehs";
const char* TOPIC_PRICE_USD = "home/bitcoin/price/usd";

String hashrateEhs = "";
RTC_DATA_ATTR char lastHashrateEhs[16] = "--";
RTC_DATA_ATTR char lastPriceUsd[16]    = "--";  

const uint64_t SLEEP_MINUTES = 60;  // deep sleep interval

// Timezone for Switzerland (CH)
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600;      // UTC+1
const int   DAYLIGHT_OFFSET_SEC = 3600; // +1 hour in summer

// =========================
// Hardware pins (Vision Master E213)
// =========================
const int PIN_EINK_POWER = 45;  // E-ink VCC enable
const int PIN_BAT_ADC    = 7;   // VBAT_Read
const int PIN_ADC_CTRL   = 46;  // ADC_Ctrl gate (enables VBAT divider)

// =========================
// Battery measurement constants
// =========================
const float ADC_MAX   = 4095.0;
const float ADC_REF_V = 3.3;
const float VBAT_SCALE = 4.9;   // divider ~390k/100k

// =========================
// RTC data: persists across deep sleep
// =========================
RTC_DATA_ATTR bool hasPrevData = false;
RTC_DATA_ATTR char lastBlockHeight[16]      = "--";
RTC_DATA_ATTR char lastBlocksRemaining[16]  = "--";
RTC_DATA_ATTR float lastBatteryVoltage      = 0.0;
RTC_DATA_ATTR int   lastBatteryPercent      = -1;

// =========================
// Globals for this boot
// =========================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
EInkDisplay_VisionMasterE213 display;

String blockHeight      = "";
String blocksRemaining  = "";
String priceUsd         = "";
bool   newDataReceived  = false;

// =========================
// Battery helpers
// =========================

int batteryPercent(float v)
{
  // Custom SOC curve based on your measurements
  const int NUM_POINTS = 8;
  const float voltTable[NUM_POINTS] = {
    3.20, 3.30, 3.60, 3.75, 3.85, 3.95, 4.05, 4.15
  };
  const int socTable[NUM_POINTS] = {
    0,    2,    25,   50,   70,   90,   97,   100
  };

  // Clamp out-of-range
  if (v <= voltTable[0])   return socTable[0];
  if (v >= voltTable[NUM_POINTS - 1]) return socTable[NUM_POINTS - 1];

  // Find segment where v sits between two table points
  for (int i = 0; i < NUM_POINTS - 1; i++) {
    float v1 = voltTable[i];
    float v2 = voltTable[i + 1];

    if (v >= v1 && v <= v2) {
      int soc1 = socTable[i];
      int soc2 = socTable[i + 1];

      float t = (v - v1) / (v2 - v1); // normalize segment
      float soc = soc1 + t * (soc2 - soc1);

      // Clamp + round
      if (soc < 0) soc = 0;
      if (soc > 100) soc = 100;
      return (int)(soc + 0.5f);
    }
  }

  return 0; // fallback
}

float readBatteryVoltage() {
  pinMode(PIN_ADC_CTRL, OUTPUT);
  digitalWrite(PIN_ADC_CTRL, HIGH);
  delay(5);

  pinMode(PIN_BAT_ADC, INPUT);
  int raw = analogRead(PIN_BAT_ADC);

  digitalWrite(PIN_ADC_CTRL, LOW);

  float v_adc  = (raw / ADC_MAX) * ADC_REF_V;
  float v_batt = v_adc * VBAT_SCALE;
  return v_batt;
}

// =========================
// MQTT callback
// =========================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("MQTT msg on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  if (String(topic) == TOPIC_HEIGHT) {
    blockHeight = msg;
    newDataReceived = true;
  } else if (String(topic) == TOPIC_HALVING) {
    blocksRemaining = msg;
    newDataReceived = true;
  } else if (String(topic) == TOPIC_HASHRATE) {
    hashrateEhs = msg;  // already formatted like "458.73"
    newDataReceived = true;
  } else if (String(topic) == TOPIC_PRICE_USD) {      
    priceUsd = msg;                                   
    newDataReceived = true;                           
  } 
}

// =========================
// WiFi + MQTT connect
// =========================

bool connectWiFi(uint16_t timeout_ms = 15000) {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("WiFi connect FAILED");
    return false;
  }
}

bool connectMQTT(uint16_t timeout_ms = 5000) {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  uint32_t start = millis();
  while (!mqttClient.connected() && (millis() - start) < timeout_ms) {
    Serial.print("Connecting to MQTT... ");
    if (mqttClient.connect("VM_E213_Client", MQTT_USER, MQTT_PASS)) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retry in 1s");
      delay(1000);
    }
  }

  if (!mqttClient.connected()) {
    Serial.println("MQTT connect FAILED");
    return false;
  }

  mqttClient.subscribe(TOPIC_HEIGHT);
  mqttClient.subscribe(TOPIC_HALVING);
  mqttClient.subscribe(TOPIC_HASHRATE);
  mqttClient.subscribe(TOPIC_PRICE_USD);
  Serial.println("Subscribed to topics.");

  return true;
}

// =========================
// Display drawing
// =========================

void drawDisplay(float vbat, int pct, String timestamp){
  Serial.println(">>> Entered drawDisplay()");

  display.clear();
  display.setRotation(1);      // landscape
  display.setTextColor(BLACK);

  // Use latest known data, fallback to RTC last data if empty
  String h = blockHeight.length()     ? blockHeight     : String(lastBlockHeight);
  String r = blocksRemaining.length() ? blocksRemaining : String(lastBlocksRemaining);
  String hr = hashrateEhs.length()    ? hashrateEhs     : String(lastHashrateEhs);
  String p  = priceUsd.length()        ? priceUsd        : String(lastPriceUsd);

  // ------- First line: block height -------
  display.setTextSize(3);   // big numbers
  display.setCursor(10, 10);
  display.println(h);

  display.setTextSize(1);   // small label
  display.setCursor(10, 35);
  display.println("BLOCK HEIGHT");

  // ------- Hashrate --------
  int hr_int = hr.toInt();  // convert String to integer
  display.setTextSize(3);   // big numbers
  display.setCursor(135, 10);
  display.println(hr_int);

  display.setTextSize(1);   // small label
  display.setCursor(135, 35);
  display.println("HASHRATE EH/s");

  // ------- Second line: blocks to halving -------
  display.setTextSize(3);
  display.setCursor(10, 55);
  display.println(r);

  display.setTextSize(1);
  display.setCursor(10, 80);
  display.println("BLOCKS TO HALVING");

    // ------- BTC price -------
  display.setTextSize(3);
  display.setCursor(135, 55);
  display.println(p);

  display.setTextSize(1);
  display.setCursor(135, 80);
  display.println("BTC PRICE USD");

  // ------- Battery info line -------
  display.setTextSize(1);
  display.setCursor(10, 100);

  display.print("Battery: ");
  if (pct >= 0) {
    display.print(pct);
    display.print("%  (");
    display.print(vbat, 2);
    display.print(" V)");
  } else {
    display.print("N/A");
  }

  // ------- Last update time -------
  display.setTextSize(1);
  display.setCursor(10, 110);
  display.print("Last update: ");
  display.print(timestamp);

  Serial.println("Calling display.update() ...");
  display.update();
  Serial.println("Display update DONE");
}

// =========================
// Deep sleep helper
// =========================

void goToSleep() {
  Serial.println("Going to deep sleep...");
  // Turn off e-ink power to save energy
  digitalWrite(PIN_EINK_POWER, LOW);

  uint64_t sleep_us = SLEEP_MINUTES * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleep_us);
  esp_deep_sleep_start();
}

String makeTimestamp()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "0000-00-00 00:00";   // fallback if time failed
    }

    char buffer[20];  // "YYYY-MM-DD HH:MM" = 16 chars + null terminator
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &timeinfo);
    return String(buffer);
}

// =========================
// SETUP
// =========================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("==== VM E213 Bitcoin Dashboard Boot ====");

  // Power E-ink
  pinMode(PIN_EINK_POWER, OUTPUT);
  digitalWrite(PIN_EINK_POWER, HIGH);
  delay(100);

  // Init display
  display.begin();
  Serial.println("Display.begin OK");

  // Connect WiFi
  bool wifiOK = connectWiFi();
  String timestamp = "--:--";
  if (wifiOK) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    timestamp = makeTimestamp();
  }
  bool mqttOK = false;

  if (wifiOK) {
    mqttOK = connectMQTT();
  }

  // Let MQTT run for some seconds to get messages
  if (mqttOK) {
    uint32_t start = millis();
    while (millis() - start < 8000) { // 8 seconds window
      mqttClient.loop();
      delay(10);
    }
  }

  // Read battery
  float vbat = readBatteryVoltage();
  int pct    = batteryPercent(vbat);

  Serial.print("VBAT = ");
  Serial.print(vbat, 3);
  Serial.print(" V (");
  Serial.print(pct);
  Serial.println(" %)");

  // Store latest values in RTC for next wake
  if (blockHeight.length() > 0) {
    strncpy(lastBlockHeight, blockHeight.c_str(), sizeof(lastBlockHeight) - 1);
    lastBlockHeight[sizeof(lastBlockHeight) - 1] = '\0';
  }
  if (blocksRemaining.length() > 0) {
    strncpy(lastBlocksRemaining, blocksRemaining.c_str(), sizeof(lastBlocksRemaining) - 1);
    lastBlocksRemaining[sizeof(lastBlocksRemaining) - 1] = '\0';
  }
  if (hashrateEhs.length() > 0) {
  strncpy(lastHashrateEhs, hashrateEhs.c_str(), sizeof(lastHashrateEhs) - 1);
  lastHashrateEhs[sizeof(lastHashrateEhs) - 1] = '\0';
  }
  if (priceUsd.length() > 0) {  
  strncpy(lastPriceUsd, priceUsd.c_str(), sizeof(lastPriceUsd) - 1);
  lastPriceUsd[sizeof(lastPriceUsd) - 1] = '\0';
  }

  lastBatteryVoltage = vbat;
  lastBatteryPercent = pct;
  hasPrevData = true;

  // Draw on e-ink
  drawDisplay(vbat, pct, timestamp);

  // Disconnect MQTT/WiFi to be nice
  if (mqttClient.connected()) mqttClient.disconnect();
  WiFi.disconnect(true);

  // Sleep
  goToSleep();
}

// =========================
// LOOP (never reached after deep sleep start)
// =========================

void loop() {
  // Not used; device deep-sleeps from setup().
}
