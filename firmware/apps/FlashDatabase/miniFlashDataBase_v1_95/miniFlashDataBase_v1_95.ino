#include <Wire.h>
#include <RTClib.h>
#include "FlashLogger.h"

RTC_DS3231 rtc;
FlashLogger logger;

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(19, 20);            // your RTC pins
  if (!rtc.begin()) { Serial.println("RTC not found"); while(1){} }

  FlashLoggerConfig cfg;
  cfg.rtc            = &rtc;
  cfg.spi_cs_pin     = 4;
  cfg.dateStyle      = DATE_THAI;       // 1=Thai, 2=ISO, 3=US
  cfg.totalSizeBytes = 8*1024*1024;     // adapt if different
  cfg.sectorSize     = 4096;
  cfg.retentionDays  = 7;
  cfg.dailyBytesHint = 3500;
  cfg.defaultOut     = OUT_JSONL;
  cfg.csvColumns     = "ts,bat,temp";
  cfg.model          = "AirMonitor C6";
  cfg.flashModel     = "W25Q64JV";
  cfg.deviceId       = "001245678912";
  cfg.resetCode12    = "847291506314";
  cfg.enableShell    = true;

  if (!logger.begin(cfg)) {
    Serial.println("FlashLogger init failed");
    while(1){}
  }

  // quick smoke
  logger.printFactoryInfo();
}

static String readLine(Stream& s) {
  static String buf;
  while (s.available()) {
    char c = (char)s.read();
    if (c == '\r') continue;
    if (c == '\n') { String out = buf; buf = ""; return out; }
    buf += c;
  }
  return "";
}

void loop() {
  // Log something periodically
  static uint32_t last=0;
  if (millis()-last > 10000) {
    last = millis();
    logger.append("{\"temp\":25.9,\"hum\":60,\"bat\":87}");
  }

  // Shell (one-liner handling)
  String line = readLine(Serial);
  if (line.length()) logger.handleCommand(line, Serial);

  // You can also use programmatic queries anywhere:
  //   - latest 10
  //   - filtered week
  //   - export since cursor
}
