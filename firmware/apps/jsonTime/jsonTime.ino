/*
 *
 * @hardware
 * - DFRobot Beetle ESP32-C6 Mini
 * - DS3231 Real-Time Clock (RTC) Module
 *
 * @connections (I2C)
 * - ESP32-C6 G (GND) -> DS3231 GND
 * - ESP32-C6 V (VCC) -> DS3231 VCC (Connect to 3.3V or 5V depending on your module)
 * - ESP32-C6 D8 (SDA) -> DS3231 SDA
 * - ESP32-C6 D9 (SCL) -> DS3231 SCL
 *
 * @libraries
 * - RTClib by Adafruit: Install from the Arduino IDE Library Manager.
 * - ArduinoJson by Benoit Blanchon: Install from the Arduino IDE Library Manager.
 */

#include <Wire.h>
#include <RTClib.h>
#include <ArduinoJson.h>

// Create an RTC object
RTC_DS3231 rtc;

void setup() {
  // Start serial communication at 115200 baud rate
  Serial.begin(115200);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }

  Serial.println("DS3231 JSON Time Logger Initializing...");

  // Initialize the I2C bus (SDA, SCL pins)
  // For the Beetle ESP32-C6, default I2C pins are D8 (SDA) and D9 (SCL).
  // Wire.begin(SDA_PIN, SCL_PIN) can be used if you use different pins.
  if (!Wire.begin()) {
    Serial.println("Failed to initialize I2C communication!");
    while (1);
  }

  // Initialize the RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC module!");
    Serial.flush();
    while (1);
  }

  // The following section checks if the RTC has lost power. If so, it sets
  // the time to the date and time this sketch was compiled.
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    // The following line sets the RTC to the date & time this sketch was compiled.
    // IMPORTANT: UNCOMMENT THIS LINE AND UPLOAD ONCE TO SET THE TIME.
    // AFTER THAT, RE-COMMENT AND RE-UPLOAD TO PREVENT RESETTING THE TIME
    // ON EVERY REBOOT.
    // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial.println("Initialization complete. Starting time logs...");
}

void loop() {
  // Get the current date and time from the RTC
  DateTime now = rtc.now();

  // Create a JSON document.
  // The size can be adjusted, but 128 bytes is plenty for this data.
  JsonDocument doc;

  // Add members to the JSON object
  doc["year"] = now.year();
  doc["month"] = now.month();
  doc["day"] = now.day();
  doc["hour"] = now.hour();
  doc["minute"] = now.minute();
  doc["second"] = now.second();

  // Serialize the JSON document to the Serial port
  serializeJson(doc, Serial);
  
  // Print a newline character to separate the JSON objects
  Serial.println(" ");

  // Wait for 5 seconds before logging again
  delay(5000);
}
