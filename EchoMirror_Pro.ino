/***********************************************
 * EchoMirror Pro — Assistive Smart Mirror
 * MCU     : ESP32 Wroom 32E
 * Author  : Junite Rose P J
 * Date    : August 2025
 *
 * System Overview:
 *   Clap-triggered assistive mirror for visually
 *   and hearing-impaired users. On clap detection,
 *   reads temperature, pressure, air quality, and
 *   proximity, then outputs to OLED display and
 *   I2S audio via PAM8403 amplifier.
 *
 * Bus Layout:
 *   I2C (SDA=21, SCL=22) : OLED (0x3C), APDS9960, BMP180
 *   SPI (SCK=18, MISO=19, MOSI=23, CS=5) : SD card
 *   ADC1_CH6 (GPIO34)    : MQ135 air quality (input-only pin)
 *   Digital input GPIO32  : Sound/clap sensor
 *   Digital output GPIO33 : Buzzer
 ***********************************************/

/***************  LIBRARIES  ***************/
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LightProximityAndGesture.h>   // APDS9960 (proximity/gesture)
#include <Adafruit_BMP085_U.h>          // BMP180 unified driver
#include <DHT.h>                        // DHT11
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include "driver/adc.h"                 // ESP-IDF ADC driver (for GPIO34, input-only)

/***************  OLED CONFIG  ***************/
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/***************  SENSORS  ***************/
LightProximityAndGesture apds;
Adafruit_BMP085_Unified bmp = Adafruit_BMP085_Unified(10085);
DHT dht(4, DHT11);   // DHT11 data pin on GPIO4

/***************  PINS  ***************/
#define SOUND_PIN  32    // Digital sound/clap sensor (LOW = clap detected)
#define BUZZER_PIN 33    // Buzzer for audio confirmation
#define MQ135_PIN  34    // MQ135 air quality — GPIO34 is input-only, use ADC1_CH6

// SD card SPI pins (hardware SPI bus)
#define SD_CS   5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23

// MQ135 threshold — raw ADC (12-bit, 0–4095) above which air is flagged poor
// At 3.3V reference with DB_11 attenuation: ~200 raw ≈ elevated gas concentration
// Calibrate against known CO2/NH3 levels for production use
#define AIR_QUALITY_THRESHOLD 200

/***************  AUDIO  ***************/
AudioGeneratorWAV  *wav;
AudioFileSourceSD  *file;
AudioOutputI2S     *out;

/***************  FLAGS  ***************/
bool apdsAvailable = false;  // Set false if APDS9960 not detected at init

/***************  HELPER: LIST SD FILES  ***************/
void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  File root = fs.open(dirname);
  if (!root || !root.isDirectory()) {
    Serial.println("Failed to open directory");
    return;
  }
  File entry = root.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      Serial.print("DIR : ");
      Serial.println(entry.name());
      if (levels) listDir(fs, entry.name(), levels - 1);
    } else {
      Serial.print("FILE: ");
      Serial.print(entry.name());
      Serial.print("\tSIZE: ");
      Serial.println(entry.size());
    }
    entry = root.openNextFile();
  }
}

/***************  SETUP  ***************/
void setup() {
  Serial.begin(115200);

  // I2C bus: SDA=21, SCL=22 — shared by OLED, APDS9960, BMP180
  Wire.begin(21, 22);

  pinMode(SOUND_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // --- OLED init ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
    while (true);  // OLED is critical — halt if missing
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.println("EchoMirror");
  display.setTextSize(1);
  display.setCursor(20, 45);
  display.println("Waiting for clap...");
  display.display();

  // --- APDS9960 init (non-critical — system runs without it) ---
  if (apds.begin()) {
    apdsAvailable = true;
    apds.enableAmbientLightSensor();
    apds.enableProximitySensor();
    Serial.println("APDS9960 OK");
  } else {
    Serial.println("APDS9960 not found — proximity disabled");
  }

  // --- BMP180 init ---
  if (!bmp.begin()) {
    Serial.println("BMP180 not found!");
  }

  // --- DHT11 init ---
  dht.begin();

  // --- MQ135 ADC config ---
  // GPIO34 is input-only on ESP32; must use ESP-IDF ADC1 driver directly
  // DB_11 attenuation allows full 3.3V input range (0–3.9V)
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);

  // --- SD card init (SPI) ---
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 4000000U)) {
    Serial.println("SD init failed — audio disabled");
    // Non-critical: system continues without audio
  } else {
    Serial.println("SD card OK");
    Serial.printf("SD Card Size: %llu MB\n", SD.cardSize() / (1024 * 1024));
    listDir(SD, "/", 0);
  }

  // --- I2S audio output (internal DAC) ---
  out = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
  out->SetOutputModeMono(true);
  out->SetGain(0.8);
}

/***************  LOOP  ***************/
void loop() {

  // Clap detection — sensor pulls LOW on sound event
  int clapVal = digitalRead(SOUND_PIN);

  if (clapVal == LOW) {
    Serial.println("Clap detected!");

    // Buzzer confirmation beep
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(BUZZER_PIN, LOW);

    // Welcome screen
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(15, 20);
    display.println("Welcome!");
    display.display();
    delay(2000);

    // --- Read all sensors and display ---
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);

    // Temperature (DHT11)
    float temp = dht.readTemperature();
    display.print("Temp: ");
    display.print(temp);
    display.println(" C");

    // Pressure (BMP180)
    sensors_event_t event;
    bmp.getEvent(&event);
    if (event.pressure) {
      display.print("Pressure: ");
      display.print(event.pressure);
      display.println(" hPa");
    }

    // Air quality (MQ135 via ADC1_CH6)
    int airVal = adc1_get_raw(ADC1_CHANNEL_6);
    display.print("Air: ");
    display.println(airVal);
    if (airVal > AIR_QUALITY_THRESHOLD) {
      display.println("WARNING: Poor Air");
    }

    display.display();
    delay(3000);

    // Return to idle screen
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(10, 20);
    display.println("EchoMirror");
    display.setTextSize(1);
    display.setCursor(20, 45);
    display.println("Waiting for clap...");
    display.display();
  }
}
