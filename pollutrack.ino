#define TINY_GSM_MODEM_SIM800

#include <TinyGsmClient.h>
#include <ThingSpeak.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <RTClib.h>
#include <HardwareSerial.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

// =========================
// RTC manual set compile flag
// Uncomment to set RTC from code, upload once, then comment out
// #define SET_RTC_MANUALLY

#ifdef SET_RTC_MANUALLY
  #define RTC_MANUAL_YEAR  2026
  #define RTC_MANUAL_MONTH 5
  #define RTC_MANUAL_DAY   12
  #define RTC_MANUAL_HOUR  14
  #define RTC_MANUAL_MIN   45
  #define RTC_MANUAL_SEC   0
#endif

// Optional: enable to dump raw PMS bytes for debugging
// #define DEBUG_PMS_RAW

// ================= SERIAL =================
#define SerialMon Serial

// ================= LED =================
#define LED_ERROR 2

// ================= GSM =================
#define MODEM_RST         5
#define MODEM_PWRKEY      4
#define MODEM_POWER_ON    23
#define MODEM_TX          27
#define MODEM_RX          26

HardwareSerial SerialAT(1);

// ================= PMS =================
// Keep the working pin mapping you provided
#define PMS_RX 33
#define PMS_TX 25
HardwareSerial PMS_Serial(2);

float pm1 = 0;
float pm2_5 = 0;
float pm10 = 0;

// ================= AHT10 =================
#define AHT_SDA 21
#define AHT_SCL 22
Adafruit_AHTX0 aht;

// ================= RTC =================
RTC_DS3231 rtc;

// ================= NOISE (analog microphone) =================
#define MIC_PIN 35
const int SAMPLE_COUNT = 1000;

// ================= WIND SENSOR =================
#define WIND_SPEED_PIN 32
#define WIND_DIR_PIN 34

volatile unsigned long windPulses = 0;
volatile unsigned long lastPulseMicros = 0;

// ================= SD =================
#define SD_MISO 19
#define SD_MOSI 13
#define SD_SCK  18
#define SD_CS   15

// ================= NETWORK =================
const char apn[]  = "internet.mtn";
const char user[] = "";
const char pass[] = "";

// ================= THINGSPEAK =================
unsigned long myChannelNumber = 2486375;
const char* myWriteAPIKey = "1OAT065C2TAFHEPZ";

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);

// ================= TIMING =================
unsigned long lastTime = 0;
const unsigned long postingInterval = 60000UL; // 60 seconds

// ================= HELPERS =================
void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_ERROR, HIGH);
    delay(delayMs);
    digitalWrite(LED_ERROR, LOW);
    delay(delayMs);
  }
}

void printStatus(const char* device, bool status) {
  SerialMon.print("[");
  SerialMon.print(device);
  SerialMon.print("] => ");
  if (status) SerialMon.println("OK ✅");
  else SerialMon.println("FAIL ❌");
}

// =====================================================
// INTERRUPT (wind pulse counter)
// =====================================================
void IRAM_ATTR countWindPulse() {
  unsigned long now = micros();
  if (now - lastPulseMicros > 5000) { // debounce 5 ms
    windPulses++;
    lastPulseMicros = now;
  }
}

// =====================================================
// Robust PMS7003 reader (32-byte frame, checksum verified)
// =====================================================
bool readPMS() {
  const int FRAME_LEN = 32;
  static uint8_t buf[FRAME_LEN];

#ifdef DEBUG_PMS_RAW
  while (PMS_Serial.available()) {
    uint8_t b = PMS_Serial.read();
    if (b < 16) SerialMon.print('0');
    SerialMon.print(b, HEX);
    SerialMon.print(' ');
  }
  if (PMS_Serial.available()) SerialMon.println();
  return false;
#endif

  if (PMS_Serial.available() <= 0) return false;

  while (PMS_Serial.available() > 0) {
    int b = PMS_Serial.read();
    if (b != 0x42) continue;

    unsigned long start = millis();
    while (PMS_Serial.available() == 0 && (millis() - start) < 200) { }
    if (PMS_Serial.available() == 0) return false;

    int b2 = PMS_Serial.read();
    if (b2 != 0x4D) continue;

    buf[0] = 0x42;
    buf[1] = 0x4D;
    int idx = 2;
    start = millis();
    while (idx < FRAME_LEN && (millis() - start) < 300) {
      if (PMS_Serial.available() > 0) {
        int r = PMS_Serial.read();
        if (r >= 0) buf[idx++] = (uint8_t)r;
      }
    }
    if (idx < FRAME_LEN) {
      SerialMon.println("PMS: frame timeout");
      return false;
    }

    uint16_t checksum = ((uint16_t)buf[30] << 8) | buf[31];
    uint16_t sum = 0;
    for (int i = 0; i < 30; ++i) sum += buf[i];

    if (sum != checksum) {
      SerialMon.print("PMS: checksum fail sum=");
      SerialMon.print(sum);
      SerialMon.print(" chk=");
      SerialMon.println(checksum);
      return false;
    }

    uint16_t raw_pm1   = ((uint16_t)buf[10] << 8) | buf[11];
    uint16_t raw_pm2_5 = ((uint16_t)buf[12] << 8) | buf[13];
    uint16_t raw_pm10  = ((uint16_t)buf[14] << 8) | buf[15];

    pm1   = (float)raw_pm1;
    pm2_5 = (float)raw_pm2_5;
    pm10  = (float)raw_pm10;

    return true;
  }

  return false;
}

// =====================================================
// NOISE SENSOR (analog microphone)
// =====================================================
float readNoiseLevel() {
  long sum = 0;
  int minVal = 4095;
  int maxVal = 0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int val = analogRead(MIC_PIN);
    sum += val;
    if (val < minVal) minVal = val;
    if (val > maxVal) maxVal = val;
    delayMicroseconds(100);
  }

  float peakToPeak = maxVal - minVal;
  float voltage = peakToPeak * (3.3 / 4095.0);
  if (voltage < 0.001f) voltage = 0.001f;

  return 20.0 * log10(voltage / 0.001) + 40.0;
}

// =====================================================
// WIND SPEED
// =====================================================
float readWindSpeed() {
  noInterrupts();
  unsigned long pulses = windPulses;
  windPulses = 0;
  interrupts();

  float pulsePerSecond = pulses / 60.0f; // postingInterval is 60s
  return pulsePerSecond * 2.4f;
}

// =====================================================
// WIND DIRECTION
// =====================================================
const char* readWindDirection() {
  int adc = analogRead(WIND_DIR_PIN);
  if (adc < 300) return "N";
  else if (adc < 700) return "NE";
  else if (adc < 1100) return "E";
  else if (adc < 1500) return "SE";
  else if (adc < 1900) return "S";
  else if (adc < 2300) return "SW";
  else if (adc < 3000) return "W";
  else return "NW";
}

// =====================================================
// SAVE TO SD: CSV, JSON, TXT (three separate files)
// Files: /poll_gprs.csv, /poll_gprs.json, /poll_gprs.txt
// =====================================================
void saveToFiles(float t, float h, float p1, float p25, float p10, float noise, float ws, const char* wd) {
  DateTime now = rtc.now();

  char dateBuf[16];
  char timeBuf[16];
  char dateTimeBuf[32];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", now.year(), now.month(), now.day());
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  snprintf(dateTimeBuf, sizeof(dateTimeBuf), "%s %s", dateBuf, timeBuf);

  // ---------- CSV ----------
  File csvFile = SD.open("/poll_gprs.csv", FILE_APPEND);
  if (csvFile) {
    if (csvFile.size() == 0) {
      csvFile.println("Date,Time,Temperature,Humidity,PM1,PM2.5,PM10,Noise,WindSpeed_kmh,WindDir");
    }
    csvFile.print(dateBuf); csvFile.print(",");
    csvFile.print(timeBuf); csvFile.print(",");
    csvFile.print(t, 2); csvFile.print(",");
    csvFile.print(h, 2); csvFile.print(",");
    csvFile.print(p1, 2); csvFile.print(",");
    csvFile.print(p25, 2); csvFile.print(",");
    csvFile.print(p10, 2); csvFile.print(",");
    csvFile.print(noise, 2); csvFile.print(",");
    csvFile.print(ws, 2); csvFile.print(",");
    csvFile.println(wd);
    csvFile.close();
  } else {
    SerialMon.println("[SD] CSV OPEN FAIL ❌");
    digitalWrite(LED_ERROR, HIGH);
  }

  // ---------- JSON ----------
  File jsonFile = SD.open("/poll_gprs.json", FILE_APPEND);
  String jsonObject;
  jsonObject.reserve(256);
  jsonObject += "{";
  jsonObject += "\"datetime\":\""; jsonObject += dateTimeBuf; jsonObject += "\",";
  jsonObject += "\"temperature\":"; jsonObject += String(t, 2); jsonObject += ",";
  jsonObject += "\"humidity\":"; jsonObject += String(h, 2); jsonObject += ",";
  jsonObject += "\"pm1\":"; jsonObject += String(p1, 2); jsonObject += ",";
  jsonObject += "\"pm2_5\":"; jsonObject += String(p25, 2); jsonObject += ",";
  jsonObject += "\"pm10\":"; jsonObject += String(p10, 2); jsonObject += ",";
  jsonObject += "\"noise\":"; jsonObject += String(noise, 2); jsonObject += ",";
  jsonObject += "\"wind_speed_kmh\":"; jsonObject += String(ws, 2); jsonObject += ",";
  jsonObject += "\"wind_dir\":\""; jsonObject += wd; jsonObject += "\"";
  jsonObject += "}";

  if (jsonFile) {
    if (jsonFile.size() == 0) {
      jsonFile.println("[");
      jsonFile.print("  ");
      jsonFile.println(jsonObject);
      jsonFile.println("]");
    } else {
      uint32_t sz = jsonFile.size();
      uint32_t seekPos = (sz > 2) ? sz - 2 : 0;
      jsonFile.seek(seekPos);
      jsonFile.print(",\n  ");
      jsonFile.println(jsonObject);
      jsonFile.println("]");
    }
    jsonFile.close();
  } else {
    SerialMon.println("[SD] JSON OPEN FAIL ❌");
    digitalWrite(LED_ERROR, HIGH);
  }

  // ---------- TXT ----------
  File txtFile = SD.open("/poll_gprs.txt", FILE_APPEND);
  if (txtFile) {
    if (txtFile.size() == 0) {
      txtFile.println("Sensor Data Log (poll_gprs)");
      txtFile.println("Date | Time | Temp | Humidity | PM1 | PM2.5 | PM10 | Noise | WindSpeed_kmh | WindDir");
    }
    txtFile.print(dateBuf); txtFile.print(" ");
    txtFile.print(timeBuf); txtFile.print(" | ");
    txtFile.print(t, 2); txtFile.print(" | ");
    txtFile.print(h, 2); txtFile.print(" | ");
    txtFile.print(p1, 2); txtFile.print(" | ");
    txtFile.print(p25, 2); txtFile.print(" | ");
    txtFile.print(p10, 2); txtFile.print(" | ");
    txtFile.print(noise, 2); txtFile.print(" | ");
    txtFile.print(ws, 2); txtFile.print(" | ");
    txtFile.println(wd);
    txtFile.close();
  } else {
    SerialMon.println("[SD] TXT OPEN FAIL ❌");
    digitalWrite(LED_ERROR, HIGH);
  }

  SerialMon.println("[SD] DATA SAVED TO CSV, JSON, TXT ✅");
}

// =========================
// SETUP
// =========================
void setup() {
  SerialMon.begin(115200);
  delay(1000);

  pinMode(LED_ERROR, OUTPUT);
  digitalWrite(LED_ERROR, LOW);

  SerialMon.println("\n--- SYSTEM START ---");

  // GSM power pins
  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  pinMode(MODEM_RST, OUTPUT);

  digitalWrite(MODEM_POWER_ON, HIGH);
  digitalWrite(MODEM_RST, HIGH);

  digitalWrite(MODEM_PWRKEY, LOW);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(2000);

  // GSM serial
  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  modem.restart();
  bool gsmOK = modem.waitForNetwork(60000) && modem.gprsConnect(apn, user, pass);
  printStatus("GSM", gsmOK);
  if (!gsmOK) digitalWrite(LED_ERROR, HIGH);

  // AHT10
  Wire.begin(AHT_SDA, AHT_SCL);
  bool ahtOK = aht.begin();
  printStatus("AHT10", ahtOK);
  if (!ahtOK) digitalWrite(LED_ERROR, HIGH);

  // RTC
  bool rtcOK = rtc.begin();
  printStatus("RTC", rtcOK);

  #ifdef SET_RTC_MANUALLY
    rtc.adjust(DateTime(RTC_MANUAL_YEAR, RTC_MANUAL_MONTH, RTC_MANUAL_DAY,
                        RTC_MANUAL_HOUR, RTC_MANUAL_MIN, RTC_MANUAL_SEC));
    SerialMon.println("[RTC] Manually set via SET_RTC_MANUALLY");
  #endif

  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    SerialMon.println("[RTC] Set from compile time because RTC lost power");
  }

  // PMS sensor serial (rxPin, txPin)
  PMS_Serial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  printStatus("PMS SENSOR", true);

  // Microphone
  pinMode(MIC_PIN, INPUT);
  printStatus("MIC", true);

  // Wind sensor
  pinMode(WIND_SPEED_PIN, INPUT_PULLUP);
  pinMode(WIND_DIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(WIND_SPEED_PIN), countWindPulse, FALLING);
  printStatus("WIND SENSOR", true);

  // SD card
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  bool sdOK = SD.begin(SD_CS);
  printStatus("SD CARD", sdOK);
  if (!sdOK) digitalWrite(LED_ERROR, HIGH);

  // Ensure the three files exist and have headers (create if missing)
  if (sdOK) {
    // CSV header
    if (!SD.exists("/poll_gprs.csv")) {
      File f = SD.open("/poll_gprs.csv", FILE_WRITE);
      if (f) {
        f.println("Date,Time,Temperature,Humidity,PM1,PM2.5,PM10,Noise,WindSpeed_kmh,WindDir");
        f.close();
      }
    }
    // JSON initial array
    if (!SD.exists("/poll_gprs.json")) {
      File f = SD.open("/poll_gprs.json", FILE_WRITE);
      if (f) {
        f.println("[");
        f.println("]");
        f.close();
      }
    }
    // TXT header
    if (!SD.exists("/poll_gprs.txt")) {
      File f = SD.open("/poll_gprs.txt", FILE_WRITE);
      if (f) {
        f.println("Sensor Data Log (poll_gprs)");
        f.println("Date | Time | Temp | Humidity | PM1 | PM2.5 | PM10 | Noise | WindSpeed_kmh | WindDir");
        f.close();
      }
    }
  }

  // ThingSpeak
  ThingSpeak.begin(client);
  printStatus("ThingSpeak", true);

  SerialMon.println("--- SETUP COMPLETE ---");
}

// =========================
// LOOP
// =========================
void loop() {
  // Try to read PMS frames whenever available (non-blocking)
  readPMS();

  if (millis() - lastTime > postingInterval) {
    lastTime = millis();

    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    float temperature = temp.temperature;
    float hum = humidity.relative_humidity;
    float noise = readNoiseLevel();
    float windSpeed = readWindSpeed();
    const char* windDirection = readWindDirection();

    DateTime now = rtc.now();

    SerialMon.println("\n--- DATA LOG ---");
    SerialMon.print("DateTime: ");
    SerialMon.print(now.year()); SerialMon.print("-");
    SerialMon.print(now.month()); SerialMon.print("-");
    SerialMon.print(now.day()); SerialMon.print(" ");
    SerialMon.print(now.hour()); SerialMon.print(":");
    SerialMon.print(now.minute()); SerialMon.print(":");
    SerialMon.println(now.second());

    SerialMon.print("Temperature : "); SerialMon.print(temperature); SerialMon.println(" °C");
    SerialMon.print("Humidity    : "); SerialMon.print(hum); SerialMon.println(" %");
    SerialMon.print("PM1.0       : "); SerialMon.print(pm1); SerialMon.println(" ug/m3");
    SerialMon.print("PM2.5       : "); SerialMon.print(pm2_5); SerialMon.println(" ug/m3");
    SerialMon.print("PM10        : "); SerialMon.print(pm10); SerialMon.println(" ug/m3");
    SerialMon.print("Noise       : "); SerialMon.print(noise); SerialMon.println(" dB");
    SerialMon.print("Wind Speed  : "); SerialMon.print(windSpeed); SerialMon.println(" km/h");
    SerialMon.print("Wind Dir    : "); SerialMon.println(windDirection);

    // Save to CSV, JSON, TXT
    saveToFiles(temperature, hum, pm1, pm2_5, pm10, noise, windSpeed, windDirection);

    blinkLED(1, 200);

    // ThingSpeak upload
    ThingSpeak.setField(1, temperature);
    ThingSpeak.setField(2, hum);
    ThingSpeak.setField(3, pm1);
    ThingSpeak.setField(4, pm2_5);
    ThingSpeak.setField(5, pm10);
    ThingSpeak.setField(6, noise);
    ThingSpeak.setField(7, windSpeed);
    ThingSpeak.setField(8, String(windDirection));

    char dateTimeBuf[32];
    snprintf(dateTimeBuf, sizeof(dateTimeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    String statusText = String(dateTimeBuf) + " | WD: " + String(windDirection);
    ThingSpeak.setStatus(statusText);

    int status = ThingSpeak.writeFields(myChannelNumber, myWriteAPIKey);
    if (status == 200) {
      SerialMon.println("[THINGSPEAK] DATA SENT ✅");
      blinkLED(3, 150);
    } else {
      SerialMon.print("[THINGSPEAK] SEND FAIL ❌ Status: ");
      SerialMon.println(status);
      digitalWrite(LED_ERROR, HIGH);
    }
  }

  modem.maintain();
}
