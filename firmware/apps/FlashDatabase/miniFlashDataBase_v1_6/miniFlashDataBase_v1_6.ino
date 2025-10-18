#include <Wire.h>
#include <RTClib.h>
#include "FlashLogger.h"

// Pins: SDA=19, SCL=20, CS=4
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

  // OPTIONAL one-time set RTC:
  // rtc.adjust(DateTime(2025, 10, 8, 16, 0, 0));

  logger.begin(&rtc);

  // Set factory info ONCE (binary, stored last sector)
  // Leave empty "" if you don't want to overwrite existing
  logger.setFactoryInfo("AirMonitor C6", "W25Q64JV", "001245678912");

  // Date style at runtime: 1=Thai (DD/MM/YY), 2=ISO, 3=US
  logger.setDateStyle(1);

  // Simulate logs — now each record prints on its own line due to '\n' auto
  logger.append("{\"temp\":25.5,\"hum\":60,\"bat\":88}");
  logger.append("{\"temp\":25.7,\"hum\":61,\"bat\":88}");
  logger.append("{\"temp\":26.0,\"hum\":62,\"bat\":87}");

  // Mark current day pushed AFTER your app confirms upload (then GC can erase after 7 days)
  // logger.markCurrentDayPushed();

  // Pretty print (group by day)
  logger.printFormattedLogs();

  // Stats for UI
  FlashStats fs = logger.getFlashStats(3500.0f); // assume ~3.5KB/day
  Serial.printf("Total: %.2f MB  Used: %.2f MB  Free: %.2f MB  Used: %.1f%%  Health: %.1f%%  EstDays: %u\n",
                fs.totalMB, fs.usedMB, fs.freeMB, fs.usedPercent, fs.healthPercent, fs.estimatedDaysLeft);

  // Raw dump (debug)
  // logger.readAll();

  // Run GC (only erases sectors that are pushed AND >7 days old)
  // logger.gc();

  // Show factory info
  logger.printFactoryInfo();

  // Example factory reset (KEEP factory info)
  // bool ok = logger.factoryReset("847291506314");
  // Serial.println(ok ? "Factory reset OK" : "Factory reset FAILED");    
}

void loop() {
  static uint32_t last = 0;

  // Periodic logging every 10s (demo)
  if (millis() - last > 10000) {
    last = millis();
    logger.append("{\"temp\":25.9,\"hum\":60,\"bat\":87}");
  }

  // Serial commands:
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim();

    if (cmd.equalsIgnoreCase("pf")) {
      Serial.println(F("\n📜 Printing all formatted logs...\n"));
      logger.printFormattedLogs();
      Serial.println(F("----------------------------------\n"));
    } else if (cmd.equalsIgnoreCase("stats")) {
      FlashStats fs = logger.getFlashStats(3500.0f);
      Serial.printf("Total: %.2f MB  Used: %.2f MB  Free: %.2f MB  Used: %.1f%%  Health: %.1f%%  EstDays: %u\n",
                    fs.totalMB, fs.usedMB, fs.freeMB, fs.usedPercent, fs.healthPercent, fs.estimatedDaysLeft);
    } else if (cmd.equalsIgnoreCase("factory")) {
      logger.printFactoryInfo();
    } else if (cmd.equalsIgnoreCase("reset")) {
      Serial.println(F("⚠️ Enter 12-digit code:"));
      while (!Serial.available());
      String code = Serial.readStringUntil('\n'); code.trim();
      bool ok = logger.factoryReset(code.c_str());
      Serial.println(ok ? F("✅ Factory reset OK") : F("❌ Invalid code"));
    }
  }
}

