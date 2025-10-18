#include <Wire.h>
#include <RTClib.h>
#include "FlashLogger.h"

// Pins: SDA=19, SCL=20, CS=4
RTC_DS3231 rtc;
FlashLogger logger(4);

  // One small helper (line reader)
  String readLine(Stream& s) {
    static String buf;
    while (s.available()) {
      char c = (char)s.read();
      if (c == '\r') continue;
      if (c == '\n') { String out = buf; buf = ""; return out; }
      buf += c;
    }
    return "";
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(19, 20);
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    while (1) { delay(1); }
  }

  // rtc.adjust(DateTime(2025, 10, 9, 12, 0, 0)); // set once if needed

  if (!logger.begin(&rtc)) {
    Serial.println("Logger init failed"); while (1) {}
  }

  // One-time info (kept in factory sector)
  logger.setFactoryInfo("AirMonitor C6", "W25Q64JV", "001245678912");
  logger.setDateStyle(1); // 1=Thai

  // Optional daily cap (e.g., 16 KB/day)
  // logger.setMaxDailyBytes(16 * 1024);

  // quick sample logs
  logger.append("{\"temp\":25.5,\"hum\":60,\"bat\":88}");
  logger.append("{\"temp\":25.7,\"hum\":61,\"bat\":88}");
  logger.append("{\"temp\":26.0,\"hum\":62,\"bat\":87}");

  // show once
  logger.printFormattedLogs();

  FlashStats fs = logger.getFlashStats(3500.0f);
  Serial.printf("Total: %.2f MB  Used: %.2f MB  Free: %.2f MB  Used: %.1f%%  Health: %.1f%%  EstDays: %u  LowSpace:%s\n",
                fs.totalMB, fs.usedMB, fs.freeMB, fs.usedPercent, fs.healthPercent, fs.estimatedDaysLeft,
                logger.isLowSpace() ? "YES":"NO");

  logger.printFactoryInfo();

  // After your app confirms upload:
  // logger.markCurrentDayPushed();
  // logger.gc();

}

void loop() {
  // periodic demo logging
  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    logger.append("{\"temp\":25.9,\"hum\":60,\"bat\":87}");
  }

  // Shell: only runs when you type a line
  String line = readLine(Serial);
  if (line.length()) {
    if (!logger.handleCommand(line, Serial)) {
      // your other commands (pf, stats, factory, reset, gc, etc.)
      if (line.equalsIgnoreCase("pf"))        { logger.printFormattedLogs(); }
      else if (line.equalsIgnoreCase("stats")){ auto fs=logger.getFlashStats(3500); Serial.printf("Used: %.2fMB\n", fs.usedMB); }
      else if (line.equalsIgnoreCase("factory")) { logger.printFactoryInfo(); }
      else if (line.equalsIgnoreCase("gc"))   { logger.gc(); }
      else if (line.startsWith("reset"))      { /* ask code… then logger.factoryReset(code); */ }
      else { Serial.println("unknown command"); }
    }
  }
}