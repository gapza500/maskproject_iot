
#include "OledDriver.h"
#include <Arduino.h>

bool OledDriver::begin() {
  // Stub: hook up your real OLED lib here
  return true;
}
void OledDriver::showVersion(const char* v) {
  Serial.print("[OLED] Version: "); Serial.println(v);
}
void OledDriver::printLine(uint8_t row, const String& s) {
  Serial.print("[OLED] row "); Serial.print(row); Serial.print(": "); Serial.println(s);
}
void OledDriver::clear() {
  Serial.println("[OLED] clear");
}
