#include <Wire.h>
#include "RTClib.h"
#include "FlashLogger.h"

RTC_DS3231 rtc;
FlashLogger logger(4); // CS = 4

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(19, 20);  // SDA19, SCL20
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    while (1);
  }

  logger.begin(&rtc);

  // simulate sensor data
  logger.append("{\"temp\":25.5,\"hum\":60}");
  logger.append("{\"temp\":25.7,\"hum\":61}");
  logger.append("{\"temp\":26.0,\"hum\":62}");

  delay(500);
  logger.printFormattedLogs();

  // Run GC occasionally
  logger.gc();
}

void loop() {}
