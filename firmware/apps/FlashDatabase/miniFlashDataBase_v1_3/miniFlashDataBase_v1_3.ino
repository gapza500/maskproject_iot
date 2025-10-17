#include <Wire.h>
#include <RTClib.h>
#include "FlashLogger.h"

// Pins: SDA=19, SCL=20 (per your board), CS=4 (for flash)
RTC_DS3231 rtc;
FlashLogger logger(4);

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(19, 20);
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    while (1) { delay(1); }
  }

  // Optional: set RTC once (comment out after set)
  // rtc.adjust(DateTime(2025, 10, 8, 16, 0, 0)); // YYYY,MM,DD,hh,mm,ss (local)

  logger.begin(&rtc);

  // Choose date style at runtime:
  // 1 = Thai (DD/MM/YY) [default], 2 = ISO (YYYY-MM-DD), 3 = US (MM/DD/YY)
  logger.setDateStyle(1); // Thai

  // Simulate logs
  logger.append("{\"temp\":25.5,\"hum\":60,\"bat\":88}");
  logger.append("{\"temp\":25.7,\"hum\":61,\"bat\":88}");
  logger.append("{\"temp\":26.0,\"hum\":62,\"bat\":87}");

  // Mark current day as pushed (call this ONLY after your mobile app confirms upload)
  // logger.markCurrentDayPushed();

  // Pretty print grouped by day
  logger.printFormattedLogs();

  // Raw dump (debug)
  // logger.readAll();

  // Run GC (will only erase sectors that are pushed + older than 7 days)
  // logger.gc();
}

void loop() {
  // Example periodic logging:
  // static uint32_t last = 0;
  // if (millis() - last > 10000) {
  //   last = millis();
  //   logger.append("{\"temp\":25.9,\"hum\":60,\"bat\":87}");
  // }
}
