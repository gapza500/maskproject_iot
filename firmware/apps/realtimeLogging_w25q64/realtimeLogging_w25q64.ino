/*
 * @project
 * ESP32-C6 Time-Series Data Logger (Corrected for External Flash)
 */

#include <Wire.h>
#include <RTClib.h>
#include <ArduinoJson.h>

// Libraries for External SPI Flash
#include <SPI.h>
#include <Adafruit_SPIFlash.h>
#include <Adafruit_LittleFS.h>

// Define the Chip Select pin for the W25Q64
#define W25Q64_CS 4 
#define LOG_FILE "/datalog.json" // Adafruit_LittleFS uses a forward slash

// 1. Set up the flash transport object
Adafruit_FlashTransport_SPI flashTransport(W25Q64_CS, SPI);

// 2. Set up the flash object and specify the chip
Adafruit_SPIFlash flash(&flashTransport);

// 3. Set up the filesystem object
Adafruit_LittleFS filesys;

// Create an RTC object
RTC_DS3231 rtc;

// Initialize the external flash filesystem
void initFlashFS() {
  if (!flash.begin()) {
    Serial.println("Error, failed to initialize external flash chip!");
    while(1);
  }
  Serial.print("Flash chip JEDEC ID: 0x"); Serial.println(flash.getJEDECID(), HEX);

  // First time run, format the flash to set up the filesystem.
  // After the first run, you can comment this out.
  // bool formatted = filesys.format(&flash);
  // if (!formatted) {
  //   Serial.println("Failed to format filesystem!");
  // }

  if (!filesys.begin(&flash)) {
    Serial.println("Error, failed to mount filesystem!");
    while(1);
  }
  Serial.println("External flash filesystem mounted successfully.");
}

// Write a new log entry to the file
void writeLogEntry(const char* path, DateTime& dt) {
  // Open the file in append mode. NOTE: We use 'filesys', not 'LittleFS'
  File file = filesys.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file for appending!");
    return;
  }

  JsonDocument doc;
  doc["year"] = dt.year();
  doc["month"] = dt.month();
  doc["day"] = dt.day();
  doc["hour"] = dt.hour();
  doc["minute"] = dt.minute();
  doc["second"] = dt.second();

  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write to file");
  } else {
    file.println(); 
    Serial.print("Log entry written: ");
    serializeJson(doc, Serial);
    Serial.println();
  }
  
  file.close();
}

// Read all log entries from the file
void readLogFile(const char* path) {
  File file = filesys.open(path, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading!");
    return;
  }

  Serial.println("\n--- Reading Log File ---");
  while (file.available()) {
    String line = file.readStringUntil('\n');
    Serial.println(line);
  }
  file.close();
  Serial.println("--- End of Log File ---\n");
}

void setup() {
  Serial.begin(115200);
  delay(2000); 
  Serial.println("Initializing Data Logger...");

  // 1. Initialize the external flash filesystem
  initFlashFS();

  // 2. Initialize I2C for the RTC
  if (!Wire.begin()) {
    Serial.println("Failed to initialize I2C!");
    while (1);
  }

  // 3. Initialize the RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC module!");
    while (1);
  }

  // 4. Set the RTC time if needed
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting time to compile time!");
    // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial.println("Initialization complete.");
  readLogFile(LOG_FILE);
}

void loop() {
  DateTime now = rtc.now();
  writeLogEntry(LOG_FILE, now);

  Serial.println("Waiting 60 seconds for next log...");
  delay(60000); 
}